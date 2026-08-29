/*
 * Playback.
 *
 * The ALSA backend decodes in-process on a worker thread: Helix AAC/MP3 into
 * 16-bit PCM, straight out to the device. That is the one that runs on the
 * N31, where there is no mpv or ffmpeg to hand and no room to ship one.
 *
 * The external backend stays for development hosts, where handing the file to
 * whatever player is installed is the quickest way to hear it. Both answer the
 * same pause/resume/stop calls, so the UI does not know the difference.
 */
#include "tp_player.h"
#include "tp_decode.h"
#include "tp_sink.h"
#include "tp_util.h"
#include "tp_log.h"
#include "tp_file_probe.h"

#include <errno.h>
#include <pthread.h>
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

    /* internal decode thread */
    int async;          /* caller will notice failures itself */
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int thread_live;
    int req_stop;
    int paused;
    int done;

    char err[256];
    char codec[16];
    int rate;
    int channels;
    unsigned long dur_ms;
    unsigned long long played_samples;
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
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, NULL);
    return p;
}

static void kill_child(struct tp_player *p)
{
    int status;

    if (p->child > 0) {
        /* A stopped child ignores SIGTERM until it runs again. */
        kill(p->child, SIGCONT);
        kill(p->child, SIGTERM);
        waitpid(p->child, &status, 0);
        p->child = -1;
    }
}

/* ------------------------------------------------------- decode thread --- */

static void stop_thread(struct tp_player *p)
{
    pthread_mutex_lock(&p->lock);
    if (!p->thread_live) {
        pthread_mutex_unlock(&p->lock);
        return;
    }
    p->req_stop = 1;
    p->paused = 0;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);

    pthread_join(p->thread, NULL);

    pthread_mutex_lock(&p->lock);
    p->thread_live = 0;
    pthread_mutex_unlock(&p->lock);
}

static void *play_thread(void *arg)
{
    struct tp_player *p = arg;
    struct tp_dec *dec = NULL;
    struct tp_sink *sink = NULL;
    int16_t *block = NULL;
    char err[256] = "";
    int paused_sink = 0;

    block = malloc(sizeof(int16_t) * TP_DEC_MAX_BLOCK);
    if (!block) {
        snprintf(err, sizeof(err), "out of memory");
        goto done;
    }

    dec = tp_dec_open(p->current_path, err, sizeof(err));
    if (!dec)
        goto done;

    sink = tp_sink_open(tp_dec_rate(dec), tp_dec_channels(dec), err, sizeof(err));
    if (!sink)
        goto done;

    pthread_mutex_lock(&p->lock);
    p->rate = tp_dec_rate(dec);
    p->channels = tp_dec_channels(dec);
    p->dur_ms = (unsigned long)tp_dec_duration_ms(dec);
    snprintf(p->codec, sizeof(p->codec), "%s", tp_dec_codec_name(dec));
    pthread_mutex_unlock(&p->lock);

    for (;;) {
        int n;

        pthread_mutex_lock(&p->lock);
        while (p->paused && !p->req_stop) {
            if (!paused_sink) {
                tp_sink_pause(sink, 1);
                paused_sink = 1;
            }
            pthread_cond_wait(&p->cond, &p->lock);
        }
        if (p->req_stop) {
            pthread_mutex_unlock(&p->lock);
            break;
        }
        pthread_mutex_unlock(&p->lock);

        if (paused_sink) {
            tp_sink_pause(sink, 0);
            paused_sink = 0;
        }

        n = tp_dec_read(dec, block);
        if (n < 0) {
            snprintf(err, sizeof(err), "decoding stopped part-way through the track");
            break;
        }
        if (n == 0)
            break;                      /* end of track */

        if (tp_sink_write(sink, block, n) != 0) {
            snprintf(err, sizeof(err), "the audio device stopped accepting data");
            break;
        }

        pthread_mutex_lock(&p->lock);
        p->played_samples += (unsigned long long)n;
        pthread_mutex_unlock(&p->lock);
    }

done:
    if (sink) {
        tp_sink_drain(sink);
        tp_sink_close(sink);
    }
    if (dec)
        tp_dec_close(dec);
    free(block);

    pthread_mutex_lock(&p->lock);
    if (err[0])
        snprintf(p->err, sizeof(p->err), "%s", err);
    p->state = TP_PLAYER_STOPPED;
    p->done = 1;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

static int play_internal(struct tp_player *p, const char *path)
{
    stop_thread(p);

    pthread_mutex_lock(&p->lock);
    free(p->current_path);
    p->current_path = tp_strdup(path);
    p->req_stop = 0;
    p->paused = 0;
    p->done = 0;
    p->played_samples = 0;
    p->rate = 0;
    p->channels = 0;
    p->dur_ms = 0;
    p->err[0] = 0;
    p->codec[0] = 0;
    p->state = TP_PLAYER_PLAYING;
    pthread_mutex_unlock(&p->lock);

    if (!p->current_path) {
        p->state = TP_PLAYER_STOPPED;
        return -1;
    }
    if (pthread_create(&p->thread, NULL, play_thread, p) != 0) {
        pthread_mutex_lock(&p->lock);
        p->state = TP_PLAYER_STOPPED;
        snprintf(p->err, sizeof(p->err), "could not start the playback thread");
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    pthread_mutex_lock(&p->lock);
    p->thread_live = 1;
    pthread_mutex_unlock(&p->lock);
    return 0;
}

void tp_player_destroy(struct tp_player *p)
{
    if (!p)
        return;
    stop_thread(p);
    kill_child(p);
    free(p->current_path);
    free(p->current_title);
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

enum tp_player_state tp_player_state(struct tp_player *p)
{
    enum tp_player_state st;

    if (!p)
        return TP_PLAYER_STOPPED;

    if (p->backend == TP_PLAYER_EXTERNAL && p->child > 0) {
        int status;
        pid_t r = waitpid(p->child, &status, WNOHANG);
        if (r == p->child) {
            p->child = -1;
            p->state = TP_PLAYER_STOPPED;
        }
    }

    pthread_mutex_lock(&p->lock);
    st = p->state;
    pthread_mutex_unlock(&p->lock);
    return st;
}

int tp_player_wait(struct tp_player *p)
{
    if (!p)
        return -1;

    if (p->backend == TP_PLAYER_EXTERNAL) {
        int status;
        if (p->child <= 0)
            return 0;
        if (waitpid(p->child, &status, 0) < 0)
            return -1;
        p->child = -1;
        p->state = TP_PLAYER_STOPPED;
        return 0;
    }

    pthread_mutex_lock(&p->lock);
    while (p->thread_live && !p->done)
        pthread_cond_wait(&p->cond, &p->lock);
    pthread_mutex_unlock(&p->lock);
    stop_thread(p);

    return p->err[0] ? -1 : 0;
}

unsigned long tp_player_position_ms(struct tp_player *p)
{
    unsigned long ms;

    if (!p)
        return 0;
    pthread_mutex_lock(&p->lock);
    if (p->rate > 0 && p->channels > 0)
        ms = (unsigned long)(p->played_samples * 1000ull /
                             ((unsigned long long)p->rate * (unsigned long long)p->channels));
    else
        ms = 0;
    pthread_mutex_unlock(&p->lock);
    return ms;
}

unsigned long tp_player_duration_ms(struct tp_player *p)
{
    unsigned long v;

    if (!p)
        return 0;
    pthread_mutex_lock(&p->lock);
    v = p->dur_ms;
    pthread_mutex_unlock(&p->lock);
    return v;
}

int tp_player_rate(struct tp_player *p)
{
    int v;

    if (!p)
        return 0;
    pthread_mutex_lock(&p->lock);
    v = p->rate;
    pthread_mutex_unlock(&p->lock);
    return v;
}

int tp_player_channels(struct tp_player *p)
{
    int v;

    if (!p)
        return 0;
    pthread_mutex_lock(&p->lock);
    v = p->channels;
    pthread_mutex_unlock(&p->lock);
    return v;
}

const char *tp_player_codec(struct tp_player *p)
{
    return p ? p->codec : "";
}

const char *tp_player_last_error(struct tp_player *p)
{
    return p ? p->err : "";
}

const char *tp_player_current_title(struct tp_player *p)
{
    if (!p)
        return "";
    if (p->current_title)
        return p->current_title;
    return p->current_path ? p->current_path : "";
}

/* ------------------------------------------------------------ backends --- */

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

/*
 * Development hosts: hand the file to whatever is installed. Ordered by how
 * little they argue about being driven headlessly from a script.
 */
static int play_external(struct tp_player *p, const char *path)
{
    static const char *const players[] = { "mpv", "ffplay", "mplayer", "cvlc",
                                           "mpg123", "ffmpeg", NULL };
    const char *player = NULL;
    pid_t pid;
    int i;

    kill_child(p);

    for (i = 0; players[i]; i++) {
        if (which_ok(players[i])) {
            player = players[i];
            break;
        }
    }
    if (!player) {
        tp_error("No external player found (tried mpv, ffplay, mplayer, cvlc,\n"
                 "mpg123, ffmpeg). Install one, use --backend alsa to decode\n"
                 "in-process, or --backend null to only resolve tracks.");
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
            execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet",
                   path, (char *)NULL);
        else if (strcmp(player, "mplayer") == 0)
            execlp("mplayer", "mplayer", "-really-quiet", "-vo", "null", path,
                   (char *)NULL);
        else if (strcmp(player, "cvlc") == 0)
            execlp("cvlc", "cvlc", "--intf", "dummy", "--play-and-exit", "--quiet",
                   path, (char *)NULL);
        else if (strcmp(player, "mpg123") == 0)
            execlp("mpg123", "mpg123", "-q", path, (char *)NULL);
        else
            execlp("ffmpeg", "ffmpeg", "-loglevel", "quiet", "-i", path, "-f",
                   "alsa", "default", (char *)NULL);
        _exit(127);
    }

    p->child = pid;
    free(p->current_path);
    p->current_path = tp_strdup(path);
    p->state = TP_PLAYER_PLAYING;
    tp_info("external (%s): playing\n  %s", player, path);
    return 0;
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
        tp_error("This track is protected (FairPlay) and cannot be decoded.\n  %s", path);
        return -1;
    }

    switch (p->backend) {
    case TP_PLAYER_NULL:
        return play_null(p, path, path);
    case TP_PLAYER_ALSA:
        if (play_internal(p, path) != 0) {
            tp_error("%s", p->err[0] ? p->err : "could not start playback");
            return -1;
        }
        /*
         * Failures surface on the thread. Give it a moment to open the file
         * and the device so an unplayable track reports here, not silently.
         *
         * Skipped in async mode: a UI is still running and will see the error
         * through tp_player_last_error(), whereas waiting here would freeze it
         * for a fifth of a second on every track change.
         */
        if (p->async)
            return 0;

        usleep(120000);
        pthread_mutex_lock(&p->lock);
        if (p->done && p->err[0]) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s", p->err);
            pthread_mutex_unlock(&p->lock);
            stop_thread(p);
            tp_error("%s\n  %s", msg, path);
            return -1;
        }
        pthread_mutex_unlock(&p->lock);
        return 0;
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

    free(p->current_title);
    p->current_title = NULL;
    if (t->title) {
        char title[512];
        snprintf(title, sizeof(title), "%s - %s", t->artist ? t->artist : "?", t->title);
        p->current_title = tp_strdup(title);
    }

    tp_info("Now playing:\n  %s - %s\n  %s\n  %s", t->artist ? t->artist : "?",
            t->title ? t->title : "?", t->album ? t->album : "", t->absolute_path);
    return tp_player_play_file(p, t->absolute_path);
}

int tp_player_pause(struct tp_player *p)
{
    if (!p)
        return -1;

    if (p->backend == TP_PLAYER_ALSA) {
        int ok = 0;
        pthread_mutex_lock(&p->lock);
        if (p->thread_live && !p->done && p->state == TP_PLAYER_PLAYING) {
            p->paused = 1;
            p->state = TP_PLAYER_PAUSED;
            ok = 1;
        }
        pthread_mutex_unlock(&p->lock);
        return ok ? 0 : -1;
    }
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

    if (p->backend == TP_PLAYER_ALSA) {
        int ok = 0;
        pthread_mutex_lock(&p->lock);
        if (p->thread_live && !p->done && p->state == TP_PLAYER_PAUSED) {
            p->paused = 0;
            p->state = TP_PLAYER_PLAYING;
            pthread_cond_broadcast(&p->cond);
            ok = 1;
        }
        pthread_mutex_unlock(&p->lock);
        return ok ? 0 : -1;
    }
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

void tp_player_set_async(struct tp_player *p, int async)
{
    if (p)
        p->async = async;
}

int tp_player_stop(struct tp_player *p)
{
    if (!p)
        return -1;
    stop_thread(p);
    kill_child(p);
    p->state = TP_PLAYER_STOPPED;
    tp_info("stopped");
    return 0;
}

/* -------------------------------------------------------------- queue ---- */

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
