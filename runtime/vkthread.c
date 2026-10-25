/* The renderer's calls (vk.h) from the game thread, and the render thread (DESIGN.md decision 4,
   "The render thread").

   The game's own work and the renderer's share one thread unless the renderer has one of its own:
   then each call is recorded here (what it reads of the game's memory and of the device's state
   copied, as it was at the call) and the render thread runs vk.c's function for it (vkr_*) in the
   same order, a frame behind at most, while the game goes on with its next draws. Everything else
   waits for the render thread to finish what it has (vk_sync) and then runs on the caller's thread:
   reading or writing a surface's pixels on the CPU (vk_cpu_access, Lock), a surface going, new
   meshes for the far terrain and the shadows, new options. The two threads never run vk.c's code at
   the same time.

   Off in test runs (--test: one thread, as before) and with "render_thread = off" in options.txt
   (TD6_RENDER_THREAD=0/1 overrides): each call then runs its vk.c function at once.

   vk.c reads what the game side had set when a call was made (the distance fade and fog, the smoke's
   puff, the widescreen HUD's place in the frame, the gamma ramp) from vkr: the game thread is
   changing these variables meanwhile. */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime.h"
#include "gfx.h"
#include "vk.h"
#include "vkr.h"
#include "options.h"
#include "widescreen.h"

/* the game side's variables (vk.h) */
uint32_t vk_fade;
int vk_nocast;
float vk_fog_end, vk_fog_start;
int vk_tnl_min = 16;
int vk_smoke_parts;
const VkPuff *vk_puff;

/* ------------------------------------------------------------------ the queue
   One producer (the game thread), one consumer (the render thread): blocks of commands, each
   command a header and its data; the producer publishes how far a block is written, the consumer
   follows. Full blocks go back to a free list. */
#define BLOCK (4u << 20)
typedef struct Block {
    struct Block *_Atomic next;
    size_t cap, used;                   /* (used: the producer's) */
    atomic_size_t ready;                /* written and published up to here */
    atomic_int closed;                  /* the producer went on to the next block */
    uint8_t *data;
} Block;

typedef struct { uint32_t type, size; } Cmd;     /* size: of the whole command, header included */

enum { C_CTX, C_STATE, C_DRAW, C_DRAW_TNL, C_DRAW_INST, C_CLEAR, C_FILL, C_BLT, C_PRESENT, C_FAR_DRAW, C_SH_BEGIN, C_SH_CLASS,
       C_SH_END, C_SM_BEGIN, C_SM_END, C_SM_TAKE, C_SC_BEGIN, C_SC_CAMERA, C_SC_CLASS, C_SC_END, C_FLIP };

static int on;                          /* the render thread runs */
static int tried;
static pthread_t thread;
static Block *head, *tail;              /* consumer's, producer's */
static Block *spare;                    /* free list (under lk) */
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER, idle = PTHREAD_COND_INITIALIZER;
static atomic_int sleeping, waiting, quit;
static unsigned long nput;              /* commands put (the producer's) */
static atomic_ulong sent, done;         /* commands published; commands run */
static atomic_ulong frames_sent, frames_done;
static double now_real_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static double sync_ms, ahead_ms;        /* the game thread's waits for it: vk_sync's, a frame ahead's */
static unsigned long syncs, aheads;

/* TD6_FRAME_TRACE=<file>: a line a frame at exit, for finding the slow ones: the game thread's time
   from one present to the next (wall, its CPU, its waits for the render thread: a frame ahead, and to
   catch up, with what for), and the render thread's for the same frame (its CPU, the present's wall) */
#define TRACE_MAX 200000
typedef struct { float wall, cpu, ahead, sync; uint16_t nsync, why; float r_wall, r_cpu, r_present, r_pipe; } FrameTrace;
static FrameTrace *trace;
static char trace_path[512];
static double tr_wall0, tr_cpu0, tr_sync0, tr_rwall0, tr_rcpu0, tr_rpipe0;
static unsigned long tr_syncs0;
static unsigned tr_why;                 /* what this frame's catch-ups were for (SYNC_*) */
static int sync_why;                    /* the next vk_sync's reason */
enum { SYNC_CPU = 1, SYNC_GONE = 2, SYNC_FAR = 4, SYNC_SHTERR = 8, SYNC_LIGHTMAP = 16, SYNC_OPTIONS = 32, SYNC_OTHER = 64 };
/* Textures written while the render thread may still read them for calls made before (the game
   rewrites some every frame): a copy of their pixels at the write's Lock, for the calls made up to
   then (by number: seq), instead of waiting for the render thread. It looks a texture's copy up as it
   reads it (vk_snap_pixels), having first said which surface it reads (rd_s), so a Lock either finds
   its read done or the copy there first. Freed once the render thread is past them. */
#define SNAP_MAX 64
typedef struct { const Surface *s; unsigned long seq, dirty; uint8_t *px; } Snap;
static Snap snaps[SNAP_MAX];
static int nsnaps;                      /* (under snap_lk) */
static atomic_int snaps_live;
static pthread_mutex_t snap_lk = PTHREAD_MUTEX_INITIALIZER;
static unsigned long cur_cmd;           /* the render thread's: the number of the call it runs */
static const Surface *_Atomic rd_s;     /* ... and the surface whose pixels it reads in it */
static unsigned long snaps_made;

static double thread_cpu_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static Block *block_new(size_t need)
{
    Block *b = NULL;
    pthread_mutex_lock(&lk);
    if (spare && spare->cap >= need) {
        b = spare;
        spare = atomic_load(&b->next);
    }
    pthread_mutex_unlock(&lk);
    if (!b) {
        b = calloc(1, sizeof *b);
        b->cap = need > BLOCK ? need : BLOCK;
        b->data = malloc(b->cap);
        if (!b->data)
            fatal("out of memory (render thread queue)");
    }
    atomic_store(&b->next, NULL);
    b->used = 0;
    atomic_store(&b->ready, 0);
    atomic_store(&b->closed, 0);
    return b;
}

/* room for a command of n bytes (8-aligned) */
static Cmd *put(uint32_t type, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (tail->used + n > tail->cap) {
        Block *b = block_new(n);
        atomic_store(&tail->ready, tail->used);  /* (all of it, before it is closed: the render thread leaves
                                                    a closed block at its ready) */
        atomic_store(&tail->closed, 1);
        atomic_store(&tail->next, b);
        tail = b;
    }
    Cmd *c = (Cmd *)(tail->data + tail->used);
    c->type = type;
    c->size = (uint32_t)n;
    tail->used += n;
    nput++;
    return c;
}

/* the commands put so far go to the render thread (seq_cst with its sleeping: it looks at the queue
   again after saying it sleeps, so it either sees these or is woken) */
static void publish(void)
{
    atomic_store(&tail->ready, tail->used);
    atomic_store(&sent, nput);
    if (atomic_load(&sleeping)) {
        pthread_mutex_lock(&lk);
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&lk);
    }
}

/* ------------------------------------------------------------------ what a call carries */
/* the device state vk.c reads of a draw (gfx.h D3DState): render and stage states, textures,
   world/view/projection, viewport, targets (not the lights or material: the GPU transform's are in
   TnlConst, the CPU path's vertices are lit) */
typedef struct {
    uint32_t rs[RS_MAX];
    uint32_t tss[STAGES][TSS_MAX];
    Surface *tex[STAGES];
    Mat4 xf[4];
    struct { uint32_t x, y, w, h; float minz, maxz; } vp;
    Surface *rt, *zb;
} StateSnap;

typedef struct {
    uint32_t fade;
    int nocast;
    float fog_end, fog_start;
    int smoke_parts, has_puff;
    VkPuff puff;
    int ws_race_depth, ws_hud_depth, ws_menu_depth;
} CtxSnap;

static CtxSnap ctx_sent;
static int ctx_valid;

static void seen(Surface *s)
{
    if (s)
        s->vk_seen = 1;
}

static void send_ctx(void)
{
    CtxSnap c;
    memset(&c, 0, sizeof c);
    c.fade = vk_fade;
    c.nocast = vk_nocast;
    c.fog_end = vk_fog_end;
    c.fog_start = vk_fog_start;
    c.smoke_parts = vk_smoke_parts;
    c.has_puff = vk_puff != NULL;
    if (vk_puff)
        c.puff = *vk_puff;
    c.ws_race_depth = ws_race_depth;
    c.ws_hud_depth = ws_hud_depth;
    c.ws_menu_depth = ws_menu_depth;
    if (ctx_valid && !memcmp(&c, &ctx_sent, sizeof c))
        return;
    ctx_sent = c;
    ctx_valid = 1;
    Cmd *cmd = put(C_CTX, sizeof(Cmd) + sizeof c);
    memcpy(cmd + 1, &c, sizeof c);
}

static void send_state(const D3DState *st)
{
    Cmd *cmd = put(C_STATE, sizeof(Cmd) + sizeof(StateSnap));
    StateSnap *s = (StateSnap *)(cmd + 1);
    memcpy(s->rs, st->rs, sizeof s->rs);
    memcpy(s->tss, st->tss, sizeof s->tss);
    memcpy(s->tex, st->tex, sizeof s->tex);
    memcpy(s->xf, st->xf, sizeof s->xf);
    memcpy(&s->vp, &st->vp, sizeof s->vp);
    s->rt = st->rt;
    s->zb = st->zb;
    seen(st->rt);
    seen(st->zb);
    for (int i = 0; i < STAGES; i++)
        seen(st->tex[i]);
}

/* the inline way: what vk.c reads, from the game side's variables, then its function at once */
static void ctx_inline(void)
{
    vkr.fade = vk_fade;
    vkr.nocast = vk_nocast;
    vkr.fog_end = vk_fog_end;
    vkr.fog_start = vk_fog_start;
    vkr.smoke_parts = vk_smoke_parts;
    vkr.puff = vk_puff;
    vkr.ws_race_depth = ws_race_depth;
    vkr.ws_hud_depth = ws_hud_depth;
    vkr.ws_menu_depth = ws_menu_depth;
}

/* ------------------------------------------------------------------ the render thread */
static D3DState rst;                    /* the state of the calls it runs (StateSnap's parts of it) */
static VkPuff rpuff;

static void run(const Cmd *c)
{
    const uint8_t *p = (const uint8_t *)(c + 1);
    switch (c->type) {
    case C_CTX: {
        const CtxSnap *s = (const CtxSnap *)p;
        vkr.fade = s->fade;
        vkr.nocast = s->nocast;
        vkr.fog_end = s->fog_end;
        vkr.fog_start = s->fog_start;
        vkr.smoke_parts = s->smoke_parts;
        rpuff = s->puff;
        vkr.puff = s->has_puff ? &rpuff : NULL;
        vkr.ws_race_depth = s->ws_race_depth;
        vkr.ws_hud_depth = s->ws_hud_depth;
        vkr.ws_menu_depth = s->ws_menu_depth;
        break;
    }
    case C_STATE: {
        const StateSnap *s = (const StateSnap *)p;
        memcpy(rst.rs, s->rs, sizeof s->rs);
        memcpy(rst.tss, s->tss, sizeof s->tss);
        memcpy(rst.tex, s->tex, sizeof s->tex);
        memcpy(rst.xf, s->xf, sizeof s->xf);
        memcpy(&rst.vp, &s->vp, sizeof s->vp);
        rst.rt = s->rt;
        rst.zb = s->zb;
        break;
    }
    case C_DRAW: {
        const int32_t *a = (const int32_t *)p;     /* prim, nv, ni, pretransformed */
        const SwVertex *v = (const SwVertex *)(p + 16);
        const uint16_t *idx = a[2] ? (const uint16_t *)(v + a[1]) : NULL;
        vkr_draw(&rst, a[0], v, a[1], idx, a[2], a[3]);
        break;
    }
    case C_DRAW_TNL: {
        const int32_t *a = (const int32_t *)p;     /* prim, nv, ni */
        const TnlConst *k = (const TnlConst *)(p + 16);
        const uint8_t *raw = (const uint8_t *)(k + 1);
        const uint16_t *idx = a[2] ? (const uint16_t *)(raw + (((size_t)a[1] * 40 + 7) & ~(size_t)7)) : NULL;
        vkr_draw_tnl(&rst, a[0], raw, a[1], idx, a[2], k);
        break;
    }
    case C_DRAW_INST: {
        const int32_t *a = (const int32_t *)p;     /* prim, nv, ni, instances */
        const TnlConst *k = (const TnlConst *)(p + 16);
        const uint8_t *raw = (const uint8_t *)(k + 1);
        size_t nvb = ((size_t)a[1] * 40 + 7) & ~(size_t)7, nib = ((size_t)a[2] * 2 + 7) & ~(size_t)7;
        const uint16_t *idx = a[2] ? (const uint16_t *)(raw + nvb) : NULL;
        vkr_draw_tnl_inst(&rst, a[0], raw, a[1], idx, a[2], k, (const float *)(raw + nvb + nib), a[3]);
        break;
    }
    case C_CLEAR: {
        const uint32_t *a = (const uint32_t *)p;   /* n, flags, color, stencil, z */
        float z;
        memcpy(&z, &a[4], 4);
        vkr_clear(&rst, (int)a[0], (const int *)(a + 6), a[1], a[2], z, a[3]);
        break;
    }
    case C_FILL: {
        Surface *s;
        memcpy(&s, p, sizeof s);
        const int *r = (const int *)(p + 8);       /* has, x0, y0, x1, y1, raw */
        vkr_fill(s, r[0] ? r + 1 : NULL, (uint32_t)r[5]);
        break;
    }
    case C_BLT: {
        Surface *d, *s;
        memcpy(&d, p, sizeof d);
        memcpy(&s, p + 8, sizeof s);
        const int *r = (const int *)(p + 16);      /* hasd, d[4], hass, s[4], key, lo, hi */
        vkr_blt(d, r[0] ? r + 1 : NULL, s, r[5] ? r + 6 : NULL, r[10], (uint32_t)r[11], (uint32_t)r[12]);
        break;
    }
    case C_PRESENT: {
        Surface *s;
        memcpy(&s, p, sizeof s);
        const int *r = (const int *)(p + 8);       /* has, rect[4] */
        memcpy(vkr.gamma, p + 32, sizeof vkr.gamma);
        double tp = trace ? now_real_ms() : 0;
        vkr_present(s, r[0] ? r + 1 : NULL);
        unsigned long fd = atomic_load(&frames_done);
        if (trace && fd < TRACE_MAX) {
            double w = now_real_ms(), cpu = thread_cpu_now();
            trace[fd].r_present = (float)(w - tp);
            trace[fd].r_wall = (float)(tr_rwall0 ? w - tr_rwall0 : 0);
            trace[fd].r_cpu = (float)(tr_rcpu0 ? cpu - tr_rcpu0 : 0);
            double pm = vkr_pipe_ms();
            trace[fd].r_pipe = (float)(pm - tr_rpipe0);
            tr_rwall0 = w, tr_rcpu0 = cpu, tr_rpipe0 = pm;
        }
        atomic_fetch_add(&frames_done, 1);
        if (atomic_load(&waiting)) {    /* (the game thread may be a frame ahead, waiting for this) */
            pthread_mutex_lock(&lk);
            pthread_cond_broadcast(&idle);
            pthread_mutex_unlock(&lk);
        }
        break;
    }
    case C_FAR_DRAW: {
        const int32_t *a = (const int32_t *)p;     /* first, count, vbase */
        vkr_far_draw(&rst, (const TnlConst *)(p + 16), (uint32_t)a[0], (uint32_t)a[1], a[2]);
        break;
    }
    case C_SH_BEGIN: vkr_shadow_begin((const VkShadowView *)p); break;
    case C_SH_CLASS: vkr_shadow_class(*(const int *)p); break;
    case C_SH_END: vkr_shadow_end(); break;
    case C_SM_BEGIN: vkr_smoke_view_begin((const VkSmokeView *)p); break;
    case C_SM_END: vkr_smoke_view_end(); break;
    case C_SM_TAKE: {
        const uint32_t *a = (const uint32_t *)p;   /* n, diffuse */
        const float (*pos)[3] = (const float (*)[3])(p + 8);
        const float (*uv)[2] = (const float (*)[2])(p + 8 + 12 * (size_t)a[0]);
        vkr_smoke_take(&rst, pos, (int)a[0], a[1], uv);
        break;
    }
    case C_SC_BEGIN: vkr_scene_begin((const VkScene *)p); break;
    case C_SC_CAMERA: vkr_scene_camera((const VkScene *)p); break;
    case C_SC_CLASS: vkr_scene_class(*(const int *)p); break;
    case C_SC_END: vkr_scene_end(); break;
    case C_FLIP: {
        Surface *s, *t;
        memcpy(&s, p, sizeof s);
        memcpy(&t, p + 8, sizeof t);
        vkr_flip(s, t);
        break;
    }
    }
}

static void *render_main(void *arg)
{
    (void)arg;
    size_t at = 0;
    unsigned long ran = 0;
    for (;;) {
        size_t ready = atomic_load(&head->ready);
        if (at < ready) {
            while (at < ready) {
                const Cmd *c = (const Cmd *)(head->data + at);
                cur_cmd = ran + 1;
                run(c);
                at += c->size;
                atomic_store(&rd_s, NULL);
                atomic_store(&done, ++ran);
            }
            continue;
        }
        if (atomic_load(&head->closed) && at == atomic_load(&head->ready)) {
            Block *n = atomic_load(&head->next);
            if (n) {
                Block *old = head;
                head = n;
                at = 0;
                pthread_mutex_lock(&lk);
                atomic_store(&old->next, spare);
                spare = old;
                pthread_mutex_unlock(&lk);
                continue;
            }
        }
        /* nothing to run: whoever waits for it to catch up (vk_sync) may look */
        if (atomic_load(&waiting)) {
            pthread_mutex_lock(&lk);
            pthread_cond_broadcast(&idle);
            pthread_mutex_unlock(&lk);
        }
        if (atomic_load(&quit))
            break;
        /* a little spinning before sleeping (the next draw is usually microseconds away) */
        int got = 0;
        for (int i = 0; i < 2000 && !got; i++) {
            got = at < atomic_load(&head->ready) || atomic_load(&head->closed) || atomic_load(&quit);
            if (!got)
                __builtin_ia32_pause();
        }
        if (got)
            continue;
        pthread_mutex_lock(&lk);
        atomic_store(&sleeping, 1);
        while (at >= atomic_load(&head->ready) && !atomic_load(&head->closed) && !atomic_load(&quit))
            pthread_cond_wait(&wake, &lk);
        atomic_store(&sleeping, 0);
        pthread_mutex_unlock(&lk);
    }
    return NULL;
}

/* ------------------------------------------------------------------ starting, waiting, stopping */

/* the render thread, when wanted (not in test runs; the option), once Vulkan runs */
static int going(void)
{
    if (on)
        return 1;
    if (tried || !vk_on())
        return 0;
    tried = 1;
    const char *e = getenv("TD6_RENDER_THREAD");
    int want = e && *e ? atoi(e) != 0 : !deterministic && option_bool("render_thread", 1);
    if (!want)
        return 0;
    head = tail = block_new(BLOCK);
    const char *tp = getenv("TD6_FRAME_TRACE");
    if (tp && *tp) {
        snprintf(trace_path, sizeof trace_path, "%s", tp);
        trace = calloc(TRACE_MAX, sizeof *trace);
    }
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8 << 20);
    int r = pthread_create(&thread, &at, render_main, NULL);
    pthread_attr_destroy(&at);
    if (r) {
        LOG(LOG_ALWAYS, "vk: no render thread (%d): drawing on the game's", r);
        return 0;
    }
    on = 1;
    atexit(vk_stop);                    /* (process_exit stops it first; this for any other way out) */
    LOG(LOG_ALWAYS, "vk: drawing on a render thread of its own");
    return 1;
}

void vk_sync(void)
{
    if (!on)
        return;
    unsigned long want = atomic_load(&sent);
    if (atomic_load(&done) >= want)
        return;
    pthread_mutex_lock(&lk);
    atomic_store(&waiting, 1);
    if (atomic_load(&sleeping))
        pthread_cond_signal(&wake);
    double t0 = now_real_ms();
    while (atomic_load(&done) < want)
        pthread_cond_wait(&idle, &lk);
    atomic_store(&waiting, 0);
    pthread_mutex_unlock(&lk);
    sync_ms += now_real_ms() - t0;
    syncs++;
    tr_why |= sync_why ? (unsigned)sync_why : SYNC_OTHER;
    sync_why = 0;
}

/* TD6_TEX_SNAP=0: a texture's write waits for the render thread instead (to compare) */
static int getenv_snap_off(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TD6_TEX_SNAP");
        v = e && *e == '0';
    }
    return v;
}

/* (the game thread) the copies the render thread is past */
static void snaps_free_done(int all)
{
    if (!atomic_load(&snaps_live))
        return;
    unsigned long d = atomic_load(&done);
    pthread_mutex_lock(&snap_lk);
    int k = 0;
    for (int i = 0; i < nsnaps; i++) {
        if (all || snaps[i].seq <= d)
            free(snaps[i].px);
        else
            snaps[k++] = snaps[i];
    }
    nsnaps = k;
    atomic_store(&snaps_live, k);
    pthread_mutex_unlock(&snap_lk);
}

/* (the game thread, at a write's Lock of a texture) its pixels kept for the calls made so far; 0 if
   there is no room (then it waits for the render thread as before) */
static int snap_take(Surface *s)
{
    snaps_free_done(0);
    size_t n = (size_t)s->pitch * s->height;
    if (!s->mem || !n || n > s->memsize)
        return 0;
    uint8_t *px = malloc(n);
    if (!px)
        return 0;
    memcpy(px, gptr(s->mem), n);
    pthread_mutex_lock(&snap_lk);
    if (nsnaps == SNAP_MAX) {
        pthread_mutex_unlock(&snap_lk);
        free(px);
        return 0;
    }
    snaps[nsnaps++] = (Snap){s, nput, s->dirty, px};
    atomic_store(&snaps_live, nsnaps);
    pthread_mutex_unlock(&snap_lk);
    snaps_made++;
    /* the render thread may be reading these pixels now (it looked before the copy was there):
       its call ends first */
    while (atomic_load(&rd_s) == s && atomic_load(&done) < nput)
        __builtin_ia32_pause();
    return 1;
}

const uint8_t *vk_snap_pixels(const Surface *s, unsigned long *dirty)
{
    if (!on)
        return NULL;
    atomic_store(&rd_s, s);             /* (said before looking: snap_take's side looks the other way round) */
    if (!atomic_load(&snaps_live))
        return NULL;
    const Snap *best = NULL;
    pthread_mutex_lock(&snap_lk);
    for (int i = 0; i < nsnaps; i++)
        if (snaps[i].s == s && snaps[i].seq >= cur_cmd && (!best || snaps[i].seq < best->seq))
            best = &snaps[i];
    const uint8_t *px = best ? best->px : NULL;
    if (best && dirty)
        *dirty = best->dirty;
    pthread_mutex_unlock(&snap_lk);
    return px;
}

static void trace_write(void)
{
    if (!trace)
        return;
    FILE *f = fopen(trace_path, "w");
    if (!f)
        return;
    fprintf(f, "# frame game_wall game_cpu ahead_wait sync_wait syncs why render_wall render_cpu render_present render_pipelines"
               "  (ms; why: 1 cpu access, 2 surface gone, 4 far mesh, 8 shadow terrain, 16 lightmap, 32 options, 64 other)\n");
    unsigned long n = atomic_load(&frames_done);
    for (unsigned long i = 0; i < n && i < TRACE_MAX; i++) {
        const FrameTrace *t = &trace[i];
        fprintf(f, "%lu %.3f %.3f %.3f %.3f %u %u %.3f %.3f %.3f %.3f\n", i, t->wall, t->cpu, t->ahead, t->sync, t->nsync,
                t->why, t->r_wall, t->r_cpu, t->r_present, t->r_pipe);
    }
    fclose(f);
    LOG(LOG_ALWAYS, "vk: frame trace of %lu frames written to %s", n < TRACE_MAX ? n : TRACE_MAX, trace_path);
}

void vk_stop(void)
{
    if (!on)
        return;
    gfx_lock();                         /* (a guest thread's exit: no other thread's calls meanwhile) */
    vk_sync();
    atomic_store(&quit, 1);
    pthread_mutex_lock(&lk);
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lk);
    pthread_join(thread, NULL);
    on = 0;
    trace_write();
    gfx_unlock();
    LOG(LOG_ALWAYS, "vk: render thread: %lu frames; the game thread waited for it %lu times (%.0f ms) to catch "
        "up and %lu times (%.0f ms) a frame ahead; %lu textures written meanwhile (kept for it)", atomic_load(&frames_done),
        syncs, sync_ms, aheads, ahead_ms, snaps_made);
    snaps_free_done(1);
}

/* ------------------------------------------------------------------ the calls */
static int tnl_count;                   /* GPU-lit draws this frame (vk.c's are counted the same way) */
static int sh_active, sh_views;         /* a view with ray-traced shadows is open; views this frame */

/* A camera the game built widened (vk.c's proj_widened) */
static int proj_widened(const D3DState *st)
{
    const float *P = &st->xf[XF_PROJ].m[0][0];
    return P[0] != 0.0f && ws_world_aspect > 1.34f && fabsf(P[5] / P[0] - ws_world_aspect) < 0.01f;
}

int vk_tnl_ok(const D3DState *st, int nv)
{
    if (!vk_on() || nv < vk_tnl_min || tnl_count >= VKR_TNL_MAX)
        return 0;
    /* the race HUD and name labels in widescreen are placed from their vertex positions (vk.c
       draw_impl); the main view with ray-traced shadows has neither (and needs the GPU's transform to
       cast) */
    if (widescreen_on() && ws_race_depth > 0 && ws_menu_depth == 0 && (ws_hud_depth > 0 || !proj_widened(st)) &&
        !sh_active)
        return 0;
    return 1;
}

void vk_draw(const D3DState *st, int prim, const SwVertex *v, int nv, const uint16_t *idx, int ni,
             int pretransformed)
{
    if (!going()) {
        ctx_inline();
        vkr_draw(st, prim, v, nv, idx, ni, pretransformed);
        return;
    }
    if (nv <= 0)
        return;
    send_ctx();
    send_state(st);
    size_t nvb = (size_t)nv * sizeof *v;
    Cmd *c = put(C_DRAW, sizeof(Cmd) + 16 + nvb + (size_t)(idx ? ni : 0) * 2);
    int32_t *a = (int32_t *)(c + 1);
    a[0] = prim;
    a[1] = nv;
    a[2] = idx ? ni : 0;
    a[3] = pretransformed;
    memcpy(a + 4, v, nvb);
    if (idx && ni)
        memcpy((uint8_t *)(a + 4) + nvb, idx, (size_t)ni * 2);
    publish();
}

void vk_draw_tnl(const D3DState *st, int prim, const void *raw, int nv, const uint16_t *idx, int ni,
                 const TnlConst *k)
{
    tnl_count++;
    if (!going()) {
        ctx_inline();
        vkr_draw_tnl(st, prim, raw, nv, idx, ni, k);
        return;
    }
    if (nv <= 0)
        return;
    send_ctx();
    send_state(st);
    size_t nvb = ((size_t)nv * 40 + 7) & ~(size_t)7;
    Cmd *c = put(C_DRAW_TNL, sizeof(Cmd) + 16 + sizeof *k + nvb + (size_t)(idx ? ni : 0) * 2);
    int32_t *a = (int32_t *)(c + 1);
    a[0] = prim;
    a[1] = nv;
    a[2] = idx ? ni : 0;
    a[3] = 0;
    uint8_t *q = (uint8_t *)(a + 4);
    memcpy(q, k, sizeof *k);
    memcpy(q + sizeof *k, raw, (size_t)nv * 40);
    if (idx && ni)
        memcpy(q + sizeof *k + nvb, idx, (size_t)ni * 2);
    publish();
}

void vk_draw_tnl_inst(const D3DState *st, int prim, const void *raw, int nv, const uint16_t *idx, int ni,
                      const TnlConst *k, const float *recs, int n)
{
    tnl_count++;
    if (!going()) {
        ctx_inline();
        vkr_draw_tnl_inst(st, prim, raw, nv, idx, ni, k, recs, n);
        return;
    }
    if (nv <= 0 || n <= 0)
        return;
    send_ctx();
    send_state(st);
    size_t nvb = ((size_t)nv * 40 + 7) & ~(size_t)7, nib = ((size_t)(idx ? ni : 0) * 2 + 7) & ~(size_t)7;
    Cmd *c = put(C_DRAW_INST, sizeof(Cmd) + 16 + sizeof *k + nvb + nib + (size_t)n * 64);
    int32_t *a = (int32_t *)(c + 1);
    a[0] = prim;
    a[1] = nv;
    a[2] = idx ? ni : 0;
    a[3] = n;
    uint8_t *q = (uint8_t *)(a + 4);
    memcpy(q, k, sizeof *k);
    memcpy(q + sizeof *k, raw, (size_t)nv * 40);
    if (idx && ni)
        memcpy(q + sizeof *k + nvb, idx, (size_t)ni * 2);
    memcpy(q + sizeof *k + nvb + nib, recs, (size_t)n * 64);
    publish();
}

void vk_clear(const D3DState *st, int n, const int *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
    if (!going()) {
        ctx_inline();
        vkr_clear(st, n, rects, flags, color, z, stencil);
        return;
    }
    send_ctx();
    send_state(st);
    Cmd *c = put(C_CLEAR, sizeof(Cmd) + 24 + (size_t)n * 16);
    uint32_t *a = (uint32_t *)(c + 1);
    a[0] = (uint32_t)n;
    a[1] = flags;
    a[2] = color;
    a[3] = stencil;
    memcpy(&a[4], &z, 4);
    a[5] = 0;
    if (n)
        memcpy(a + 6, rects, (size_t)n * 16);
    publish();
}

void vk_fill(Surface *s, const int *rect, uint32_t raw)
{
    if (!going()) {
        ctx_inline();
        vkr_fill(s, rect, raw);
        return;
    }
    seen(s);
    Cmd *c = put(C_FILL, sizeof(Cmd) + 8 + 24);
    uint8_t *p = (uint8_t *)(c + 1);
    memcpy(p, &s, sizeof s);
    int *r = (int *)(p + 8);
    r[0] = rect != NULL;
    if (rect)
        memcpy(r + 1, rect, 16);
    r[5] = (int)raw;
    publish();
}

void vk_blt(Surface *dst, const int *drect, Surface *src, const int *srect, int key, uint32_t lo, uint32_t hi)
{
    if (!going()) {
        ctx_inline();
        vkr_blt(dst, drect, src, srect, key, lo, hi);
        return;
    }
    seen(dst);
    seen(src);
    Cmd *c = put(C_BLT, sizeof(Cmd) + 16 + 13 * 4);
    uint8_t *p = (uint8_t *)(c + 1);
    memcpy(p, &dst, sizeof dst);
    memcpy(p + 8, &src, sizeof src);
    int *r = (int *)(p + 16);
    memset(r, 0, 13 * 4);
    r[0] = drect != NULL;
    if (drect)
        memcpy(r + 1, drect, 16);
    r[5] = srect != NULL;
    if (srect)
        memcpy(r + 6, srect, 16);
    r[10] = key;
    r[11] = (int)lo;
    r[12] = (int)hi;
    publish();
}

void vk_present(Surface *src, const int *rect)
{
    tnl_count = 0;
    sh_views = 0;
    if (!going()) {
        ctx_inline();
        memcpy(vkr.gamma, gfx_gamma, sizeof vkr.gamma);
        vkr_present(src, rect);
        return;
    }
    seen(src);
    send_ctx();
    Cmd *c = put(C_PRESENT, sizeof(Cmd) + 32 + sizeof gfx_gamma);
    uint8_t *p = (uint8_t *)(c + 1);
    memcpy(p, &src, sizeof src);
    int *r = (int *)(p + 8);
    r[0] = rect != NULL;
    if (rect)
        memcpy(r + 1, rect, 16);
    memcpy(p + 32, gfx_gamma, sizeof gfx_gamma);
    unsigned long n = atomic_fetch_add(&frames_sent, 1) + 1;
    publish();
    FrameTrace *tr = trace && n - 1 < TRACE_MAX ? &trace[n - 1] : NULL;
    if (tr) {                           /* this frame's game side: since the last present (before its wait) */
        double w = now_real_ms(), cpu = thread_cpu_now();
        tr->wall = (float)(tr_wall0 ? w - tr_wall0 : 0);
        tr->cpu = (float)(tr_cpu0 ? cpu - tr_cpu0 : 0);
        tr->sync = (float)(sync_ms - tr_sync0);
        tr->nsync = (uint16_t)(syncs - tr_syncs0);
        tr->why = (uint16_t)tr_why;
        tr_cpu0 = cpu, tr_sync0 = sync_ms, tr_syncs0 = syncs, tr_why = 0;
    }
    double ahead0 = ahead_ms;
    /* at most a frame ahead of the render thread: its previous one shown before this one's draws go on */
    if (n - atomic_load(&frames_done) > 1) {
        double t0 = now_real_ms();
        aheads++;
        pthread_mutex_lock(&lk);
        atomic_store(&waiting, 1);
        while (n - atomic_load(&frames_done) > 1) {
            struct timespec ts;         /* (woken by the render thread's present; the timeout only a safety) */
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20000000;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&idle, &lk, &ts);
        }
        atomic_store(&waiting, 0);
        pthread_mutex_unlock(&lk);
        ahead_ms += now_real_ms() - t0;
    }
    if (tr) {                           /* (the next frame's time starts after the wait) */
        tr->ahead = (float)(ahead_ms - ahead0);
        tr_wall0 = now_real_ms();
    }
}

void vk_flip(Surface *s, Surface *t)
{
    if (!on) {                          /* (no Vulkan call: the images' pointers only) */
        vkr_flip(s, t);
        return;
    }
    Cmd *c = put(C_FLIP, sizeof(Cmd) + 16);
    uint8_t *p = (uint8_t *)(c + 1);
    memcpy(p, &s, sizeof s);
    memcpy(p + 8, &t, sizeof t);
    publish();
}

void vk_far_draw(const D3DState *st, const TnlConst *k, uint32_t first, uint32_t count, int32_t vbase)
{
    tnl_count++;
    if (!going()) {
        ctx_inline();
        vkr_far_draw(st, k, first, count, vbase);
        return;
    }
    send_ctx();
    send_state(st);
    Cmd *c = put(C_FAR_DRAW, sizeof(Cmd) + 16 + sizeof *k);
    int32_t *a = (int32_t *)(c + 1);
    a[0] = (int32_t)first;
    a[1] = (int32_t)count;
    a[2] = vbase;
    a[3] = 0;
    memcpy(a + 4, k, sizeof *k);
    publish();
}

void vk_shadow_begin(const VkShadowView *v)
{
    sh_active = vk_rt_available() && sh_views < 4;      /* (vk.c SH_VIEWS) */
    if (sh_active)
        sh_views++;
    if (!going()) {
        vkr_shadow_begin(v);
        return;
    }
    Cmd *c = put(C_SH_BEGIN, sizeof(Cmd) + sizeof *v);
    memcpy(c + 1, v, sizeof *v);
    publish();
}

static void put_int(uint32_t type, int v)
{
    Cmd *c = put(type, sizeof(Cmd) + 8);
    *(int *)(c + 1) = v;
    publish();
}

void vk_shadow_class(int cls)
{
    if (!going()) {
        vkr_shadow_class(cls);
        return;
    }
    put_int(C_SH_CLASS, cls);
}

void vk_shadow_end(void)
{
    sh_active = 0;
    if (!going()) {
        vkr_shadow_end();
        return;
    }
    put(C_SH_END, sizeof(Cmd));
    publish();
}

void vk_smoke_view_begin(const VkSmokeView *v)
{
    if (!going()) {
        vkr_smoke_view_begin(v);
        return;
    }
    Cmd *c = put(C_SM_BEGIN, sizeof(Cmd) + sizeof *v);
    memcpy(c + 1, v, sizeof *v);
    publish();
}

void vk_smoke_view_end(void)
{
    if (!going()) {
        vkr_smoke_view_end();
        return;
    }
    put(C_SM_END, sizeof(Cmd));
    publish();
}

void vk_smoke_take(const D3DState *st, const float (*pos)[3], int n, uint32_t diffuse, const float (*uv)[2])
{
    if (!going()) {
        ctx_inline();
        vkr_smoke_take(st, pos, n, diffuse, uv);
        return;
    }
    if (n <= 0)
        return;
    send_ctx();
    send_state(st);
    Cmd *c = put(C_SM_TAKE, sizeof(Cmd) + 8 + 20 * (size_t)n);
    uint32_t *a = (uint32_t *)(c + 1);
    a[0] = (uint32_t)n;
    a[1] = diffuse;
    memcpy((uint8_t *)(a + 2), pos, 12 * (size_t)n);
    memcpy((uint8_t *)(a + 2) + 12 * (size_t)n, uv, 8 * (size_t)n);
    publish();
}

static void put_scene(uint32_t type, const VkScene *s)
{
    Cmd *c = put(type, sizeof(Cmd) + sizeof *s);
    memcpy(c + 1, s, sizeof *s);
    publish();
}

void vk_scene_begin(const VkScene *s)
{
    if (!going()) {
        vkr_scene_begin(s);
        return;
    }
    put_scene(C_SC_BEGIN, s);
}

void vk_scene_camera(const VkScene *s)
{
    if (!going()) {
        vkr_scene_camera(s);
        return;
    }
    put_scene(C_SC_CAMERA, s);
}

void vk_scene_class(int cls)
{
    if (!going()) {
        vkr_scene_class(cls);
        return;
    }
    put_int(C_SC_CLASS, cls);
}

void vk_scene_end(void)
{
    if (!going()) {
        vkr_scene_end();
        return;
    }
    put(C_SC_END, sizeof(Cmd));
    publish();
}

/* ---- the ones that wait for the render thread, then run here */
void vk_cpu_access(Surface *s, int write)
{
    if (on) {
        if (!s->vk_seen)
            return;                     /* (never named to the renderer: no GPU copy) */
        /* a texture: the GPU never draws into one, so nothing comes back; a write keeps a copy of its
           pixels for the calls made so far (the render thread may not have read them yet) */
        if ((s->caps & DDSCAPS_TEXTURE) && !(s->caps & DDSCAPS_ZBUFFER) && !getenv_snap_off()) {
            if (!write || atomic_load(&done) >= nput || snap_take(s))
                return;
        }
        sync_why = SYNC_CPU;
        double t0 = trace ? now_real_ms() : 0;
        vk_sync();
        if (trace)
            LOG(LOG_ALWAYS, "frame trace: frame %lu: CPU %s of a %ux%u %u-bit surface (caps %x) waited %.2f ms",
                atomic_load(&frames_sent), write ? "write" : "read", s->width, s->height, s->bpp * 8, s->caps,
                now_real_ms() - t0);
    }
    vkr_cpu_access(s, write);
}

void vk_surface_destroyed(Surface *s)
{
    sync_why = SYNC_GONE;
    vk_sync();
    vkr_surface_destroyed(s);
}

int vk_far_mesh(const void *verts, uint32_t nv, const uint32_t *idx, uint32_t ni)
{
    sync_why = SYNC_FAR;
    vk_sync();
    return vkr_far_mesh(verts, nv, idx, ni);
}

int vk_shadow_terrain(const float *xyz, uint32_t nv, const uint32_t *idx, uint32_t ni)
{
    sync_why = SYNC_SHTERR;
    vk_sync();
    return vkr_shadow_terrain(xyz, nv, idx, ni);
}

int vk_shadow_lightmap(const uint8_t *lm)
{
    sync_why = SYNC_LIGHTMAP;
    vk_sync();
    return vkr_shadow_lightmap(lm);
}

void vk_options(void)
{
    sync_why = SYNC_OPTIONS;
    vk_sync();
    vkr_options();
}
