#include "tp_player.h"
#include "tp_util.h"
#include "tp_log.h"
#include "tp_file_probe.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct tp_player {
    enum tp_player_backend backend;
    enum tp_player_state state;
    pid_t child;
    char *current_path;
    char *current_title;
};

const char *tp_player_backend_name(enum tp_player_backend b)
{
    switch (b) {
    case TP_PLAYER_NULL: return "null";
    case TP_PLAYER_EXTERNAL: return "external";
    case TP_PLAYER_ALSA: return "alsa";
    default: return "unknown";
    }
}

enum tp_player_backend tp_player_backend_from_name(const char *name)
{
    if (!name)
        return TP_PLAYER_EXTERNAL;
    if (strcmp(name, "null") == 0)
        return TP_PLAYER_NULL;
    if (strcmp(name, "alsa") == 0)
        return TP_PLAYER_ALSA;
    return TP_PLAYER_EXTERNAL;
}

struct tp_player *tp_player_create(enum tp_player_backend backend)
{
    struct tp_player *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->backend = backend;
    p->state = TP_PLAYER_STOPPED;
    p->child = -1;
    return p;
}

static void kill_child(struct tp_player *p)
{
    int status;
    if (p->child > 0) {
        kill(p->child, SIGTERM);
        waitpid(p->child, &status, 0);
        p->child = -1;
    }
}

void tp_player_destroy(struct tp_player *p)
{
    if (!p)
        return;
    kill_child(p);
    free(p->current_path);
    free(p->current_title);
    free(p);
}

enum tp_player_state tp_player_state(struct tp_player *p)
{
    int status;
    if (!p)
        return TP_PLAYER_STOPPED;
    if (p->backend == TP_PLAYER_EXTERNAL && p->child > 0) {
        pid_t r = waitpid(p->child, &status, WNOHANG);
        if (r == p->child) {
            p->child = -1;
            p->state = TP_PLAYER_STOPPED;
        }
    }
    return p->state;
}

static int play_null(struct tp_player *p, const char *path, const char *title)
{
    free(p->current_path);
    free(p->current_title);
    p->current_path = tp_strdup(path);
    p->current_title = tp_strdup(title ? title : path);
    p->state = TP_PLAYER_PLAYING;
    tp_info("null backend: now playing:\n  %s\n  %s", p->current_title, path);
    return 0;
}

static int which_ok(const char *bin)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", bin);
    return system(cmd) == 0;
}

static int play_external(struct tp_player *p, const char *path)
{
    const char *player = NULL;
    pid_t pid;
    kill_child(p);
    if (which_ok("mpv"))
        player = "mpv";
    else if (which_ok("ffplay"))
        player = "ffplay";
    else if (which_ok("mpg123"))
        player = "mpg123";
    else {
        tp_error("No external player found (tried mpv, ffplay, mpg123).\n"
                 "Install one, or use --backend null.");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        tp_error("fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (strcmp(player, "mpv") == 0)
            execlp("mpv", "mpv", "--no-video", "--really-quiet", path, (char *)NULL);
        else if (strcmp(player, "ffplay") == 0)
            execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", path,
                   (char *)NULL);
        else
            execlp("mpg123", "mpg123", "-q", path, (char *)NULL);
        _exit(127);
    }
    p->child = pid;
    free(p->current_path);
    p->current_path = tp_strdup(path);
    p->state = TP_PLAYER_PLAYING;
    tp_info("external (%s): playing\n  %s", player, path);
    return 0;
}

/* ALSA backend: try tinyalsa/aplay-style; report clearly if unavailable */
static int play_alsa(struct tp_player *p, const char *path)
{
    if (!tp_is_dir("/dev/snd") && !tp_file_exists("/dev/snd/pcmC0D0p")) {
        tp_error("ALSA backend unavailable: no /dev/snd on this system.\n"
                 "On N31, load nano7-audio / CS42 modules first.\n"
                 "File ready:\n  %s",
                 path);
        return -1;
    }
    /* Prefer aplay if present for PCM/WAV; for compressed use external helper */
    if (which_ok("aplay") && (tp_file_probe_codec(path) == TP_CODEC_WAV)) {
        return play_external(p, path); /* reuse fork with aplay via PATH hack — use mpg path */
    }
    /* Decode via mpg123/mpv to ALSA if available */
    if (which_ok("mpg123") || which_ok("mpv"))
        return play_external(p, path);
    tp_error("ALSA device present but no decoder helper (mpg123/mpv) to feed it.\n"
             "Track exists but cannot be decoded by the available codec backend.\n"
             "  %s",
             path);
    (void)p;
    return -1;
}

int tp_player_play_file(struct tp_player *p, const char *path)
{
    enum tp_codec c;
    if (!p || !path)
        return -1;
    if (!tp_is_readable_file(path)) {
        tp_error("Could not find the audio file for this track.\nResolved path:\n  %s", path);
        return -1;
    }
    c = tp_file_probe_codec(path);
    if (c == TP_CODEC_PROTECTED_UNSUPPORTED) {
        tp_error("Track exists but cannot be decoded by the available codec backend.\n  %s",
                 path);
        return -1;
    }
    switch (p->backend) {
    case TP_PLAYER_NULL:
        return play_null(p, path, path);
    case TP_PLAYER_ALSA:
        return play_alsa(p, path);
    default:
        return play_external(p, path);
    }
}

int tp_player_play_track(struct tp_player *p, const struct tp_track *t)
{
    if (!t)
        return -1;
    if (!t->absolute_path || !t->file_exists) {
        tp_error("Could not find the audio file for this track.\nDatabase path:\n  %s\n"
                 "Resolved path:\n  %s",
                 t->ipod_path ? t->ipod_path : "(none)",
                 t->absolute_path ? t->absolute_path : "(unresolved)");
        return -1;
    }
    if (t->codec == TP_CODEC_PROTECTED_UNSUPPORTED || !t->playable_probe_ok) {
        tp_error("Track exists but cannot be decoded by the available codec backend.\n  %s",
                 t->absolute_path);
        return -1;
    }
    if (p->backend == TP_PLAYER_NULL) {
        char title[512];
        snprintf(title, sizeof(title), "%s - %s", t->artist ? t->artist : "?",
                 t->title ? t->title : "?");
        return play_null(p, t->absolute_path, title);
    }
    tp_info("Now playing:\n  %s - %s\n  %s\n  %s", t->artist ? t->artist : "?",
            t->title ? t->title : "?", t->album ? t->album : "", t->absolute_path);
    return tp_player_play_file(p, t->absolute_path);
}

int tp_player_pause(struct tp_player *p)
{
    if (!p)
        return -1;
    if (p->backend == TP_PLAYER_NULL && p->state == TP_PLAYER_PLAYING) {
        p->state = TP_PLAYER_PAUSED;
        tp_info("paused");
        return 0;
    }
    if (p->child > 0) {
        kill(p->child, SIGSTOP);
        p->state = TP_PLAYER_PAUSED;
        return 0;
    }
    return -1;
}

int tp_player_resume(struct tp_player *p)
{
    if (!p)
        return -1;
    if (p->backend == TP_PLAYER_NULL && p->state == TP_PLAYER_PAUSED) {
        p->state = TP_PLAYER_PLAYING;
        tp_info("resumed");
        return 0;
    }
    if (p->child > 0) {
        kill(p->child, SIGCONT);
        p->state = TP_PLAYER_PLAYING;
        return 0;
    }
    return -1;
}

int tp_player_stop(struct tp_player *p)
{
    if (!p)
        return -1;
    kill_child(p);
    p->state = TP_PLAYER_STOPPED;
    tp_info("stopped");
    return 0;
}

void tp_queue_init(struct tp_play_queue *q)
{
    memset(q, 0, sizeof(*q));
}

void tp_queue_free(struct tp_play_queue *q)
{
    free(q->ids);
    memset(q, 0, sizeof(*q));
}

int tp_queue_from_library(struct tp_play_queue *q, struct tp_library *lib, int shuffle)
{
    size_t i;
    tp_queue_free(q);
    if (!lib->track_count)
        return -1;
    q->ids = calloc(lib->track_count, sizeof(uint64_t));
    if (!q->ids)
        return -1;
    q->count = lib->track_count;
    for (i = 0; i < lib->track_count; i++)
        q->ids[i] = lib->tracks[i].track_id;
    q->shuffle = shuffle;
    if (shuffle) {
        for (i = q->count - 1; i > 0; i--) {
            size_t j = (size_t)(rand() % (int)(i + 1));
            uint64_t tmp = q->ids[i];
            q->ids[i] = q->ids[j];
            q->ids[j] = tmp;
        }
    }
    q->pos = 0;
    return 0;
}

int tp_queue_next(struct tp_play_queue *q)
{
    if (!q || !q->count)
        return -1;
    q->pos = (q->pos + 1) % q->count;
    return 0;
}

int tp_queue_prev(struct tp_play_queue *q)
{
    if (!q || !q->count)
        return -1;
    q->pos = (q->pos == 0) ? q->count - 1 : q->pos - 1;
    return 0;
}
