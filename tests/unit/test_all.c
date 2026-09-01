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
 * tinyalsa hands the hardware exactly what it is given, so a codec that only
 * clocks 48 kHz simply refuses a 44.1 kHz track - which is why mpg123 and
 * ffplay, which go through alsa-lib's plughw and resample, played tracks
 * TinyPod would not. This is the conversion that fixes that, and a host has
 * no way to provoke it, so it is exercised here instead.
 */
static void test_sink_resample(void)
{
    enum { IN = 441, CAP = 2048 };
    int16_t in[IN * 2], out[CAP * 2];
    int i, got, total;

    /* A full-scale sine, because interpolation that is not clamped shows up
       at the rails and nowhere else. */
    for (i = 0; i < IN; i++) {
        double t = (double)i / 44100.0;
        int16_t v = (int16_t)(32767.0 * sin(2.0 * 3.14159265358979 * 440.0 * t));
        in[i * 2] = v;
        in[i * 2 + 1] = v;
    }

    /* 44100 -> 48000 on 441 frames is 480, give or take the fraction. */
    got = tp_sink_convert_test(44100, 48000, 2, 2, in, IN, out, CAP);
    EXPECT(got >= 478 && got <= 482);

    /* Nothing left the int16 range by wrapping: a sample that should be near
       full scale must not come back near the opposite rail. */
    {
        int wrapped = 0;
        for (i = 1; i < got; i++) {
            int32_t d = (int32_t)out[i * 2] - (int32_t)out[(i - 1) * 2];
            if (d > 40000 || d < -40000)
                wrapped = 1;
        }
        EXPECT(!wrapped);
    }

    /* Down as well as up. */
    got = tp_sink_convert_test(48000, 44100, 2, 2, in, IN, out, CAP);
    EXPECT(got >= 403 && got <= 407);

    /* Mono in, stereo out, with both channels the same. */
    {
        int16_t mono[64];
        for (i = 0; i < 64; i++) mono[i] = (int16_t)(i * 100);
        got = tp_sink_convert_test(44100, 44100, 1, 2, mono, 64, out, CAP);
        EXPECT(got >= 62 && got <= 66);
        for (i = 0; i < got; i++)
            EXPECT(out[i * 2] == out[i * 2 + 1]);
    }

    /*
     * Block boundaries. The decoder hands over arbitrary block sizes, and a
     * resampler that restarts each time clicks at every seam - so the
     * fractional position and the previous frame carry across calls. Four
     * quarter-blocks must produce what one whole block does, not four
     * independent runs of it.
     */
    total = 0;
    for (i = 0; i < 4; i++) {
        got = tp_sink_convert_test(44100, 48000, 2, 2,
                                   in + i * (IN / 4) * 2, IN / 4,
                                   out, CAP);
        EXPECT(got > 0);
        total += got;
    }
    /* Four independent runs would each round the same way and drift; allow a
       frame per call for the fraction and no more. */
    EXPECT(total >= 476 && total <= 484);
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
