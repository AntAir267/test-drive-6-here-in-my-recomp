/* Direct3D 7 stand-in (DESIGN.md point 4): IDirect3D7, IDirect3DDevice7 and
   IDirect3DVertexBuffer7 with the DirectX 7 vtable layouts. No GPU yet: the device keeps a full
   record of the current state (D3DState in gfx.h: render and texture stage states, textures,
   transforms, viewport, material, lights) for the Vulkan renderer to come, logs every call,
   counts the draws per (API, vertex format, primitive type), and on dumped frames hands the
   transformed and lit vertices to the software rasterizer (swrast.c).

   We enumerate the devices a GeForce2 had under DX7 (RGB emulation, HAL, T&L HAL). The game
   (Game.exe 0x410ae0) keeps the ones with hardware rasterization; T&L + DrawPrimitives2Ex
   makes it use video-memory vertex buffers. What it calls (0x410110-0x4180a0): CreateDevice on
   the back buffer, EnumTextureFormats, Clear, Begin/EndScene, SetTransform (world, view, proj),
   SetViewport/GetViewport, SetMaterial, SetLight/LightEnable/GetLightEnable, SetRenderState,
   SetTextureStageState, SetTexture, and all its geometry through vertex buffers:
   CreateVertexBuffer (XYZ|NORMAL|DIFFUSE|SPECULAR[|TEX1] sources, XYZRHW|DIFFUSE|SPECULAR[|TEX1]
   destinations), Lock/Unlock, ProcessVertices (TRANSFORM[|CLIP][|LIGHT]), DrawPrimitiveVB and
   DrawIndexedPrimitiveVB.

   The game sets DDSCL_FPUPRESERVE, so device creation leaves its x87 control word alone. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "runtime.h"
#include "gfx.h"
#include "vk.h"

static double now_real_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

#define D3D_OK                  0u
#define D3DENUMRET_OK           1u
#define D3DENUMRET_CANCEL       0u
#define D3DERR_SURFACENOTINVIDMEM DDHR(747)
#define D3DERR_VERTEXBUFFEROPTIMIZED DDHR(2060)

#define FVF_XYZ                 0x002u
#define FVF_XYZRHW              0x004u
#define FVF_POSMASK             0x00eu
#define FVF_NORMAL              0x010u
#define FVF_RESERVED1           0x020u
#define FVF_DIFFUSE             0x040u
#define FVF_SPECULAR            0x080u
#define VOP_TRANSFORM           0x001u
#define VOP_CLIP                0x004u
#define VOP_EXTENTS             0x008u
#define VOP_LIGHT               0x400u
#define PV_DONOTCOPYDATA        0x001u
#define VBCAPS_OPTIMIZED        0x80000000u
#define CLEAR_TARGET            1u
#define CLEAR_ZBUFFER           2u
#define CLEAR_STENCIL           4u

enum { DEV_RGB, DEV_HAL, DEV_TNL };
static const int dev_guid[3] = {G_RGBDEVICE, G_HALDEVICE, G_TNLHALDEVICE};
static const char *const dev_desc[3] = {
    "Microsoft Direct3D RGB Software Emulation",
    "Microsoft Direct3D Hardware acceleration through Direct3D HAL",
    "Microsoft Direct3D Hardware Transform and Lighting acceleration capable device"};
/* The names the game's Video Options shows as INTERFACE: every device draws through Vulkan. */
static const char *const dev_name[3] = {"RGB Emulation", "Vulkan", "Vulkan T&L"};

/* ------------------------------------------------------------------ objects */
typedef struct D3D {
    struct D3D *next;
    DDraw *dd;
    int id, refs;
    uint32_t obj;
} D3D;

typedef struct Device {
    int id, refs, type;
    uint32_t obj;
    D3D *d3d;
    D3DState st;
    int in_scene;
    uint32_t clipstatus[8];
} Device;

typedef struct VBuf {
    int id, refs, locks;
    uint32_t obj, caps, fvf, n, stride, mem, memsize;
    int mem_kind;
    unsigned long dirty;
    float *clip;                  /* clip-space positions written by ProcessVertices */
    uint8_t *clip_ok;
} VBuf;

static D3D *d3ds;
static int next_id = 1;
static Device *drawn_dev;           /* the device of the last draw (d3d_last_state) */
static ComClass d3d_cls, dev_cls, vb_cls, dead_d3d_cls, dead_dev_cls, dead_vb_cls;

/* ------------------------------------------------------------------ names */
static const char *const rs_names[RS_MAX] = {
    [1] = "TEXTUREHANDLE", [2] = "ANTIALIAS", [3] = "TEXTUREADDRESS", [4] = "TEXTUREPERSPECTIVE",
    [5] = "WRAPU", [6] = "WRAPV", [7] = "ZENABLE", [8] = "FILLMODE", [9] = "SHADEMODE",
    [10] = "LINEPATTERN", [11] = "MONOENABLE", [12] = "ROP2", [13] = "PLANEMASK", [14] = "ZWRITEENABLE",
    [15] = "ALPHATESTENABLE", [16] = "LASTPIXEL", [17] = "TEXTUREMAG", [18] = "TEXTUREMIN",
    [19] = "SRCBLEND", [20] = "DESTBLEND", [21] = "TEXTUREMAPBLEND", [22] = "CULLMODE", [23] = "ZFUNC",
    [24] = "ALPHAREF", [25] = "ALPHAFUNC", [26] = "DITHERENABLE", [27] = "ALPHABLENDENABLE",
    [28] = "FOGENABLE", [29] = "SPECULARENABLE", [30] = "ZVISIBLE", [31] = "SUBPIXEL", [32] = "SUBPIXELX",
    [33] = "STIPPLEDALPHA", [34] = "FOGCOLOR", [35] = "FOGTABLEMODE", [36] = "FOGSTART", [37] = "FOGEND",
    [38] = "FOGDENSITY", [39] = "STIPPLEENABLE", [40] = "EDGEANTIALIAS", [41] = "COLORKEYENABLE",
    [43] = "BORDERCOLOR", [44] = "TEXTUREADDRESSU", [45] = "TEXTUREADDRESSV", [46] = "MIPMAPLODBIAS",
    [47] = "ZBIAS", [48] = "RANGEFOGENABLE", [49] = "ANISOTROPY", [50] = "FLUSHBATCH",
    [51] = "TRANSLUCENTSORTINDEPENDENT", [52] = "STENCILENABLE", [53] = "STENCILFAIL",
    [54] = "STENCILZFAIL", [55] = "STENCILPASS", [56] = "STENCILFUNC", [57] = "STENCILREF",
    [58] = "STENCILMASK", [59] = "STENCILWRITEMASK", [60] = "TEXTUREFACTOR",
    [128] = "WRAP0", [129] = "WRAP1", [130] = "WRAP2", [131] = "WRAP3", [132] = "WRAP4", [133] = "WRAP5",
    [134] = "WRAP6", [135] = "WRAP7", [136] = "CLIPPING", [137] = "LIGHTING", [138] = "EXTENTS",
    [139] = "AMBIENT", [140] = "FOGVERTEXMODE", [141] = "COLORVERTEX", [142] = "LOCALVIEWER",
    [143] = "NORMALIZENORMALS", [144] = "COLORKEYBLENDENABLE", [145] = "DIFFUSEMATERIALSOURCE",
    [146] = "SPECULARMATERIALSOURCE", [147] = "AMBIENTMATERIALSOURCE", [148] = "EMISSIVEMATERIALSOURCE",
    [151] = "VERTEXBLEND", [152] = "CLIPPLANEENABLE"};
static const char *const tss_names[TSS_MAX] = {
    [1] = "COLOROP", [2] = "COLORARG1", [3] = "COLORARG2", [4] = "ALPHAOP", [5] = "ALPHAARG1",
    [6] = "ALPHAARG2", [7] = "BUMPENVMAT00", [8] = "BUMPENVMAT01", [9] = "BUMPENVMAT10",
    [10] = "BUMPENVMAT11", [11] = "TEXCOORDINDEX", [12] = "ADDRESS", [13] = "ADDRESSU", [14] = "ADDRESSV",
    [15] = "BORDERCOLOR", [16] = "MAGFILTER", [17] = "MINFILTER", [18] = "MIPFILTER",
    [19] = "MIPMAPLODBIAS", [20] = "MAXMIPLEVEL", [21] = "MAXANISOTROPY", [22] = "BUMPENVLSCALE",
    [23] = "BUMPENVLOFFSET", [24] = "TEXTURETRANSFORMFLAGS"};
static const char *const blend_names[] = {"?", "ZERO", "ONE", "SRCCOLOR", "INVSRCCOLOR", "SRCALPHA",
    "INVSRCALPHA", "DESTALPHA", "INVDESTALPHA", "DESTCOLOR", "INVDESTCOLOR", "SRCALPHASAT",
    "BOTHSRCALPHA", "BOTHINVSRCALPHA"};
static const char *const cmp_names[] = {"?", "NEVER", "LESS", "EQUAL", "LESSEQUAL", "GREATER", "NOTEQUAL",
    "GREATEREQUAL", "ALWAYS"};
static const char *const top_names[] = {"?", "DISABLE", "SELECTARG1", "SELECTARG2", "MODULATE",
    "MODULATE2X", "MODULATE4X", "ADD", "ADDSIGNED", "ADDSIGNED2X", "SUBTRACT", "ADDSMOOTH",
    "BLENDDIFFUSEALPHA", "BLENDTEXTUREALPHA", "BLENDFACTORALPHA", "BLENDTEXTUREALPHAPM",
    "BLENDCURRENTALPHA", "PREMODULATE", "MODULATEALPHA_ADDCOLOR", "MODULATECOLOR_ADDALPHA",
    "MODULATEINVALPHA_ADDCOLOR", "MODULATEINVCOLOR_ADDALPHA", "BUMPENVMAP", "BUMPENVMAPLUMINANCE",
    "DOTPRODUCT3"};
static const char *const ta_names[] = {"DIFFUSE", "CURRENT", "TEXTURE", "TFACTOR", "SPECULAR"};
static const char *const prim_names[] = {"?", "POINTLIST", "LINELIST", "LINESTRIP", "TRIANGLELIST",
    "TRIANGLESTRIP", "TRIANGLEFAN"};
static const char *const fog_names[] = {"NONE", "EXP", "EXP2", "LINEAR"};
static const char *const cull_names[] = {"?", "NONE", "CW", "CCW"};
static const char *const shade_names[] = {"?", "FLAT", "GOURAUD", "PHONG"};
static const char *const fill_names[] = {"?", "POINT", "WIREFRAME", "SOLID"};
static const char *const mcs_names[] = {"MATERIAL", "COLOR1", "COLOR2"};
static const char *const addr_names[] = {"?", "WRAP", "MIRROR", "CLAMP", "BORDER"};
static const char *const magf_names[] = {"?", "POINT", "LINEAR", "FLATCUBIC", "GAUSSIANCUBIC", "ANISOTROPIC"};
static const char *const minf_names[] = {"?", "POINT", "LINEAR", "ANISOTROPIC"};
static const char *const mipf_names[] = {"?", "NONE", "POINT", "LINEAR"};
static const char *const xf_names[] = {"?", "WORLD", "VIEW", "PROJECTION", "WORLD1", "WORLD2", "WORLD3"};

#define NAME(t, v) ((v) < (uint32_t)COUNT(t) && t[v] ? t[v] : "?")

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static const char *rs_value(uint32_t s, uint32_t v, char *buf, size_t n)
{
    switch (s) {
    case 19: case 20: snprintf(buf, n, "%s", NAME(blend_names, v)); break;
    case 23: case 25: case 56: snprintf(buf, n, "%s", NAME(cmp_names, v)); break;
    case 22: snprintf(buf, n, "%s", NAME(cull_names, v)); break;
    case 9: snprintf(buf, n, "%s", NAME(shade_names, v)); break;
    case 8: snprintf(buf, n, "%s", NAME(fill_names, v)); break;
    case 35: case 140: snprintf(buf, n, "%s", NAME(fog_names, v)); break;
    case 7: snprintf(buf, n, "%s", v == 0 ? "FALSE" : v == 1 ? "TRUE" : v == 2 ? "USEW" : "?"); break;
    case 145: case 146: case 147: case 148: snprintf(buf, n, "%s", NAME(mcs_names, v)); break;
    case 36: case 37: case 38: case 46: snprintf(buf, n, "%g", u2f(v)); break;
    case 34: case 139: case 60: case 43: snprintf(buf, n, "%08x", v); break;
    case 24: case 47: case 57: snprintf(buf, n, "%u", v); break;
    default:
        if (v <= 1)
            snprintf(buf, n, "%s", v ? "TRUE" : "FALSE");
        else
            snprintf(buf, n, "0x%x", v);
    }
    return buf;
}

static const char *ta_str(uint32_t v, char *buf, size_t n)
{
    snprintf(buf, n, "%s%s%s", NAME(ta_names, v & 0xf), v & 0x10 ? "|COMPLEMENT" : "", v & 0x20 ? "|ALPHAREPLICATE" : "");
    return buf;
}

static const char *tss_value(uint32_t t, uint32_t v, char *buf, size_t n)
{
    switch (t) {
    case 1: case 4: snprintf(buf, n, "%s", NAME(top_names, v)); break;
    case 2: case 3: case 5: case 6: ta_str(v, buf, n); break;
    case 12: case 13: case 14: snprintf(buf, n, "%s", NAME(addr_names, v)); break;
    case 16: snprintf(buf, n, "%s", NAME(magf_names, v)); break;
    case 17: snprintf(buf, n, "%s", NAME(minf_names, v)); break;
    case 18: snprintf(buf, n, "%s", NAME(mipf_names, v)); break;
    case 7: case 8: case 9: case 10: case 19: case 22: case 23: snprintf(buf, n, "%g", u2f(v)); break;
    case 11: snprintf(buf, n, "%u%s", v & 0xffff, v >> 16 ? " +texgen" : ""); break;
    default: snprintf(buf, n, "0x%x", v);
    }
    return buf;
}

/* ------------------------------------------------------------------ vertex formats */
typedef struct {
    uint32_t fvf, stride;
    int pos;                      /* 0 none, 1 XYZ(+blend weights), 2 XYZRHW */
    int nblend;
    int normal, diffuse, specular;   /* byte offsets, -1 if absent */
    int ntex, tex[8], texdim[8];
} Layout;

static int fvf_layout(uint32_t fvf, Layout *l)
{
    memset(l, 0, sizeof *l);
    l->fvf = fvf;
    l->normal = l->diffuse = l->specular = -1;
    uint32_t p = fvf & FVF_POSMASK, off;
    if (p == FVF_XYZ) {
        l->pos = 1;
        off = 12;
    } else if (p == FVF_XYZRHW) {
        l->pos = 2;
        off = 16;
    } else if (p >= 6) {
        l->pos = 1;
        l->nblend = (int)(p - 4) / 2;
        off = 12 + 4 * (uint32_t)l->nblend;
    } else {
        return 0;
    }
    if (fvf & FVF_NORMAL) {
        if (l->pos == 2)
            return 0;
        l->normal = (int)off;
        off += 12;
    }
    if (fvf & FVF_RESERVED1)
        off += 4;
    if (fvf & FVF_DIFFUSE) {
        l->diffuse = (int)off;
        off += 4;
    }
    if (fvf & FVF_SPECULAR) {
        l->specular = (int)off;
        off += 4;
    }
    l->ntex = (int)((fvf >> 8) & 0xf);
    if (l->ntex > 8)
        return 0;
    for (int i = 0; i < l->ntex; i++) {
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        l->texdim[i] = f == 0 ? 2 : f == 1 ? 3 : f == 2 ? 4 : 1;
        l->tex[i] = (int)off;
        off += 4 * (uint32_t)l->texdim[i];
    }
    l->stride = off;
    return 1;
}

static const char *fvf_str(uint32_t fvf, char *buf, size_t n)
{
    Layout l;
    int ok = fvf_layout(fvf, &l);
    uint32_t p = fvf & FVF_POSMASK;
    size_t k = (size_t)snprintf(buf, n, "%s", p == FVF_XYZ ? "XYZ" : p == FVF_XYZRHW ? "XYZRHW" : p >= 6 ? "XYZB" : "?");
    if (p >= 6)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "%d", l.nblend);
    if (fvf & FVF_NORMAL)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "|NORMAL");
    if (fvf & FVF_RESERVED1)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "|RESERVED1");
    if (fvf & FVF_DIFFUSE)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "|DIFFUSE");
    if (fvf & FVF_SPECULAR)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "|SPECULAR");
    if (l.ntex)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "|TEX%d", l.ntex);
    for (int i = 0; i < l.ntex; i++)
        if (l.texdim[i] != 2)
            k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "(set %d: %dD)", i, l.texdim[i]);
    snprintf(buf + k, k < n ? n - k : 0, " (0x%x, %u bytes%s)", fvf, ok ? l.stride : 0, ok ? "" : ", INVALID");
    return buf;
}

/* Where each component of a vertex is: an interleaved buffer or D3D's strided arrays. */
typedef struct {
    Layout l;
    uint32_t pos, normal, diffuse, specular, tex[8];
    uint32_t spos, snormal, sdiffuse, sspecular, stex[8];
} Fetch;

static void fetch_interleaved(Fetch *f, const Layout *l, uint32_t base)
{
    memset(f, 0, sizeof *f);
    f->l = *l;
    f->pos = base;
    f->spos = l->stride;
    if (l->normal >= 0) { f->normal = base + (uint32_t)l->normal; f->snormal = l->stride; }
    if (l->diffuse >= 0) { f->diffuse = base + (uint32_t)l->diffuse; f->sdiffuse = l->stride; }
    if (l->specular >= 0) { f->specular = base + (uint32_t)l->specular; f->sspecular = l->stride; }
    for (int i = 0; i < l->ntex; i++) {
        f->tex[i] = base + (uint32_t)l->tex[i];
        f->stex[i] = l->stride;
    }
}

/* D3DDRAWPRIMITIVESTRIDEDDATA: {lpvData, dwStride} for position, normal, diffuse, specular, tex[8] */
static void fetch_strided(Fetch *f, const Layout *l, uint32_t sd)
{
    memset(f, 0, sizeof *f);
    f->l = *l;
    f->pos = R32(sd);
    f->spos = R32(sd + 4);
    if (l->normal >= 0) { f->normal = R32(sd + 8); f->snormal = R32(sd + 12); }
    if (l->diffuse >= 0) { f->diffuse = R32(sd + 16); f->sdiffuse = R32(sd + 20); }
    if (l->specular >= 0) { f->specular = R32(sd + 24); f->sspecular = R32(sd + 28); }
    for (int i = 0; i < l->ntex; i++) {
        f->tex[i] = R32(sd + 32 + 8 * (uint32_t)i);
        f->stex[i] = R32(sd + 36 + 8 * (uint32_t)i);
    }
}

typedef struct {
    float pos[4];                 /* x, y, z and rhw (1 for XYZ) */
    float normal[3];
    uint32_t diffuse, specular;
    float tex[8][4];
} RawVtx;

static void fetch_vertex(const Fetch *f, uint32_t i, RawVtx *v)
{
    uint32_t a = f->pos + i * f->spos;
    v->pos[0] = RF32(a);
    v->pos[1] = RF32(a + 4);
    v->pos[2] = RF32(a + 8);
    v->pos[3] = f->l.pos == 2 ? RF32(a + 12) : 1.0f;
    if (f->l.normal >= 0) {
        a = f->normal + i * f->snormal;
        v->normal[0] = RF32(a);
        v->normal[1] = RF32(a + 4);
        v->normal[2] = RF32(a + 8);
    } else {
        v->normal[0] = v->normal[1] = v->normal[2] = 0;
    }
    v->diffuse = f->l.diffuse >= 0 ? R32(f->diffuse + i * f->sdiffuse) : 0xffffffffu;
    v->specular = f->l.specular >= 0 ? R32(f->specular + i * f->sspecular) : 0xff000000u;
    for (int t = 0; t < f->l.ntex; t++) {
        a = f->tex[t] + i * f->stex[t];
        for (int k = 0; k < 4; k++)
            v->tex[t][k] = k < f->l.texdim[t] ? RF32(a + 4 * (uint32_t)k) : 0.0f;
    }
}

static const char *vertex_str(const Fetch *f, uint32_t i, char *buf, size_t n)
{
    RawVtx v;
    fetch_vertex(f, i, &v);
    size_t k;
    if (f->l.pos == 2)
        k = (size_t)snprintf(buf, n, "xyz=(%.2f,%.2f,%.4f) rhw=%g", v.pos[0], v.pos[1], v.pos[2], v.pos[3]);
    else
        k = (size_t)snprintf(buf, n, "xyz=(%.3f,%.3f,%.3f)", v.pos[0], v.pos[1], v.pos[2]);
    if (f->l.normal >= 0)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, " n=(%.3f,%.3f,%.3f)", v.normal[0], v.normal[1], v.normal[2]);
    if (f->l.diffuse >= 0)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, " diff=%08x", v.diffuse);
    if (f->l.specular >= 0)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, " spec=%08x", v.specular);
    for (int t = 0; t < f->l.ntex; t++)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, " t%d=(%.4f,%.4f)", t, v.tex[t][0], v.tex[t][1]);
    return buf;
}

/* ------------------------------------------------------------------ draw statistics */
enum { API_DP, API_DIP, API_DPS, API_DIPS, API_DPVB, API_DIPVB, API_PV, API_PVS };
static const char *const api_names[] = {"DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveStrided",
    "DrawIndexedPrimitiveStrided", "DrawPrimitiveVB", "DrawIndexedPrimitiveVB", "ProcessVertices",
    "ProcessVerticesStrided"};

typedef struct {
    int api;
    uint32_t fvf, prim;           /* for ProcessVertices: prim = destination FVF */
    unsigned long calls, verts, idx, prims;
} Stat;
static Stat stats[512];
static int nstats;
static unsigned long stats_frames;

static void count(int api, uint32_t fvf, uint32_t prim, uint32_t verts, uint32_t idx, uint32_t prims)
{
    Stat *s = NULL;
    for (int i = 0; i < nstats; i++)
        if (stats[i].api == api && stats[i].fvf == fvf && stats[i].prim == prim) {
            s = &stats[i];
            break;
        }
    if (!s) {
        if (nstats == COUNT(stats))
            return;
        s = &stats[nstats++];
        s->api = api;
        s->fvf = fvf;
        s->prim = prim;
    }
    s->calls++;
    s->verts += verts;
    s->idx += idx;
    s->prims += prims;
}

static void print_stats(const char *why)
{
    char f1[160], f2[160];
    logmsg(LOG_ALWAYS, "D3D draw summary (%s, %lu frames): %d kinds", why, stats_frames, nstats);
    for (int i = 0; i < nstats; i++) {
        Stat *s = &stats[i];
        if (s->api >= API_PV)
            logmsg(LOG_ALWAYS, "  %-27s %s -> %s: %lu calls, %lu vertices", api_names[s->api],
                   fvf_str(s->fvf, f1, sizeof f1), fvf_str(s->prim, f2, sizeof f2), s->calls, s->verts);
        else
            logmsg(LOG_ALWAYS, "  %-27s %-13s %s: %lu calls, %lu vertices, %lu indices, %lu primitives",
                   api_names[s->api], NAME(prim_names, s->prim), fvf_str(s->fvf, f1, sizeof f1), s->calls,
                   s->verts, s->idx, s->prims);
    }
}

/* Runs at exit without the lock: the process may be ending inside another thread's call. */
static void stats_at_exit(void)
{
    if (nstats)
        print_stats("exit");
}

void d3d_frame_stats(unsigned long frame)
{
    stats_frames = frame;
    if (frame % 500 == 0 && nstats)
        print_stats("every 500 frames");
}

static uint32_t prim_count(uint32_t prim, uint32_t n)
{
    switch (prim) {
    case PT_POINTLIST: return n;
    case PT_LINELIST: return n / 2;
    case PT_LINESTRIP: return n ? n - 1 : 0;
    case PT_TRIANGLELIST: return n / 3;
    case PT_TRIANGLESTRIP: case PT_TRIANGLEFAN: return n > 2 ? n - 2 : 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ matrices and lighting */
static void mat_mul(Mat4 *r, const Mat4 *a, const Mat4 *b)
{
    Mat4 t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] +
                        a->m[i][3] * b->m[3][j];
    *r = t;
}

static void mat_identity(Mat4 *m)
{
    memset(m, 0, sizeof *m);
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1;
}

static void xform4(const Mat4 *m, const float *p, float *o)   /* row vector (x, y, z, 1) * m */
{
    for (int j = 0; j < 4; j++)
        o[j] = p[0] * m->m[0][j] + p[1] * m->m[1][j] + p[2] * m->m[2][j] + m->m[3][j];
}

static void normalize3(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

static void color4(uint32_t c, float *o)
{
    o[0] = (float)((c >> 16) & 0xff) / 255.0f;
    o[1] = (float)((c >> 8) & 0xff) / 255.0f;
    o[2] = (float)(c & 0xff) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

static uint32_t pack4(const float *c)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        uint32_t b = (uint32_t)(v * 255.0f + 0.5f);
        r |= b << (i == 3 ? 24 : 16 - 8 * i);
    }
    return r;
}

typedef struct {
    int type;
    float pos[3], dir[3];         /* camera space; dir points from the light */
    float diffuse[4], specular[4], ambient[4];
    float range, falloff, att0, att1, att2, cos_theta, cos_phi;
} CamLight;

/* Everything a draw needs to transform and light its vertices. */
typedef struct {
    Mat4 wv, wvp;
    float nm[3][3];               /* normal matrix: inverse transpose of wv's 3x3 */
    int light, colorvertex, specular, localviewer, normalize;
    int src_diffuse, src_specular, src_ambient, src_emissive;
    float ambient[4], mat[17];
    CamLight lights[LIGHTS_MAX];
    int nlights;
    int fogmode, rangefog;        /* vertex fog (0 = none) */
    float fogstart, fogend, fogdensity;
} Xform;

static void xform_setup(const D3DState *st, Xform *x, int light)
{
    mat_mul(&x->wv, &st->xf[XF_WORLD], &st->xf[XF_VIEW]);
    mat_mul(&x->wvp, &x->wv, &st->xf[XF_PROJ]);
    const float (*a)[4] = x->wv.m;
    float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    float id = fabsf(det) > 1e-20f ? 1.0f / det : 0.0f;
    /* inverse transpose = cofactor matrix / det */
    x->nm[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * id;
    x->nm[0][1] = -(a[1][0] * a[2][2] - a[1][2] * a[2][0]) * id;
    x->nm[0][2] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * id;
    x->nm[1][0] = -(a[0][1] * a[2][2] - a[0][2] * a[2][1]) * id;
    x->nm[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * id;
    x->nm[1][2] = -(a[0][0] * a[2][1] - a[0][1] * a[2][0]) * id;
    x->nm[2][0] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * id;
    x->nm[2][1] = -(a[0][0] * a[1][2] - a[0][2] * a[1][0]) * id;
    x->nm[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * id;
    x->light = light;
    x->colorvertex = st->rs[RS_COLORVERTEX] != 0;
    x->specular = st->rs[RS_SPECULARENABLE] != 0;
    x->localviewer = st->rs[RS_LOCALVIEWER] != 0;
    x->normalize = st->rs[RS_NORMALIZENORMALS] != 0;
    x->src_diffuse = (int)st->rs[RS_DIFFUSEMATERIALSOURCE];
    x->src_specular = (int)st->rs[RS_SPECULARMATERIALSOURCE];
    x->src_ambient = (int)st->rs[RS_AMBIENTMATERIALSOURCE];
    x->src_emissive = (int)st->rs[RS_EMISSIVEMATERIALSOURCE];
    color4(st->rs[RS_AMBIENT], x->ambient);
    memcpy(x->mat, st->material, sizeof x->mat);
    x->nlights = 0;
    if (light) {
        const Mat4 *v = &st->xf[XF_VIEW];
        for (int i = 0; i < LIGHTS_MAX && x->nlights < 8; i++) {
            const Light *l = &st->lights[i];
            if (!l->set || !l->enabled)
                continue;
            CamLight *c = &x->lights[x->nlights++];
            c->type = (int)l->type;
            float p[4];
            xform4(v, l->pos, p);
            memcpy(c->pos, p, sizeof c->pos);
            for (int j = 0; j < 3; j++)
                c->dir[j] = l->dir[0] * v->m[0][j] + l->dir[1] * v->m[1][j] + l->dir[2] * v->m[2][j];
            normalize3(c->dir);
            memcpy(c->diffuse, l->diffuse, sizeof c->diffuse);
            memcpy(c->specular, l->specular, sizeof c->specular);
            memcpy(c->ambient, l->ambient, sizeof c->ambient);
            c->range = l->range;
            c->falloff = l->falloff;
            c->att0 = l->att0;
            c->att1 = l->att1;
            c->att2 = l->att2;
            c->cos_theta = cosf(l->theta / 2);
            c->cos_phi = cosf(l->phi / 2);
        }
    }
    x->fogmode = 0;
    if (st->rs[RS_FOGENABLE] && st->rs[RS_FOGTABLEMODE] == 0) {
        x->fogmode = (int)st->rs[RS_FOGVERTEXMODE];
        x->rangefog = st->rs[RS_RANGEFOGENABLE] != 0;
        x->fogstart = u2f(st->rs[RS_FOGSTART]);
        x->fogend = u2f(st->rs[RS_FOGEND]);
        x->fogdensity = u2f(st->rs[RS_FOGDENSITY]);
    }
}

static const float *mat_source(const Xform *x, int src, const float *vd, const float *vs, int has_d, int has_s,
                               const float *mat)
{
    if (x->colorvertex) {
        if (src == 1 && has_d)
            return vd;
        if (src == 2 && has_s)
            return vs;
    }
    return mat;
}

/* D3D7 fixed-function vertex processing: clip-space position, lit colours, vertex fog. */
static void tnl_vertex(const Xform *x, const Layout *l, const RawVtx *v, SwVertex *o, float *eye)
{
    float pc[4], e[4];
    xform4(&x->wvp, v->pos, pc);
    o->x = pc[0];
    o->y = pc[1];
    o->z = pc[2];
    o->w = pc[3];
    /* eye space: for lighting, fog (non-range fog needs only z) and the caller */
    if (x->light || eye || (x->fogmode && x->rangefog))
        xform4(&x->wv, v->pos, e);
    else if (x->fogmode)
        e[2] = v->pos[0] * x->wv.m[0][2] + v->pos[1] * x->wv.m[1][2] + v->pos[2] * x->wv.m[2][2] + x->wv.m[3][2];
    if (eye)
        memcpy(eye, e, 3 * sizeof(float));
    float vd[4], vs[4];
    color4(v->diffuse, vd);
    color4(v->specular, vs);
    o->packed = 0;
    if (!x->light) {
        memcpy(o->diffuse, vd, sizeof vd);
        memcpy(o->specular, vs, sizeof vs);
        /* the colours as they came, for the GPU (A8R8G8B8 -> R8G8B8A8 bytes) */
        uint32_t dc = v->diffuse, sc = v->specular;
        o->rgba[0] = (dc & 0xff00ff00u) | ((dc >> 16) & 0xffu) | ((dc & 0xffu) << 16);
        o->rgba[1] = (sc & 0xff00ff00u) | ((sc >> 16) & 0xffu) | ((sc & 0xffu) << 16);
        o->packed = 1;
    } else {
        int hd = l->diffuse >= 0, hs = l->specular >= 0;
        const float *md = mat_source(x, x->src_diffuse, vd, vs, hd, hs, &x->mat[0]);
        const float *ma = mat_source(x, x->src_ambient, vd, vs, hd, hs, &x->mat[4]);
        const float *ms = mat_source(x, x->src_specular, vd, vs, hd, hs, &x->mat[8]);
        const float *me = mat_source(x, x->src_emissive, vd, vs, hd, hs, &x->mat[12]);
        float n[3] = {0, 0, 0};
        for (int j = 0; j < 3; j++)
            n[j] = v->normal[0] * x->nm[0][j] + v->normal[1] * x->nm[1][j] + v->normal[2] * x->nm[2][j];
        if (x->normalize)
            normalize3(n);
        float dif[3] = {0, 0, 0}, amb[3], spe[3] = {0, 0, 0};
        for (int j = 0; j < 3; j++)
            amb[j] = x->ambient[j];
        float view[3] = {0, 0, -1};
        if (x->localviewer) {
            view[0] = -e[0];
            view[1] = -e[1];
            view[2] = -e[2];
            normalize3(view);
        }
        for (int i = 0; i < x->nlights; i++) {
            const CamLight *c = &x->lights[i];
            float L[3], att = 1, spot = 1;
            if (c->type == 3) {
                L[0] = -c->dir[0];
                L[1] = -c->dir[1];
                L[2] = -c->dir[2];
            } else {
                L[0] = c->pos[0] - e[0];
                L[1] = c->pos[1] - e[1];
                L[2] = c->pos[2] - e[2];
                float d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
                if (c->range > 0 && d > c->range)
                    continue;
                float den = c->att0 + c->att1 * d + c->att2 * d * d;
                att = den > 0 ? 1.0f / den : 1.0f;
                normalize3(L);
                if (c->type == 2) {
                    float rho = -(L[0] * c->dir[0] + L[1] * c->dir[1] + L[2] * c->dir[2]);
                    if (rho <= c->cos_phi)
                        spot = 0;
                    else if (rho < c->cos_theta && c->cos_theta > c->cos_phi)
                        spot = powf((rho - c->cos_phi) / (c->cos_theta - c->cos_phi), c->falloff > 0 ? c->falloff : 1);
                }
            }
            float k = att * spot;
            for (int j = 0; j < 3; j++)
                amb[j] += k * c->ambient[j];
            float nl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
            if (nl <= 0)
                continue;
            for (int j = 0; j < 3; j++)
                dif[j] += k * nl * c->diffuse[j];
            if (x->specular && x->mat[16] > 0) {
                float h[3] = {L[0] + view[0], L[1] + view[1], L[2] + view[2]};
                normalize3(h);
                float nh = n[0] * h[0] + n[1] * h[1] + n[2] * h[2];
                if (nh > 0) {
                    float p = k * powf(nh, x->mat[16]);
                    for (int j = 0; j < 3; j++)
                        spe[j] += p * c->specular[j];
                }
            }
        }
        for (int j = 0; j < 3; j++) {
            o->diffuse[j] = me[j] + ma[j] * amb[j] + md[j] * dif[j];
            o->specular[j] = x->specular ? ms[j] * spe[j] : 0;
        }
        o->diffuse[3] = md[3];
        o->specular[3] = ms[3];
        for (int j = 0; j < 4; j++) {
            o->diffuse[j] = o->diffuse[j] < 0 ? 0 : o->diffuse[j] > 1 ? 1 : o->diffuse[j];
            o->specular[j] = o->specular[j] < 0 ? 0 : o->specular[j] > 1 ? 1 : o->specular[j];
        }
    }
    if (x->fogmode) {
        float d = x->rangefog ? sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]) : fabsf(e[2]), f;
        if (x->fogmode == 3)
            f = x->fogend != x->fogstart ? (x->fogend - d) / (x->fogend - x->fogstart) : 1;
        else if (x->fogmode == 1)
            f = expf(-x->fogdensity * d);
        else
            f = expf(-(x->fogdensity * d) * (x->fogdensity * d));
        o->specular[3] = f < 0 ? 0 : f > 1 ? 1 : f;
        if (o->packed)
            o->rgba[1] = (o->rgba[1] & 0x00ffffffu) | (uint32_t)(o->specular[3] * 255.0f + 0.5f) << 24;
    }
    for (int t = 0; t < 8; t++) {
        o->tc[t][0] = t < l->ntex ? v->tex[t][0] : 0;
        o->tc[t][1] = t < l->ntex ? v->tex[t][1] : 0;
    }
}

/* Big draws (detailed fan cars: tens of thousands of lit vertices) spread their vertices over a
   few worker threads; each vertex is processed on its own, so the result is the same. */
#define TNL_MAXW 7
#define TNL_MIN 1024                    /* vertices before a draw is worth splitting */
typedef struct { const Fetch *f; const Xform *x; SwVertex *sv; uint32_t start, from, to; } TnlJob;
static int tnl_nw = -1;
static pthread_mutex_t tnl_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tnl_go = PTHREAD_COND_INITIALIZER, tnl_done = PTHREAD_COND_INITIALIZER;
static TnlJob tnl_jobs[TNL_MAXW];
static unsigned tnl_gen, tnl_left;

static void tnl_range(const TnlJob *j)
{
    for (uint32_t i = j->from; i < j->to; i++) {
        RawVtx v;
        fetch_vertex(j->f, j->start + i, &v);
        tnl_vertex(j->x, &j->f->l, &v, &j->sv[i], NULL);
    }
}

static void *tnl_worker(void *arg)
{
    int k = (int)(intptr_t)arg;
    unsigned seen = 0;
    pthread_mutex_lock(&tnl_mx);
    for (;;) {
        while (tnl_gen == seen)
            pthread_cond_wait(&tnl_go, &tnl_mx);
        seen = tnl_gen;
        TnlJob j = tnl_jobs[k];
        pthread_mutex_unlock(&tnl_mx);
        tnl_range(&j);
        pthread_mutex_lock(&tnl_mx);
        if (--tnl_left == 0)
            pthread_cond_signal(&tnl_done);
    }
    return NULL;
}

static void tnl_parallel(const Fetch *f, const Xform *x, SwVertex *sv, uint32_t start, uint32_t n)
{
    if (tnl_nw < 0) {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        tnl_nw = cores > 1 ? (int)(cores - 1 < TNL_MAXW ? cores - 1 : TNL_MAXW) : 0;
        for (int k = 0; k < tnl_nw; k++) {
            pthread_t t;
            if (pthread_create(&t, NULL, tnl_worker, (void *)(intptr_t)k)) {
                tnl_nw = k;
                break;
            }
            pthread_detach(t);
        }
    }
    TnlJob mine = {f, x, sv, start, 0, n};
    if (tnl_nw == 0 || n < TNL_MIN) {
        tnl_range(&mine);
        return;
    }
    uint32_t parts = (uint32_t)tnl_nw + 1, step = (n + parts - 1) / parts;
    pthread_mutex_lock(&tnl_mx);
    for (int k = 0; k < tnl_nw; k++) {
        uint32_t a = step * (uint32_t)k, b = a + step < n ? a + step : n;
        tnl_jobs[k] = (TnlJob){f, x, sv, start, a < n ? a : n, b};
    }
    tnl_left = (unsigned)tnl_nw;
    tnl_gen++;
    pthread_cond_broadcast(&tnl_go);
    pthread_mutex_unlock(&tnl_mx);
    mine.from = step * (uint32_t)tnl_nw < n ? step * (uint32_t)tnl_nw : n;
    tnl_range(&mine);
    pthread_mutex_lock(&tnl_mx);
    while (tnl_left)
        pthread_cond_wait(&tnl_done, &tnl_mx);
    pthread_mutex_unlock(&tnl_mx);
}

/* The GPU's copy of a draw's transform and lighting state (TnlConst, shaders/tnl.vert) */
static void tnl_const(const Xform *x, TnlConst *k)
{
    memset(k, 0, sizeof *k);
    memcpy(k->wvp, x->wvp.m, sizeof k->wvp);
    memcpy(k->wv, x->wv.m, sizeof k->wv);
    for (int i = 0; i < 3; i++)
        memcpy(k->nm[i], x->nm[i], 3 * sizeof(float));
    for (int i = 0; i < 4; i++)
        memcpy(k->mat[i], &x->mat[4 * i], 4 * sizeof(float));
    k->power_fog[0] = x->mat[16];
    k->power_fog[1] = x->fogstart;
    k->power_fog[2] = x->fogend;
    k->power_fog[3] = x->fogdensity;
    memcpy(k->ambient, x->ambient, sizeof k->ambient);
    k->flags[0] = (x->light ? 1u : 0) | (x->colorvertex ? 2u : 0) | (x->specular ? 4u : 0) |
                  (x->localviewer ? 8u : 0) | (x->normalize ? 16u : 0) | (x->rangefog ? 32u : 0);
    k->flags[1] = (uint32_t)x->fogmode;
    k->flags[2] = x->light ? (uint32_t)x->nlights : 0;
    k->flags[3] = ((uint32_t)x->src_diffuse & 15) | ((uint32_t)x->src_specular & 15) << 4 |
                  ((uint32_t)x->src_ambient & 15) << 8 | ((uint32_t)x->src_emissive & 15) << 12;
    for (int i = 0; i < x->nlights && x->light; i++) {
        const CamLight *c = &x->lights[i];
        TnlLight *t = &k->lights[i];
        t->type_range[0] = (float)c->type;
        t->type_range[1] = c->range;
        t->type_range[2] = c->falloff;
        memcpy(t->pos, c->pos, 3 * sizeof(float));
        memcpy(t->dir, c->dir, 3 * sizeof(float));
        t->att[0] = c->att0;
        t->att[1] = c->att1;
        t->att[2] = c->att2;
        t->spot[0] = c->cos_theta;
        t->spot[1] = c->cos_phi;
        memcpy(t->diffuse, c->diffuse, sizeof t->diffuse);
        memcpy(t->specular, c->specular, sizeof t->specular);
        memcpy(t->ambient, c->ambient, sizeof t->ambient);
    }
}

/* the vertices are the game's usual layout, interleaved in one buffer: the GPU can take them as
   they are */
static int gpu_layout(const Fetch *f)
{
    return f->l.fvf == 0x1d2 && f->spos == 40 && f->normal == f->pos + 12 && f->diffuse == f->pos + 24 &&
           f->specular == f->pos + 28 && f->tex[0] == f->pos + 32 && f->l.texdim[0] == 2;
}

static void pretransformed_vertex(const Layout *l, const RawVtx *v, SwVertex *o)
{
    o->packed = 0;
    o->x = v->pos[0];
    o->y = v->pos[1];
    o->z = v->pos[2];
    o->w = v->pos[3] > 0 ? 1.0f / v->pos[3] : 1.0f;
    color4(v->diffuse, o->diffuse);
    color4(v->specular, o->specular);
    for (int t = 0; t < 8; t++) {
        o->tc[t][0] = t < l->ntex ? v->tex[t][0] : 0;
        o->tc[t][1] = t < l->ntex ? v->tex[t][1] : 0;
    }
}

/* ------------------------------------------------------------------ draws */
void (*d3d_draw_tap)(const D3DState *st, int prim, const void *raw, int n, const uint16_t *ix, int ni);

/* A draw of the game's vertex layout (0x1d2, 40 bytes) with this state, transformed and lit on the
   GPU as the game's own draws are (drawdist.c's far objects); 0 if the GPU path cannot take it */
int d3d_tnl_draw(const D3DState *st, int prim, const void *raw, int n, const uint16_t *ix, int ni)
{
    if (!vk_on() || n <= 0 || !vk_tnl_ok(st, n))
        return 0;
    Xform x;
    TnlConst k;
    xform_setup(st, &x, st->rs[RS_LIGHTING] != 0);
    tnl_const(&x, &k);
    tnl_world(k.world, &st->xf[XF_WORLD]);
    vk_draw_tnl(st, prim, raw, n, ix, ni, &k);
    return 1;
}

/* The same, for many copies (vk_draw_tnl_inst; st's world matrix the identity) */
int d3d_tnl_draw_inst(const D3DState *st, int prim, const void *raw, int n, const uint16_t *ix, int ni,
                      const float *recs, int ncopies)
{
    if (!vk_on() || n <= 0 || ncopies <= 0 || !vk_tnl_ok(st, n))
        return 0;
    Xform x;
    TnlConst k;
    xform_setup(st, &x, st->rs[RS_LIGHTING] != 0);
    tnl_const(&x, &k);
    tnl_world(k.world, &st->xf[XF_WORLD]);
    vk_draw_tnl_inst(st, prim, raw, n, ix, ni, &k, recs, ncopies);
    return 1;
}

#define DEV(o) ((Device *)com_host(o))
#define VB(o) ((VBuf *)com_host(o))

/* Log, count and (on dumped frames) rasterize one draw. Vertices start..start+n-1 of f; indices
   (program address, 16-bit) refer to them relative to start. */
static uint32_t draw(Device *d, int api, uint32_t prim, const Fetch *f, uint32_t start, uint32_t n,
                     uint32_t idx, uint32_t nidx, VBuf *vb)
{
    char fs[160], vs[320], tn[96];
    if (prim < PT_POINTLIST || prim > PT_TRIANGLEFAN)
        return D3DERR_INVALIDPRIMITIVETYPE;
    uint32_t prims = prim_count(prim, idx ? nidx : n);
    count(api, f->l.fvf, prim, n, idx ? nidx : 0, prims);
    if (LOGON(LOG_DRAW)) {
        uint32_t first = idx && nidx ? R16(idx) : 0;
        char chain[256] = "";
        {   /* the game functions on the way here */
            void *bt[64];
            int nb = backtrace(bt, 64), k = 0;
            for (int i = 0; i < nb && k < 200; i++) {
                Dl_info di;
                if (dladdr(bt[i], &di) && di.dli_sname && di.dli_sname[0] == 'F' && di.dli_sname[1] == '_')
                    k += snprintf(chain + k, sizeof chain - (size_t)k, " %s", di.dli_sname + 4);
            }
        }
        const uint32_t *rs = d->st.rs, *t0 = d->st.tss[0];
        LOG(LOG_DRAW, "%s %s %s: %u vertices from %u, %u indices, %u primitives, tex0 %s | v0 %s | blend %u %u/%u "
            "atest %u %u/%u tf %08x | st0 c %u %u,%u a %u %u,%u | in%s", api_names[api],
            NAME(prim_names, prim), fvf_str(f->l.fvf, fs, sizeof fs), n, start, idx ? nidx : 0, prims,
            d->st.tex[0] ? surface_name(d->st.tex[0], tn, sizeof tn) : "none",
            n ? vertex_str(f, start + first, vs, sizeof vs) : "-", rs[RS_ALPHABLENDENABLE], rs[RS_SRCBLEND],
            rs[RS_DESTBLEND], rs[RS_ALPHATESTENABLE], rs[RS_ALPHAFUNC], rs[RS_ALPHAREF], rs[RS_TEXTUREFACTOR],
            t0[TSS_COLOROP], t0[TSS_COLORARG1], t0[TSS_COLORARG2], t0[TSS_ALPHAOP], t0[TSS_ALPHAARG1],
            t0[TSS_ALPHAARG2], chain);
    }
    int gpu = vk_on();
    drawn_dev = d;
    if (gpu && vk_puff && f->l.pos != 2) {
        /* smoke.c: a puff's sprite (camera-facing quads of 4 vertices), taken for its volume */
        for (uint32_t q = 0; q + 4 <= n; q += 4) {
            float p[4][3], uv[4][2];
            uint32_t dif = 0xffffffffu;
            for (int i = 0; i < 4; i++) {
                RawVtx v;
                fetch_vertex(f, start + q + (uint32_t)i, &v);
                for (int j = 0; j < 3; j++)
                    p[i][j] = v.pos[j];
                uv[i][0] = v.tex[0][0];
                uv[i][1] = v.tex[0][1];
                if (!i)
                    dif = v.diffuse;
            }
            vk_smoke_take(&d->st, (const float (*)[3])p, 4, dif, (const float (*)[2])uv);
        }
        if (vk_puff->kind != VK_PUFF_SPRAY)  /* (a splash's droplets are drawn too: its mist is the volume) */
            return D3D_OK;
    }
    if ((!gpu && !swrast_active()) || !n || !prims)
        return D3D_OK;
    double t0 = gpu ? now_real_ms() : 0;
    if (gpu && f->l.pos != 2 && gpu_layout(f) && vk_tnl_ok(&d->st, (int)n)) {
        /* transform and lighting on the GPU: the raw vertices and this draw's constants */
        Xform x;
        TnlConst k;
        xform_setup(&d->st, &x, d->st.rs[RS_LIGHTING] != 0);
        tnl_const(&x, &k);
        tnl_world(k.world, &d->st.xf[XF_WORLD]);
        uint16_t *ix = nidx ? malloc(nidx * sizeof *ix) : NULL;
        if (nidx && !ix)
            fatal("out of memory");
        for (uint32_t i = 0; i < nidx; i++)
            ix[i] = (uint16_t)R16(idx + 2 * i);
        vk_tnl_ms += now_real_ms() - t0;
        if (d3d_draw_tap)                       /* drawdist.c: an object's mesh kept for its far copies */
            d3d_draw_tap(&d->st, (int)prim, gptr(f->pos + start * 40), (int)n, ix, (int)nidx);
        vk_draw_tnl(&d->st, (int)prim, gptr(f->pos + start * 40), (int)n, ix, (int)nidx, &k);
        free(ix);
        return D3D_OK;
    }
    SwVertex *sv = malloc(n * sizeof *sv);
    uint16_t *ix = nidx ? malloc(nidx * sizeof *ix) : NULL;
    if (!sv || (nidx && !ix))
        fatal("out of memory");
    int pre = f->l.pos == 2;
    Xform *x = NULL;
    if (!pre) {
        x = malloc(sizeof *x);
        if (!x)
            fatal("out of memory");
        xform_setup(&d->st, x, d->st.rs[RS_LIGHTING] != 0);
    }
    /* processed vertices with clip-space positions kept: clip them like untransformed ones */
    int useclip = pre && vb && vb->clip && d->st.rs[RS_CLIPPING];
    for (uint32_t i = 0; i < n && useclip; i++)
        if (!vb->clip_ok[start + i])
            useclip = 0;
    if (!pre)
        tnl_parallel(f, x, sv, start, n);
    for (uint32_t i = 0; i < n && pre; i++) {
        RawVtx v;
        fetch_vertex(f, start + i, &v);
        {
            pretransformed_vertex(&f->l, &v, &sv[i]);
            if (useclip) {
                const float *c = &vb->clip[4 * (start + i)];
                sv[i].x = c[0];
                sv[i].y = c[1];
                sv[i].z = c[2];
                sv[i].w = c[3];
            }
        }
    }
    for (uint32_t i = 0; i < nidx; i++)
        ix[i] = (uint16_t)R16(idx + 2 * i);
    if (gpu) {
        vk_tnl_ms += now_real_ms() - t0;
        vk_draw(&d->st, (int)prim, sv, (int)n, ix, (int)nidx, pre && !useclip);
    }
    else
        swrast_draw(&d->st, (int)prim, sv, (int)n, ix, (int)nidx, pre && !useclip);
    free(x);
    free(sv);
    free(ix);
    return D3D_OK;
}

/* ------------------------------------------------------------------ device description */
static void fill_devdesc(uint32_t p, int type)
{
    uint8_t b[SZ_D3DDEVICEDESC7];
    memset(b, 0, sizeof b);
#define DW(off, v) do { uint32_t _v = (v); memcpy(b + (off), &_v, 4); } while (0)
#define FL(off, v) do { float _v = (v); memcpy(b + (off), &_v, 4); } while (0)
#define WD(off, v) do { uint16_t _v = (v); memcpy(b + (off), &_v, 2); } while (0)
    /* FLOATTLVERTEX | EXECUTESYSTEMMEMORY | EXECUTEVIDEOMEMORY | TLVERTEXSYSTEMMEMORY |
       TLVERTEXVIDEOMEMORY | TEXTUREVIDEOMEMORY | DRAWPRIMTLVERTEX | CANRENDERAFTERFLIP |
       TEXTURENONLOCALVIDMEM | DRAWPRIMITIVES2 | DRAWPRIMITIVES2EX | HWTRANSFORMANDLIGHT |
       CANBLTSYSTONONLOCAL | HWRASTERIZATION */
    uint32_t devcaps = 0x1u | 0x10u | 0x20u | 0x40u | 0x80u | 0x200u | 0x400u | 0x800u | 0x1000u |
                       0x2000u | 0x8000u | 0x10000u | 0x20000u | 0x80000u;
    if (type == DEV_HAL)
        devcaps &= ~0x10000u;
    if (type == DEV_RGB)
        devcaps = (devcaps & ~(0x10000u | 0x80000u | 0x200u | 0x80u | 0x20u | 0x1000u | 0x20000u)) | 0x100u;
    DW(0x00, devcaps);
    for (int k = 0; k < 2; k++) {
        uint32_t o = k ? 0x3c : 0x04;
        DW(o + 0x00, 56);
        DW(o + 0x04, 0x2u | 0x10u | 0x20u | 0x40u);        /* MASKZ, CULLNONE/CW/CCW */
        /* DITHER | ZTEST | SUBPIXEL | FOGVERTEX | FOGTABLE | MIPMAPLODBIAS | ZBIAS | FOGRANGE |
           ANISOTROPY | WFOG | ZFOG */
        DW(o + 0x08, 0x1u | 0x10u | 0x20u | 0x80u | 0x100u | 0x2000u | 0x4000u | 0x10000u | 0x20000u |
                     0x100000u | 0x200000u);
        DW(o + 0x0c, 0xff);                                  /* all Z compares */
        DW(o + 0x10, 0x1fff);                                /* source blend factors */
        DW(o + 0x14, 0x03ff);                                /* destination blend factors */
        DW(o + 0x18, 0xff);                                  /* alpha compares */
        /* COLORFLATRGB | COLORGOURAUDRGB | SPECULARFLATRGB | SPECULARGOURAUDRGB | ALPHAFLATBLEND |
           ALPHAGOURAUDBLEND | FOGFLAT | FOGGOURAUD */
        DW(o + 0x1c, 0x2u | 0x8u | 0x80u | 0x200u | 0x1000u | 0x4000u | 0x40000u | 0x80000u);
        /* PERSPECTIVE | POW2 | ALPHA | TRANSPARENCY | TEXREPEATNOTSCALEDBYSIZE | PROJECTED | CUBEMAP */
        DW(o + 0x20, 0x1u | 0x2u | 0x4u | 0x8u | 0x40u | 0x400u | 0x800u);
        DW(o + 0x24, 0x1u | 0x2u | 0x4u | 0x8u | 0x10u | 0x20u | 0x100u | 0x200u | 0x400u | 0x10000u |
                     0x20000u | 0x1000000u | 0x2000000u | 0x4000000u);   /* filters */
        DW(o + 0x28, 0xff);                                  /* legacy texture blends */
        DW(o + 0x2c, 0x1f);                                  /* WRAP MIRROR CLAMP BORDER INDEPENDENTUV */
    }
    DW(0x74, DDBD_16 | DDBD_32);                             /* render target depths */
    DW(0x78, DDBD_16 | DDBD_24 | DDBD_32);                   /* z-buffer depths */
    DW(0x7c, 1);
    DW(0x80, 1);
    DW(0x84, 2048);
    DW(0x88, 2048);
    DW(0x8c, 8192);                                          /* max texture repeat */
    DW(0x90, 2048);                                          /* max aspect ratio */
    DW(0x94, 2);                                             /* max anisotropy */
    FL(0x98, -16384.0f);                                     /* guard band */
    FL(0x9c, -16384.0f);
    FL(0xa0, 16383.0f);
    FL(0xa4, 16383.0f);
    FL(0xa8, 0.0f);
    DW(0xac, 0xff);                                          /* stencil ops */
    DW(0xb0, 8);                                             /* FVF caps: 8 texture coordinate sets */
    DW(0xb4, 0x9effffu);                                     /* texture ops (no bump mapping) */
    WD(0xb8, 8);                                             /* blend stages */
    WD(0xba, 2);                                             /* simultaneous textures */
    DW(0xbc, 8);                                             /* active lights */
    FL(0xc0, 1e10f);                                         /* max vertex W */
    /* deviceGUID at 0xc4 is written below */
    WD(0xd4, 0);                                             /* user clip planes */
    WD(0xd6, type == DEV_TNL ? 2 : 4);                       /* vertex blend matrices */
    DW(0xd8, 0x3f);                                          /* TEXGEN MATERIALSOURCE7 VERTEXFOG DIRECTIONAL POSITIONAL LOCALVIEWER */
#undef DW
#undef FL
#undef WD
    memcpy(gptr(p), b, sizeof b);
    gfx_write_guid(p + 0xc4, dev_guid[type]);
}

static void default_state(D3DState *st, Surface *rt)
{
    memset(st, 0, sizeof *st);
    uint32_t *rs = st->rs;
    rs[4] = 1;                    /* TEXTUREPERSPECTIVE */
    rs[RS_ZENABLE] = surface_zbuffer(rt) ? 1 : 0;
    rs[RS_FILLMODE] = 3;
    rs[RS_SHADEMODE] = 2;
    rs[RS_ZWRITEENABLE] = 1;
    rs[16] = 1;                   /* LASTPIXEL */
    rs[RS_SRCBLEND] = 2;
    rs[RS_DESTBLEND] = 1;
    rs[RS_CULLMODE] = 3;
    rs[RS_ZFUNC] = 4;
    rs[RS_ALPHAFUNC] = 8;
    rs[RS_FOGEND] = f2u(1.0f);
    rs[RS_FOGDENSITY] = f2u(1.0f);
    rs[53] = rs[54] = rs[55] = 1; /* STENCILFAIL/ZFAIL/PASS = KEEP */
    rs[56] = 8;                   /* STENCILFUNC = ALWAYS */
    rs[58] = rs[59] = 0xffffffffu;
    rs[RS_TEXTUREFACTOR] = 0xffffffffu;
    rs[RS_CLIPPING] = 1;
    rs[RS_LIGHTING] = 1;
    rs[RS_COLORVERTEX] = 1;
    rs[RS_LOCALVIEWER] = 1;
    rs[RS_DIFFUSEMATERIALSOURCE] = 1;
    rs[RS_SPECULARMATERIALSOURCE] = 2;
    for (int s = 0; s < STAGES; s++) {
        uint32_t *t = st->tss[s];
        t[TSS_COLOROP] = s == 0 ? 4 : 1;
        t[TSS_COLORARG1] = 2;
        t[TSS_COLORARG2] = 1;
        t[TSS_ALPHAOP] = s == 0 ? 2 : 1;
        t[TSS_ALPHAARG1] = 2;
        t[TSS_ALPHAARG2] = 1;
        t[TSS_TEXCOORDINDEX] = (uint32_t)s;
        t[TSS_ADDRESS] = t[TSS_ADDRESSU] = t[TSS_ADDRESSV] = 1;
        t[TSS_MAGFILTER] = t[TSS_MINFILTER] = t[TSS_MIPFILTER] = 1;
        t[TSS_MAXANISOTROPY] = 1;
    }
    for (int i = 0; i < XF_MAX; i++)
        mat_identity(&st->xf[i]);
    st->vp.w = rt->width;
    st->vp.h = rt->height;
    st->vp.maxz = 1;
    st->rt = rt;
    st->zb = surface_zbuffer(rt);
}

/* ------------------------------------------------------------------ IDirect3D7 */
#define D3DOBJ(o) ((D3D *)com_host(o))

/* The render state of the device that drew last (drawdist.c draws the far terrain with it) */
const D3DState *d3d_last_state(void)
{
    return drawn_dev && drawn_dev->refs > 0 && drawn_dev->st.rt ? &drawn_dev->st : NULL;
}

uint32_t d3d_create(DDraw *dd)
{
    for (D3D *d = d3ds; d; d = d->next)
        if (d->dd == dd && d->refs > 0) {
            d->refs++;
            return d->obj;
        }
    D3D *d = calloc(1, sizeof *d);
    if (!d)
        fatal("out of memory");
    d->id = next_id++;
    d->dd = dd;
    d->refs = 1;
    d->obj = com_new(&d3d_cls, d);
    d->next = d3ds;
    d3ds = d;
    dd_addref(dd);
    LOG(LOG_D3D, "IDirect3D7 #%d created", d->id);
    return d->obj;
}

GMETHOD(d3d_QueryInterface, 3)
{
    uint32_t riid = ARG(1), ppv = ARG(2);
    char g[64];
    if (!ppv)
        return DDERR_INVALIDPARAMS;
    if (gfx_is_guid(riid, G_IUNKNOWN) || gfx_is_guid(riid, G_IDIRECT3D7)) {
        D3DOBJ(ARG(0))->refs++;
        W32(ppv, ARG(0));
        LOG(LOG_D3D, "IDirect3D7::QueryInterface(%s) -> self", gfx_guid_name(riid, g, sizeof g));
        return D3D_OK;
    }
    W32(ppv, 0);
    LOG(LOG_ALWAYS, "IDirect3D7::QueryInterface(%s): not implemented", gfx_guid_name(riid, g, sizeof g));
    return E_NOINTERFACE;
}

GMETHOD(d3d_AddRef, 1)
{
    D3D *d = D3DOBJ(ARG(0));
    LOG(LOG_D3D, "IDirect3D7::AddRef -> %d", d->refs + 1);
    return (uint32_t)++d->refs;
}

GMETHOD(d3d_Release, 1)
{
    D3D *d = D3DOBJ(ARG(0));
    int r = --d->refs;
    LOG(LOG_D3D, "IDirect3D7::Release -> %d", r);
    if (r <= 0) {
        com_set_class(d->obj, &dead_d3d_cls);
        for (D3D **pp = &d3ds; *pp; pp = &(*pp)->next)
            if (*pp == d) {
                *pp = d->next;
                break;
            }
        dd_release(d->dd);
        return 0;
    }
    return (uint32_t)r;
}

GMETHOD(d3d_EnumDevices, 3)
{
    uint32_t cb = ARG(1), ctx = ARG(2);
    if (!cb)
        return DDERR_INVALIDPARAMS;
    LOG(LOG_D3D, "IDirect3D7::EnumDevices(cb %08x)", cb);
    uint32_t desc = galloc(SZ_D3DDEVICEDESC7);
    uint32_t sdesc = galloc(128), sname = galloc(64);
    for (int t = DEV_RGB; t <= DEV_TNL; t++) {
        fill_devdesc(desc, t);
        gstrncpy(sdesc, dev_desc[t], 128);
        gstrncpy(sname, dev_name[t], 64);
        LOG(LOG_D3D, "  device \"%s\"", dev_name[t]);
        gfx_unlock();
        uint32_t r = guest_call(c, cb, 4, sdesc, sname, desc, ctx);
        gfx_lock();
        if (r == D3DENUMRET_CANCEL)
            break;
    }
    gfree(desc);
    gfree(sdesc);
    gfree(sname);
    return D3D_OK;
}

static uint32_t device_new(CPU *c, D3D *d3, int type, Surface *rt, uint32_t pp);

GMETHOD(d3d_CreateDevice, 4)
{
    D3D *d3 = D3DOBJ(ARG(0));
    uint32_t guid = ARG(1), pp = ARG(3);
    Surface *rt = surface_get(ARG(2));
    char g[64], nm[96];
    if (!pp || !rt)
        return DDERR_INVALIDPARAMS;
    W32(pp, 0);
    int type = -1;
    for (int t = 0; t < 3; t++)
        if (gfx_is_guid(guid, dev_guid[t]))
            type = t;
    LOG(LOG_D3D, "IDirect3D7::CreateDevice(%s, %s)", gfx_guid_name(guid, g, sizeof g), surface_name(rt, nm, sizeof nm));
    if (type < 0) {
        LOG(LOG_ALWAYS, "IDirect3D7::CreateDevice: unknown device %s", gfx_guid_name(guid, g, sizeof g));
        return DDERR_INVALIDPARAMS;
    }
    if (!(rt->caps & DDSCAPS_3DDEVICE))
        return DDERR_INVALIDCAPS;
    if (type != DEV_RGB && !(rt->caps & DDSCAPS_VIDEOMEMORY))
        return D3DERR_SURFACENOTINVIDMEM;
    return device_new(c, d3, type, rt, pp);
}

GMETHOD(d3d_CreateVertexBuffer, 4)
{
    uint32_t desc = ARG(1), pp = ARG(2), flags = ARG(3);
    char f[160];
    if (!desc || !pp || R32(desc) < 16)
        return DDERR_INVALIDPARAMS;
    W32(pp, 0);
    uint32_t caps = R32(desc + 4), fvf = R32(desc + 8), n = R32(desc + 12);
    Layout l;
    LOG(LOG_D3D, "IDirect3D7::CreateVertexBuffer(caps %x, %s, %u vertices, flags %x)", caps, fvf_str(fvf, f, sizeof f),
        n, flags);
    if (!fvf_layout(fvf, &l) || !l.pos)
        return D3DERR_INVALIDVERTEXFORMAT;
    if (!n || n > 0x100000)
        return DDERR_INVALIDPARAMS;
    VBuf *vb = calloc(1, sizeof *vb);
    if (!vb)
        fatal("out of memory");
    vb->id = next_id++;
    vb->refs = 1;
    vb->caps = caps;
    vb->fvf = fvf;
    vb->n = n;
    vb->stride = l.stride;
    vb->memsize = n * l.stride;
    if (vb->memsize >= 0x10000) {
        vb->mem = vm_alloc(0, vb->memsize, 0);
        vb->mem_kind = 2;
    } else {
        vb->mem = galloc(vb->memsize);
        vb->mem_kind = 1;
    }
    if (!vb->mem) {
        free(vb);
        return DDERR_OUTOFMEMORY;
    }
    vb->obj = com_new(&vb_cls, vb);
    W32(pp, vb->obj);
    LOG(LOG_D3D, "  -> vb#%d %08x, memory %08x (%u bytes)", vb->id, vb->obj, vb->mem, vb->memsize);
    return D3D_OK;
}

GMETHOD(d3d_EnumZBufferFormats, 4)
{
    uint32_t riid = ARG(1), cb = ARG(2), ctx = ARG(3);
    char g[64], pf[64];
    if (!cb)
        return DDERR_INVALIDPARAMS;
    LOG(LOG_D3D, "IDirect3D7::EnumZBufferFormats(%s)", gfx_guid_name(riid, g, sizeof g));
    uint32_t p = galloc(SZ_DDPIXELFORMAT);
    for (int i = 0; i < gfx_nz_formats; i++) {
        memcpy(gptr(p), gfx_z_formats[i], SZ_DDPIXELFORMAT);
        LOG(LOG_D3D, "  %s", pf_str(gfx_z_formats[i], pf, sizeof pf));
        gfx_unlock();
        uint32_t r = guest_call(c, cb, 2, p, ctx);
        gfx_lock();
        if (r == D3DENUMRET_CANCEL)
            break;
    }
    gfree(p);
    return D3D_OK;
}

GMETHOD(d3d_EvictManagedTextures, 1)
{
    LOG(LOG_D3D, "IDirect3D7::EvictManagedTextures");
    return D3D_OK;
}

/* ------------------------------------------------------------------ IDirect3DDevice7 */
static uint32_t device_new(CPU *c, D3D *d3, int type, Surface *rt, uint32_t pp)
{
    Device *d = calloc(1, sizeof *d);
    if (!d)
        fatal("out of memory");
    d->id = next_id++;
    d->refs = 1;
    d->type = type;
    d->d3d = d3;
    d3->refs++;
    surface_addref(rt);
    default_state(&d->st, rt);
    d->obj = com_new(&dev_cls, d);
    W32(pp, d->obj);
    uint32_t coop = dd_coop_flags(d3->dd);
    if (!(coop & DDSCL_FPUPRESERVE)) {
        LOG(LOG_D3D, "  x87 control word %04x -> single precision", c->fcw);
        fpu_setcw(c, c->fcw & ~0x300u);
    } else {
        LOG(LOG_D3D, "  x87 control word %04x kept (FPUPRESERVE)", c->fcw);
    }
    LOG(LOG_D3D, "  -> device#%d \"%s\" %08x, render target %ux%u, z-buffer %s", d->id, dev_name[type], d->obj,
        rt->width, rt->height, d->st.zb ? "yes" : "no");
    return D3D_OK;
}

GMETHOD(dev_QueryInterface, 3)
{
    uint32_t riid = ARG(1), ppv = ARG(2);
    char g[64];
    if (!ppv)
        return DDERR_INVALIDPARAMS;
    if (gfx_is_guid(riid, G_IUNKNOWN) || gfx_is_guid(riid, G_IDIRECT3DDEVICE7)) {
        DEV(ARG(0))->refs++;
        W32(ppv, ARG(0));
        return D3D_OK;
    }
    W32(ppv, 0);
    LOG(LOG_ALWAYS, "IDirect3DDevice7::QueryInterface(%s): not implemented", gfx_guid_name(riid, g, sizeof g));
    return E_NOINTERFACE;
}

GMETHOD(dev_AddRef, 1)
{
    Device *d = DEV(ARG(0));
    LOG(LOG_D3D, "IDirect3DDevice7::AddRef -> %d", d->refs + 1);
    return (uint32_t)++d->refs;
}

GMETHOD(dev_Release, 1)
{
    Device *d = DEV(ARG(0));
    int r = --d->refs;
    LOG(LOG_D3D, "IDirect3DDevice7::Release -> %d", r);
    if (r > 0)
        return (uint32_t)r;
    for (int s = 0; s < STAGES; s++)
        if (d->st.tex[s]) {
            surface_release(d->st.tex[s]);
            d->st.tex[s] = NULL;
        }
    surface_release(d->st.rt);
    d->st.rt = d->st.zb = NULL;
    com_set_class(d->obj, &dead_dev_cls);
    /* the IDirect3D7 reference */
    D3D *d3 = d->d3d;
    if (--d3->refs <= 0) {
        com_set_class(d3->obj, &dead_d3d_cls);
        for (D3D **pp = &d3ds; *pp; pp = &(*pp)->next)
            if (*pp == d3) {
                *pp = d3->next;
                break;
            }
        dd_release(d3->dd);
    }
    return 0;
}

GMETHOD(dev_GetCaps, 2)
{
    Device *d = DEV(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    fill_devdesc(ARG(1), d->type);
    LOG(LOG_D3D, "IDirect3DDevice7::GetCaps (%s)", dev_name[d->type]);
    return D3D_OK;
}

GMETHOD(dev_EnumTextureFormats, 3)
{
    uint32_t cb = ARG(1), ctx = ARG(2);
    char pf[64];
    if (!cb)
        return DDERR_INVALIDPARAMS;
    LOG(LOG_D3D, "IDirect3DDevice7::EnumTextureFormats(cb %08x)", cb);
    uint32_t p = galloc(SZ_DDPIXELFORMAT);
    for (int i = 0; i < gfx_ntex_formats; i++) {
        memcpy(gptr(p), gfx_tex_formats[i], SZ_DDPIXELFORMAT);
        LOG(LOG_D3D, "  %s", pf_str(gfx_tex_formats[i], pf, sizeof pf));
        gfx_unlock();
        uint32_t r = guest_call(c, cb, 2, p, ctx);
        gfx_lock();
        if (r == D3DENUMRET_CANCEL)
            break;
    }
    gfree(p);
    return D3D_OK;
}

GMETHOD(dev_BeginScene, 1)
{
    Device *d = DEV(ARG(0));
    LOG(LOG_D3D, "IDirect3DDevice7::BeginScene");
    if (d->in_scene)
        return D3DERR_SCENE_IN_SCENE;
    d->in_scene = 1;
    return D3D_OK;
}

GMETHOD(dev_EndScene, 1)
{
    Device *d = DEV(ARG(0));
    LOG(LOG_D3D, "IDirect3DDevice7::EndScene");
    if (!d->in_scene)
        return D3DERR_SCENE_NOT_IN_SCENE;
    d->in_scene = 0;
    return D3D_OK;
}

GMETHOD(dev_GetDirect3D, 2)
{
    Device *d = DEV(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    d->d3d->refs++;
    W32(ARG(1), d->d3d->obj);
    LOG(LOG_D3D, "IDirect3DDevice7::GetDirect3D -> %08x", d->d3d->obj);
    return D3D_OK;
}

GMETHOD(dev_SetRenderTarget, 3)
{
    Device *d = DEV(ARG(0));
    Surface *rt = surface_get(ARG(1));
    char nm[96];
    LOG(LOG_D3D, "IDirect3DDevice7::SetRenderTarget(%s, %x)", rt ? surface_name(rt, nm, sizeof nm) : "NULL", ARG(2));
    if (!rt || !(rt->caps & DDSCAPS_3DDEVICE))
        return DDERR_INVALIDPARAMS;
    surface_addref(rt);
    surface_release(d->st.rt);
    d->st.rt = rt;
    d->st.zb = surface_zbuffer(rt);
    d->st.vp.x = d->st.vp.y = 0;
    d->st.vp.w = rt->width;
    d->st.vp.h = rt->height;
    return D3D_OK;
}

GMETHOD(dev_GetRenderTarget, 2)
{
    Device *d = DEV(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    surface_addref(d->st.rt);
    W32(ARG(1), surface_obj7(d->st.rt));
    LOG(LOG_D3D, "IDirect3DDevice7::GetRenderTarget");
    return D3D_OK;
}

GMETHOD(dev_Clear, 7)
{
    Device *d = DEV(ARG(0));
    uint32_t n = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4), stencil = ARG(6);
    float z = u2f(ARG(5));
    LOG(LOG_D3D, "IDirect3DDevice7::Clear(%u rects, %s%s%s, colour %08x, z %g, stencil %u)", n,
        flags & CLEAR_TARGET ? "TARGET " : "", flags & CLEAR_ZBUFFER ? "ZBUFFER " : "",
        flags & CLEAR_STENCIL ? "STENCIL" : "", color, z, stencil);
    Surface *zb = d->st.zb;
    if ((flags & (CLEAR_ZBUFFER | CLEAR_STENCIL)) && !zb)
        return D3DERR_ZBUFFER_NOTPRESENT;
    int vp[4] = {(int)d->st.vp.x, (int)d->st.vp.y, (int)(d->st.vp.x + d->st.vp.w), (int)(d->st.vp.y + d->st.vp.h)};
    uint32_t zraw = 0;
    if (zb) {
        uint32_t zm = zb->zmask, sh = zm ? (uint32_t)__builtin_ctz(zm) : 0, max = zm >> sh;
        float zc = z < 0 ? 0 : z > 1 ? 1 : z;
        zraw = ((uint32_t)((double)zc * max + 0.5) << sh) & zm;
        if (zb->stencilmask && (flags & CLEAR_STENCIL))
            zraw |= (stencil << __builtin_ctz(zb->stencilmask)) & zb->stencilmask;
    }
    uint32_t nr = n && rects ? n : 1;
    int *rr = malloc(nr * 4 * sizeof *rr);
    if (!rr)
        fatal("out of memory");
    for (uint32_t i = 0; i < nr; i++) {
        int *r = &rr[4 * i];
        memcpy(r, vp, 4 * sizeof *r);
        if (n && rects) {
            for (int k = 0; k < 4; k++) {
                int v = (int32_t)R32(rects + 16 * i + 4 * (uint32_t)k);
                r[k] = k < 2 ? (v > vp[k] ? v : vp[k]) : (v < vp[k] ? v : vp[k]);
            }
        }
    }
    int gpu = vk_on();
    if (gpu)
        vk_clear(&d->st, (int)nr, rr, flags, color, z, stencil);
    /* With the GPU drawing, program memory gets the clear too only in test runs (they compare
       memory with the reference; TD6_BENCH=1 measures without); otherwise a Lock reads the
       target back from the GPU. */
    static int mirror = -1;
    if (mirror < 0)
        mirror = deterministic && !getenv("TD6_BENCH");
    for (uint32_t i = 0; i < nr && (!gpu || mirror); i++) {
        int *r = &rr[4 * i];
        if (flags & CLEAR_TARGET)
            (gpu ? surface_fill_mem : surface_fill)(d->st.rt, r, pix_encode(d->st.rt, color));
        if (flags & (CLEAR_ZBUFFER | CLEAR_STENCIL))
            (gpu ? surface_fill_mem : surface_fill)(zb, r, zraw);
    }
    free(rr);
    return D3D_OK;
}

static const char *mat_str(uint32_t m, char *buf, size_t n)
{
    size_t k = 0;
    for (int i = 0; i < 16; i++)
        k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "%s%g", i == 0 ? "[" : i % 4 == 0 ? " | " : " ", RF32(m + 4 * (uint32_t)i));
    snprintf(buf + k, k < n ? n - k : 0, "]");
    return buf;
}

GMETHOD(dev_SetTransform, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t t = ARG(1), m = ARG(2);
    char ms[400];
    if (!m || t >= XF_MAX || t == 0)
        return DDERR_INVALIDPARAMS;
    memcpy(&d->st.xf[t], gptr(m), sizeof(Mat4));
    LOG(LOG_D3D, "IDirect3DDevice7::SetTransform(%s, %s)", t < 7 ? xf_names[t] : t >= 16 ? "TEXTURE" : "?", mat_str(m, ms, sizeof ms));
    return D3D_OK;
}

GMETHOD(dev_GetTransform, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t t = ARG(1), m = ARG(2);
    if (!m || t >= XF_MAX || t == 0)
        return DDERR_INVALIDPARAMS;
    memcpy(gptr(m), &d->st.xf[t], sizeof(Mat4));
    LOG(LOG_D3D, "IDirect3DDevice7::GetTransform(%u)", t);
    return D3D_OK;
}

GMETHOD(dev_MultiplyTransform, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t t = ARG(1), m = ARG(2);
    char ms[400];
    if (!m || t >= XF_MAX || t == 0)
        return DDERR_INVALIDPARAMS;
    Mat4 a;
    memcpy(&a, gptr(m), sizeof a);
    mat_mul(&d->st.xf[t], &a, &d->st.xf[t]);
    LOG(LOG_D3D, "IDirect3DDevice7::MultiplyTransform(%u, %s)", t, mat_str(m, ms, sizeof ms));
    return D3D_OK;
}

GMETHOD(dev_SetViewport, 2)
{
    Device *d = DEV(ARG(0));
    uint32_t p = ARG(1);
    if (!p)
        return DDERR_INVALIDPARAMS;
    uint32_t x = R32(p), y = R32(p + 4), w = R32(p + 8), h = R32(p + 12);
    float mn = RF32(p + 16), mx = RF32(p + 20);
    LOG(LOG_D3D, "IDirect3DDevice7::SetViewport(%u,%u %ux%u, z %g-%g)", x, y, w, h, mn, mx);
    if ((uint64_t)x + w > d->st.rt->width || (uint64_t)y + h > d->st.rt->height)
        return DDERR_INVALIDPARAMS;
    d->st.vp.x = x;
    d->st.vp.y = y;
    d->st.vp.w = w;
    d->st.vp.h = h;
    d->st.vp.minz = mn;
    d->st.vp.maxz = mx;
    return D3D_OK;
}

GMETHOD(dev_GetViewport, 2)
{
    Device *d = DEV(ARG(0));
    uint32_t p = ARG(1);
    if (!p)
        return DDERR_INVALIDPARAMS;
    W32(p, d->st.vp.x);
    W32(p + 4, d->st.vp.y);
    W32(p + 8, d->st.vp.w);
    W32(p + 12, d->st.vp.h);
    WF32(p + 16, d->st.vp.minz);
    WF32(p + 20, d->st.vp.maxz);
    LOG(LOG_D3D, "IDirect3DDevice7::GetViewport -> %u,%u %ux%u", d->st.vp.x, d->st.vp.y, d->st.vp.w, d->st.vp.h);
    return D3D_OK;
}

GMETHOD(dev_SetMaterial, 2)
{
    Device *d = DEV(ARG(0));
    uint32_t p = ARG(1);
    if (!p)
        return DDERR_INVALIDPARAMS;
    for (int i = 0; i < 17; i++)
        d->st.material[i] = RF32(p + 4 * (uint32_t)i);
    const float *m = d->st.material;
    LOG(LOG_D3D, "IDirect3DDevice7::SetMaterial(diffuse %g %g %g %g, ambient %g %g %g %g, specular %g %g %g %g, "
        "emissive %g %g %g %g, power %g)", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
        m[12], m[13], m[14], m[15], m[16]);
    return D3D_OK;
}

GMETHOD(dev_GetMaterial, 2)
{
    Device *d = DEV(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    for (int i = 0; i < 17; i++)
        WF32(ARG(1) + 4 * (uint32_t)i, d->st.material[i]);
    LOG(LOG_D3D, "IDirect3DDevice7::GetMaterial");
    return D3D_OK;
}

GMETHOD(dev_SetLight, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t i = ARG(1), p = ARG(2);
    if (!p || i >= LIGHTS_MAX) {
        LOG(LOG_ALWAYS, "IDirect3DDevice7::SetLight(%u): index out of our range", i);
        return DDERR_INVALIDPARAMS;
    }
    uint32_t type = R32(p);
    if (type < 1 || type > 3)
        return DDERR_INVALIDPARAMS;
    Light *l = &d->st.lights[i];
    l->type = type;
    for (int k = 0; k < 4; k++) {
        l->diffuse[k] = RF32(p + 4 + 4 * (uint32_t)k);
        l->specular[k] = RF32(p + 20 + 4 * (uint32_t)k);
        l->ambient[k] = RF32(p + 36 + 4 * (uint32_t)k);
    }
    for (int k = 0; k < 3; k++) {
        l->pos[k] = RF32(p + 52 + 4 * (uint32_t)k);
        l->dir[k] = RF32(p + 64 + 4 * (uint32_t)k);
    }
    l->range = RF32(p + 76);
    l->falloff = RF32(p + 80);
    l->att0 = RF32(p + 84);
    l->att1 = RF32(p + 88);
    l->att2 = RF32(p + 92);
    l->theta = RF32(p + 96);
    l->phi = RF32(p + 100);
    l->set = 1;
    static const char *const lt[] = {"?", "POINT", "SPOT", "DIRECTIONAL"};
    LOG(LOG_D3D, "IDirect3DDevice7::SetLight(%u, %s diffuse %g %g %g, ambient %g %g %g, pos %g %g %g, dir %g %g %g, "
        "range %g, att %g %g %g)", i, lt[type], l->diffuse[0], l->diffuse[1], l->diffuse[2], l->ambient[0],
        l->ambient[1], l->ambient[2], l->pos[0], l->pos[1], l->pos[2], l->dir[0], l->dir[1], l->dir[2], l->range,
        l->att0, l->att1, l->att2);
    return D3D_OK;
}

GMETHOD(dev_GetLight, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t i = ARG(1), p = ARG(2);
    if (!p || i >= LIGHTS_MAX || !d->st.lights[i].set)
        return DDERR_INVALIDPARAMS;
    const Light *l = &d->st.lights[i];
    W32(p, l->type);
    for (int k = 0; k < 4; k++) {
        WF32(p + 4 + 4 * (uint32_t)k, l->diffuse[k]);
        WF32(p + 20 + 4 * (uint32_t)k, l->specular[k]);
        WF32(p + 36 + 4 * (uint32_t)k, l->ambient[k]);
    }
    for (int k = 0; k < 3; k++) {
        WF32(p + 52 + 4 * (uint32_t)k, l->pos[k]);
        WF32(p + 64 + 4 * (uint32_t)k, l->dir[k]);
    }
    WF32(p + 76, l->range);
    WF32(p + 80, l->falloff);
    WF32(p + 84, l->att0);
    WF32(p + 88, l->att1);
    WF32(p + 92, l->att2);
    WF32(p + 96, l->theta);
    WF32(p + 100, l->phi);
    LOG(LOG_D3D, "IDirect3DDevice7::GetLight(%u)", i);
    return D3D_OK;
}

GMETHOD(dev_SetRenderState, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t s = ARG(1), v = ARG(2);
    char vs[48];
    if (s >= RS_MAX) {
        LOG(LOG_ALWAYS, "IDirect3DDevice7::SetRenderState(%u, %x): state out of range", s, v);
        return DDERR_INVALIDPARAMS;
    }
    LOG(LOG_D3D, "IDirect3DDevice7::SetRenderState(%s, %s)", rs_names[s] ? rs_names[s] : "?", rs_value(s, v, vs, sizeof vs));
    d->st.rs[s] = v;
    return D3D_OK;
}

GMETHOD(dev_GetRenderState, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t s = ARG(1), p = ARG(2);
    if (s >= RS_MAX || !p)
        return DDERR_INVALIDPARAMS;
    W32(p, d->st.rs[s]);
    LOG(LOG_D3D, "IDirect3DDevice7::GetRenderState(%s) -> %x", rs_names[s] ? rs_names[s] : "?", d->st.rs[s]);
    return D3D_OK;
}

GMETHOD(dev_BeginStateBlock, 1)
{
    LOG(LOG_ALWAYS, "IDirect3DDevice7::BeginStateBlock: not implemented");
    return DDERR_UNSUPPORTED;
}

GMETHOD(dev_EndStateBlock, 2)
{
    LOG(LOG_ALWAYS, "IDirect3DDevice7::EndStateBlock: not implemented");
    return DDERR_UNSUPPORTED;
}

GMETHOD(dev_PreLoad, 2)
{
    LOG(LOG_D3D, "IDirect3DDevice7::PreLoad(%08x)", ARG(1));
    return D3D_OK;
}

GMETHOD(dev_DrawPrimitive, 6)
{
    Device *d = DEV(ARG(0));
    uint32_t prim = ARG(1), fvf = ARG(2), verts = ARG(3), n = ARG(4);
    Layout l;
    Fetch f;
    if (!verts || !fvf_layout(fvf, &l) || !l.pos)
        return D3DERR_INVALIDVERTEXTYPE;
    fetch_interleaved(&f, &l, verts);
    return draw(d, API_DP, prim, &f, 0, n, 0, 0, NULL);
}

GMETHOD(dev_DrawIndexedPrimitive, 8)
{
    Device *d = DEV(ARG(0));
    uint32_t prim = ARG(1), fvf = ARG(2), verts = ARG(3), n = ARG(4), idx = ARG(5), ni = ARG(6);
    Layout l;
    Fetch f;
    if (!verts || !idx || !fvf_layout(fvf, &l) || !l.pos)
        return D3DERR_INVALIDVERTEXTYPE;
    fetch_interleaved(&f, &l, verts);
    return draw(d, API_DIP, prim, &f, 0, n, idx, ni, NULL);
}

GMETHOD(dev_SetClipStatus, 2)
{
    Device *d = DEV(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    memcpy(d->clipstatus, gptr(ARG(1)), sizeof d->clipstatus);
    LOG(LOG_D3D, "IDirect3DDevice7::SetClipStatus");
    return D3D_OK;
}

GMETHOD(dev_GetClipStatus, 2)
{
    Device *d = DEV(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    memcpy(gptr(ARG(1)), d->clipstatus, sizeof d->clipstatus);
    LOG(LOG_D3D, "IDirect3DDevice7::GetClipStatus");
    return D3D_OK;
}

GMETHOD(dev_DrawPrimitiveStrided, 6)
{
    Device *d = DEV(ARG(0));
    uint32_t prim = ARG(1), fvf = ARG(2), sd = ARG(3), n = ARG(4);
    Layout l;
    Fetch f;
    if (!sd || !fvf_layout(fvf, &l) || l.pos != 1)
        return D3DERR_INVALIDVERTEXTYPE;
    fetch_strided(&f, &l, sd);
    return draw(d, API_DPS, prim, &f, 0, n, 0, 0, NULL);
}

GMETHOD(dev_DrawIndexedPrimitiveStrided, 8)
{
    Device *d = DEV(ARG(0));
    uint32_t prim = ARG(1), fvf = ARG(2), sd = ARG(3), n = ARG(4), idx = ARG(5), ni = ARG(6);
    Layout l;
    Fetch f;
    if (!sd || !idx || !fvf_layout(fvf, &l) || l.pos != 1)
        return D3DERR_INVALIDVERTEXTYPE;
    fetch_strided(&f, &l, sd);
    return draw(d, API_DIPS, prim, &f, 0, n, idx, ni, NULL);
}

GMETHOD(dev_DrawPrimitiveVB, 6)
{
    Device *d = DEV(ARG(0));
    VBuf *vb = ARG(2) ? VB(ARG(2)) : NULL;
    uint32_t prim = ARG(1), start = ARG(3), n = ARG(4);
    Layout l;
    Fetch f;
    if (!vb || (uint64_t)start + n > vb->n)
        return DDERR_INVALIDPARAMS;
    fvf_layout(vb->fvf, &l);
    fetch_interleaved(&f, &l, vb->mem);
    return draw(d, API_DPVB, prim, &f, start, n, 0, 0, vb);
}

GMETHOD(dev_DrawIndexedPrimitiveVB, 8)
{
    Device *d = DEV(ARG(0));
    VBuf *vb = ARG(2) ? VB(ARG(2)) : NULL;
    uint32_t prim = ARG(1), start = ARG(3), n = ARG(4), idx = ARG(5), ni = ARG(6);
    Layout l;
    Fetch f;
    if (!vb || !idx || (uint64_t)start + n > vb->n)
        return DDERR_INVALIDPARAMS;
    fvf_layout(vb->fvf, &l);
    fetch_interleaved(&f, &l, vb->mem);
    return draw(d, API_DIPVB, prim, &f, start, n, idx, ni, vb);
}

GMETHOD(dev_ComputeSphereVisibility, 6)
{
    uint32_t n = ARG(3), out = ARG(5);
    for (uint32_t i = 0; out && i < n; i++)
        W32(out + 4 * i, 0);
    LOG(LOG_D3D, "IDirect3DDevice7::ComputeSphereVisibility(%u spheres) -> all visible", n);
    return D3D_OK;
}

GMETHOD(dev_GetTexture, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t s = ARG(1), p = ARG(2);
    if (s >= STAGES || !p)
        return DDERR_INVALIDPARAMS;
    Surface *t = d->st.tex[s];
    if (t)
        surface_addref(t);
    W32(p, t ? surface_obj7(t) : 0);
    LOG(LOG_D3D, "IDirect3DDevice7::GetTexture(%u)", s);
    return D3D_OK;
}

GMETHOD(dev_SetTexture, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t s = ARG(1);
    Surface *t = surface_get(ARG(2));
    char nm[96];
    LOG(LOG_D3D, "IDirect3DDevice7::SetTexture(%u, %s)", s, t ? surface_name(t, nm, sizeof nm) : "NULL");
    if (s >= STAGES || (t && !(t->caps & DDSCAPS_TEXTURE)))
        return DDERR_INVALIDPARAMS;
    if (t)
        surface_addref(t);
    if (d->st.tex[s])
        surface_release(d->st.tex[s]);
    d->st.tex[s] = t;
    return D3D_OK;
}

GMETHOD(dev_GetTextureStageState, 4)
{
    Device *d = DEV(ARG(0));
    uint32_t s = ARG(1), t = ARG(2), p = ARG(3);
    if (s >= STAGES || t >= TSS_MAX || !p)
        return DDERR_INVALIDPARAMS;
    W32(p, d->st.tss[s][t]);
    LOG(LOG_D3D, "IDirect3DDevice7::GetTextureStageState(%u, %s) -> %x", s, tss_names[t] ? tss_names[t] : "?", d->st.tss[s][t]);
    return D3D_OK;
}

GMETHOD(dev_SetTextureStageState, 4)
{
    Device *d = DEV(ARG(0));
    uint32_t s = ARG(1), t = ARG(2), v = ARG(3);
    char vs[64];
    if (s >= STAGES || t >= TSS_MAX || t == 0) {
        LOG(LOG_ALWAYS, "IDirect3DDevice7::SetTextureStageState(%u, %u, %x): out of range", s, t, v);
        return DDERR_INVALIDPARAMS;
    }
    LOG(LOG_D3D, "IDirect3DDevice7::SetTextureStageState(%u, %s, %s)", s, tss_names[t] ? tss_names[t] : "?",
        tss_value(t, v, vs, sizeof vs));
    d->st.tss[s][t] = v;
    if (t == TSS_ADDRESS)
        d->st.tss[s][TSS_ADDRESSU] = d->st.tss[s][TSS_ADDRESSV] = v;
    return D3D_OK;
}

GMETHOD(dev_ValidateDevice, 2)
{
    if (ARG(1))
        W32(ARG(1), 1);
    LOG(LOG_D3D, "IDirect3DDevice7::ValidateDevice -> 1 pass");
    return D3D_OK;
}

GMETHOD(dev_ApplyStateBlock, 2)
{
    LOG(LOG_ALWAYS, "IDirect3DDevice7::ApplyStateBlock: not implemented");
    return DDERR_UNSUPPORTED;
}

GMETHOD(dev_CaptureStateBlock, 2)
{
    LOG(LOG_ALWAYS, "IDirect3DDevice7::CaptureStateBlock: not implemented");
    return DDERR_UNSUPPORTED;
}

GMETHOD(dev_DeleteStateBlock, 2)
{
    LOG(LOG_ALWAYS, "IDirect3DDevice7::DeleteStateBlock: not implemented");
    return DDERR_UNSUPPORTED;
}

GMETHOD(dev_CreateStateBlock, 3)
{
    LOG(LOG_ALWAYS, "IDirect3DDevice7::CreateStateBlock: not implemented");
    if (ARG(2))
        W32(ARG(2), 0);
    return DDERR_UNSUPPORTED;
}

GMETHOD(dev_Load, 6)
{
    Surface *dst = surface_get(ARG(1)), *src = surface_get(ARG(3));
    uint32_t pt = ARG(2), sr = ARG(4);
    char n1[96], n2[96];
    if (!dst || !src)
        return DDERR_INVALIDPARAMS;
    LOG(LOG_D3D, "IDirect3DDevice7::Load(%s <- %s, flags %x)", surface_name(dst, n1, sizeof n1),
        surface_name(src, n2, sizeof n2), ARG(5));
    if (!pt && !sr) {
        for (Surface *d = dst, *s = src; d && s; d = d->next, s = s->next) {
            surface_copy(d, NULL, s, NULL, 0, 0, 0);
            if (d->next == dst || s->next == src)
                break;
        }
        return D3D_OK;
    }
    int r[4] = {0, 0, (int)src->width, (int)src->height};
    if (sr)
        for (int k = 0; k < 4; k++)
            r[k] = (int32_t)R32(sr + 4 * (uint32_t)k);
    int x = pt ? (int32_t)R32(pt) : 0, y = pt ? (int32_t)R32(pt + 4) : 0;
    int dr[4] = {x, y, x + r[2] - r[0], y + r[3] - r[1]};
    surface_copy(dst, dr, src, r, 0, 0, 0);
    return D3D_OK;
}

GMETHOD(dev_LightEnable, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t i = ARG(1), on = ARG(2);
    LOG(LOG_D3D, "IDirect3DDevice7::LightEnable(%u, %s)", i, on ? "TRUE" : "FALSE");
    if (i >= LIGHTS_MAX) {
        LOG(LOG_ALWAYS, "IDirect3DDevice7::LightEnable(%u): index out of our range", i);
        return DDERR_INVALIDPARAMS;
    }
    Light *l = &d->st.lights[i];
    if (!l->set) {                /* D3D7 makes a default white directional light */
        memset(l, 0, sizeof *l);
        l->type = 3;
        l->diffuse[0] = l->diffuse[1] = l->diffuse[2] = 1;
        l->dir[2] = 1;
        l->set = 1;
    }
    l->enabled = on != 0;
    return D3D_OK;
}

GMETHOD(dev_GetLightEnable, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t i = ARG(1), p = ARG(2);
    if (i >= LIGHTS_MAX || !p || !d->st.lights[i].set) {
        LOG(LOG_D3D, "IDirect3DDevice7::GetLightEnable(%u) -> no such light", i);
        return DDERR_INVALIDPARAMS;
    }
    W32(p, d->st.lights[i].enabled ? 1 : 0);
    LOG(LOG_D3D, "IDirect3DDevice7::GetLightEnable(%u) -> %d", i, d->st.lights[i].enabled);
    return D3D_OK;
}

GMETHOD(dev_SetClipPlane, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t i = ARG(1), p = ARG(2);
    if (i >= 6 || !p)
        return DDERR_INVALIDPARAMS;
    for (int k = 0; k < 4; k++)
        d->st.clipplane[i][k] = RF32(p + 4 * (uint32_t)k);
    LOG(LOG_D3D, "IDirect3DDevice7::SetClipPlane(%u)", i);
    return D3D_OK;
}

GMETHOD(dev_GetClipPlane, 3)
{
    Device *d = DEV(ARG(0));
    uint32_t i = ARG(1), p = ARG(2);
    if (i >= 6 || !p)
        return DDERR_INVALIDPARAMS;
    for (int k = 0; k < 4; k++)
        WF32(p + 4 * (uint32_t)k, d->st.clipplane[i][k]);
    return D3D_OK;
}

GMETHOD(dev_GetInfo, 4)
{
    if (ARG(2) && ARG(3))
        memset(gptr(ARG(2)), 0, ARG(3));
    LOG(LOG_D3D, "IDirect3DDevice7::GetInfo(%u) -> no information", ARG(1));
    return S_FALSE;
}

/* ------------------------------------------------------------------ IDirect3DVertexBuffer7 */
GMETHOD(vb_QueryInterface, 3)
{
    uint32_t riid = ARG(1), ppv = ARG(2);
    char g[64];
    if (!ppv)
        return DDERR_INVALIDPARAMS;
    if (gfx_is_guid(riid, G_IUNKNOWN) || gfx_is_guid(riid, G_IDIRECT3DVERTEXBUFFER7)) {
        VB(ARG(0))->refs++;
        W32(ppv, ARG(0));
        return D3D_OK;
    }
    W32(ppv, 0);
    LOG(LOG_ALWAYS, "IDirect3DVertexBuffer7::QueryInterface(%s): not implemented", gfx_guid_name(riid, g, sizeof g));
    return E_NOINTERFACE;
}

GMETHOD(vb_AddRef, 1)
{
    VBuf *vb = VB(ARG(0));
    LOG(LOG_D3D, "vb#%d::AddRef -> %d", vb->id, vb->refs + 1);
    return (uint32_t)++vb->refs;
}

GMETHOD(vb_Release, 1)
{
    VBuf *vb = VB(ARG(0));
    int r = --vb->refs;
    LOG(LOG_D3D, "vb#%d::Release -> %d", vb->id, r);
    if (r > 0)
        return (uint32_t)r;
    if (vb->mem_kind == 1)
        gfree(vb->mem);
    else
        vm_free(vb->mem, 0, 1);
    vb->mem = 0;
    free(vb->clip);
    free(vb->clip_ok);
    vb->clip = NULL;
    vb->clip_ok = NULL;
    com_set_class(vb->obj, &dead_vb_cls);
    return 0;
}

GMETHOD(vb_Lock, 4)
{
    VBuf *vb = VB(ARG(0));
    uint32_t flags = ARG(1), pp = ARG(2), psize = ARG(3);
    LOG(LOG_D3D, "vb#%d::Lock(%x) -> %08x, %u bytes", vb->id, flags, vb->mem, vb->memsize);
    if (!pp)
        return DDERR_INVALIDPARAMS;
    if (vb->caps & VBCAPS_OPTIMIZED)
        return D3DERR_VERTEXBUFFEROPTIMIZED;
    if (vb->locks)
        LOG(LOG_D3D, "  (already locked %d times)", vb->locks);
    vb->locks++;
    W32(pp, vb->mem);
    if (psize)
        W32(psize, vb->memsize);
    /* the program may rewrite the vertices: positions kept by ProcessVertices are stale unless
       it promised not to (NOOVERWRITE) or only reads */
    if (vb->clip_ok && !(flags & (0x1000u | 0x10u)))
        memset(vb->clip_ok, 0, vb->n);
    return D3D_OK;
}

GMETHOD(vb_Unlock, 1)
{
    VBuf *vb = VB(ARG(0));
    LOG(LOG_D3D, "vb#%d::Unlock", vb->id);
    if (!vb->locks)
        return DDERR_NOTLOCKED;
    vb->locks--;
    vb->dirty++;
    return D3D_OK;
}

static uint32_t process(CPU *c, VBuf *dst, uint32_t op, uint32_t di, uint32_t n, const Fetch *f, uint32_t si,
                        Device *d, uint32_t flags, int api)
{
    char f1[160], f2[160];
    Layout dl;
    fvf_layout(dst->fvf, &dl);
    LOG(LOG_D3D, "vb#%d::%s(%s%s%s%s, %u vertices: %s [%u] -> %s [%u], flags %x)", dst->id, api_names[api],
        op & VOP_TRANSFORM ? "TRANSFORM " : "", op & VOP_LIGHT ? "LIGHT " : "", op & VOP_CLIP ? "CLIP " : "",
        op & VOP_EXTENTS ? "EXTENTS" : "", n, fvf_str(f->l.fvf, f1, sizeof f1), si, fvf_str(dst->fvf, f2, sizeof f2),
        di, flags);
    if (f->l.pos != 1 || dl.pos != 2 || (uint64_t)di + n > dst->n || !(op & VOP_TRANSFORM))
        return DDERR_INVALIDPARAMS;
    count(api, f->l.fvf, dst->fvf, n, 0, 0);
    if (!dst->clip) {
        dst->clip = malloc(dst->n * 4 * sizeof(float));
        dst->clip_ok = calloc(dst->n, 1);
        if (!dst->clip || !dst->clip_ok)
            fatal("out of memory");
    }
    Xform *x = malloc(sizeof *x);
    if (!x)
        fatal("out of memory");
    xform_setup(&d->st, x, (op & VOP_LIGHT) != 0);
    int copy = !(flags & PV_DONOTCOPYDATA);
    const D3DState *st = &d->st;
    for (uint32_t i = 0; i < n; i++) {
        RawVtx v;
        SwVertex o;
        fetch_vertex(f, si + i, &v);
        tnl_vertex(x, &f->l, &v, &o, NULL);
        float w = fabsf(o.w) > 1e-20f ? o.w : 1e-20f, rhw = 1.0f / w;
        uint32_t a = dst->mem + (di + i) * dst->stride;
        WF32(a, (o.x * rhw + 1.0f) * (float)st->vp.w * 0.5f + (float)st->vp.x);
        WF32(a + 4, (1.0f - o.y * rhw) * (float)st->vp.h * 0.5f + (float)st->vp.y);
        WF32(a + 8, o.z * rhw * (st->vp.maxz - st->vp.minz) + st->vp.minz);
        WF32(a + 12, rhw);
        int lit = (op & VOP_LIGHT) != 0, fog = x->fogmode != 0;
        if (dl.diffuse >= 0 && (lit || (copy && f->l.diffuse >= 0)))
            W32(a + (uint32_t)dl.diffuse, pack4(o.diffuse));
        if (dl.specular >= 0 && (lit || fog || (copy && f->l.specular >= 0)))
            W32(a + (uint32_t)dl.specular, pack4(o.specular));
        for (int t = 0; t < dl.ntex && t < f->l.ntex && copy; t++)
            for (int k = 0; k < dl.texdim[t]; k++)
                WF32(a + (uint32_t)dl.tex[t] + 4 * (uint32_t)k, v.tex[t][k]);
        float *cp = &dst->clip[4 * (di + i)];
        cp[0] = o.x;
        cp[1] = o.y;
        cp[2] = o.z;
        cp[3] = o.w;
        dst->clip_ok[di + i] = 1;
    }
    free(x);
    dst->dirty++;
    (void)c;
    return D3D_OK;
}

GMETHOD(vb_ProcessVertices, 8)
{
    VBuf *dst = VB(ARG(0)), *src = ARG(4) ? VB(ARG(4)) : NULL;
    Device *d = ARG(6) ? DEV(ARG(6)) : NULL;
    uint32_t op = ARG(1), di = ARG(2), n = ARG(3), si = ARG(5), flags = ARG(7);
    if (!src || !d || (uint64_t)si + n > src->n)
        return DDERR_INVALIDPARAMS;
    Layout l;
    Fetch f;
    fvf_layout(src->fvf, &l);
    fetch_interleaved(&f, &l, src->mem);
    return process(c, dst, op, di, n, &f, si, d, flags, API_PV);
}

GMETHOD(vb_GetVertexBufferDesc, 2)
{
    VBuf *vb = VB(ARG(0));
    uint32_t p = ARG(1);
    if (!p)
        return DDERR_INVALIDPARAMS;
    W32(p, 16);
    W32(p + 4, vb->caps);
    W32(p + 8, vb->fvf);
    W32(p + 12, vb->n);
    LOG(LOG_D3D, "vb#%d::GetVertexBufferDesc", vb->id);
    return D3D_OK;
}

GMETHOD(vb_Optimize, 3)
{
    VBuf *vb = VB(ARG(0));
    vb->caps |= VBCAPS_OPTIMIZED;
    LOG(LOG_D3D, "vb#%d::Optimize", vb->id);
    return D3D_OK;
}

GMETHOD(vb_ProcessVerticesStrided, 8)
{
    VBuf *dst = VB(ARG(0));
    Device *d = ARG(6) ? DEV(ARG(6)) : NULL;
    uint32_t op = ARG(1), di = ARG(2), n = ARG(3), sd = ARG(4), fvf = ARG(5), flags = ARG(7);
    Layout l;
    Fetch f;
    if (!sd || !d || !fvf_layout(fvf, &l))
        return DDERR_INVALIDPARAMS;
    fetch_strided(&f, &l, sd);
    return process(c, dst, op, di, n, &f, 0, d, flags, API_PVS);
}

/* ------------------------------------------------------------------ released objects */
static int dead_logged;
static uint32_t dead_call(CPU *c, int nargs)
{
    if (dead_logged < 20) {
        dead_logged++;
        LOG(LOG_ALWAYS, "d3d: call with %d args on released object %08x (return address %08x)", nargs, ARG(0),
            R32(c->esp));
    }
    return nargs == 1 ? 0 : DDERR_INVALIDOBJECT;
}
METHOD(dead1, 1) { return dead_call(c, 1); }
METHOD(dead2, 2) { return dead_call(c, 2); }
METHOD(dead3, 3) { return dead_call(c, 3); }
METHOD(dead4, 4) { return dead_call(c, 4); }
METHOD(dead6, 6) { return dead_call(c, 6); }
METHOD(dead7, 7) { return dead_call(c, 7); }
METHOD(dead8, 8) { return dead_call(c, 8); }

/* ------------------------------------------------------------------ vtables */
static void (*const d3d_methods[])(CPU *) = {
    d3d_QueryInterface, d3d_AddRef, d3d_Release, d3d_EnumDevices, d3d_CreateDevice, d3d_CreateVertexBuffer,
    d3d_EnumZBufferFormats, d3d_EvictManagedTextures};
static const char *const d3d_names[] = {
    "QueryInterface", "AddRef", "Release", "EnumDevices", "CreateDevice", "CreateVertexBuffer",
    "EnumZBufferFormats", "EvictManagedTextures"};
static void (*const dead_d3d_methods[])(CPU *) = {dead3, dead1, dead1, dead3, dead4, dead4, dead4, dead1};

static void (*const dev_methods[])(CPU *) = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCaps, dev_EnumTextureFormats, dev_BeginScene,
    dev_EndScene, dev_GetDirect3D, dev_SetRenderTarget, dev_GetRenderTarget, dev_Clear, dev_SetTransform,
    dev_GetTransform, dev_SetViewport, dev_MultiplyTransform, dev_GetViewport, dev_SetMaterial,
    dev_GetMaterial, dev_SetLight, dev_GetLight, dev_SetRenderState, dev_GetRenderState,
    dev_BeginStateBlock, dev_EndStateBlock, dev_PreLoad, dev_DrawPrimitive, dev_DrawIndexedPrimitive,
    dev_SetClipStatus, dev_GetClipStatus, dev_DrawPrimitiveStrided, dev_DrawIndexedPrimitiveStrided,
    dev_DrawPrimitiveVB, dev_DrawIndexedPrimitiveVB, dev_ComputeSphereVisibility, dev_GetTexture,
    dev_SetTexture, dev_GetTextureStageState, dev_SetTextureStageState, dev_ValidateDevice,
    dev_ApplyStateBlock, dev_CaptureStateBlock, dev_DeleteStateBlock, dev_CreateStateBlock, dev_Load,
    dev_LightEnable, dev_GetLightEnable, dev_SetClipPlane, dev_GetClipPlane, dev_GetInfo};
static const char *const dev_names[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "EnumTextureFormats", "BeginScene",
    "EndScene", "GetDirect3D", "SetRenderTarget", "GetRenderTarget", "Clear", "SetTransform",
    "GetTransform", "SetViewport", "MultiplyTransform", "GetViewport", "SetMaterial",
    "GetMaterial", "SetLight", "GetLight", "SetRenderState", "GetRenderState",
    "BeginStateBlock", "EndStateBlock", "PreLoad", "DrawPrimitive", "DrawIndexedPrimitive",
    "SetClipStatus", "GetClipStatus", "DrawPrimitiveStrided", "DrawIndexedPrimitiveStrided",
    "DrawPrimitiveVB", "DrawIndexedPrimitiveVB", "ComputeSphereVisibility", "GetTexture",
    "SetTexture", "GetTextureStageState", "SetTextureStageState", "ValidateDevice",
    "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock", "Load",
    "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "GetInfo"};
static void (*const dead_dev_methods[])(CPU *) = {
    dead3, dead1, dead1, dead2, dead3, dead1, dead1, dead2, dead3, dead2, dead7, dead3, dead3, dead2, dead3,
    dead2, dead2, dead2, dead3, dead3, dead3, dead3, dead1, dead2, dead2, dead6, dead8, dead2, dead2, dead6,
    dead8, dead6, dead8, dead6, dead3, dead3, dead4, dead4, dead2, dead2, dead2, dead2, dead3, dead6, dead3,
    dead3, dead3, dead3, dead4};

static void (*const vb_methods[])(CPU *) = {
    vb_QueryInterface, vb_AddRef, vb_Release, vb_Lock, vb_Unlock, vb_ProcessVertices, vb_GetVertexBufferDesc,
    vb_Optimize, vb_ProcessVerticesStrided};
static const char *const vb_names[] = {
    "QueryInterface", "AddRef", "Release", "Lock", "Unlock", "ProcessVertices", "GetVertexBufferDesc",
    "Optimize", "ProcessVerticesStrided"};
static void (*const dead_vb_methods[])(CPU *) = {dead3, dead1, dead1, dead4, dead1, dead8, dead2, dead3, dead8};

static const char *const dead_names[] = {
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released"};

_Static_assert(COUNT(d3d_methods) == 8 && COUNT(d3d_names) == 8 && COUNT(dead_d3d_methods) == 8,
               "IDirect3D7 has 8 methods");
_Static_assert(COUNT(dev_methods) == 49 && COUNT(dev_names) == 49 && COUNT(dead_dev_methods) == 49,
               "IDirect3DDevice7 has 49 methods");
_Static_assert(COUNT(vb_methods) == 9 && COUNT(vb_names) == 9 && COUNT(dead_vb_methods) == 9,
               "IDirect3DVertexBuffer7 has 9 methods");
_Static_assert(COUNT(dead_names) >= 49, "names for released vtables");

void d3d_init(void)
{
    com_class_init(&d3d_cls, "IDirect3D7", 8, d3d_methods, d3d_names);
    com_class_init(&dev_cls, "IDirect3DDevice7", 49, dev_methods, dev_names);
    com_class_init(&vb_cls, "IDirect3DVertexBuffer7", 9, vb_methods, vb_names);
    com_class_init(&dead_d3d_cls, "IDirect3D7(released)", 8, dead_d3d_methods, dead_names);
    com_class_init(&dead_dev_cls, "IDirect3DDevice7(released)", 49, dead_dev_methods, dead_names);
    com_class_init(&dead_vb_cls, "IDirect3DVertexBuffer7(released)", 9, dead_vb_methods, dead_names);
    atexit(stats_at_exit);
}
