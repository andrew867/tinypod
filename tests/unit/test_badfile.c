/*
 * The unreadable-file list.
 *
 * Standalone, because tp_badfile.c depends on nothing but libc and pthreads
 * and there is no reason to link a decoder and an SQLite to test it:
 *
 *   gcc -Wall -Wextra -Wpedantic -std=c99 -o /tmp/test_badfile \
 *       tests/unit/test_badfile.c src/util/tp_badfile.c -pthread && \
 *       /tmp/test_badfile
 *
 * The cases that matter are the ones where getting it wrong is invisible on
 * a healthy volume and wrong on a failing one: re-marking a file must not
 * consume a second slot, overflow must evict rather than grow or refuse, and
 * two different paths must never be confused - a false positive here greys
 * out a track that plays perfectly well, which looks like a bug in the
 * browser and cannot be cleared by the person seeing it.
 */
#include "../../src/util/tp_badfile.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void check(const char *name, bool ok)
{
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok)
        g_fail++;
}

static void test_mark_and_query(void)
{
    tp_badfile_reset();
    tp_badfile_mark("/iPod_Control/Music/F00/AAAA.mp3");

    check("marked path reads back bad",
          tp_badfile_is_bad("/iPod_Control/Music/F00/AAAA.mp3"));
    check("unmarked path reads back good",
          !tp_badfile_is_bad("/iPod_Control/Music/F00/BBBB.mp3"));
    check("one mark is one entry", tp_badfile_count() == 1);
}

static void test_mark_twice(void)
{
    tp_badfile_reset();
    tp_badfile_mark("/iPod_Control/Music/F13/QRLF.mp3");
    tp_badfile_mark("/iPod_Control/Music/F13/QRLF.mp3");
    tp_badfile_mark("/iPod_Control/Music/F13/QRLF.mp3");

    check("marking the same path three times leaves count at 1",
          tp_badfile_count() == 1);
    check("and it is still bad",
          tp_badfile_is_bad("/iPod_Control/Music/F13/QRLF.mp3"));
}

static void test_clear(void)
{
    tp_badfile_reset();
    tp_badfile_mark("/a.mp3");
    tp_badfile_mark("/b.mp3");
    tp_badfile_clear("/a.mp3");

    check("clear removes the path it names", !tp_badfile_is_bad("/a.mp3"));
    check("clear leaves the other one alone", tp_badfile_is_bad("/b.mp3"));
    check("clear drops the count", tp_badfile_count() == 1);

    tp_badfile_clear("/never-marked.mp3");
    check("clearing an unknown path is a no-op", tp_badfile_count() == 1);
}

static void test_reset(void)
{
    char path[64];
    int i;

    tp_badfile_reset();
    for (i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "/iPod_Control/Music/F%02d/X.mp3", i);
        tp_badfile_mark(path);
    }
    check("ten marks make ten entries", tp_badfile_count() == 10);

    tp_badfile_reset();
    check("reset empties the table", tp_badfile_count() == 0);
    check("and nothing is bad afterwards",
          !tp_badfile_is_bad("/iPod_Control/Music/F00/X.mp3"));
}

/*
 * Overflow. 300 paths into a table of 256: the count must cap, the last 256
 * must still be known bad, and the first 44 must have been evicted rather
 * than the table having quietly refused the later ones - which is the failure
 * that would matter, because the recent marks are the ones the queue is about
 * to act on.
 */
static void test_overflow(void)
{
    char path[64];
    int i, recent_bad = 0, oldest_bad = 0;

    tp_badfile_reset();
    for (i = 0; i < 300; i++) {
        snprintf(path, sizeof(path), "/iPod_Control/Music/F%02d/T%04d.mp3",
                 i % 50, i);
        tp_badfile_mark(path);
    }
    check("300 marks cap at 256 entries", tp_badfile_count() == 256);

    for (i = 44; i < 300; i++) {
        snprintf(path, sizeof(path), "/iPod_Control/Music/F%02d/T%04d.mp3",
                 i % 50, i);
        if (tp_badfile_is_bad(path))
            recent_bad++;
    }
    check("the most recent 256 are all still known bad", recent_bad == 256);

    for (i = 0; i < 44; i++) {
        snprintf(path, sizeof(path), "/iPod_Control/Music/F%02d/T%04d.mp3",
                 i % 50, i);
        if (tp_badfile_is_bad(path))
            oldest_bad++;
    }
    check("the 44 oldest were evicted, not the newest", oldest_bad == 0);
}

/*
 * Paths built to defeat a weak hash.
 *
 * Each pair is an anagram: same length, same bytes, different order. A sum or
 * an XOR of the bytes gives both members of a pair the same value, and so
 * does a hash that only looks at the length and the first and last character
 * - all of which are hashes someone might reach for when the input is "short
 * file names on a music volume". If the table confused them, marking one
 * track would grey out a different one that plays.
 */
static void test_weak_hash_collisions(void)
{
    static const char *const pairs[][2] = {
        { "/Music/AB.mp3",                   "/Music/BA.mp3" },
        { "/a/xy.mp3",                       "/a/yx.mp3" },
        { "/iPod_Control/Music/F00/ABCD.mp3",
          "/iPod_Control/Music/F00/BACD.mp3" },
        { "/iPod_Control/Music/F01/AB.mp3",
          "/iPod_Control/Music/F10/AB.mp3" },
    };
    size_t n = sizeof(pairs) / sizeof(pairs[0]);
    size_t i;
    int marked_bad = 0, other_bad = 0;

    tp_badfile_reset();
    for (i = 0; i < n; i++)
        tp_badfile_mark(pairs[i][0]);

    for (i = 0; i < n; i++) {
        if (tp_badfile_is_bad(pairs[i][0]))
            marked_bad++;
        if (tp_badfile_is_bad(pairs[i][1]))
            other_bad++;
    }
    check("anagram pairs: the marked one of each is bad", marked_bad == (int)n);
    check("anagram pairs: its twin is not", other_bad == 0);
    check("anagram pairs: one entry each", tp_badfile_count() == (unsigned)n);
}

/*
 * Bucket collisions, which are not optional.
 *
 * With 200 entries in 512 buckets the pigeonhole makes chains unavoidable
 * whatever the hash function is, so this exercises the chain walk rather than
 * hoping for it. 200 marked paths must all be found and 200 unmarked
 * neighbours interleaved with them must all be missed.
 */
static void test_bucket_chains(void)
{
    char path[80];
    int i, hits = 0, misses = 0;

    tp_badfile_reset();
    for (i = 0; i < 200; i++) {
        snprintf(path, sizeof(path), "/iPod_Control/Music/F%02d/BAD%03d.m4a",
                 i % 50, i);
        tp_badfile_mark(path);
    }
    for (i = 0; i < 200; i++) {
        snprintf(path, sizeof(path), "/iPod_Control/Music/F%02d/BAD%03d.m4a",
                 i % 50, i);
        if (tp_badfile_is_bad(path))
            hits++;
        snprintf(path, sizeof(path), "/iPod_Control/Music/F%02d/OK%03d.m4a",
                 i % 50, i);
        if (!tp_badfile_is_bad(path))
            misses++;
    }
    check("200 chained entries all found", hits == 200);
    check("200 interleaved unmarked paths all missed", misses == 200);
}

/*
 * Long paths, where only the tail is stored.
 *
 * Entries keep the last 255 bytes of a path plus the hash of the whole one,
 * so the case to prove is two paths of equal length sharing that entire tail
 * and differing only in a prefix the entry never sees. The hash covers the
 * prefix, so they must stay distinct.
 */
static void test_long_paths_differing_in_prefix(void)
{
    char a[900], b[900];
    size_t i;

    memset(a, 'x', sizeof(a));
    a[0] = '/';
    a[1] = 'a';
    memcpy(b, a, sizeof(a));
    b[1] = 'b';                       /* differs 898 bytes from the end */
    for (i = sizeof(a) - 10; i < sizeof(a) - 1; i++) {
        a[i] = "/T001.mp3"[i - (sizeof(a) - 10)];
        b[i] = a[i];
    }
    a[sizeof(a) - 1] = '\0';
    b[sizeof(b) - 1] = '\0';

    tp_badfile_reset();
    tp_badfile_mark(a);
    check("long path marked bad", tp_badfile_is_bad(a));
    check("long path differing only in its prefix is not bad",
          !tp_badfile_is_bad(b));
    check("long paths: one entry", tp_badfile_count() == 1);

    tp_badfile_mark(b);
    check("both long paths held separately", tp_badfile_count() == 2 &&
          tp_badfile_is_bad(a) && tp_badfile_is_bad(b));
}

/* NULL and empty are ignored rather than stored, since a caller with neither
   a path nor a check for one is the caller most likely to reach here. */
static void test_degenerate_input(void)
{
    tp_badfile_reset();
    tp_badfile_mark(NULL);
    tp_badfile_mark("");
    tp_badfile_clear(NULL);
    check("NULL and empty are not stored", tp_badfile_count() == 0);
    check("NULL is not bad", !tp_badfile_is_bad(NULL));
    check("empty is not bad", !tp_badfile_is_bad(""));
}

int main(void)
{
    printf("badfile:\n");
    test_mark_and_query();
    test_mark_twice();
    test_clear();
    test_reset();
    test_overflow();
    test_weak_hash_collisions();
    test_bucket_chains();
    test_long_paths_differing_in_prefix();
    test_degenerate_input();

    printf("badfile: %s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
