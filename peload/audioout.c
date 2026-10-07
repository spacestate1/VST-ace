/* audioout -- see audioout.h */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "audioout.h"

#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#  if __has_include(<jack/jack.h>)
#    include <jack/jack.h>
#    define AO_HAVE_JACK 1
#  endif
#endif

struct ao {
    int           backend;
    char          name[160];
    ao_render_fn  fn;
    void         *ud;
    int           rate, chunk;
    float        *buf;                   /* chunk * 2, one block */
    atomic_int    running, dead;
    atomic_ulong  xruns;
    /* ALSA */
    snd_pcm_t    *pcm;
    pthread_t     th;
    int           th_on;
    short        *pcm16;
#ifdef AO_HAVE_JACK
    jack_client_t *jc;
    jack_port_t   *port[2];
#endif
};

/* ------------------------------------------------------------------ ALSA */

static void *alsa_thread(void *ud)
{
    ao *a = ud;
    struct sched_param sp = { .sched_priority = 60 };
    /* Realtime if the system lets this user have it; a plain thread if not. */
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    while (atomic_load_explicit(&a->running, memory_order_acquire)) {
        long n;
        int i;
        a->fn(a->ud, a->buf, a->chunk);
        for (i = 0; i < a->chunk * 2; i++) {
            float v = a->buf[i];
            if (!(v == v)) v = 0.0f;
            a->pcm16[i] = (short)(v > 1.0f ? 32767.0f : v < -1.0f ? -32768.0f : v * 32767.0f);
        }
        n = snd_pcm_writei(a->pcm, a->pcm16, (snd_pcm_uframes_t)a->chunk);
        if (n < 0) {
            atomic_fetch_add(&a->xruns, 1);
            if (snd_pcm_recover(a->pcm, (int)n, 1) < 0 && snd_pcm_prepare(a->pcm) < 0) {
                /* A device that cannot be brought back: say so and stop trying
                 * at full speed; the caller sees ao_alive() go 0. */
                atomic_store(&a->dead, 1);
                { struct timespec ts = { 0, 20000000 }; nanosleep(&ts, NULL); }
            }
        } else if (atomic_load(&a->dead)) {
            atomic_store(&a->dead, 0);
        }
    }
    return NULL;
}

static int open_alsa(ao *a, const char *device, char *err, size_t errn)
{
    const char *dev = device && *device ? device : "default";
    int r;
    if ((r = snd_pcm_open(&a->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        snprintf(err, errn, "the ALSA device %s will not open: %s", dev, snd_strerror(r));
        a->pcm = NULL;
        return -1;
    }
    if ((r = snd_pcm_set_params(a->pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                2, (unsigned)a->rate, 1, 50000)) < 0) {
        snprintf(err, errn, "the ALSA device %s refused %d Hz stereo: %s", dev, a->rate, snd_strerror(r));
        snd_pcm_close(a->pcm);
        a->pcm = NULL;
        return -1;
    }
    if (!(a->pcm16 = malloc((size_t)a->chunk * 2 * sizeof *a->pcm16))) {
        snprintf(err, errn, "out of memory");
        snd_pcm_close(a->pcm);
        a->pcm = NULL;
        return -1;
    }
    atomic_store(&a->running, 1);
    if (pthread_create(&a->th, NULL, alsa_thread, a) != 0) {
        snprintf(err, errn, "could not start the audio thread");
        atomic_store(&a->running, 0);
        snd_pcm_close(a->pcm);
        free(a->pcm16);
        a->pcm = NULL; a->pcm16 = NULL;
        return -1;
    }
    a->th_on = 1;
    snprintf(a->name, sizeof a->name, "ALSA %s, %d frames at %d Hz", dev, a->chunk, a->rate);
    return 0;
}

/* ------------------------------------------------------------------ JACK */

#ifdef AO_HAVE_JACK
static struct {
    void *lib;
    jack_client_t *(*client_open)(const char *, jack_options_t, jack_status_t *, ...);
    int  (*client_close)(jack_client_t *);
    int  (*set_process)(jack_client_t *, JackProcessCallback, void *);
    void (*on_shutdown)(jack_client_t *, JackShutdownCallback, void *);
    jack_port_t *(*port_register)(jack_client_t *, const char *, const char *, unsigned long, unsigned long);
    int  (*activate)(jack_client_t *);
    int  (*deactivate)(jack_client_t *);
    void *(*port_buffer)(jack_port_t *, jack_nframes_t);
    jack_nframes_t (*sample_rate)(jack_client_t *);
    jack_nframes_t (*buffer_size)(jack_client_t *);
    const char **(*get_ports)(jack_client_t *, const char *, const char *, unsigned long);
    int  (*connect)(jack_client_t *, const char *, const char *);
    const char *(*port_name)(const jack_port_t *);
    void (*jfree)(void *);
} J;

static int jack_load(char *why, size_t n)
{
    if (J.lib) return 0;
    {
        void *l = dlopen("libjack.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!l) { snprintf(why, n, "libjack is not installed"); return -1; }
#define SYM(field, name) do { *(void **)&J.field = dlsym(l, name); if (!J.field) { dlclose(l); \
            snprintf(why, n, "libjack has no %s", name); memset(&J, 0, sizeof J); return -1; } } while (0)
        SYM(client_open, "jack_client_open"); SYM(client_close, "jack_client_close");
        SYM(set_process, "jack_set_process_callback"); SYM(on_shutdown, "jack_on_shutdown");
        SYM(port_register, "jack_port_register"); SYM(activate, "jack_activate");
        SYM(deactivate, "jack_deactivate"); SYM(port_buffer, "jack_port_get_buffer");
        SYM(sample_rate, "jack_get_sample_rate"); SYM(buffer_size, "jack_get_buffer_size");
        SYM(get_ports, "jack_get_ports"); SYM(connect, "jack_connect");
        SYM(port_name, "jack_port_name"); SYM(jfree, "jack_free");
#undef SYM
        J.lib = l;
    }
    return 0;
}

static int jack_process_cb(jack_nframes_t nframes, void *arg)
{
    ao *a = arg;
    float *l = J.port_buffer(a->port[0], nframes), *r = J.port_buffer(a->port[1], nframes);
    jack_nframes_t done = 0;
    if (!atomic_load_explicit(&a->running, memory_order_acquire)) {
        memset(l, 0, nframes * sizeof *l);
        memset(r, 0, nframes * sizeof *r);
        return 0;
    }
    while (done < nframes) {
        int c = nframes - done > (jack_nframes_t)a->chunk ? a->chunk : (int)(nframes - done), i;
        a->fn(a->ud, a->buf, c);
        for (i = 0; i < c; i++) {
            float x = a->buf[2 * i], y = a->buf[2 * i + 1];
            l[done + i] = x == x ? x : 0.0f;
            r[done + i] = y == y ? y : 0.0f;
        }
        done += (jack_nframes_t)c;
    }
    return 0;
}

static void jack_shutdown_cb(void *arg) { atomic_store(&((ao *)arg)->dead, 1); }

static int open_jack(ao *a, const char *client, char *err, size_t errn)
{
    jack_status_t st;
    const char **ports;
    unsigned have_rate;
    int i;
    if (jack_load(err, errn)) return -1;
    a->jc = J.client_open(client && *client ? client : "studio", JackNoStartServer, &st);
    if (!a->jc) {
        snprintf(err, errn, (st & JackServerFailed) || (st & JackServerError) || !(st & JackServerStarted)
                              ? "no JACK server is running" : "could not connect to JACK");
        return -1;
    }
    have_rate = J.sample_rate(a->jc);
    if ((int)have_rate != a->rate) {
        snprintf(err, errn, "JACK runs at %u Hz and the plug-ins at %d Hz", have_rate, a->rate);
        J.client_close(a->jc); a->jc = NULL;
        return -1;
    }
    J.set_process(a->jc, jack_process_cb, a);
    J.on_shutdown(a->jc, jack_shutdown_cb, a);
    a->port[0] = J.port_register(a->jc, "out_1", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    a->port[1] = J.port_register(a->jc, "out_2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    if (!a->port[0] || !a->port[1]) {
        snprintf(err, errn, "JACK would not give the studio two output ports");
        J.client_close(a->jc); a->jc = NULL;
        return -1;
    }
    atomic_store(&a->running, 1);
    if (J.activate(a->jc) != 0) {
        snprintf(err, errn, "JACK would not start the studio's client");
        atomic_store(&a->running, 0);
        J.client_close(a->jc); a->jc = NULL;
        return -1;
    }
    /* The first two physical playback ports: the speakers, as a player would. */
    if ((ports = J.get_ports(a->jc, NULL, NULL, JackPortIsPhysical | JackPortIsInput))) {
        for (i = 0; i < 2 && ports[i]; i++) J.connect(a->jc, J.port_name(a->port[i]), ports[i]);
        J.jfree(ports);
    }
    snprintf(a->name, sizeof a->name, "JACK, %u frames at %u Hz", (unsigned)J.buffer_size(a->jc), have_rate);
    return 0;
}
#endif

/* ------------------------------------------------------------------ public */

ao *ao_open(int backend, const char *device, const char *client, int rate, int chunk,
            ao_render_fn fn, void *ud, char *err, size_t errn)
{
    ao *a;
    if (err && errn) err[0] = 0;
    if ((backend != AO_JACK && backend != AO_ALSA) || !fn || rate < 8000 || chunk < 16 || chunk > 65536) {
        if (err) snprintf(err, errn, "not a backend this can open");
        return NULL;
    }
    a = calloc(1, sizeof *a);
    if (!a) { if (err) snprintf(err, errn, "out of memory"); return NULL; }
    a->backend = backend; a->fn = fn; a->ud = ud; a->rate = rate; a->chunk = chunk;
    a->buf = calloc((size_t)chunk * 2, sizeof *a->buf);
    if (!a->buf) { free(a); if (err) snprintf(err, errn, "out of memory"); return NULL; }
    if (backend == AO_ALSA) {
        if (open_alsa(a, device, err, errn)) { free(a->buf); free(a); return NULL; }
    } else {
#ifdef AO_HAVE_JACK
        if (open_jack(a, client, err, errn)) { free(a->buf); free(a); return NULL; }
#else
        snprintf(err, errn, "built without JACK support");
        free(a->buf); free(a);
        return NULL;
#endif
    }
    return a;
}

void ao_close(ao *a)
{
    if (!a) return;
    atomic_store(&a->running, 0);
    if (a->backend == AO_ALSA) {
        if (a->th_on) pthread_join(a->th, NULL);
        if (a->pcm) { snd_pcm_drop(a->pcm); snd_pcm_close(a->pcm); }
        free(a->pcm16);
    }
#ifdef AO_HAVE_JACK
    if (a->jc) { J.deactivate(a->jc); J.client_close(a->jc); }
#endif
    free(a->buf);
    free(a);
}

int ao_alive(const ao *a) { return a && !atomic_load(&a->dead); }
const char *ao_describe(const ao *a) { return a ? a->name : ""; }
unsigned long ao_xruns(const ao *a) { return a ? atomic_load(&a->xruns) : 0; }

int ao_jack_available(char *why, size_t n)
{
#ifdef AO_HAVE_JACK
    jack_status_t st;
    jack_client_t *c;
    if (jack_load(why, n)) return 0;
    c = J.client_open("studio-probe", JackNoStartServer, &st);
    if (!c) { snprintf(why, n, "no JACK server is running"); return 0; }
    J.client_close(c);
    if (n) why[0] = 0;
    return 1;
#else
    snprintf(why, n, "built without JACK support");
    return 0;
#endif
}

int ao_alsa_devices(char names[][128], char labels[][96], int max)
{
    void **hints = NULL, **h;
    int n = 0, pass;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return 0;
    for (pass = 0; pass < 2; pass++)
        for (h = hints; *h && n < max; h++) {
            char *name = snd_device_name_get_hint(*h, "NAME"), *desc = snd_device_name_get_hint(*h, "DESC"),
                 *io = snd_device_name_get_hint(*h, "IOID");
            int special, hw, dup = 0, i;
            if (!name || (io && !strcmp(io, "Input"))) goto next;
            special = !strcmp(name, "default") || !strcmp(name, "pipewire") || !strcmp(name, "jack") ||
                      !strcmp(name, "pulse") || !strcmp(name, "sysdefault");
            hw = !strncmp(name, "plughw:", 7);
            if ((pass == 0) != special || (pass == 1 && !hw)) goto next;
            for (i = 0; i < n; i++) if (!strcmp(names[i], name)) dup = 1;
            if (dup) goto next;
            snprintf(names[n], 128, "%s", name);
            if (!strcmp(name, "default"))         snprintf(labels[n], 96, "System default");
            else if (!strcmp(name, "sysdefault")) snprintf(labels[n], 96, "ALSA default (sysdefault)");
            else if (!strcmp(name, "pipewire"))   snprintf(labels[n], 96, "PipeWire (ALSA plug-in)");
            else if (!strcmp(name, "jack"))       snprintf(labels[n], 96, "JACK (ALSA plug-in)");
            else if (!strcmp(name, "pulse"))      snprintf(labels[n], 96, "PulseAudio");
            else {
                char *nl = desc ? strchr(desc, '\n') : NULL;
                if (nl) *nl = 0;
                snprintf(labels[n], 96, "%s", desc ? desc : name);
            }
            n++;
        next:
            free(name); free(desc); free(io);
        }
    snd_device_name_free_hint(hints);
    {   /* default first, as a person looks for it */
        int i;
        for (i = 1; i < n; i++)
            if (!strcmp(names[i], "default")) {
                char tn[128], tl[96];
                memcpy(tn, names[i], sizeof tn); memcpy(tl, labels[i], sizeof tl);
                memmove(names[1], names[0], (size_t)i * 128);
                memmove(labels[1], labels[0], (size_t)i * 96);
                memcpy(names[0], tn, sizeof tn); memcpy(labels[0], tl, sizeof tl);
                break;
            }
    }
    return n;
}

/* ---------------------------------------------------------------- choice */

static const char *conf_path(char *buf, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(buf, n, "%s/vst-ace/audio-backend", x);
    else if (h && *h) snprintf(buf, n, "%s/.config/vst-ace/audio-backend", h);
    else return NULL;
    return buf;
}

const char *ao_backend_name(int b)
{
    return b == AO_PIPEWIRE ? "pipewire" : b == AO_JACK ? "jack" : b == AO_ALSA ? "alsa" : "auto";
}

int ao_backend_from_name(const char *s)
{
    if (!s) return -1;
    if (!strcmp(s, "auto")) return AO_AUTO;
    if (!strcmp(s, "pipewire")) return AO_PIPEWIRE;
    if (!strcmp(s, "jack")) return AO_JACK;
    if (!strcmp(s, "alsa")) return AO_ALSA;
    return -1;
}

void ao_choice_load(ao_choice *c)
{
    char path[1024], line[256];
    FILE *f;
    const char *env = getenv("DW_BACKEND");
    memset(c, 0, sizeof *c);
    c->backend = AO_AUTO;
    if (conf_path(path, sizeof path) && (f = fopen(path, "r"))) {
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            if (!strncmp(line, "backend ", 8)) {
                int b = ao_backend_from_name(line + 8);
                if (b >= 0) c->backend = b;
            } else if (!strncmp(line, "device ", 7)) {
                snprintf(c->device, sizeof c->device, "%s", line + 7);
            }
        }
        fclose(f);
    }
    if (env && ao_backend_from_name(env) >= 0) c->backend = ao_backend_from_name(env);
}

int ao_choice_save(const ao_choice *c)
{
    char path[1024], dir[1024], tmp[1100], *slash;
    FILE *f;
    if (!conf_path(path, sizeof path)) return -1;
    snprintf(dir, sizeof dir, "%s", path);
    if ((slash = strrchr(dir, '/'))) {
        char *q;
        *slash = 0;
        for (q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0700); *q = '/'; }
        mkdir(dir, 0700);
    }
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return -1;
    fprintf(f, "backend %s\n", ao_backend_name(c->backend));
    if (c->device[0] && !strpbrk(c->device, "\n\r")) fprintf(f, "device %s\n", c->device);
    if (fclose(f) != 0 || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}
