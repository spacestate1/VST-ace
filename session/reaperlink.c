#define _GNU_SOURCE
#include "reaperlink.h"

#include <ctype.h>
#include <pipewire/pipewire.h>
#include <spa/utils/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAXSYN 32

typedef struct { uint32_t id; char name[128], app[128], bin[64]; } rnode;
typedef struct {
    uint32_t id, node;
    char     name[128], chan[8];
    int      dir_out;                       /* 1 an output port, 0 an input */
    int      midi, audio;
} rport;
typedef struct { uint32_t id, out, in; } rlink;
typedef struct { struct pw_proxy *proxy; struct spa_hook hook; uint32_t out, in; int dead, hooked; } made;

struct rl {
    struct pw_thread_loop *loop;
    struct pw_context     *ctx;
    struct pw_core        *core;
    struct pw_registry    *reg;
    struct spa_hook        core_l, reg_l;
    struct spa_source     *timer;
    /* Everything the graph holds, growing as it does -- a studio with many
     * streams and a REAPER with many channels is a big graph, and one that
     * outgrew a fixed table would silently stop being linked. */
    rnode *nodes;  int nn, cn;
    rport *ports;  int np, cp;
    rlink *links;  int nl, cl;
    made **mine;   int nm, cm;              /* each heap-allocated: the hook inside is a list node */
    rl_synth syn[MAXSYN]; int nsyn;
    int    enabled, dirty;
    int    reaper_nodes, linked_audio, linked_midi;
    char   status[200];
};

/* Room for one more of whatever `*arr` holds. */
static int grow(void **arr, int *cap, int n, size_t sz)
{
    void *g;
    if (n < *cap) return 1;
    g = realloc(*arr, (size_t)(*cap ? *cap * 2 : 64) * sz);
    if (!g) return 0;
    *arr = g;
    *cap = *cap ? *cap * 2 : 64;
    return 1;
}

static int icontains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return 1;
    return 0;
}

static int istarts(const char *s, const char *prefix)
{ return !strncasecmp(s, prefix, strlen(prefix)); }

/* ---------------------------------------------------------------- registry */

static void reg_global(void *d, uint32_t id, uint32_t perm, const char *type, uint32_t ver,
                       const struct spa_dict *props)
{
    rl *r = d;
    const char *v;
    (void)perm; (void)ver;
    if (!props) return;
    if (!strcmp(type, PW_TYPE_INTERFACE_Node) && grow((void **)&r->nodes, &r->cn, r->nn, sizeof *r->nodes)) {
        rnode *n = &r->nodes[r->nn++];
        memset(n, 0, sizeof *n);
        n->id = id;
        if ((v = spa_dict_lookup(props, PW_KEY_NODE_NAME)))            snprintf(n->name, sizeof n->name, "%s", v);
        if ((v = spa_dict_lookup(props, PW_KEY_APP_NAME)))             snprintf(n->app, sizeof n->app, "%s", v);
        if ((v = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY)))   snprintf(n->bin, sizeof n->bin, "%s", v);
        r->dirty = 1;
    } else if (!strcmp(type, PW_TYPE_INTERFACE_Port) && grow((void **)&r->ports, &r->cp, r->np, sizeof *r->ports)) {
        rport *p = &r->ports[r->np++];
        memset(p, 0, sizeof *p);
        p->id = id;
        if ((v = spa_dict_lookup(props, PW_KEY_NODE_ID))) p->node = (uint32_t)atoi(v);
        if ((v = spa_dict_lookup(props, PW_KEY_PORT_NAME))) snprintf(p->name, sizeof p->name, "%s", v);
        if ((v = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL))) snprintf(p->chan, sizeof p->chan, "%s", v);
        if ((v = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION))) p->dir_out = !strcmp(v, "out");
        if ((v = spa_dict_lookup(props, PW_KEY_FORMAT_DSP))) {
            p->midi = icontains(v, "midi");
            p->audio = icontains(v, "audio");
        }
        r->dirty = 1;
    } else if (!strcmp(type, PW_TYPE_INTERFACE_Link) && grow((void **)&r->links, &r->cl, r->nl, sizeof *r->links)) {
        rlink *l = &r->links[r->nl++];
        l->id = id;
        l->out = (v = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT)) ? (uint32_t)atoi(v) : 0;
        l->in  = (v = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT))  ? (uint32_t)atoi(v) : 0;
        r->dirty = 1;
    }
}

static void reg_remove(void *d, uint32_t id)
{
    rl *r = d;
    int i;
    for (i = 0; i < r->nn; i++) if (r->nodes[i].id == id) { r->nodes[i] = r->nodes[--r->nn]; r->dirty = 1; return; }
    for (i = 0; i < r->np; i++) if (r->ports[i].id == id) { r->ports[i] = r->ports[--r->np]; r->dirty = 1; return; }
    for (i = 0; i < r->nl; i++) if (r->links[i].id == id) { r->links[i] = r->links[--r->nl]; r->dirty = 1; return; }
}

static const struct pw_registry_events k_reg = { PW_VERSION_REGISTRY_EVENTS, .global = reg_global, .global_remove = reg_remove };

/* ------------------------------------------------------------------ links */

static const rnode *node_of(rl *r, uint32_t id)
{
    int i;
    for (i = 0; i < r->nn; i++) if (r->nodes[i].id == id) return &r->nodes[i];
    return NULL;
}

/* REAPER, and not anything with the word in its name: the node is called REAPER
 * (a JACK client), or it belongs to an application or binary called reaper. */
static int is_reaper_node(const rnode *n)
{
    return !strcasecmp(n->name, "reaper") || !strcasecmp(n->app, "reaper") || !strcasecmp(n->bin, "reaper");
}

/* Ports in a stable order -- by name, numbers compared as numbers, so in2 comes
 * before in10. */
static int port_cmp(const void *a, const void *b)
{ return strverscmp((*(const rport *const *)a)->name, (*(const rport *const *)b)->name); }

static int exists(rl *r, uint32_t out, uint32_t in)
{
    int i;
    for (i = 0; i < r->nl; i++) if (r->links[i].out == out && r->links[i].in == in) return 1;
    return 0;
}

static int ours(rl *r, uint32_t out, uint32_t in)
{
    int i;
    for (i = 0; i < r->nm; i++) if (!r->mine[i]->dead && r->mine[i]->out == out && r->mine[i]->in == in) return 1;
    return 0;
}

static void on_proxy_removed(void *d)
{ made *m = d; m->dead = 1; }
/* The hook lives in the proxy's own list, which goes with the proxy: it is taken
 * out here, while that list still exists, and never touched after. */
static void on_proxy_destroy(void *d)
{
    made *m = d;
    if (m->hooked) { spa_hook_remove(&m->hook); m->hooked = 0; }
    m->dead = 1;
    m->proxy = NULL;
}
static const struct pw_proxy_events k_proxy = { PW_VERSION_PROXY_EVENTS, .removed = on_proxy_removed, .destroy = on_proxy_destroy };

static void make_link(rl *r, uint32_t out, uint32_t in)
{
    struct pw_properties *p;
    struct pw_proxy *px;
    made *m;
    char a[24], b[24];
    if (!grow((void **)&r->mine, &r->cm, r->nm, sizeof *r->mine) || !(m = calloc(1, sizeof *m))) return;
    snprintf(a, sizeof a, "%u", out);
    snprintf(b, sizeof b, "%u", in);
    p = pw_properties_new(PW_KEY_LINK_OUTPUT_PORT, a, PW_KEY_LINK_INPUT_PORT, b,
                          PW_KEY_OBJECT_LINGER, "false", NULL);       /* it goes when we do */
    px = pw_core_create_object(r->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &p->dict, 0);
    pw_properties_free(p);
    if (!px) { free(m); return; }
    m->proxy = px; m->out = out; m->in = in;
    pw_proxy_add_listener(px, &m->hook, &k_proxy, m);
    m->hooked = 1;
    r->mine[r->nm++] = m;
}

static void drop_dead(rl *r)
{
    int i, j = 0;
    for (i = 0; i < r->nm; i++) {
        if (r->mine[i]->dead) {
            made *m = r->mine[i];
            if (m->proxy) pw_proxy_destroy(m->proxy);       /* a link the server refused: its destroy event takes the hook out */
            if (m->hooked) spa_hook_remove(&m->hook);
            free(m);
        }
        else r->mine[j++] = r->mine[i];
    }
    r->nm = j;
}

static void remove_mine(rl *r, int keep_desired, const uint32_t *want_out, const uint32_t *want_in, int nwant)
{
    int i, k;
    for (i = 0; i < r->nm; i++) {
        made *m = r->mine[i];
        int wanted = 0;
        if (m->dead) continue;
        if (keep_desired) for (k = 0; k < nwant; k++)
            if (want_out[k] == m->out && want_in[k] == m->in) wanted = 1;
        if (!wanted && m->proxy) { m->dead = 1; pw_proxy_destroy(m->proxy); }
    }
    drop_dead(r);
}

/* Work out the links that should exist, make the missing ones, remove ours that
 * no longer should. On the loop's thread. */
static void reconcile(rl *r)
{
    uint32_t *wo = NULL, *wi = NULL;
    int nw = 0, i, s;
    rport **rin = NULL, **rmidi = NULL;
    int nrin = 0, nrmidi = 0, found = 0;
    uint32_t anode = 0;                     /* the one REAPER node whose inputs are used */

    r->dirty = 0;
    r->linked_audio = r->linked_midi = 0;
    drop_dead(r);
    if (!r->enabled) { remove_mine(r, 0, NULL, NULL, 0); r->reaper_nodes = 0; return; }

    for (i = 0; i < r->nn; i++) if (is_reaper_node(&r->nodes[i])) found++;
    r->reaper_nodes = found;

    wo = malloc(sizeof *wo * (size_t)(r->nsyn * 4 + 4));
    wi = malloc(sizeof *wi * (size_t)(r->nsyn * 4 + 4));
    rin = malloc(sizeof *rin * (size_t)(r->np + 1));
    rmidi = malloc(sizeof *rmidi * (size_t)(r->np + 1));
    if (!wo || !wi || !rin || !rmidi) goto done;

    /* REAPER's audio inputs, and its MIDI outputs wherever they are: on a JACK
     * client called REAPER, or on the Midi-Bridge as a REAPER ALSA port. */
    /* More than one thing in the graph can have REAPER in its name (REAPER on
     * JACK and an ALSA stream of it, say): audio goes to the lowest-numbered one
     * that has inputs, so the pairs are one node's and not a mixture. */
    for (i = 0; i < r->np; i++) {
        rport *p = &r->ports[i];
        const rnode *n = node_of(r, p->node);
        if (n && is_reaper_node(n) && p->audio && !p->dir_out && (!anode || p->node < anode)) anode = p->node;
    }
    for (i = 0; i < r->np; i++) {
        rport *p = &r->ports[i];
        const rnode *n = node_of(r, p->node);
        if (!n) continue;
        if (anode && p->node == anode && p->audio && !p->dir_out) rin[nrin++] = p;
        if (p->midi && p->dir_out && (is_reaper_node(n) || istarts(p->name, "reaper:"))) rmidi[nrmidi++] = p;
    }
    qsort(rin, (size_t)nrin, sizeof rin[0], port_cmp);
    qsort(rmidi, (size_t)nrmidi, sizeof rmidi[0], port_cmp);

    for (s = 0; s < r->nsyn; s++) {
        rport *out[2] = { NULL, NULL };
        char prefix[140];
        for (i = 0; i < r->np; i++) {
            rport *p = &r->ports[i];
            const rnode *n = node_of(r, p->node);
            if (!n || !p->audio || !p->dir_out || strcmp(n->name, r->syn[s].node)) continue;
            if (!strcasecmp(p->chan, "FL") && !out[0]) out[0] = p;
            else if (!strcasecmp(p->chan, "FR") && !out[1]) out[1] = p;
        }
        for (i = 0; i < 2; i++)
            if (out[i] && 2 * s + i < nrin) { wo[nw] = out[i]->id; wi[nw] = rin[2 * s + i]->id; nw++; r->linked_audio += 1; }
        if (r->syn[s].alsa_client[0] && s < nrmidi) {
            snprintf(prefix, sizeof prefix, "%s:", r->syn[s].alsa_client);   /* "name: port" or "name:port", by the bridge's mood */
            for (i = 0; i < r->np; i++) {
                rport *p = &r->ports[i];
                if (p->midi && !p->dir_out && istarts(p->name, prefix) && icontains(p->name, "(playback)") &&
                    (!r->syn[s].midi_port[0] || icontains(p->name, r->syn[s].midi_port))) {
                    wo[nw] = rmidi[s]->id; wi[nw] = p->id; nw++; r->linked_midi++;
                    break;
                }
            }
        }
    }

    remove_mine(r, 1, wo, wi, nw);
    for (i = 0; i < nw; i++)
        if (!exists(r, wo[i], wi[i]) && !ours(r, wo[i], wi[i])) make_link(r, wo[i], wi[i]);
done:
    free(wo); free(wi); free(rin); free(rmidi);
}

static void on_timer(void *d, uint64_t exp)
{
    rl *r = d;
    (void)exp;
    if (r->dirty) reconcile(r);
}

/* A link the server refused: forget it, so a later pass can try again. */
static void core_error(void *d, uint32_t id, int seq, int res, const char *msg)
{
    rl *r = d;
    int i;
    (void)seq; (void)res; (void)msg;
    for (i = 0; i < r->nm; i++)
        if (r->mine[i]->proxy && pw_proxy_get_id(r->mine[i]->proxy) == id) r->mine[i]->dead = 1;
}
static const struct pw_core_events k_core = { PW_VERSION_CORE_EVENTS, .error = core_error };

/* -------------------------------------------------------------------- API */

rl *rl_open(void)
{
    rl *r = calloc(1, sizeof *r);
    static int inited;
    struct timespec first = { 0, 200 * 1000000L }, every = { 0, 300 * 1000000L };
    if (!r) return NULL;
    if (!inited) { pw_init(NULL, NULL); inited = 1; }
    if (!(r->loop = pw_thread_loop_new("studio reaper links", NULL))) goto fail;
    if (!(r->ctx = pw_context_new(pw_thread_loop_get_loop(r->loop), NULL, 0))) goto fail;
    pw_thread_loop_lock(r->loop);
    if (!(r->core = pw_context_connect(r->ctx, NULL, 0))) { pw_thread_loop_unlock(r->loop); goto fail; }
    pw_core_add_listener(r->core, &r->core_l, &k_core, r);
    r->reg = pw_core_get_registry(r->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(r->reg, &r->reg_l, &k_reg, r);
    r->timer = pw_loop_add_timer(pw_thread_loop_get_loop(r->loop), on_timer, r);
    pw_loop_update_timer(pw_thread_loop_get_loop(r->loop), r->timer, &first, &every, false);
    pw_thread_loop_unlock(r->loop);
    if (pw_thread_loop_start(r->loop) < 0) goto fail;
    snprintf(r->status, sizeof r->status, "REAPER: off");
    return r;
fail:
    rl_close(r);
    return NULL;
}

void rl_close(rl *r)
{
    if (!r) return;
    if (r->loop) {
        pw_thread_loop_lock(r->loop);
        r->enabled = 0;
        if (r->core) remove_mine(r, 0, NULL, NULL, 0);
        pw_thread_loop_unlock(r->loop);
        pw_thread_loop_stop(r->loop);
    }
    if (r->timer && r->loop) pw_loop_destroy_source(pw_thread_loop_get_loop(r->loop), r->timer);
    if (r->reg) pw_proxy_destroy((struct pw_proxy *)r->reg);
    if (r->core) pw_core_disconnect(r->core);
    if (r->ctx) pw_context_destroy(r->ctx);
    if (r->loop) pw_thread_loop_destroy(r->loop);
    free(r->nodes); free(r->ports); free(r->links); free(r->mine);
    free(r);
}

void rl_set(rl *r, const rl_synth *synths, int n, int enabled)
{
    if (!r) return;
    pw_thread_loop_lock(r->loop);
    if (n > MAXSYN) n = MAXSYN;
    if (n > 0) memcpy(r->syn, synths, (size_t)n * sizeof *synths);
    r->nsyn = n;
    r->enabled = enabled;
    r->dirty = 1;
    pw_thread_loop_unlock(r->loop);
}

void rl_status(rl *r, char *buf, size_t n)
{
    if (!r) { snprintf(buf, n, "REAPER: PipeWire is not available"); return; }
    pw_thread_loop_lock(r->loop);
    if (!r->enabled)               snprintf(buf, n, "REAPER: off");
    else if (!r->reaper_nodes)     snprintf(buf, n, "REAPER: on -- waiting for REAPER (start it with pw-jack, or enable a MIDI port)");
    else if (!r->nsyn)             snprintf(buf, n, "REAPER: found -- no synth loaded to link");
    else snprintf(buf, n, "REAPER: found -- %d audio and %d MIDI link%s for %d synth%s",
                  r->linked_audio, r->linked_midi, r->linked_audio + r->linked_midi == 1 ? "" : "s",
                  r->nsyn, r->nsyn == 1 ? "" : "s");
    pw_thread_loop_unlock(r->loop);
}
