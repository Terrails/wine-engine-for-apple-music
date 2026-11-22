/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* libampulse.so: names the agent's audio stream after the track being played.
 *
 * Linux library, preloaded (LD_PRELOAD) into the Wine session by bin/amd. Wine's
 * winepulse.so names every stream after the program; mixers show the client name (application.name,
 * "Apple Music" through PULSE_PROP_OVERRIDE) and the stream name (media.name), e.g. Plasma:
 * "Apple Music · <media.name>". amd.exe writes "<artist> - <title>" to the file named by
 * AM_MEDIA_NAME_FILE (src/medianame.c); this library uses it as media.name:
 *   - pa_stream_new: a new stream starts with the current name (Wine's own name when the file is
 *     empty or missing);
 *   - the mainloop's poll function (wrapped through pa_mainloop_set_poll_func) re-reads the file
 *     at most every 250 ms while streams exist and renames them with pa_stream_set_name.
 * winepulse holds its lock around pa_stream_new and while its mainloop thread runs (its poll
 * function drops it only around poll()), so all libpulse calls here are made under that lock.
 * Without AM_MEDIA_NAME_FILE everything is passed through untouched. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pulse/mainloop.h>
#include <pulse/stream.h>

#define EXPORT __attribute__((visibility("default")))

struct tracked { pa_stream *s; char *orig; char *applied; };
struct pollwrap { pa_poll_func fn; void *ud; };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct tracked *streams;
static size_t nstreams, cap;
static char want[1024];               /* current name from the file, "" = Wine's own */
static struct timespec last_read;

static void *real(const char *name)
{
    void *p = dlsym(RTLD_NEXT, name), *h;
    /* winepulse.so's libpulse is a local dependency, not in the global scope RTLD_NEXT searches */
    if (!p && (h = dlopen("libpulse.so.0", RTLD_LAZY | RTLD_NOLOAD))) { p = dlsym(h, name); dlclose(h); }
    return p;
}

static const char *name_file(void)
{
    static const char *path; static int done;
    if (!done) { path = getenv("AM_MEDIA_NAME_FILE"); if (path && !*path) path = NULL; done = 1; }
    return path;
}

/* re-read the file into want; called with lock held */
static void read_name(void)
{
    char buf[sizeof(want)]; ssize_t n = 0; int fd;
    clock_gettime(CLOCK_MONOTONIC, &last_read);
    if ((fd = open(name_file(), O_RDONLY | O_CLOEXEC)) >= 0) { n = read(fd, buf, sizeof(buf) - 1); close(fd); }
    if (n < 0) n = 0;
    buf[n] = 0;
    strcpy(want, buf);
}

static int due(void)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - last_read.tv_sec) * 1000 + (now.tv_nsec - last_read.tv_nsec) / 1000000 >= 250;
}

/* rename what is ready, forget what has ended; called on the mainloop thread with lock held */
static void apply(void)
{
    static pa_stream_state_t (*get_state)(const pa_stream *);
    static pa_operation *(*set_name)(pa_stream *, const char *, pa_stream_success_cb_t, void *);
    static void (*op_unref)(pa_operation *);
    static void (*s_unref)(pa_stream *);
    size_t i = 0;

    if (!get_state && (!(get_state = real("pa_stream_get_state")) || !(set_name = real("pa_stream_set_name"))
                       || !(op_unref = real("pa_operation_unref")) || !(s_unref = real("pa_stream_unref"))))
    { get_state = NULL; return; }
    while (i < nstreams)
    {
        struct tracked *t = &streams[i];
        pa_stream_state_t st = get_state(t->s);
        if (st == PA_STREAM_FAILED || st == PA_STREAM_TERMINATED)
        {
            s_unref(t->s); free(t->orig); free(t->applied);
            streams[i] = streams[--nstreams];
            continue;
        }
        if (st == PA_STREAM_READY)
        {
            const char *name = want[0] ? want : t->orig;
            if (!t->applied || strcmp(t->applied, name))
            {
                pa_operation *op = set_name(t->s, name, NULL, NULL);
                if (op) { op_unref(op); free(t->applied); t->applied = strdup(name); }
            }
        }
        i++;
    }
}

static int poll_wrapper(struct pollfd *fds, unsigned long nfds, int timeout, void *ud)
{
    struct pollwrap *w = ud;
    pthread_mutex_lock(&lock);
    if (nstreams)
    {
        if (due()) read_name();
        apply();
        if (nstreams && (timeout < 0 || timeout > 250)) timeout = 250;   /* keep checking while paused */
    }
    pthread_mutex_unlock(&lock);
    if (w->fn) return w->fn(fds, nfds, timeout, w->ud);
    return poll(fds, nfds, timeout);
}

EXPORT void pa_mainloop_set_poll_func(pa_mainloop *m, pa_poll_func fn, void *ud)
{
    static void (*orig)(pa_mainloop *, pa_poll_func, void *);
    struct pollwrap *w;
    if (!orig && !(orig = real("pa_mainloop_set_poll_func"))) abort();
    if (!name_file() || !(w = malloc(sizeof(*w)))) { orig(m, fn, ud); return; }
    w->fn = fn; w->ud = ud;                           /* kept for the mainloop's lifetime */
    orig(m, poll_wrapper, w);
}

EXPORT pa_stream *pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map)
{
    static pa_stream *(*orig)(pa_context *, const char *, const pa_sample_spec *, const pa_channel_map *);
    static pa_stream *(*ref)(pa_stream *);
    pa_stream *s;
    if (!orig && !(orig = real("pa_stream_new"))) abort();
    if (!name_file()) return orig(c, name, ss, map);

    pthread_mutex_lock(&lock);
    read_name();
    s = orig(c, want[0] ? want : name, ss, map);
    if (nstreams == cap)
    {
        struct tracked *n = realloc(streams, (cap * 2 + 4) * sizeof(*streams));
        if (n) { streams = n; cap = cap * 2 + 4; }
    }
    if (s && nstreams < cap && (ref || (ref = real("pa_stream_ref"))))
    {
        struct tracked *t = &streams[nstreams++];
        t->s = ref(s);                                /* our own reference: the pointer stays valid */
        t->orig = strdup(name ? name : "");
        t->applied = strdup(want[0] ? want : t->orig);
    }
    pthread_mutex_unlock(&lock);
    return s;
}
