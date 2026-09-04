/*
 * Which files would not read, and how that is remembered cheaply.
 *
 * The awkward part is not the bookkeeping, it is who asks and how often.
 * tp_badfile_mark runs once per track that fails, which is a human-speed
 * event; tp_badfile_is_bad runs from the list row renderer, so it is called
 * for every visible row of every frame while someone spins the wheel. Those
 * two want opposite things, and the whole shape of this file follows from
 * letting the reader win: lookups walk a hash bucket that is empty or one
 * entry long, and inserts are allowed to be as slow as a scan of the table.
 *
 * A linear list of 256 paths compared with strcmp would have been fine on a
 * desktop. Here it is up to 256 string comparisons per row, times a dozen
 * rows, sixty times a second, on a 533 MHz Cortex-A8 that is also decoding
 * audio - which is exactly how a browser that scrolls smoothly turns into one
 * that stutters only on the volumes that have failures, which is to say only
 * on the devices where anyone would notice.
 */
#include "tp_badfile.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

/*
 * How many are remembered, and how wide the index is.
 *
 * 256 is far more than a working volume produces and small enough that being
 * wrong about it costs nothing: the table below is about 70 KiB of BSS, which
 * this machine can spare.
 *
 * A list that grew on demand could not be spared, and the arithmetic is the
 * wrong way round for it. The situation this module exists to survive is a
 * translation layer erroring over half the volume, which is precisely the
 * situation in which an unbounded list is longest - so the failure would
 * spend the 55 MiB the whole system has on a record of it. When the table
 * overflows the oldest goes, on the reasoning that a path marked bad three
 * hundred files ago is one nobody is looking at now, and the cost of
 * forgetting it is one more failed open rather than a wrong answer.
 *
 * Twice as many buckets as entries, and a power of two so the index is a mask
 * rather than a division. At full occupancy that is a load factor of 0.5, so
 * the average chain the renderer walks is half an entry long.
 */
#define TP_BADFILE_MAX     256
#define TP_BADFILE_BUCKETS 512

/*
 * How much of each path is kept.
 *
 * Paths can be TP_BROWSE_PATH_MAX (1024) bytes, and 256 of those would be a
 * quarter of a megabyte of BSS to hold a list that is almost always empty.
 * So each entry keeps the last 255 bytes and the 64-bit hash of the whole
 * thing, and a match has to agree on the hash, the full length, and that
 * tail. The tail rather than the head because paths on this volume share
 * their beginnings - everything is under /iPod_Control/Music/Fnn - and differ
 * at the end, so the head is the part that carries no information.
 *
 * The trade-off is stated rather than hidden: two paths could still be
 * confused if they are the same length, end in the same 255 bytes, AND
 * collide in FNV-1a 64. Nothing on a music volume does that by accident, and
 * the consequence if one ever did is a single row drawn grey that would have
 * played - not data loss, not a crash, and cleared by tp_badfile_clear or by
 * the next mount. Real iPod paths are under 60 bytes, so in practice the
 * stored copy is the whole path and this reasoning never comes up.
 */
#define TP_BADFILE_KEEP 256

struct entry {
    uint64_t hash;                  /* FNV-1a over the whole path */
    uint32_t len;                   /* strlen of the whole path */
    uint32_t seq;                   /* insertion order, for evicting */
    uint16_t next;                  /* next in bucket, +1 encoded; 0 ends */
    uint8_t  used;
    char     tail[TP_BADFILE_KEEP]; /* the last KEEP-1 bytes, NUL terminated */
};

/*
 * Bucket heads and chain links hold an entry index plus one, so that zero
 * means "empty" and the entire table is correct as static zero-initialised
 * memory. That is worth a little arithmetic: the header deliberately has no
 * init call for an integration to forget, and a lazy "have we set up yet"
 * flag would be a race in exactly the module that exists to be called from
 * two threads.
 */
static struct entry   g_entries[TP_BADFILE_MAX];
static uint16_t       g_bucket[TP_BADFILE_BUCKETS];
static unsigned       g_count;
static uint32_t       g_seq;

/*
 * One static mutex, held for the whole of every call.
 *
 * tp_player.c guards its own state with a plain pthread_mutex_t and no
 * cleverness, and this follows it. The read side does not strictly need a
 * lock on ARMv7 for a table this shape, but "does not strictly need" is an
 * argument that has to be re-derived by whoever next changes the eviction
 * path, and an uncontended pthread mutex is a compare-and-swap and no
 * syscall - a few nanoseconds against a list row that is about to be drawn.
 * The writer is the decode thread, which reports a file that would not read
 * from inside playback, so the two really are concurrent.
 *
 * Statically initialised for the same reason the table is: there is no
 * tp_badfile_init to call and none is wanted.
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * FNV-1a, 64-bit. Not a cryptographic hash and does not need to be. What it
 * does need is to scatter paths that differ in one or two characters into
 * different buckets, because that is what a music volume is made of -
 * /iPod_Control/Music/F04/GHTQ.mp3 next to /iPod_Control/Music/F04/GHTR.mp3 -
 * and the additive hashes people reach for first put every one of those in
 * the same place. The 64-bit variant rather than the 32-bit one because the
 * hash is also what keeps the truncated tail above honest.
 */
static uint64_t path_hash(const char *path, size_t *len_out)
{
    uint64_t h = 14695981039346656037ull;
    size_t i;

    for (i = 0; path[i]; i++) {
        h ^= (unsigned char)path[i];
        h *= 1099511628211ull;
    }
    *len_out = i;
    return h;
}

/* The part of the path an entry stores. See TP_BADFILE_KEEP above. */
static const char *path_tail(const char *path, size_t len)
{
    if (len < TP_BADFILE_KEEP)
        return path;
    return path + (len - (TP_BADFILE_KEEP - 1));
}

/* The hot path. Caller holds g_lock. Returns an entry index, or -1. */
static int find_locked(const char *path, uint64_t hash, size_t len)
{
    uint16_t slot = g_bucket[hash & (TP_BADFILE_BUCKETS - 1)];
    const char *tail = path_tail(path, len);

    while (slot) {
        struct entry *e = &g_entries[slot - 1];
        /*
         * Hash first, because it rejects almost everything without touching
         * the string at all; then the length, which is free; and only then
         * the bytes, so that a collision cannot grey out an innocent file.
         */
        if (e->hash == hash && e->len == (uint32_t)len &&
            strcmp(e->tail, tail) == 0)
            return (int)slot - 1;
        slot = e->next;
    }
    return -1;
}

/* Take an entry out of its bucket chain and mark it free. Caller holds. */
static void unlink_locked(int idx)
{
    struct entry *e = &g_entries[idx];
    uint16_t *link = &g_bucket[e->hash & (TP_BADFILE_BUCKETS - 1)];

    while (*link) {
        if (*link == (uint16_t)(idx + 1)) {
            *link = e->next;
            break;
        }
        link = &g_entries[*link - 1].next;
    }
    e->used = 0;
    e->next = 0;
    if (g_count)
        g_count--;
}

/*
 * A slot to write into, evicting the oldest if there is no free one. Caller
 * holds g_lock.
 *
 * Two linear scans of 256 entries, which is the deliberate half of the
 * trade-off at the top of this file: this runs once per file that fails to
 * open, and paying a few microseconds there buys the renderer its constant
 * time lookup. Keeping a free list and an LRU order threaded through the
 * table would make this O(1) and would be more code to be wrong in, for a
 * saving nothing on this device can measure.
 */
static int claim_slot_locked(void)
{
    int i, oldest = 0;

    for (i = 0; i < TP_BADFILE_MAX; i++)
        if (!g_entries[i].used)
            return i;

    /*
     * Full, so the lowest sequence number goes. g_seq wraps after four
     * billion marks, which at one failed file per second is a hundred and
     * thirty years of uptime; if it ever did wrap the only symptom is that
     * eviction picks a less-old entry than it meant to for one cycle.
     */
    for (i = 1; i < TP_BADFILE_MAX; i++)
        if (g_entries[i].seq < g_entries[oldest].seq)
            oldest = i;
    unlink_locked(oldest);
    return oldest;
}

/* Caller holds g_lock, and has already established the path is not present. */
static void insert_locked(int idx, const char *path, uint64_t hash, size_t len)
{
    struct entry *e = &g_entries[idx];
    uint16_t *head = &g_bucket[hash & (TP_BADFILE_BUCKETS - 1)];
    const char *tail = path_tail(path, len);
    size_t keep = len < TP_BADFILE_KEEP ? len : TP_BADFILE_KEEP - 1;

    e->hash = hash;
    e->len = (uint32_t)len;
    e->seq = ++g_seq;
    e->used = 1;
    /*
     * memcpy of a length worked out here rather than strncpy of the buffer
     * size: this is the one place an over-long path could run off the end of
     * an entry, and a copy whose bound is visibly the number of bytes there
     * are is easier to be sure about than one that relies on truncation.
     */
    memcpy(e->tail, tail, keep);
    e->tail[keep] = '\0';

    e->next = *head;
    *head = (uint16_t)(idx + 1);
    g_count++;
}

void tp_badfile_mark(const char *path)
{
    uint64_t hash;
    size_t len;

    if (!path || !path[0])
        return;
    hash = path_hash(path, &len);

    pthread_mutex_lock(&g_lock);
    /*
     * Marking one twice is not an error and must not consume a second slot:
     * a track that fails is retried by anyone who selects it again, and a
     * caller that re-marks on every failure is the expected caller.
     */
    if (find_locked(path, hash, len) < 0)
        insert_locked(claim_slot_locked(), path, hash, len);
    pthread_mutex_unlock(&g_lock);
}

bool tp_badfile_is_bad(const char *path)
{
    uint64_t hash;
    size_t len;
    bool bad;

    if (!path || !path[0])
        return false;
    hash = path_hash(path, &len);

    pthread_mutex_lock(&g_lock);
    bad = find_locked(path, hash, len) >= 0;
    pthread_mutex_unlock(&g_lock);
    return bad;
}

void tp_badfile_clear(const char *path)
{
    uint64_t hash;
    size_t len;
    int idx;

    if (!path || !path[0])
        return;
    hash = path_hash(path, &len);

    pthread_mutex_lock(&g_lock);
    idx = find_locked(path, hash, len);
    if (idx >= 0)
        unlink_locked(idx);
    pthread_mutex_unlock(&g_lock);
}

unsigned tp_badfile_count(void)
{
    unsigned n;

    pthread_mutex_lock(&g_lock);
    n = g_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

/*
 * Everything forgotten, for a remount.
 *
 * The list is only ever evidence about one mount of one volume. Unplugging
 * the device, or the translation layer recovering a region, invalidates all
 * of it at once, and carrying the old answers across would leave rows greyed
 * out on a volume that reads perfectly well - a fault the user cannot clear
 * and would reasonably report as a bug in the browser.
 */
void tp_badfile_reset(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_entries, 0, sizeof(g_entries));
    memset(g_bucket, 0, sizeof(g_bucket));
    g_count = 0;
    g_seq = 0;
    pthread_mutex_unlock(&g_lock);
}
