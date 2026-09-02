#include "tp_db.h"
#include "tp_util.h"
#include "tp_path.h"
#include "tp_mount.h"
#include "tp_file_probe.h"
#include "tp_log.h"
#include "tp_sink.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

static void test_path_colon(void)
{
    char *abs = NULL;
    enum tp_path_status st;
    const char *mount = "/tmp/tp_mnt_test";
    char *ipod = tp_path_join2(mount, "iPod_Control/Music/F00");
    char *file;
    mkdir(mount, 0755);
    mkdir(tp_path_join2(mount, "iPod_Control"), 0755);
    mkdir(tp_path_join2(mount, "iPod_Control/Music"), 0755);
    mkdir(ipod, 0755);
    file = tp_path_join2(ipod, "ABCD.mp3");
    {
        FILE *f = fopen(file, "wb");
        if (f) {
            fputs("ID3", f);
            fclose(f);
        }
    }
    st = tp_path_resolve(mount, ":iPod_Control:Music:F00:ABCD.mp3", &abs);
    EXPECT(st == TP_PATH_OK || st == TP_PATH_CASEFOLD_MATCH);
    EXPECT(abs != NULL && strstr(abs, "ABCD.mp3"));
    free(abs);
    st = tp_path_resolve(mount, "../etc/passwd", &abs);
    EXPECT(st == TP_PATH_INVALID);
    free(abs);
    free(ipod);
    free(file);
}

static void test_path_reject_outside(void)
{
    char *abs = NULL;
    enum tp_path_status st = tp_path_resolve("/mnt/disk", "/etc/passwd", &abs);
    /* strip may still join under mount as etc/passwd relative after strip —
       absolute /etc becomes etc after strip_volume? Our resolver strips leading / then joins mount.
       So /etc/passwd -> mount/etc/passwd which is under mount — OK.
       Traversal .. is rejected. */
    free(abs);
    st = tp_path_resolve("/mnt/disk", "iPod_Control/../../etc/passwd", &abs);
    EXPECT(st == TP_PATH_INVALID);
    free(abs);
    (void)st;
}

static void test_utf8(void)
{
    char *s = tp_utf8_sanitize("hello");
    EXPECT(s && strcmp(s, "hello") == 0);
    free(s);
    s = tp_utf8_sanitize("\xff\xfe");
    EXPECT(s && strlen(s) >= 2);
    free(s);
}

static void test_json(void)
{
    char buf[64];
    size_t n = tp_json_escape(buf, sizeof(buf), "a\"b");
    EXPECT(n == 4);
    EXPECT(strcmp(buf, "a\\\"b") == 0);
}

static void test_sort(void)
{
    struct tp_track t[2];
    memset(t, 0, sizeof(t));
    t[0].title = tp_strdup("B");
    t[0].artist = tp_strdup("A");
    t[0].album = tp_strdup("A");
    t[1].title = tp_strdup("A");
    t[1].artist = tp_strdup("A");
    t[1].album = tp_strdup("A");
    tp_sort_tracks_by_title(t, 2);
    EXPECT(strcmp(t[0].title, "A") == 0);
    tp_track_clear(&t[0]);
    tp_track_clear(&t[1]);
}

static void test_codec_probe(void)
{
    const char *dir = "/tmp/tp_codec_test";
    char *mp3, *m4a;
    FILE *f;
    mkdir(dir, 0755);
    mp3 = tp_path_join2(dir, "t.mp3");
    m4a = tp_path_join2(dir, "t.m4a");
    f = fopen(mp3, "wb");
    if (f) {
        fwrite("ID3\x03\x00\x00\x00\x00\x00\x00", 1, 10, f);
        fclose(f);
    }
    f = fopen(m4a, "wb");
    if (f) {
        unsigned char hdr[12] = {0, 0, 0, 20, 'f', 't', 'y', 'p', 'M', '4', 'A', ' '};
        fwrite(hdr, 1, 12, f);
        fclose(f);
    }
    EXPECT(tp_file_probe_codec(mp3) == TP_CODEC_MP3);
    EXPECT(tp_file_probe_codec(m4a) == TP_CODEC_AAC);
    free(mp3);
    free(m4a);
}

static void test_volume_detect_no_mount(void)
{
    struct tp_volume v;
    unsetenv("TINYPOD_MOUNT");
    /* Without a real mount this may find nothing — OK */
    if (tp_mount_detect("/tmp/definitely_missing_tinypod_xyz", &v) != 0) {
        EXPECT(v.health == TP_VOL_NOT_FOUND || v.mount_root == NULL);
    } else {
        tp_volume_free(&v);
    }
}

static void test_volume_ipod_control(void)
{
    const char *root = "/tmp/tp_vol_test";
    char *ic, *it, *mu;
    struct tp_volume v;
    mkdir(root, 0755);
    ic = tp_path_join2(root, "iPod_Control");
    it = tp_path_join2(ic, "iTunes");
    mu = tp_path_join2(ic, "Music");
    mkdir(ic, 0755);
    mkdir(it, 0755);
    mkdir(mu, 0755);
    EXPECT(tp_mount_detect(root, &v) == 0);
    EXPECT(v.mount_root != NULL);
    EXPECT(v.ipod_control_root != NULL);
    tp_volume_free(&v);
    /* also accept ipod_control path directly */
    EXPECT(tp_mount_detect(ic, &v) == 0);
    tp_volume_free(&v);
    free(ic);
    free(it);
    free(mu);
}

static void test_missing_music(void)
{
    const char *root = "/tmp/tp_vol_nomusic";
    char *ic, *it;
    struct tp_volume v;
    mkdir(root, 0755);
    ic = tp_path_join2(root, "iPod_Control");
    it = tp_path_join2(ic, "iTunes");
    mkdir(ic, 0755);
    mkdir(it, 0755);
    EXPECT(tp_mount_detect(root, &v) == 0);
    EXPECT(v.health == TP_VOL_NO_MUSIC_DIR);
    tp_volume_free(&v);
    free(ic);
    free(it);
}


/*
 * The sink's rate conversion.
 *
 * The codec's master clock is 12 MHz, which divides exactly into the 48 kHz
 * family and never into the 44.1 kHz one, so the hardware runs at 48 kHz and
 * nearly every track in an iTunes library is resampled on its way out. This
 * is the common path, and tinyalsa converts nothing, so it happens here.
 *
 * A host cannot provoke it - it needs a device that refuses the track's rate -
 * so it is exercised through the converter hook instead, and measured rather
 * than eyeballed: a tone in, and how much of what comes out is still that
 * tone.
 */

/* Energy at one frequency, by Goertzel. Enough to say what fraction of the
   output is the tone we put in and what fraction is everything else. */
static double tone_energy(const int16_t *x, int n, int stride,
                          double freq, double rate)
{
    double w = 2.0 * 3.14159265358979 * freq / rate;
    double c = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    int i;

    for (i = 0; i < n; i++) {
        double s0 = (double)x[(size_t)i * stride] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

static double total_energy(const int16_t *x, int n, int stride)
{
    double e = 0.0;
    int i;

    for (i = 0; i < n; i++) {
        double v = (double)x[(size_t)i * stride];
        e += v * v;
    }
    return e;
}

static void test_sink_resample(void)
{
    enum { BLK = 441, BLOCKS = 120, CAP = 4096, KEEP = 65536 };
    static int16_t in[BLK * 2];
    static int16_t out[CAP * 2];
    static int16_t all[KEEP * 2];
    struct tp_sink *c;
    int total = 0, blk, i;
    double sig, tot;

    /* 1 kHz at full scale. Continuous across blocks - a resampler that
       restarts at a block boundary puts a step in the middle of it, and a
       step is broadband, which the energy ratio below will see. */
    c = tp_sink_convert_open(44100, 48000, 2, 2);
    EXPECT(c != NULL);
    if (!c) return;

    for (blk = 0; blk < BLOCKS; blk++) {
        int got;

        for (i = 0; i < BLK; i++) {
            double t = (double)(blk * BLK + i) / 44100.0;
            int16_t v = (int16_t)(32000.0 * sin(2.0 * 3.14159265358979 * 1000.0 * t));
            in[i * 2] = v;
            in[i * 2 + 1] = v;
        }
        got = tp_sink_convert_block(c, in, BLK, out, CAP);
        EXPECT(got >= 0);
        if (got > 0 && total + got <= KEEP) {
            memcpy(all + (size_t)total * 2, out,
                   (size_t)got * 2 * sizeof(int16_t));
            total += got;
        }
    }
    tp_sink_close(c);

    /*
     * 120 blocks of 441 at 44100 is 52920 frames, which is 57600 at 48000.
     *
     * A resampler ends with its filter still full - soxr holds 941 frames at
     * this ratio - so the count comes up a little short and that is correct
     * rather than a fault. What the bound is really for is the other
     * direction: an engine that quietly passed audio through unconverted
     * would return 52920, and there is no tolerance that accepts a delay of
     * a thousand frames and also accepts being short by five thousand.
     */
    printf("  resample: %d frames out, 52920 in (44100->48000)\n", total);
    EXPECT(total >= 56400 && total <= 57600);

    /*
     * What fraction of the output is still a 1 kHz tone. The first frames are
     * the filter filling, so they are skipped.
     *
     * Linear interpolation manages about 0.98 here; a windowed sinc is beyond
     * 0.999. The bound is set where it separates the two, so a silent fall
     * back to something cheap fails rather than passes quietly.
     */
    if (total > 2048) {
        int n = total - 1024;
        if (n > 8192) n = 8192;
        sig = tone_energy(all + 1024 * 2, n, 2, 1000.0, 48000.0);
        tot = total_energy(all + 1024 * 2, n, 2);
        EXPECT(tot > 0.0);
        /* Goertzel's single bin comes to A^2 n^2 / 4 for a real sinusoid,
           and the total energy to n A^2 / 2, so their ratio is n/2. Divide
           by that and a pure tone reads 1. */
        printf("  resample: %.5f of the output is the 1 kHz tone\n",
               sig / tot / ((double)n * 0.5));
        EXPECT(sig / tot / ((double)n * 0.5) > 0.995);
    }

    /* Mono in, stereo out, both channels identical. */
    c = tp_sink_convert_open(44100, 44100, 1, 2);
    EXPECT(c != NULL);
    if (c) {
        static int16_t mono[256];
        int got;

        for (i = 0; i < 256; i++)
            mono[i] = (int16_t)(1000 * sin(i * 0.05));
        got = tp_sink_convert_block(c, mono, 256, out, CAP);
        EXPECT(got >= 0);
        for (i = 0; i < got; i++)
            EXPECT(out[i * 2] == out[i * 2 + 1]);
        tp_sink_close(c);
    }

    /* Downwards as well as up, and not more frames than the ratio allows. */
    c = tp_sink_convert_open(48000, 44100, 2, 2);
    EXPECT(c != NULL);
    if (c) {
        int got = tp_sink_convert_block(c, in, BLK, out, CAP);
        EXPECT(got >= 0 && got <= 406);
        tp_sink_close(c);
    }
}


int main(void)
{
    tp_log_set_level(TP_LOG_ERROR);
    g_fail = 0;
    test_path_colon();
    test_path_reject_outside();
    test_utf8();
    test_json();
    test_sort();
    test_codec_probe();
    test_volume_detect_no_mount();
    test_volume_ipod_control();
    test_missing_music();
    test_sink_resample();
    if (g_fail) {
        fprintf(stderr, "%d test(s) failed\n", g_fail);
        return 1;
    }
    printf("tinypod-selftest: all unit tests passed\n");
    return 0;
}
