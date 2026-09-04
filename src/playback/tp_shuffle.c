/*
 * Shuffle.
 *
 * A uniform shuffle is correct and feels broken. Over a hundred tracks drawn
 * from ten albums it will put two from the same record next to each other
 * about nine times, and every one of those is read by the person holding the
 * device as the shuffle having failed - they know the album, they can hear it
 * happening, and no amount of explaining that clumps are what randomness
 * actually looks like will change their mind. What people mean by shuffle is
 * not "uniformly random order", it is "surprise me, and do not repeat
 * yourself".
 *
 * So this starts from a uniform permutation, which is what gives the result
 * its unpredictability, and then rearranges it so that tracks sharing a group
 * sit as far from each other as the set allows. The rearrangement only ever
 * moves entries about; it never drops or duplicates one to make the spacing
 * come out. A queue that quietly loses a track is a far worse bug than one
 * that clumps, so the permutation property is the thing that is never traded.
 */
#include "tp_shuffle.h"

#include <stdlib.h>

/*
 * A small PCG32, kept here rather than calling rand().
 *
 * rand() is global state shared with every other part of the process, so two
 * shuffles with any other caller of rand() in between would stop being
 * reproducible - and a shuffle that cannot be replayed from its seed cannot be
 * tested, which is how a spreading bug would reach a device unnoticed. This
 * owns its state, costs a multiply and a shift, and is far better distributed
 * than the xorshift it would otherwise be tempting to write.
 */
struct tp_rng {
    uint64_t state;
};

static void rng_seed(struct tp_rng *r, unsigned seed)
{
    /* SplitMix64 on the way in. Seeds arrive from the caller as small
       consecutive numbers as often as not, and fed straight to an LCG those
       would give first outputs that are themselves nearly consecutive. */
    uint64_t z = (uint64_t)seed + 0x9e3779b97f4a7c15ull;

    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    r->state = z ^ (z >> 31);
}

static uint32_t rng_next(struct tp_rng *r)
{
    uint64_t old = r->state;
    uint32_t xorshifted, rot;

    r->state = old * 6364136223846793005ull + 1442695040888963407ull;
    xorshifted = (uint32_t)(((old >> 18) ^ old) >> 27);
    rot = (uint32_t)(old >> 59);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

/*
 * A uniform value below `bound`.
 *
 * The rejection loop matters more than it looks. A plain modulo of a 32-bit
 * draw favours the low values whenever the bound does not divide 2^32, and
 * that bias lands squarely on the early positions of a Fisher-Yates - which is
 * precisely the property the shuffle exists to provide. Throwing away the
 * short unrepresentable tail costs, on average, well under one extra draw.
 */
static uint32_t rng_below(struct tp_rng *r, uint32_t bound)
{
    uint32_t threshold, v;

    if (bound < 2)
        return 0;
    threshold = (uint32_t)((0u - bound) % bound);
    do {
        v = rng_next(r);
    } while (v < threshold);
    return v % bound;
}

/* Shuffle order[first .. first+len-1] in place. */
static void shuffle_range(size_t *order, size_t first, size_t len,
                          struct tp_rng *r)
{
    size_t i;

    /* Queues on this device are thousands of tracks, not billions, so the
       32-bit bound is not a limit anyone will meet. */
    for (i = len; i > 1; i--) {
        size_t j = (size_t)rng_below(r, (uint32_t)i);
        size_t t = order[first + i - 1];

        order[first + i - 1] = order[first + j];
        order[first + j] = t;
    }
}

/*
 * Two entries clash when they share a group. Group 0 means the track had no
 * artist and no album worth grouping on, and holding those apart from each
 * other would be inventing a relationship that is not there - a folder of
 * untagged files would come out barely shuffled at all.
 */
static int clash(const struct tp_shuffle_item *items, size_t a, size_t b)
{
    uint32_t g = items[a].group;

    return g != 0 && g == items[b].group;
}

/* One item, carrying where the uniform shuffle put it so that sorting by
   group can preserve that order inside each group instead of destroying it. */
struct tp_pair {
    uint32_t group;
    size_t   pos;
    size_t   idx;
};

/* A run of tp_pair sharing a group, plus a random key so that buckets of
   equal size are not ordered by group number - which on a library imported in
   alphabetical order would mean ordered by artist name. */
struct tp_bucket {
    size_t   start;
    size_t   len;
    uint32_t key;
};

static int cmp_pair(const void *a, const void *b)
{
    const struct tp_pair *x = a, *y = b;

    if (x->group != y->group)
        return x->group < y->group ? -1 : 1;
    if (x->pos != y->pos)
        return x->pos < y->pos ? -1 : 1;
    return 0;
}

static int cmp_bucket(const void *a, const void *b)
{
    const struct tp_bucket *x = a, *y = b;

    if (x->len != y->len)
        return x->len > y->len ? -1 : 1;   /* largest first */
    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    /* qsort is not stable, so the comparison has to be a total order or the
       result would depend on the library rather than on the seed. */
    return x->start < y->start ? -1 : 1;
}

/* Add a boundary index to a small set, skipping duplicates and anything off
   the end. Boundary b is the join between positions b-1 and b. */
static void bound_add(size_t *set, size_t *count, size_t b, size_t n)
{
    size_t i;

    if (b < 1 || b >= n)
        return;
    for (i = 0; i < *count; i++)
        if (set[i] == b)
            return;
    set[(*count)++] = b;
}

static size_t bound_cost(const struct tp_shuffle_item *items,
                         const size_t *order, const size_t *set, size_t count)
{
    size_t i, c = 0;

    for (i = 0; i < count; i++)
        if (clash(items, order[set[i] - 1], order[set[i]]))
            c++;
    return c;
}

/*
 * Mop up the few clashes the interleave leaves behind.
 *
 * Walk the order once and, wherever two neighbours share a group, look a short
 * way ahead for a track that can be swapped in without creating a clash of its
 * own. The candidate is accepted only if it lowers the number of clashing
 * joins among the four that the swap can touch, which is what stops the pass
 * from trading one clash for another somewhere else.
 *
 * The window is fixed rather than unbounded, and the pass runs once rather
 * than to convergence, because the interesting failure here is not a poor
 * arrangement but a hang. When one artist holds most of the queue there are
 * clashes that no swap can remove, and the honest thing is to leave them and
 * move on - the device would otherwise sit searching for an arrangement that
 * does not exist while the user waits for the music to start.
 */
#define TP_REPAIR_WINDOW 32

static void repair(const struct tp_shuffle_item *items, size_t *order, size_t n)
{
    size_t p;

    for (p = 1; p < n; p++) {
        size_t q, last;

        if (!clash(items, order[p - 1], order[p]))
            continue;

        last = p + TP_REPAIR_WINDOW;
        if (last > n - 1)
            last = n - 1;

        for (q = p + 1; q <= last; q++) {
            size_t set[4], count = 0, before, after, t;

            bound_add(set, &count, p, n);
            bound_add(set, &count, p + 1, n);
            bound_add(set, &count, q, n);
            bound_add(set, &count, q + 1, n);

            before = bound_cost(items, order, set, count);
            t = order[p]; order[p] = order[q]; order[q] = t;
            after = bound_cost(items, order, set, count);
            if (after < before)
                break;
            t = order[p]; order[p] = order[q]; order[q] = t;
        }
    }
}

/*
 * The spreading itself.
 *
 * Bucket the tracks by group, put the buckets in largest-first order, lay them
 * end to end into one sequence and deal that sequence round-robin into m
 * piles, where m is the size of the largest bucket. Read the piles back out
 * one after another and that is the play order.
 *
 * The reason this works is that a bucket is a run of at most m consecutive
 * entries in the sequence, and m consecutive entries dealt round-robin into m
 * piles land in m different piles. So no pile ever holds two tracks of the
 * same group, and since a pile is about n/m long, two tracks of a group are
 * about n/m apart - which is the widest spacing an artist with m tracks can
 * have in a queue of n. Taking the largest bucket first is what makes m the
 * count of the most common artist, so that artist gets the best spacing
 * available and every rarer one is spaced at least as well by consequence.
 *
 * The trade-off is that the piles come out as near-identical rotations of one
 * another, so the raw dealt order has an audible cycle to it - the same run of
 * artists over and over. Shuffling inside each pile removes that, and it is
 * free: a pile has no two tracks of a group in it to begin with, so no
 * reordering within one can create a clash. All that is left is the joins
 * between piles, and those are what the repair pass above is for.
 *
 * The alternative considered was assigning each group a stride of n/count and
 * probing for a free slot around each target. It spaces just as well when it
 * fits and degrades far worse when it does not: with one dominant group the
 * probing turns into a linear scan over an almost-full array for every
 * placement. The piles have no such cliff - they simply come out short.
 */
void tp_shuffle_order(const struct tp_shuffle_item *items, size_t n,
                      size_t *order, unsigned seed)
{
    struct tp_rng rng;
    struct tp_pair *pairs;
    struct tp_bucket *buckets;
    size_t *seq, *pile_at;
    size_t nbuckets = 0, i, j, m;

    if (!order || n == 0)
        return;

    rng_seed(&rng, seed);
    for (i = 0; i < n; i++)
        order[i] = i;
    shuffle_range(order, 0, n, &rng);

    if (!items || n < 2)
        return;

    pairs = malloc(n * sizeof *pairs);
    buckets = malloc(n * sizeof *buckets);
    seq = malloc(n * sizeof *seq);
    pile_at = malloc(n * sizeof *pile_at);
    if (!pairs || !buckets || !seq || !pile_at) {
        /* The uniform shuffle already in `order` stands. Worse spacing, but
           still every track exactly once, which is the part that matters. */
        free(pairs);
        free(buckets);
        free(seq);
        free(pile_at);
        return;
    }

    for (i = 0; i < n; i++) {
        pairs[i].group = items[order[i]].group;
        pairs[i].pos = i;
        pairs[i].idx = order[i];
    }
    /* Sorting by group brings each group together; the shuffled position as
       secondary key keeps the members of a group in the uniformly random
       order the Fisher-Yates gave them, so the within-bucket shuffle the
       spreading needs comes for free. */
    qsort(pairs, n, sizeof *pairs, cmp_pair);

    i = 0;
    while (i < n) {
        size_t run = 1;

        /* Group 0 is ungrouped, so each of those becomes a bucket of one and
           is never held apart from anything. */
        if (pairs[i].group != 0)
            while (i + run < n && pairs[i + run].group == pairs[i].group)
                run++;

        buckets[nbuckets].start = i;
        buckets[nbuckets].len = run;
        buckets[nbuckets].key = rng_next(&rng);
        nbuckets++;
        i += run;
    }
    qsort(buckets, nbuckets, sizeof *buckets, cmp_bucket);

    j = 0;
    for (i = 0; i < nbuckets; i++) {
        size_t k;

        for (k = 0; k < buckets[i].len; k++)
            seq[j++] = pairs[buckets[i].start + k].idx;
    }

    m = buckets[0].len;

    /* Where each pile begins. Pile j takes the sequence entries whose position
       is congruent to j modulo m, so it holds ceil((n - j) / m) of them. */
    j = 0;
    for (i = 0; i < m; i++) {
        pile_at[i] = j;
        j += (n - i + m - 1) / m;
    }

    for (i = 0; i < n; i++)
        order[pile_at[i % m] + i / m] = seq[i];

    for (i = 0; i < m; i++) {
        size_t end = (i + 1 < m) ? pile_at[i + 1] : n;

        shuffle_range(order, pile_at[i], end - pile_at[i], &rng);
    }

    repair(items, order, n);

    free(pairs);
    free(buckets);
    free(seq);
    free(pile_at);
}
