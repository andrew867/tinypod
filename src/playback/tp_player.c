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
#include "../fs/tp_browse.h"
#include "tp_decode.h"
#include "tp_sink.h"
#include "tp_util.h"
#include "tp_log.h"
#include "tp_file_probe.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Monotonic nanoseconds. Not the wall clock: that can step, and a position
   that jumps backwards when NTP corrects is worse than one that drifts. */
static unsigned long long mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull +
           (unsigned long long)t.tv_nsec;
}

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

    /*
     * When playback started, and how much of that was spent paused, so
     * "how long have we been playing" is answerable. Monotonic: the wall
     * clock can step, and a clock that steps backwards would make the
     * position jump about.
     */
    unsigned long long started_ns;
    unsigned long long paused_ns;      /* accumulated */
    unsigned long long pause_began_ns;

    /* The device took data faster than real time for long enough that it
       cannot be playing it. */
    int not_pacing;

    /* What the sink turned out to be, and how often it had to be restarted.
       Kept here because the sink itself is opened and closed by the playback
       thread, and the screens that want to show this outlive it. */
    char sink_desc[32];
    unsigned long restarts;
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
    tp_sink_describe(sink, p->sink_desc, sizeof(p->sink_desc));
    p->restarts = 0;
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
        p->restarts = tp_sink_restarts(sink);
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
    p->started_ns = mono_ns();
    p->paused_ns = 0;
    p->pause_began_ns = 0;
    p->not_pacing = 0;
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

/* How long we have been playing, excluding time spent paused. */
static unsigned long long playing_ms_locked(const struct tp_player *p)
{
    unsigned long long paused = p->paused_ns;
    if (p->pause_began_ns)
        paused += mono_ns() - p->pause_began_ns;
    if (!p->started_ns)
        return 0;
    unsigned long long ns = mono_ns() - p->started_ns;
    if (ns <= paused)
        return 0;
    return (ns - paused) / 1000000ull;
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

    /*
     * Bounded by how long we have actually been playing.
     *
     * played_samples counts what was handed to the driver. On a device that
     * paces the stream that is the same thing as what came out, give or take
     * the buffer. On a device that accepts everything and plays none of it,
     * the decode loop never blocks and this counter races - which is what a
     * clock running "way too fast" is measuring.
     *
     * Audio cannot leave a device faster than real time, so the elapsed time
     * is a hard ceiling. When the device behaves, the ceiling never binds.
     */
    if (p->state == TP_PLAYER_PLAYING || p->state == TP_PLAYER_PAUSED) {
        unsigned long elapsed = (unsigned long)playing_ms_locked(p);
        if (ms > elapsed) {
            /* A second or two ahead is just the output buffer. Much more than
               that and nothing is being played at all. */
            if (ms - elapsed > 2000)
                p->not_pacing = 1;
            ms = elapsed;
        }
    }
    pthread_mutex_unlock(&p->lock);
    return ms;
}

/* True when the audio device has been taking data faster than it could
   possibly play it. Almost always means the stream is not actually running -
   worth saying out loud, because the alternative is the user inferring it
   from a clock that runs fast. */
int tp_player_not_pacing(struct tp_player *p)
{
    int v;
    if (!p)
        return 0;
    pthread_mutex_lock(&p->lock);
    v = p->not_pacing;
    pthread_mutex_unlock(&p->lock);
    return v;
}

/*
 * How the sink is configured, and how many times it has had to restart.
 *
 * Both are worth showing on the device. An underrun is silent from the
 * application's side - tinyalsa re-prepares and carries on - but on this
 * codec each one costs a 60 ms settle, so a stream restarting several times a
 * second is a fragment of music, silence, a fragment of music. Without a
 * count the only way to know is to read the kernel log.
 */
int tp_player_sink_desc(struct tp_player *p, char *out, size_t cap)
{
    if (!out || !cap)
        return -1;
    out[0] = 0;
    if (!p)
        return -1;
    pthread_mutex_lock(&p->lock);
    snprintf(out, cap, "%s", p->sink_desc);
    pthread_mutex_unlock(&p->lock);
    return 0;
}

unsigned long tp_player_restarts(struct tp_player *p)
{
    unsigned long v;
    if (!p)
        return 0;
    pthread_mutex_lock(&p->lock);
    v = p->restarts;
    pthread_mutex_unlock(&p->lock);
    return v;
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

    /*
     * Whose track this is, before anything else.
     *
     * play_track sets the title from the library and then calls this; calling
     * this directly - which is what the folder browser does - used to leave
     * the PREVIOUS track's title in place, so Now Playing confidently named
     * something that was not playing. Clear it here, so the worst case is the
     * file name rather than a wrong answer. play_track fills it in again
     * after, because it knows better.
     */
    free(p->current_title);
    p->current_title = NULL;
    {
        const char *base = strrchr(path, '/');
        const char *dot;
        size_t n;

        base = base ? base + 1 : path;
        dot = strrchr(base, '.');
        n = (dot && dot != base) ? (size_t)(dot - base) : strlen(base);
        p->current_title = malloc(n + 1);
        if (p->current_title) {
            memcpy(p->current_title, base, n);
            p->current_title[n] = '\0';
        }
    }
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

    tp_info("Now playing:\n  %s - %s\n  %s\n  %s", t->artist ? t->artist : "?",
            t->title ? t->title : "?", t->album ? t->album : "", t->absolute_path);

    if (tp_player_play_file(p, t->absolute_path) != 0)
        return -1;

    /* After, not before: play_file resets the title to the file name, which
       is the right answer only when there is nothing better. There is. */
    if (t->title) {
        char title[512];

        snprintf(title, sizeof(title), "%s - %s",
                 t->artist ? t->artist : "?", t->title);
        free(p->current_title);
        p->current_title = tp_strdup(title);
    }
    return 0;
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
            /* Paused time is not playing time, or the elapsed ceiling would
               keep rising while nothing came out and stop bounding anything. */
            p->pause_began_ns = mono_ns();
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
            if (p->pause_began_ns) {
                p->paused_ns += mono_ns() - p->pause_began_ns;
                p->pause_began_ns = 0;
            }
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
    size_t i;

    for (i = 0; i < q->count; i++) {
        free(q->items[i].path);
        free(q->items[i].title);
    }
    free(q->items);
    free(q->order);
    memset(q, 0, sizeof(*q));
}

const struct tp_queue_item *tp_queue_current(const struct tp_play_queue *q)
{
    if (!q || !q->count || q->pos >= q->count)
        return NULL;
    return &q->items[q->order[q->pos]];
}

size_t tp_queue_index(const struct tp_play_queue *q)
{
    return (q && q->count) ? q->pos + 1 : 0;
}

/* ---- building ------------------------------------------------------------ */

/* Room for n items and an order to go with them, both zeroed. */
static int queue_alloc(struct tp_play_queue *q, size_t n)
{
    tp_queue_free(q);
    if (!n)
        return -1;

    q->items = calloc(n, sizeof(*q->items));
    q->order = calloc(n, sizeof(*q->order));
    if (!q->items || !q->order) {
        free(q->items);
        free(q->order);
        q->items = NULL;
        q->order = NULL;
        return -1;
    }
    return 0;
}

/* order[] as it stands, then Fisher-Yates over it. */
static void order_identity(struct tp_play_queue *q)
{
    size_t i;

    for (i = 0; i < q->count; i++)
        q->order[i] = i;
}

static void order_shuffle(struct tp_play_queue *q)
{
    size_t i;

    order_identity(q);
    for (i = q->count; i > 1; i--) {
        size_t j = (size_t)(rand() % (int)i);
        size_t t = q->order[i - 1];

        q->order[i - 1] = q->order[j];
        q->order[j] = t;
    }
}

/* Put the running order at whichever slot plays item `want`. */
static void seek_to_item(struct tp_play_queue *q, size_t want)
{
    size_t i;

    for (i = 0; i < q->count; i++) {
        if (q->order[i] == want) {
            q->pos = i;
            return;
        }
    }
    q->pos = 0;
}

void tp_queue_set_shuffle(struct tp_play_queue *q, int shuffle)
{
    size_t playing;

    if (!q || !q->count || !!q->shuffle == !!shuffle)
        return;

    playing = q->order[q->pos];
    q->shuffle = shuffle ? 1 : 0;

    if (shuffle)
        order_shuffle(q);
    else
        order_identity(q);

    seek_to_item(q, playing);
}

int tp_queue_from_library(struct tp_play_queue *q, struct tp_library *lib, int shuffle)
{
    size_t i;

    if (!lib->track_count || queue_alloc(q, lib->track_count) != 0)
        return -1;

    for (i = 0; i < lib->track_count; i++) {
        q->items[i].id = lib->tracks[i].track_id;
        q->items[i].path = tp_strdup(lib->tracks[i].absolute_path);
    }
    q->count = lib->track_count;
    q->shuffle = shuffle ? 1 : 0;

    if (shuffle)
        order_shuffle(q);
    else
        order_identity(q);

    q->pos = 0;
    return 0;
}

int tp_queue_from_indices(struct tp_play_queue *q, struct tp_library *lib,
                          const size_t *idx, size_t n, size_t start,
                          int shuffle)
{
    size_t i, start_at = 0;

    if (!lib || !idx || !n || queue_alloc(q, n) != 0)
        return -1;

    {
        size_t k = 0;

        /* Packed, not indexed by i: an index the library does not have would
           otherwise leave an item with no path in the middle of the album,
           which plays as a failure and looks like a corrupt file. */
        for (i = 0; i < n; i++) {
            if (idx[i] >= lib->track_count)
                continue;
            q->items[k].id = lib->tracks[idx[i]].track_id;
            q->items[k].path = tp_strdup(lib->tracks[idx[i]].absolute_path);
            if (idx[i] == start)
                start_at = k;
            k++;
        }
        q->count = k;
    }

    if (!q->count) {
        tp_queue_free(q);
        return -1;
    }
    q->shuffle = shuffle ? 1 : 0;

    if (shuffle)
        order_shuffle(q);
    else
        order_identity(q);

    /* On the track that was picked, wherever shuffle put it. */
    seek_to_item(q, start_at);
    return 0;
}

int tp_queue_seek_id(struct tp_play_queue *q, uint64_t id)
{
    size_t i;

    if (!q || !q->count || !id)
        return -1;

    for (i = 0; i < q->count; i++) {
        if (q->items[i].id == id) {
            seek_to_item(q, i);
            return 0;
        }
    }
    return -1;
}

/*
 * The library entry for a file, matched on the path it resolved to.
 *
 * A folder of music that IS in the database should still show its titles and
 * artists rather than file names, and the browser is a perfectly ordinary way
 * to reach it.
 */
static const struct tp_track *track_for_path(struct tp_library *lib,
                                             const char *path)
{
    size_t i;

    if (!lib || !path)
        return NULL;

    for (i = 0; i < lib->track_count; i++) {
        const char *ap = lib->tracks[i].absolute_path;

        if (ap && !strcmp(ap, path))
            return &lib->tracks[i];
    }
    return NULL;
}

/* The file name, with its extension taken off, as a last-resort title. */
static char *title_from_path(const char *path)
{
    const char *base = strrchr(path, '/');
    const char *dot;
    char *out;
    size_t n;

    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    n = dot && dot != base ? (size_t)(dot - base) : strlen(base);

    out = malloc(n + 1);
    if (!out)
        return NULL;
    memcpy(out, base, n);
    out[n] = '\0';
    return out;
}

/* Fill one item in, taking the library's word for it where there is one. */
static void item_set(struct tp_queue_item *it, struct tp_library *lib,
                     const char *path)
{
    const struct tp_track *t = track_for_path(lib, path);

    it->path = tp_strdup(path);
    if (t) {
        it->id = t->track_id;
        it->title = NULL;        /* the library has a better one */
    } else {
        it->id = 0;
        it->title = title_from_path(path);
    }
}

int tp_queue_from_file(struct tp_play_queue *q, struct tp_library *lib,
                       const char *path)
{
    if (!path || queue_alloc(q, 1) != 0)
        return -1;

    item_set(&q->items[0], lib, path);
    q->count = 1;
    q->shuffle = 0;
    order_identity(q);
    q->pos = 0;
    return 0;
}

int tp_queue_from_folder(struct tp_play_queue *q, struct tp_library *lib,
                         const char *folder, const char *start, int shuffle)
{
    struct tp_browse b;
    size_t i, n = 0, start_at = 0;

    if (!folder)
        return -1;

    /* Not show_hidden: a queue is not a listing, and a dot file that plays is
       still something nobody asked to hear. */
    if (tp_browse_open(&b, folder, 0, NULL, 0) != 0)
        return -1;

    for (i = 0; i < b.count; i++)
        if (!b.entries[i].is_dir && b.entries[i].playable)
            n++;

    if (!n || queue_alloc(q, n) != 0) {
        tp_browse_free(&b);
        return -1;
    }

    n = 0;
    for (i = 0; i < b.count; i++) {
        char path[TP_BROWSE_PATH_MAX];

        if (b.entries[i].is_dir || !b.entries[i].playable)
            continue;
        if (tp_browse_join(path, sizeof path, folder, b.entries[i].name) != 0)
            continue;

        item_set(&q->items[n], lib, path);
        if (start && !strcmp(path, start))
            start_at = n;
        n++;
    }

    q->count = n;
    tp_browse_free(&b);

    if (!q->count) {
        tp_queue_free(q);
        return -1;
    }

    q->shuffle = shuffle ? 1 : 0;
    if (shuffle)
        order_shuffle(q);
    else
        order_identity(q);

    /*
     * Start on the file that was picked, wherever shuffle put it. Playing
     * something else because shuffle was on is not what picking a file means.
     */
    seek_to_item(q, start_at);
    return 0;
}

/* ---- moving -------------------------------------------------------------- */

/* One forward, wrapping only when repeat says to. */
static int advance(struct tp_play_queue *q)
{
    if (q->pos + 1 < q->count) {
        q->pos++;
        return 0;
    }
    if (q->repeat == TP_REPEAT_ALL) {
        q->pos = 0;
        return 0;
    }
    return -1;
}

int tp_queue_next(struct tp_play_queue *q)
{
    if (!q || !q->count)
        return -1;

    /* Repeat One is about what happens when a track ENDS, which is the only
       time this is reached without the button having been pressed. */
    if (q->repeat == TP_REPEAT_ONE)
        return 0;

    return advance(q);
}

int tp_queue_skip_next(struct tp_play_queue *q)
{
    if (!q || !q->count)
        return -1;

    /*
     * The button always moves. With Repeat One the alternative is a Next that
     * plays the same track again, which reads as a broken button rather than
     * as a setting - and there is a way to hear the track again already.
     */
    if (q->repeat == TP_REPEAT_ONE && q->count > 1) {
        if (q->pos + 1 < q->count)
            q->pos++;
        else
            q->pos = 0;
        return 0;
    }
    return advance(q);
}

int tp_queue_prev(struct tp_play_queue *q)
{
    if (!q || !q->count)
        return -1;

    if (q->pos > 0) {
        q->pos--;
        return 0;
    }
    /* Back from the first track wraps only if the end is reachable at all. */
    if (q->repeat == TP_REPEAT_ALL) {
        q->pos = q->count - 1;
        return 0;
    }
    return -1;
}
