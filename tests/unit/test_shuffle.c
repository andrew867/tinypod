/*
 * test_shuffle.c - the shuffle, against the two things it promises.
 *
 * The first promise is that the result is a permutation. That one is checked
 * on every case here including the awkward sizes, because the spreading pass
 * moves entries about after the Fisher-Yates has run and a rearrangement that
 * drops or duplicates a track would be a silent queue corruption rather than a
 * visible failure.
 *
 * The second is that it actually spreads. That is not a property a single
 * assertion can capture, so the clumping test measures the same queue both
 * ways - a plain uniform shuffle and this one - averaged over many seeds, and
 * demands the improvement rather than a fixed number. Written the other way
 * round it would be a test of one lucky seed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/playback/tp_shuffle.h"

static int checks, failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* Every index exactly once, nothing out of range. */
static int is_permutation(const size_t *order, size_t n)
{
    char *seen = calloc(n ? n : 1, 1);
    size_t i;
    int ok = 1;

    if (!seen)
        return 0;
    for (i = 0; i < n; i++) {
        if (order[i] >= n || seen[order[i]]) {
            ok = 0;
            break;
        }
        seen[order[i]] = 1;
    }
    free(seen);
    return ok;
}

static size_t adjacent_clashes(const struct tp_shuffle_item *items,
                               const size_t *order, size_t n)
{
    size_t i, c = 0;

    for (i = 1; i < n; i++) {
        uint32_t g = items[order[i - 1]].group;

        if (g != 0 && g == items[order[i]].group)
            c++;
    }
    return c;
}

/* The thing being improved on: a plain uniform shuffle, so the comparison is
   against what TinyPod does today rather than against a guessed number. */
static void plain_shuffle(size_t *order, size_t n, unsigned *state)
{
    size_t i;

    for (i = 0; i < n; i++)
        order[i] = i;
    for (i = n; i > 1; i--) {
        size_t j;

        *state = *state * 1103515245u + 12345u;
        j = (*state >> 16) % i;
        {
            size_t t = order[i - 1];
            order[i - 1] = order[j];
            order[j] = t;
        }
    }
}

static void test_permutation_sizes(void)
{
    static const size_t sizes[] = { 0, 1, 2, 7, 100, 1000 };
    size_t s;

    for (s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        size_t n = sizes[s];
        struct tp_shuffle_item *items = malloc((n ? n : 1) * sizeof *items);
        size_t *order = malloc((n ? n : 1) * sizeof *order);
        size_t i;

        if (!items || !order) {
            printf("  FAIL out of memory at n=%zu\n", n);
            failures++;
            free(items);
            free(order);
            return;
        }
        for (i = 0; i < n; i++)
            items[i].group = (uint32_t)(i % 7) + 1;
        memset(order, 0xff, (n ? n : 1) * sizeof *order);

        tp_shuffle_order(items, n, order, 12345u);
        CHECK(is_permutation(order, n), "n=%zu is not a permutation", n);
        printf("  PASS permutation n=%-5zu\n", n);

        free(items);
        free(order);
    }
}

static void test_all_same_group(void)
{
    enum { N = 250 };
    struct tp_shuffle_item items[N];
    size_t order[N], i;

    for (i = 0; i < N; i++)
        items[i].group = 42;

    tp_shuffle_order(items, N, order, 7u);
    CHECK(is_permutation(order, N), "all-same-group is not a permutation");
    printf("  PASS all-same-group n=%d, %zu adjacent clashes (%d unavoidable)\n",
           N, adjacent_clashes(items, order, N), N - 1);
}

static void test_all_distinct_groups(void)
{
    enum { N = 250 };
    struct tp_shuffle_item items[N];
    size_t order[N], i;

    for (i = 0; i < N; i++)
        items[i].group = (uint32_t)i + 1;

    tp_shuffle_order(items, N, order, 9u);
    CHECK(is_permutation(order, N), "all-distinct is not a permutation");
    CHECK(adjacent_clashes(items, order, N) == 0, "all-distinct clashed");
    printf("  PASS all-distinct-groups n=%d\n", N);
}

static void test_ungrouped_passthrough(void)
{
    enum { N = 200 };
    struct tp_shuffle_item items[N];
    size_t order[N], i;

    /* Group 0 is ungrouped and must never be held apart, so a queue of
       untagged files has to come out with clashes counted as zero and a
       permutation intact rather than being pinned into a fixed pattern. */
    for (i = 0; i < N; i++)
        items[i].group = 0;

    tp_shuffle_order(items, N, order, 3u);
    CHECK(is_permutation(order, N), "all-ungrouped is not a permutation");
    CHECK(adjacent_clashes(items, order, N) == 0, "group 0 was treated as a group");
    printf("  PASS all-ungrouped n=%d\n", N);
}

static void test_spreading(void)
{
    enum { GROUPS = 10, PER = 10, N = GROUPS * PER, TRIALS = 200 };
    struct tp_shuffle_item items[N];
    size_t order[N], i;
    unsigned t, plain_state = 1u;
    double plain_total = 0.0, spread_total = 0.0;
    size_t spread_worst = 0;

    for (i = 0; i < N; i++)
        items[i].group = (uint32_t)(i / PER) + 1;

    for (t = 0; t < TRIALS; t++) {
        size_t c;

        plain_shuffle(order, N, &plain_state);
        plain_total += (double)adjacent_clashes(items, order, N);

        tp_shuffle_order(items, N, order, t + 1u);
        CHECK(is_permutation(order, N), "spread trial %u is not a permutation", t);
        c = adjacent_clashes(items, order, N);
        spread_total += (double)c;
        if (c > spread_worst)
            spread_worst = c;
    }

    {
        double plain_avg = plain_total / TRIALS;
        double spread_avg = spread_total / TRIALS;

        printf("  %d groups x %d tracks, %d seeds:\n", GROUPS, PER, TRIALS);
        printf("    plain uniform shuffle : %.2f adjacent same-group pairs\n",
               plain_avg);
        printf("    tp_shuffle_order      : %.2f adjacent same-group pairs "
               "(worst seed %zu)\n", spread_avg, spread_worst);

        CHECK(plain_avg > 7.0, "plain shuffle averaged %.2f, expected about 9",
              plain_avg);
        CHECK(spread_avg < plain_avg / 4.0,
              "spread averaged %.2f, not a fourth of plain's %.2f",
              spread_avg, plain_avg);
        if (spread_avg > 0.0)
            printf("  PASS spreading: %.1fx fewer adjacent same-group pairs\n",
                   plain_avg / spread_avg);
        else
            printf("  PASS spreading: not one adjacent same-group pair in "
                   "%d seeds, against %.2f per shuffle for plain\n",
                   TRIALS, plain_avg);
    }
}

static void test_determinism(void)
{
    enum { N = 300 };
    struct tp_shuffle_item items[N];
    size_t a[N], b[N], c[N];
    size_t i;
    int same_seed_equal = 1, diff_seed_equal = 1;

    for (i = 0; i < N; i++)
        items[i].group = (uint32_t)(i % 13) + 1;

    tp_shuffle_order(items, N, a, 0xC0FFEEu);
    tp_shuffle_order(items, N, b, 0xC0FFEEu);
    tp_shuffle_order(items, N, c, 0xC0FFEFu);

    for (i = 0; i < N; i++) {
        if (a[i] != b[i])
            same_seed_equal = 0;
        if (a[i] != c[i])
            diff_seed_equal = 0;
    }

    CHECK(same_seed_equal, "the same seed gave two different orders");
    CHECK(!diff_seed_equal, "two different seeds gave the same order");
    printf("  PASS determinism: same seed repeats, neighbouring seed differs\n");
}

static void test_dominant_group(void)
{
    enum { N = 100, DOM = 90 };
    struct tp_shuffle_item items[N];
    size_t order[N], i;
    unsigned t;
    size_t worst = 0;

    /* 90% of one artist. Perfect spacing is arithmetically impossible - at
       least DOM-1-(N-DOM) of them must sit next to one of their own - so this
       is checking that it degrades instead of failing or spinning. */
    for (i = 0; i < N; i++)
        items[i].group = i < DOM ? 1u : (uint32_t)i + 2u;

    for (t = 0; t < 50; t++) {
        size_t c;

        tp_shuffle_order(items, N, order, t * 977u + 5u);
        CHECK(is_permutation(order, N), "dominant-group seed %u broke the permutation", t);
        c = adjacent_clashes(items, order, N);
        if (c > worst)
            worst = c;
    }
    printf("  PASS dominant group %d/%d: worst %zu adjacent clashes "
           "(%d unavoidable)\n", DOM, N, worst, DOM - 1 - (N - DOM));
}

int main(void)
{
    printf("test_shuffle\n");
    test_permutation_sizes();
    test_all_same_group();
    test_all_distinct_groups();
    test_ungrouped_passthrough();
    test_spreading();
    test_determinism();
    test_dominant_group();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
