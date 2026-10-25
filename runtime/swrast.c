/* Software rasterizer for debugging and frame dumps. With TD6_DUMP_FRAMES=<dir> (and
   optionally TD6_DUMP_EVERY=<n>, default 30) every n-th presented frame is drawn here and
   written to <dir>/frame_NNNNN.ppm. Drawing goes straight into the render target's memory, in
   call order, so 2D Blts and Locks on the back buffer, Clears and triangles combine as on a
   card. On other frames nothing is drawn, unless TD6_DUMP_ALL_DRAWN=1 (needed when a frame
   blends over the previous one).

   Recognisable, not exact: clip-space triangles are clipped against the near/far planes,
   pre-transformed (XYZRHW) ones are drawn as given; perspective-correct texturing with point
   sampling (one mip level per triangle), the texture stage operations, specular add, fog,
   alpha test, colour key, z test/write and alpha blending. Lines and points are drawn plainly. */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "runtime.h"
#include "gfx.h"

static int enabled, active, always;
static char dumpdir[900];
static unsigned long every = 30, frame, tris;

void swrast_init(void)
{
    const char *d = getenv("TD6_DUMP_FRAMES");
    if (!d || !*d)
        return;
    snprintf(dumpdir, sizeof dumpdir, "%s", d);
    if (mkdir(dumpdir, 0755) && errno != EEXIST)
        logmsg(LOG_ALWAYS, "frame dumps: cannot create %s: %s", dumpdir, strerror(errno));
    const char *e = getenv("TD6_DUMP_EVERY");
    if (e && atol(e) > 0)
        every = (unsigned long)atol(e);
    enabled = 1;
    /* TD6_DUMP_ALL_DRAWN=1: draw every frame (frames that blend over the previous one come
       out right), still dump every n-th */
    const char *a = getenv("TD6_DUMP_ALL_DRAWN");
    always = a && *a && strcmp(a, "0");
    active = always || 1 % every == 0;
    logmsg(LOG_ALWAYS, "frame dumps: every %lu frames to %s", every, dumpdir);
}

int swrast_active(void)
{
    return active;
}

static void write_ppm(Surface *s, const int *rect, unsigned long n)
{
    int x0 = 0, y0 = 0, x1 = (int)s->width, y1 = (int)s->height;
    if (rect) {
        x0 = rect[0] > 0 ? rect[0] : 0;
        y0 = rect[1] > 0 ? rect[1] : 0;
        x1 = rect[2] < x1 ? rect[2] : x1;
        y1 = rect[3] < y1 ? rect[3] : y1;
    }
    if (x0 >= x1 || y0 >= y1 || !s->mem)
        return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%05lu.ppm", dumpdir, n);
    FILE *f = fopen(path, "wb");
    if (!f) {
        logmsg(LOG_ALWAYS, "frame dumps: cannot write %s: %s", path, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", x1 - x0, y1 - y0);
    uint8_t *row = malloc((size_t)(x1 - x0) * 3);
    if (!row)
        fatal("out of memory");
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            uint32_t c = pix_decode(s, surf_read(s, (uint32_t)x, (uint32_t)y));
            uint8_t *p = row + 3 * (x - x0);
            p[0] = (uint8_t)(c >> 16);
            p[1] = (uint8_t)(c >> 8);
            p[2] = (uint8_t)c;
        }
        fwrite(row, 3, (size_t)(x1 - x0), f);
    }
    free(row);
    fclose(f);
    LOG(LOG_D3D, "frame %lu dumped to %s (%lu triangles drawn)", n, path, tris);
}

void swrast_present(Surface *s, const int *rect)
{
    frame++;
    if (active && frame % every == 0)
        write_ppm(s, rect, frame);
    tris = 0;
    active = enabled && (always || (frame + 1) % every == 0);
}

/* ------------------------------------------------------------------ draw context */
#define NA 24                     /* attributes: diffuse 4, specular 4, 8 texture coordinate pairs */

typedef struct { float p[4]; float a[NA]; } CV;            /* clip space */
typedef struct { float x, y, z, q; float a[NA]; } PV;      /* screen space, q = 1/w */

typedef struct {
    Surface *s;                   /* level chosen for the current triangle */
    Surface *top;
    int tci, addru, addrv, levels, mip, maxmip;
    float lodbias;
    float border[4];
} Tex;

typedef struct {
    const D3DState *st;
    Surface *rt, *zb;
    int vx0, vy0, vx1, vy1;
    int ztest, zwrite, zfunc;
    uint32_t zmask, zshift;
    double zmax;
    float zbias;
    int blend, sblend, dblend, atest, afunc, aref, colorkey, specular, cull, flat;
    int fog, fogtable, fogmode;
    float fogstart, fogend, fogdensity, fogcolor[3], tfactor[4];
    int nstages;
    Tex tex[STAGES];
    float vpx, vpy, vpw, vph, minz, maxz;
} Ctx;

static void unpack(uint32_t c, float *o)
{
    o[0] = (float)((c >> 16) & 0xff) / 255.0f;
    o[1] = (float)((c >> 8) & 0xff) / 255.0f;
    o[2] = (float)(c & 0xff) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

static uint32_t pack(const float *c)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        r |= (uint32_t)(v * 255.0f + 0.5f) << (i == 3 ? 24 : 16 - 8 * i);
    }
    return r;
}

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static int ctx_setup(Ctx *cx, const D3DState *st)
{
    memset(cx, 0, sizeof *cx);
    cx->st = st;
    cx->rt = st->rt;
    if (!cx->rt || cx->rt->dead || !cx->rt->mem)
        return 0;
    cx->vx0 = (int)st->vp.x;
    cx->vy0 = (int)st->vp.y;
    cx->vx1 = (int)(st->vp.x + st->vp.w);
    cx->vy1 = (int)(st->vp.y + st->vp.h);
    if (cx->vx1 > (int)cx->rt->width)
        cx->vx1 = (int)cx->rt->width;
    if (cx->vy1 > (int)cx->rt->height)
        cx->vy1 = (int)cx->rt->height;
    cx->vpx = (float)st->vp.x;
    cx->vpy = (float)st->vp.y;
    cx->vpw = (float)st->vp.w;
    cx->vph = (float)st->vp.h;
    cx->minz = st->vp.minz;
    cx->maxz = st->vp.maxz;
    const uint32_t *rs = st->rs;
    cx->zb = st->zb && !st->zb->dead && st->zb->mem ? st->zb : NULL;
    cx->ztest = cx->zb && rs[RS_ZENABLE];
    cx->zwrite = cx->ztest && rs[RS_ZWRITEENABLE];
    cx->zfunc = (int)rs[RS_ZFUNC];
    if (cx->zb) {
        cx->zmask = cx->zb->zmask;
        cx->zshift = cx->zmask ? (uint32_t)__builtin_ctz(cx->zmask) : 0;
        cx->zmax = (double)(cx->zmask >> cx->zshift);
    }
    cx->zbias = (float)rs[RS_ZBIAS] / 65536.0f;
    cx->blend = rs[RS_ALPHABLENDENABLE] != 0;
    cx->sblend = (int)rs[RS_SRCBLEND];
    cx->dblend = (int)rs[RS_DESTBLEND];
    cx->atest = rs[RS_ALPHATESTENABLE] != 0;
    cx->afunc = (int)rs[RS_ALPHAFUNC];
    cx->aref = (int)(rs[RS_ALPHAREF] & 0xff);
    cx->colorkey = rs[RS_COLORKEYENABLE] != 0;
    cx->specular = rs[RS_SPECULARENABLE] != 0;
    cx->cull = (int)rs[RS_CULLMODE];
    cx->flat = rs[RS_SHADEMODE] == 1;
    cx->fog = rs[RS_FOGENABLE] != 0;
    cx->fogmode = (int)rs[RS_FOGTABLEMODE];
    cx->fogtable = cx->fog && cx->fogmode != 0;
    cx->fogstart = u2f(rs[RS_FOGSTART]);
    cx->fogend = u2f(rs[RS_FOGEND]);
    cx->fogdensity = u2f(rs[RS_FOGDENSITY]);
    float fc[4];
    unpack(rs[RS_FOGCOLOR], fc);
    memcpy(cx->fogcolor, fc, sizeof cx->fogcolor);
    unpack(rs[RS_TEXTUREFACTOR], cx->tfactor);
    for (int s = 0; s < STAGES; s++) {
        if (st->tss[s][TSS_COLOROP] <= 1)
            break;
        cx->nstages = s + 1;
        Tex *t = &cx->tex[s];
        Surface *ts = st->tex[s];
        if (ts && (ts->dead || !ts->mem || ts->bpp < 1 || ts->bpp > 4 || (ts->pf[1] & DDPF_ZBUFFER)))
            ts = NULL;
        t->top = t->s = ts;
        t->tci = (int)(st->tss[s][TSS_TEXCOORDINDEX] & 7);
        t->addru = (int)st->tss[s][TSS_ADDRESSU];
        t->addrv = (int)st->tss[s][TSS_ADDRESSV];
        unpack(st->tss[s][TSS_BORDERCOLOR], t->border);
        t->mip = st->tss[s][TSS_MIPFILTER] > 1;
        t->maxmip = (int)st->tss[s][TSS_MAXMIPLEVEL];
        t->lodbias = u2f(st->tss[s][TSS_MIPMAPLODBIAS]);
        t->levels = 1;
        if (ts && (ts->caps & DDSCAPS_MIPMAP))
            for (Surface *m = ts->next; m && !m->dead && t->levels < 16; m = m->next)
                t->levels++;
    }
    return 1;
}

/* ------------------------------------------------------------------ pixel pipeline */
static int cmp(int func, double a, double b)
{
    switch (func) {
    case 1: return 0;
    case 2: return a < b;
    case 3: return a == b;
    case 4: return a <= b;
    case 5: return a > b;
    case 6: return a != b;
    case 7: return a >= b;
    default: return 1;
    }
}

static int wrap_coord(int i, int n, int mode)
{
    switch (mode) {
    case 2: {                     /* MIRROR */
        int p = i % (2 * n);
        if (p < 0)
            p += 2 * n;
        return p < n ? p : 2 * n - 1 - p;
    }
    case 3:                       /* CLAMP */
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    case 4:                       /* BORDER */
        return i < 0 || i >= n ? -1 : i;
    default: {                    /* WRAP */
        int p = i % n;
        return p < 0 ? p + n : p;
    }
    }
}

/* returns 0 when the texel is colour-keyed out */
static int sample(const Ctx *cx, const Tex *t, float u, float v, float *out)
{
    Surface *s = t->s;
    if (!(fabsf(u) < 1e6f))
        u = 0;
    if (!(fabsf(v) < 1e6f))
        v = 0;
    int x = wrap_coord((int)floorf(u * (float)s->width), (int)s->width, t->addru);
    int y = wrap_coord((int)floorf(v * (float)s->height), (int)s->height, t->addrv);
    if (x < 0 || y < 0) {
        memcpy(out, t->border, 4 * sizeof(float));
        return 1;
    }
    uint32_t raw = surf_read(s, (uint32_t)x, (uint32_t)y);
    if (cx->colorkey && (t->top->ck_set & DDCKEY_SRCBLT) && raw >= t->top->ck[2][0] && raw <= t->top->ck[2][1])
        return 0;
    if (s->pf[1] & DDPF_PALETTEINDEXED8) {
        float g = (float)raw / 255.0f;
        out[0] = out[1] = out[2] = g;
        out[3] = 1;
        return 1;
    }
    unpack(pix_decode(s, raw), out);
    return 1;
}

static void arg(uint32_t a, const float *dif, const float *cur, const float *tex, const float *tf, const float *spec,
                float *o)
{
    const float *src;
    switch (a & 0xf) {
    case 0: src = dif; break;
    case 2: src = tex; break;
    case 3: src = tf; break;
    case 4: src = spec; break;
    default: src = cur; break;
    }
    memcpy(o, src, 4 * sizeof(float));
    if (a & 0x20)
        o[0] = o[1] = o[2] = o[3];
    if (a & 0x10)
        for (int i = 0; i < 4; i++)
            o[i] = 1 - o[i];
}

/* One D3DTOP on channels [c0, c1) of a1/a2 into r. */
static void top(uint32_t op, const float *a1, const float *a2, const float *dif, const float *cur, const float *tex,
                const float *tf, float *r, int c0, int c1)
{
    for (int i = c0; i < c1; i++) {
        float x = a1[i], y = a2[i], v;
        switch (op) {
        case 2: v = x; break;
        case 3: v = y; break;
        case 4: v = x * y; break;
        case 5: v = 2 * x * y; break;
        case 6: v = 4 * x * y; break;
        case 7: v = x + y; break;
        case 8: v = x + y - 0.5f; break;
        case 9: v = 2 * (x + y - 0.5f); break;
        case 10: v = x - y; break;
        case 11: v = x + y - x * y; break;
        case 12: v = x * dif[3] + y * (1 - dif[3]); break;
        case 13: v = x * tex[3] + y * (1 - tex[3]); break;
        case 14: v = x * tf[3] + y * (1 - tf[3]); break;
        case 15: v = x + y * (1 - tex[3]); break;
        case 16: v = x * cur[3] + y * (1 - cur[3]); break;
        case 18: v = i < 3 ? x + a1[3] * y : x * y; break;
        case 19: v = i < 3 ? x * y + a1[3] : x * y; break;
        case 20: v = i < 3 ? (1 - a1[3]) * y + x : x * y; break;
        case 21: v = i < 3 ? (1 - x) * y + a1[3] : x * y; break;
        case 24:
            v = 4 * ((a1[0] - 0.5f) * (a2[0] - 0.5f) + (a1[1] - 0.5f) * (a2[1] - 0.5f) + (a1[2] - 0.5f) * (a2[2] - 0.5f));
            break;
        default: v = x; break;   /* PREMODULATE, bump mapping: first argument */
        }
        r[i] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
}

static float blend_factor(int f, int ch, const float *s, const float *d)
{
    switch (f) {
    case 1: return 0;
    case 2: return 1;
    case 3: return s[ch];
    case 4: return 1 - s[ch];
    case 5: case 12: return s[3];
    case 6: case 13: return 1 - s[3];
    case 7: return d[3];
    case 8: return 1 - d[3];
    case 9: return d[ch];
    case 10: return 1 - d[ch];
    case 11: return ch == 3 ? 1 : (s[3] < 1 - d[3] ? s[3] : 1 - d[3]);
    default: return 1;
    }
}

static float fog_factor(const Ctx *cx, float d)
{
    float f;
    if (cx->fogmode == 3)
        f = cx->fogend != cx->fogstart ? (cx->fogend - d) / (cx->fogend - cx->fogstart) : 1;
    else if (cx->fogmode == 1)
        f = expf(-cx->fogdensity * d);
    else
        f = expf(-(cx->fogdensity * d) * (cx->fogdensity * d));
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

/* Shade, test, blend and write one pixel. a: interpolated attributes; w: eye depth. */
static void pixel(const Ctx *cx, int x, int y, float z, float w, const float *a)
{
    double zi = 0;
    uint32_t zraw = 0;
    if (cx->zb) {
        float zc = z < 0 ? 0 : z > 1 ? 1 : z;
        zi = floor((double)zc * cx->zmax + 0.5);
        if (cx->ztest) {
            zraw = surf_read(cx->zb, (uint32_t)x, (uint32_t)y);
            if (!cmp(cx->zfunc, zi, (double)((zraw & cx->zmask) >> cx->zshift)))
                return;
        }
    }
    const float *dif = a, *spec = a + 4;
    float cur[4], texel[4] = {1, 1, 1, 1};
    memcpy(cur, dif, sizeof cur);
    for (int s = 0; s < cx->nstages; s++) {
        const Tex *t = &cx->tex[s];
        const uint32_t *ts = cx->st->tss[s];
        if (t->s) {
            if (!sample(cx, t, a[8 + 2 * t->tci], a[9 + 2 * t->tci], texel))
                return;
        } else {
            texel[0] = texel[1] = texel[2] = texel[3] = 1;
        }
        float c1[4], c2[4], r[4];
        arg(ts[TSS_COLORARG1], dif, cur, texel, cx->tfactor, spec, c1);
        arg(ts[TSS_COLORARG2], dif, cur, texel, cx->tfactor, spec, c2);
        top(ts[TSS_COLOROP], c1, c2, dif, cur, texel, cx->tfactor, r, 0, 3);
        if (ts[TSS_ALPHAOP] > 1) {
            arg(ts[TSS_ALPHAARG1], dif, cur, texel, cx->tfactor, spec, c1);
            arg(ts[TSS_ALPHAARG2], dif, cur, texel, cx->tfactor, spec, c2);
            top(ts[TSS_ALPHAOP], c1, c2, dif, cur, texel, cx->tfactor, r, 3, 4);
        } else {
            r[3] = cur[3];
        }
        memcpy(cur, r, sizeof cur);
    }
    if (cx->specular)
        for (int i = 0; i < 3; i++)
            cur[i] = cur[i] + spec[i] > 1 ? 1 : cur[i] + spec[i];
    if (cx->fog) {
        float f = cx->fogtable ? fog_factor(cx, w) : spec[3];
        for (int i = 0; i < 3; i++)
            cur[i] = f * cur[i] + (1 - f) * cx->fogcolor[i];
    }
    if (cx->atest && !cmp(cx->afunc, (double)(int)(cur[3] * 255.0f + 0.5f), (double)cx->aref))
        return;
    if (cx->zwrite)
        surf_write(cx->zb, (uint32_t)x, (uint32_t)y, (zraw & ~cx->zmask) | (((uint32_t)zi << cx->zshift) & cx->zmask));
    if (cx->blend) {
        float d[4], o[4];
        unpack(pix_decode(cx->rt, surf_read(cx->rt, (uint32_t)x, (uint32_t)y)), d);
        if (!cx->rt->bits[3])
            d[3] = 1;
        int sb = cx->sblend, db = cx->dblend;
        if (sb == 12)
            db = 6;
        else if (sb == 13)
            db = 5;
        for (int i = 0; i < 4; i++) {
            float v = cur[i] * blend_factor(sb, i, cur, d) + d[i] * blend_factor(db, i, cur, d);
            o[i] = v > 1 ? 1 : v;
        }
        memcpy(cur, o, sizeof cur);
    }
    surf_write(cx->rt, (uint32_t)x, (uint32_t)y, pix_encode(cx->rt, pack(cur)));
}

/* ------------------------------------------------------------------ triangles */
static void choose_mips(Ctx *cx, const PV *v0, const PV *v1, const PV *v2, float area)
{
    for (int s = 0; s < cx->nstages; s++) {
        Tex *t = &cx->tex[s];
        if (!t->top)
            continue;
        t->s = t->top;
        if (!t->mip || t->levels < 2)
            continue;
        int k = 8 + 2 * t->tci;
        float du1 = v1->a[k] - v0->a[k], dv1 = v1->a[k + 1] - v0->a[k + 1];
        float du2 = v2->a[k] - v0->a[k], dv2 = v2->a[k + 1] - v0->a[k + 1];
        float texels = fabsf(du1 * dv2 - du2 * dv1) * (float)t->top->width * (float)t->top->height;
        float lod = texels > 0 ? 0.5f * log2f(texels / area) + t->lodbias : 0;
        int level = lod > 0 ? (int)(lod + 0.5f) : 0;
        if (level < t->maxmip)
            level = t->maxmip;
        if (level > t->levels - 1)
            level = t->levels - 1;
        for (int i = 0; i < level && t->s->next; i++)
            t->s = t->s->next;
    }
}

/* Edges are evaluated on positions snapped to 1/16 pixel, in integers: two triangles sharing an
   edge then agree exactly on which side every pixel centre lies (no cracks, no double hits). */
#define SNAP 16.0f

static inline int64_t edge(const int64_t *ax, const int64_t *bx, int64_t px, int64_t py)
{
    return (bx[0] - ax[0]) * (py - ax[1]) - (bx[1] - ax[1]) * (px - ax[0]);
}

static inline int top_left(const int64_t *a, const int64_t *b)
{
    int64_t dx = b[0] - a[0], dy = b[1] - a[1];
    return dy < 0 || (dy == 0 && dx > 0);
}

static void raster_tri(Ctx *cx, const PV *v0, const PV *v1, const PV *v2)
{
    const PV *v[3] = {v0, v1, v2};
    int64_t P[3][2];
    for (int i = 0; i < 3; i++) {
        if (!(fabsf(v[i]->x) < 4e6f) || !(fabsf(v[i]->y) < 4e6f))
            return;
        P[i][0] = llrintf(v[i]->x * SNAP);
        P[i][1] = llrintf(v[i]->y * SNAP);
    }
    int64_t area = (P[1][0] - P[0][0]) * (P[2][1] - P[0][1]) - (P[2][0] - P[0][0]) * (P[1][1] - P[0][1]);
    if (!area)
        return;
    /* y points down: area > 0 is clockwise on screen */
    if ((cx->cull == 2 && area > 0) || (cx->cull == 3 && area < 0))
        return;
    if (area < 0) {
        const PV *t = v[1];
        v[1] = v[2];
        v[2] = t;
        int64_t tx = P[1][0], ty = P[1][1];
        P[1][0] = P[2][0];
        P[1][1] = P[2][1];
        P[2][0] = tx;
        P[2][1] = ty;
        area = -area;
    }
    int64_t mnx = P[0][0], mxx = P[0][0], mny = P[0][1], mxy = P[0][1];
    for (int i = 1; i < 3; i++) {
        mnx = P[i][0] < mnx ? P[i][0] : mnx;
        mxx = P[i][0] > mxx ? P[i][0] : mxx;
        mny = P[i][1] < mny ? P[i][1] : mny;
        mxy = P[i][1] > mxy ? P[i][1] : mxy;
    }
    int64_t x0 = (mnx + 15) >> 4, x1 = mxx >> 4, y0 = (mny + 15) >> 4, y1 = mxy >> 4;
    if (x0 < cx->vx0)
        x0 = cx->vx0;
    if (y0 < cx->vy0)
        y0 = cx->vy0;
    if (x1 > cx->vx1 - 1)
        x1 = cx->vx1 - 1;
    if (y1 > cx->vy1 - 1)
        y1 = cx->vy1 - 1;
    if (x0 > x1 || y0 > y1)
        return;
    choose_mips(cx, v[0], v[1], v[2], (float)area / (SNAP * SNAP));
    tris++;
    int tl0 = top_left(P[1], P[2]), tl1 = top_left(P[2], P[0]), tl2 = top_left(P[0], P[1]);
    float aq[3][NA];
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < NA; k++)
            aq[i][k] = v[i]->a[k] * v[i]->q;
    double inv_area = 1.0 / (double)area;
    for (int64_t y = y0; y <= y1; y++)
        for (int64_t x = x0; x <= x1; x++) {
            int64_t px = x << 4, py = y << 4;
            int64_t e0 = edge(P[1], P[2], px, py), e1 = edge(P[2], P[0], px, py), e2 = edge(P[0], P[1], px, py);
            if (e0 < 0 || e1 < 0 || e2 < 0 || (e0 == 0 && !tl0) || (e1 == 0 && !tl1) || (e2 == 0 && !tl2))
                continue;
            float l0 = (float)(e0 * inv_area), l1 = (float)(e1 * inv_area), l2 = (float)(e2 * inv_area);
            float z = l0 * v[0]->z + l1 * v[1]->z + l2 * v[2]->z - cx->zbias;
            float q = l0 * v[0]->q + l1 * v[1]->q + l2 * v[2]->q;
            if (!(q > 0))
                q = 1e-20f;
            float iq = 1.0f / q, a[NA];
            for (int k = 0; k < NA; k++)
                a[k] = (l0 * aq[0][k] + l1 * aq[1][k] + l2 * aq[2][k]) * iq;
            pixel(cx, (int)x, (int)y, z, iq, a);
        }
}

static void project(const Ctx *cx, const CV *c, PV *p)
{
    float q = 1.0f / c->p[3];
    p->x = cx->vpx + (c->p[0] * q + 1.0f) * cx->vpw * 0.5f;
    p->y = cx->vpy + (1.0f - c->p[1] * q) * cx->vph * 0.5f;
    p->z = cx->minz + c->p[2] * q * (cx->maxz - cx->minz);
    p->q = q;
    memcpy(p->a, c->a, sizeof p->a);
}

/* near, far, w > 0, and a guard band of 8 viewports around the screen */
#define NPLANES 7
static float plane_dist(int plane, const CV *v)
{
    switch (plane) {
    case 0: return v->p[2];                     /* near: z >= 0 */
    case 1: return v->p[3] - v->p[2];           /* far: z <= w */
    case 2: return v->p[3] - 1e-6f;             /* w > 0 */
    case 3: return 8 * v->p[3] - v->p[0];
    case 4: return 8 * v->p[3] + v->p[0];
    case 5: return 8 * v->p[3] - v->p[1];
    default: return 8 * v->p[3] + v->p[1];
    }
}

static void clip_tri(Ctx *cx, const CV *a, const CV *b, const CV *c)
{
    CV buf[2][16];
    int n = 3;
    buf[0][0] = *a;
    buf[0][1] = *b;
    buf[0][2] = *c;
    int cur = 0;
    for (int plane = 0; plane < NPLANES; plane++) {
        CV *in = buf[cur], *out = buf[cur ^ 1];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const CV *p = &in[i], *q = &in[(i + 1) % n];
            float dp = plane_dist(plane, p), dq = plane_dist(plane, q);
            if (dp >= 0 && m < 16)
                out[m++] = *p;
            if ((dp >= 0) != (dq >= 0) && m < 16) {
                float t = dp / (dp - dq);
                CV *r = &out[m++];
                for (int k = 0; k < 4; k++)
                    r->p[k] = p->p[k] + t * (q->p[k] - p->p[k]);
                for (int k = 0; k < NA; k++)
                    r->a[k] = p->a[k] + t * (q->a[k] - p->a[k]);
            }
        }
        n = m;
        cur ^= 1;
        if (n < 3)
            return;
    }
    PV pv[16];
    for (int i = 0; i < n; i++)
        project(cx, &buf[cur][i], &pv[i]);
    for (int i = 1; i + 1 < n; i++)
        raster_tri(cx, &pv[0], &pv[i], &pv[i + 1]);
}

static void to_attrs(const SwVertex *v, float *a)
{
    memcpy(a, v->diffuse, 4 * sizeof(float));
    memcpy(a + 4, v->specular, 4 * sizeof(float));
    memcpy(a + 8, v->tc, 16 * sizeof(float));
}

static void triangle(Ctx *cx, const SwVertex *v, int i0, int i1, int i2, int flat, int pre)
{
    const SwVertex *t[3] = {&v[i0], &v[i1], &v[i2]};
    if (pre) {
        PV p[3];
        for (int i = 0; i < 3; i++) {
            p[i].x = t[i]->x;
            p[i].y = t[i]->y;
            p[i].z = t[i]->z;
            p[i].q = t[i]->w != 0 ? 1.0f / t[i]->w : 1.0f;
            to_attrs(t[i], p[i].a);
            if (cx->flat) {
                memcpy(p[i].a, v[flat].diffuse, 4 * sizeof(float));
                memcpy(p[i].a + 4, v[flat].specular, 4 * sizeof(float));
            }
        }
        raster_tri(cx, &p[0], &p[1], &p[2]);
        return;
    }
    CV c[3];
    for (int i = 0; i < 3; i++) {
        c[i].p[0] = t[i]->x;
        c[i].p[1] = t[i]->y;
        c[i].p[2] = t[i]->z;
        c[i].p[3] = t[i]->w;
        to_attrs(t[i], c[i].a);
        if (cx->flat) {
            memcpy(c[i].a, v[flat].diffuse, 4 * sizeof(float));
            memcpy(c[i].a + 4, v[flat].specular, 4 * sizeof(float));
        }
    }
    clip_tri(cx, &c[0], &c[1], &c[2]);
}

/* ------------------------------------------------------------------ lines and points */
static int screen_pos(const Ctx *cx, const SwVertex *v, int pre, PV *p)
{
    if (pre) {
        p->x = v->x;
        p->y = v->y;
        p->z = v->z;
        p->q = v->w != 0 ? 1.0f / v->w : 1.0f;
    } else {
        if (!(v->w > 1e-6f) || v->z < 0 || v->z > v->w)
            return 0;
        CV c = {{v->x, v->y, v->z, v->w}, {0}};
        project(cx, &c, p);
    }
    to_attrs(v, p->a);
    return 1;
}

static void plot(Ctx *cx, const PV *p)
{
    int x = (int)floorf(p->x + 0.5f), y = (int)floorf(p->y + 0.5f);
    if (x < cx->vx0 || x >= cx->vx1 || y < cx->vy0 || y >= cx->vy1)
        return;
    pixel(cx, x, y, p->z, p->q > 0 ? 1.0f / p->q : 1.0f, p->a);
}

static void line(Ctx *cx, const SwVertex *a, const SwVertex *b, int pre)
{
    PV p, q, r;
    if (!screen_pos(cx, a, pre, &p) || !screen_pos(cx, b, pre, &q))
        return;
    float dx = q.x - p.x, dy = q.y - p.y;
    int n = (int)fmaxf(fabsf(dx), fabsf(dy));
    if (n > 4096)
        n = 4096;
    for (int i = 0; i <= n; i++) {
        float t = n ? (float)i / (float)n : 0;
        r.x = p.x + t * dx;
        r.y = p.y + t * dy;
        r.z = p.z + t * (q.z - p.z);
        r.q = p.q + t * (q.q - p.q);
        for (int k = 0; k < NA; k++)
            r.a[k] = p.a[k] + t * (q.a[k] - p.a[k]);
        plot(cx, &r);
    }
}

/* ------------------------------------------------------------------ entry point */
void swrast_draw(const D3DState *st, int prim, const SwVertex *v, int nv, const uint16_t *idx, int ni,
              int pretransformed)
{
    Ctx *cx = malloc(sizeof *cx);
    if (!cx)
        fatal("out of memory");
    if (!ctx_setup(cx, st)) {
        free(cx);
        return;
    }
    int n = idx ? ni : nv;
#define V(i) (idx ? (int)idx[i] : (i))
#define OK(i) (V(i) < nv)
    switch (prim) {
    case PT_TRIANGLELIST:
        for (int i = 0; i + 2 < n; i += 3)
            if (OK(i) && OK(i + 1) && OK(i + 2))
                triangle(cx, v, V(i), V(i + 1), V(i + 2), V(i), pretransformed);
        break;
    case PT_TRIANGLESTRIP:
        for (int i = 0; i + 2 < n; i++)
            if (OK(i) && OK(i + 1) && OK(i + 2)) {
                if (i & 1)
                    triangle(cx, v, V(i + 1), V(i), V(i + 2), V(i), pretransformed);
                else
                    triangle(cx, v, V(i), V(i + 1), V(i + 2), V(i), pretransformed);
            }
        break;
    case PT_TRIANGLEFAN:
        for (int i = 1; i + 1 < n; i++)
            if (OK(0) && OK(i) && OK(i + 1))
                triangle(cx, v, V(0), V(i), V(i + 1), V(i), pretransformed);
        break;
    case PT_LINELIST:
        for (int i = 0; i + 1 < n; i += 2)
            if (OK(i) && OK(i + 1))
                line(cx, &v[V(i)], &v[V(i + 1)], pretransformed);
        break;
    case PT_LINESTRIP:
        for (int i = 0; i + 1 < n; i++)
            if (OK(i) && OK(i + 1))
                line(cx, &v[V(i)], &v[V(i + 1)], pretransformed);
        break;
    case PT_POINTLIST:
        for (int i = 0; i < n; i++) {
            PV p;
            if (OK(i) && screen_pos(cx, &v[V(i)], pretransformed, &p))
                plot(cx, &p);
        }
        break;
    }
#undef V
#undef OK
    cx->rt->dirty++;
    free(cx);
}
