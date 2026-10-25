/* Shared between the DirectDraw 7 stand-in (ddraw.c), the Direct3D 7 stand-in (d3d.c) and the
   software rasterizer used for frame dumps (swrast.c).

   Surfaces live in program memory (Lock hands out a pointer into it). The Direct3D device keeps
   a full record of the current state (D3DState) for the renderer to come; for now draws are
   only logged, counted and, on dumped frames, drawn by swrast into the render target. */
#pragma once
#include <stdint.h>
#include "runtime.h"

/* ------------------------------------------------------------------ HRESULTs */
#define DD_OK                   0u
#define S_FALSE                 1u
#define E_NOTIMPL               0x80004001u
#define E_NOINTERFACE           0x80004002u
#define E_FAIL                  0x80004005u
#define E_OUTOFMEMORY           0x8007000eu
#define E_INVALIDARG            0x80070057u
#define CLASS_E_NOAGGREGATION   0x80040110u
#define DDHR(code)              (0x88760000u | (code))
#define DDERR_INVALIDPARAMS     E_INVALIDARG
#define DDERR_UNSUPPORTED       E_NOTIMPL
#define DDERR_GENERIC           E_FAIL
#define DDERR_OUTOFMEMORY       E_OUTOFMEMORY
#define DDERR_CANNOTATTACHSURFACE DDHR(10)
#define DDERR_CANNOTDETACHSURFACE DDHR(20)
#define DDERR_INVALIDCAPS       DDHR(100)
#define DDERR_INVALIDMODE       DDHR(120)
#define DDERR_INVALIDOBJECT     DDHR(130)
#define DDERR_INVALIDPIXELFORMAT DDHR(145)
#define DDERR_INVALIDRECT       DDHR(150)
#define DDERR_NOCLIPLIST        DDHR(205)
#define DDERR_NOCOLORKEY        DDHR(215)
#define DDERR_NOTFOUND          DDHR(255)
#define DDERR_OUTOFVIDEOMEMORY  DDHR(380)
#define DDERR_SURFACEALREADYATTACHED DDHR(410)
#define DDERR_SURFACELOST       DDHR(450)
#define DDERR_SURFACENOTATTACHED DDHR(460)
#define DDERR_NOTFLIPPABLE      DDHR(582)
#define DDERR_NOTLOCKED         DDHR(584)
#define DDERR_CANTCREATEDC      DDHR(585)
#define DDERR_NODC              DDHR(586)
#define DDERR_NOTPALETTIZED     DDHR(589)
#define DDERR_NOPALETTEATTACHED DDHR(572)
#define DDERR_NOCLIPPERATTACHED DDHR(568)
#define DDERR_NOTONMIPMAPSUBLEVEL DDHR(603)
#define D3DERR_INVALIDPRIMITIVETYPE DDHR(736)
#define D3DERR_INVALIDVERTEXTYPE DDHR(737)
#define D3DERR_SCENE_IN_SCENE   DDHR(760)
#define D3DERR_SCENE_NOT_IN_SCENE DDHR(761)
#define D3DERR_INVALIDVERTEXFORMAT DDHR(2048)
#define D3DERR_VERTEXBUFFERLOCKED DDHR(2062)
#define D3DERR_ZBUFFER_NOTPRESENT DDHR(2070)
#define D3DERR_LIGHT_SET_FAILED DDHR(750)

/* ------------------------------------------------------------------ DirectDraw flags */
#define DDSD_CAPS               0x00000001u
#define DDSD_HEIGHT             0x00000002u
#define DDSD_WIDTH              0x00000004u
#define DDSD_PITCH              0x00000008u
#define DDSD_BACKBUFFERCOUNT    0x00000020u
#define DDSD_ZBUFFERBITDEPTH    0x00000040u
#define DDSD_ALPHABITDEPTH      0x00000080u
#define DDSD_LPSURFACE          0x00000800u
#define DDSD_PIXELFORMAT        0x00001000u
#define DDSD_CKDESTOVERLAY      0x00002000u
#define DDSD_CKDESTBLT          0x00004000u
#define DDSD_CKSRCOVERLAY       0x00008000u
#define DDSD_CKSRCBLT           0x00010000u
#define DDSD_MIPMAPCOUNT        0x00020000u
#define DDSD_REFRESHRATE        0x00040000u
#define DDSD_LINEARSIZE         0x00080000u
#define DDSD_TEXTURESTAGE       0x00100000u

#define DDSCAPS_ALPHA           0x00000002u
#define DDSCAPS_BACKBUFFER      0x00000004u
#define DDSCAPS_COMPLEX         0x00000008u
#define DDSCAPS_FLIP            0x00000010u
#define DDSCAPS_FRONTBUFFER     0x00000020u
#define DDSCAPS_OFFSCREENPLAIN  0x00000040u
#define DDSCAPS_OVERLAY         0x00000080u
#define DDSCAPS_PALETTE         0x00000100u
#define DDSCAPS_PRIMARYSURFACE  0x00000200u
#define DDSCAPS_SYSTEMMEMORY    0x00000800u
#define DDSCAPS_TEXTURE         0x00001000u
#define DDSCAPS_3DDEVICE        0x00002000u
#define DDSCAPS_VIDEOMEMORY     0x00004000u
#define DDSCAPS_VISIBLE         0x00008000u
#define DDSCAPS_WRITEONLY       0x00010000u
#define DDSCAPS_ZBUFFER         0x00020000u
#define DDSCAPS_OWNDC           0x00040000u
#define DDSCAPS_MIPMAP          0x00400000u
#define DDSCAPS_ALLOCONLOAD     0x04000000u
#define DDSCAPS_LOCALVIDMEM     0x10000000u
#define DDSCAPS_NONLOCALVIDMEM  0x20000000u
#define DDSCAPS2_TEXTUREMANAGE  0x00000010u
#define DDSCAPS2_MIPMAPSUBLEVEL 0x00010000u
#define DDSCAPS2_D3DTEXTUREMANAGE 0x00020000u

#define DDPF_ALPHAPIXELS        0x00000001u
#define DDPF_ALPHA              0x00000002u
#define DDPF_FOURCC             0x00000004u
#define DDPF_PALETTEINDEXED8    0x00000020u
#define DDPF_RGB                0x00000040u
#define DDPF_ZBUFFER            0x00000400u
#define DDPF_STENCILBUFFER      0x00004000u
#define DDPF_LUMINANCE          0x00020000u

#define DDSCL_FULLSCREEN        0x00000001u
#define DDSCL_NORMAL            0x00000008u
#define DDSCL_EXCLUSIVE         0x00000010u
#define DDSCL_FPUSETUP          0x00000800u
#define DDSCL_FPUPRESERVE       0x00001000u

#define DDCKEY_DESTBLT          0x00000002u
#define DDCKEY_DESTOVERLAY      0x00000004u
#define DDCKEY_SRCBLT           0x00000008u
#define DDCKEY_SRCOVERLAY       0x00000010u
#define DDCKEY_COLORSPACE       0x00000001u

/* DDBD_* bit-depth flags (caps) */
#define DDBD_8                  0x00000800u
#define DDBD_16                 0x00000400u
#define DDBD_24                 0x00000200u
#define DDBD_32                 0x00000100u

#define DDENUMRET_CANCEL        0u
#define DDENUMRET_OK            1u

/* ------------------------------------------------------------------ surfaces */
/* Struct layouts the program uses (32-bit Windows, DirectX 7 SDK). */
#define SZ_DDSURFACEDESC        108u
#define SZ_DDSURFACEDESC2       124u
#define SZ_DDPIXELFORMAT        32u
#define SZ_DDCAPS               380u
#define SZ_D3DDEVICEDESC7       236u
#define SZ_DDBLTFX              100u

typedef struct DDraw DDraw;

typedef struct Surface {
    int id, refs, dead;
    uint32_t obj7, obj1;          /* program interfaces (IDirectDrawSurface7 / IDirectDrawSurface) */
    DDraw *dd;
    uint32_t caps, caps2, caps3, caps4;
    uint32_t width, height, pitch, bpp;   /* bpp: bytes per pixel */
    uint32_t pf[8];               /* DDPIXELFORMAT as the program sees it */
    uint8_t shift[4], bits[4];    /* r, g, b, a channels from the masks */
    uint32_t zmask, stencilmask;  /* depth surfaces */
    uint32_t mem, memsize;        /* pixels, in program memory */
    int mem_kind;                 /* 0 program-supplied, 1 galloc, 2 vm_alloc */
    uint32_t own_mem;             /* SetSurfaceDesc pointed it elsewhere: its own memory, freed with it */
    int own_kind;
    uint32_t vidmem;              /* bytes charged to the video memory budget */
    int nonlocal;                 /* ... of the AGP part */
    struct Surface *link;         /* list of all surfaces */
    void *privdata;               /* SetPrivateData entries */
    struct GpuImg *gpu;           /* the Vulkan copy (vk.c): texture, render target or depth; with the
                                     render thread, used there and by vk_cpu_access only */
    int vk_seen;                  /* vkthread.c: a call to the renderer has named it */
    struct Surface *root;         /* owner of an implicitly created chain member, else NULL */
    struct Surface *next;         /* next in the flip chain / mipmap chain */
    struct Surface *attached[8];  /* AddAttachedSurface (z-buffer) */
    int nattached;
    int mip_level, mip_count, backbuffers;
    uint32_t ck[4][2];            /* colour keys: DESTBLT, DESTOVERLAY, SRCBLT, SRCOVERLAY (lo, hi) */
    uint32_t ck_set;              /* DDCKEY_* bits that are set */
    uint32_t palette, clipper;    /* program objects */
    unsigned long dirty;          /* bumped by Unlock and by every write through Blt/Flip */
    int font_letters;             /* font.c: the game's picture of letters (drawn from the chosen font) */
    int locks;
    uint32_t priority, lod, uniq;
    uint32_t gamma_obj;
} Surface;

#define PF_FLAGS(s) ((s)->pf[1])
#define PF_BITS(s)  ((s)->pf[3])

/* One lock for all graphics state (recursive). Methods run under it; it is dropped around
   callbacks into the program and around frame_presented(). */
void gfx_lock(void);
void gfx_unlock(void);
#define GMETHOD(fname, nargs) \
    static uint32_t fname##_locked(CPU *c); \
    METHOD(fname, nargs) \
    { \
        gfx_lock(); \
        uint32_t _r = fname##_locked(c); \
        gfx_unlock(); \
        return _r; \
    } \
    static uint32_t fname##_locked(CPU *c)
#define COUNT(a) ((int)(sizeof (a) / sizeof *(a)))

/* log helpers (ddraw.c) */
typedef struct { uint32_t bit; const char *name; } FlagName;
const char *flag_str(uint32_t v, const FlagName *t, int n, char *buf, size_t sz);
#define FLAGS(v, t, buf) flag_str(v, t, COUNT(t), buf, sizeof buf)
const char *pf_str(const uint32_t *pf, char *buf, size_t n);
const char *rect_str(uint32_t rect, char *buf, size_t n);   /* program RECT* or "NULL" */

/* GUIDs known by name */
enum {
    G_IUNKNOWN, G_IDIRECTDRAW, G_IDIRECTDRAW2, G_IDIRECTDRAW4, G_IDIRECTDRAW7,
    G_IDIRECTDRAWSURFACE, G_IDIRECTDRAWSURFACE2, G_IDIRECTDRAWSURFACE3, G_IDIRECTDRAWSURFACE4,
    G_IDIRECTDRAWSURFACE7, G_IDIRECTDRAWPALETTE, G_IDIRECTDRAWCLIPPER, G_IDIRECTDRAWCOLORCONTROL,
    G_IDIRECTDRAWGAMMACONTROL, G_IDIRECT3D, G_IDIRECT3D2, G_IDIRECT3D3, G_IDIRECT3D7,
    G_IDIRECT3DDEVICE7, G_IDIRECT3DVERTEXBUFFER7, G_IDIRECT3DTEXTURE, G_IDIRECT3DTEXTURE2,
    G_RGBDEVICE, G_HALDEVICE, G_TNLHALDEVICE, G_REFDEVICE, G_MMXDEVICE, G_NULLDEVICE, G_RAMPDEVICE,
    G_COUNT
};
int gfx_is_guid(uint32_t a, int id);
const char *gfx_guid_name(uint32_t a, char *buf, size_t n);
void gfx_write_guid(uint32_t dst, int id);

/* formats offered to the program (DDPIXELFORMAT dwords) */
extern const uint32_t gfx_tex_formats[][8];
extern const int gfx_ntex_formats;
extern const uint32_t gfx_z_formats[][8];
extern const int gfx_nz_formats;

/* the DirectDraw object as Direct3D needs it */
uint32_t dd_coop_flags(DDraw *dd);
void dd_addref(DDraw *dd);
void dd_release(DDraw *dd);
extern uint16_t gfx_gamma[3][256];            /* current gamma ramp (identity by default) */
extern struct GfxMode { uint32_t w, h, bpp, refresh; int set; } gfx_mode;   /* display mode */

Surface *surface_get(uint32_t obj);           /* program interface -> host record (NULL if 0) */
uint32_t surface_obj7(Surface *s);            /* the IDirectDrawSurface7 for a surface */
void surface_addref(Surface *s);
void surface_release(Surface *s);
const char *surface_name(const Surface *s, char *buf, size_t n);
Surface *surface_zbuffer(Surface *s);          /* attached z-buffer, if any */
/* Fill a rectangle (x0, y0, x1, y1 exclusive; NULL = whole surface) with a raw pixel value. */
void surface_fill(Surface *s, const int *rect, uint32_t raw);
/* Copy src rect to dst rect (stretching with nearest sampling, converting formats). */
void surface_copy(Surface *dst, const int *drect, Surface *src, const int *srect,
                  int srckey, uint32_t key_lo, uint32_t key_hi);
/* The same in program memory only (with a GPU, render targets live there; their memory is still
   kept as it would be without one, so runs compare equal) */
void surface_fill_mem(Surface *s, const int *rect, uint32_t raw);
void surface_copy_mem(Surface *dst, const int *drect, Surface *src, const int *srect,
                      int srckey, uint32_t key_lo, uint32_t key_hi);

static inline uint32_t surf_read(const Surface *s, uint32_t x, uint32_t y)
{
    uint32_t a = s->mem + y * s->pitch + x * s->bpp;
    switch (s->bpp) {
    case 1: return R8(a);
    case 2: return R16(a);
    case 3: return R8(a) | R8(a + 1) << 8 | R8(a + 2) << 16;
    default: return R32(a);
    }
}

static inline void surf_write(const Surface *s, uint32_t x, uint32_t y, uint32_t v)
{
    uint32_t a = s->mem + y * s->pitch + x * s->bpp;
    switch (s->bpp) {
    case 1: W8(a, v); break;
    case 2: W16(a, v); break;
    case 3: W8(a, v); W8(a + 1, v >> 8); W8(a + 2, v >> 16); break;
    default: W32(a, v); break;
    }
}

static inline uint32_t chan_expand(uint32_t v, int bits)
{
    if (bits >= 8)
        return v >> (bits - 8);
    if (bits == 0)
        return 0;
    uint32_t max = (1u << bits) - 1;
    return (v * 255 + max / 2) / max;
}

/* raw pixel -> 0xAARRGGBB (alpha 0xff when the format has none) */
static inline uint32_t pix_decode(const Surface *s, uint32_t raw)
{
    uint32_t out = 0;
    for (int c = 0; c < 4; c++) {
        uint32_t v;
        if (!s->bits[c])
            v = c == 3 ? 0xff : 0;
        else
            v = chan_expand((raw >> s->shift[c]) & ((1u << s->bits[c]) - 1), s->bits[c]);
        out |= v << (c == 3 ? 24 : 16 - 8 * c);
    }
    return out;
}

/* 0xAARRGGBB -> raw pixel */
static inline uint32_t pix_encode(const Surface *s, uint32_t argb)
{
    uint32_t out = 0;
    for (int c = 0; c < 4; c++) {
        if (!s->bits[c])
            continue;
        uint32_t v = (argb >> (c == 3 ? 24 : 16 - 8 * c)) & 0xff;
        uint32_t x = s->bits[c] >= 8 ? v << (s->bits[c] - 8) : (v * ((1u << s->bits[c]) - 1) + 127) / 255;
        out |= x << s->shift[c];
    }
    return out;
}

/* ------------------------------------------------------------------ Direct3D state */
#define RS_MAX      256
#define TSS_MAX     32
#define STAGES      8
#define LIGHTS_MAX  64
#define XF_MAX      32           /* D3DTRANSFORMSTATETYPE: WORLD 1, VIEW 2, PROJECTION 3, ... */

enum { XF_WORLD = 1, XF_VIEW = 2, XF_PROJ = 3 };

/* D3DRENDERSTATETYPE values used by the rasterizer */
enum {
    RS_ZENABLE = 7, RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15,
    RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24,
    RS_ALPHAFUNC = 25, RS_DITHERENABLE = 26, RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28,
    RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36,
    RS_FOGEND = 37, RS_FOGDENSITY = 38, RS_COLORKEYENABLE = 41, RS_ZBIAS = 47,
    RS_RANGEFOGENABLE = 48, RS_TEXTUREFACTOR = 60, RS_CLIPPING = 136, RS_LIGHTING = 137,
    RS_AMBIENT = 139, RS_FOGVERTEXMODE = 140, RS_COLORVERTEX = 141, RS_LOCALVIEWER = 142,
    RS_NORMALIZENORMALS = 143, RS_DIFFUSEMATERIALSOURCE = 145, RS_SPECULARMATERIALSOURCE = 146,
    RS_AMBIENTMATERIALSOURCE = 147, RS_EMISSIVEMATERIALSOURCE = 148,
};
/* D3DTEXTURESTAGESTATETYPE */
enum {
    TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5,
    TSS_ALPHAARG2 = 6, TSS_TEXCOORDINDEX = 11, TSS_ADDRESS = 12, TSS_ADDRESSU = 13,
    TSS_ADDRESSV = 14, TSS_BORDERCOLOR = 15, TSS_MAGFILTER = 16, TSS_MINFILTER = 17,
    TSS_MIPFILTER = 18, TSS_MIPMAPLODBIAS = 19, TSS_MAXMIPLEVEL = 20, TSS_MAXANISOTROPY = 21,
    TSS_TEXTURETRANSFORMFLAGS = 24,
};
/* D3DPRIMITIVETYPE */
enum { PT_POINTLIST = 1, PT_LINELIST, PT_LINESTRIP, PT_TRIANGLELIST, PT_TRIANGLESTRIP, PT_TRIANGLEFAN };

typedef struct { float m[4][4]; } Mat4;

typedef struct {                  /* D3DLIGHT7 */
    uint32_t type;                /* 1 point, 2 spot, 3 directional */
    float diffuse[4], specular[4], ambient[4];
    float pos[3], dir[3];
    float range, falloff, att0, att1, att2, theta, phi;
    int set, enabled;
} Light;

typedef struct D3DState {
    uint32_t rs[RS_MAX];
    uint32_t tss[STAGES][TSS_MAX];
    Surface *tex[STAGES];
    Mat4 xf[XF_MAX];
    struct { uint32_t x, y, w, h; float minz, maxz; } vp;
    float material[17];           /* D3DMATERIAL7: diffuse, ambient, specular, emissive (rgba), power */
    Light lights[LIGHTS_MAX];
    float clipplane[6][4];
    Surface *rt, *zb;             /* render target and its depth buffer */
} D3DState;

/* d3d.c */
void d3d_init(void);
uint32_t d3d_create(DDraw *dd);               /* IDirect3D7 of a DirectDraw object (QueryInterface) */
void d3d_frame_stats(unsigned long frame);    /* called once per presented frame */
const D3DState *d3d_last_state(void);         /* the device that drew last (NULL: none) */
/* drawdist.c's far objects: the game's GPU-lit draws as they go (their state, vertices, indices), and a
   draw of kept vertices with a state of its own (0: the GPU path cannot take it) */
extern void (*d3d_draw_tap)(const D3DState *st, int prim, const void *raw, int n, const uint16_t *ix, int ni);
int d3d_tnl_draw(const D3DState *st, int prim, const void *raw, int n, const uint16_t *ix, int ni);
int d3d_tnl_draw_inst(const D3DState *st, int prim, const void *raw, int n, const uint16_t *ix, int ni,
                      const float *recs, int ncopies);

/* ------------------------------------------------------------------ software rasterizer */
/* A vertex ready for rasterizing: clip-space position (pretransformed = 0) or screen x, y, z and
   w = 1/rhw (pretransformed = 1); colours as 0..1 floats; specular.a carries the fog factor. */
typedef struct {
    float x, y, z, w;
    float diffuse[4];
    float specular[4];
    float tc[8][2];
    uint32_t rgba[2];       /* diffuse, specular as R8G8B8A8 when packed is set (the GPU's input) */
    int packed;
} SwVertex;

/* GPU transform and lighting (vk.c, shaders/tnl.vert): one draw's constants, std430 layout */
typedef struct {
    float type_range[4];          /* type, range, falloff, - */
    float pos[4], dir[4];         /* camera space */
    float att[4];                 /* att0, att1, att2, - */
    float spot[4];                /* cos(theta / 2), cos(phi / 2) */
    float diffuse[4], specular[4], ambient[4];
} TnlLight;
typedef struct {
    float wvp[16], wv[16];        /* D3D row-vector matrices */
    float nm[3][4];               /* normal matrix rows */
    float place[4], place2[4];    /* filled by vk.c: clip-space mapping */
    float mat[4][4];              /* material diffuse, ambient, specular, emissive */
    float power_fog[4];           /* specular power, fog start, end, density */
    float ambient[4];
    uint32_t flags[4];            /* x: 1 light 2 colorvertex 4 specular 8 localviewer 16 normalize
                                     32 rangefog; y: fog mode; z: lights; w: material sources
                                     (diffuse | specular << 4 | ambient << 8 | emissive << 12) */
    TnlLight lights[8];
    float world[3][4];            /* the world matrix as 3 rows of a column-vector 3x4 (the
                                     layout of VkTransformMatrixKHR): ray-traced shadows */
} TnlConst;
/* the world matrix (D3D row-vector 4x4) -> TnlConst.world */
static inline void tnl_world(float out[3][4], const Mat4 *w)
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            out[i][j] = w->m[j][i];
        out[i][3] = w->m[3][i];
    }
}
/* Draw untransformed XYZ|NORMAL|DIFFUSE|SPECULAR|TEX1 vertices (40 bytes each, raw program memory)
   with the GPU doing transform and lighting; 0 if this draw needs the CPU path (vertex positions
   wanted on the CPU, too few vertices, the frame's constant space full). */
int vk_tnl_ok(const D3DState *st, int nv);
void vk_draw_tnl(const D3DState *st, int prim, const void *raw, int nv, const uint16_t *idx, int ni,
                 const TnlConst *k);
/* The same mesh for n copies (drawdist.c's map copies): k's world matrix the identity; each copy's
   16 floats: its world matrix as 3 rows of the column-vector 3x4, then its sun's scale, its fade
   (VK_FADE's bits as a float's), 0, 0 */
void vk_draw_tnl_inst(const D3DState *st, int prim, const void *raw, int nv, const uint16_t *idx, int ni,
                      const TnlConst *k, const float *recs, int n);

void swrast_init(void);
int swrast_active(void);                         /* the current frame is drawn (and dumped) */
void swrast_draw(const D3DState *st, int prim, const SwVertex *v, int nv, const uint16_t *idx, int ni,
              int pretransformed);
/* A frame was presented from surface s (rect x0, y0, x1, y1 or NULL = all): writes the dump
   when this frame was drawn, then decides about the next one. */
void swrast_present(Surface *s, const int *rect);
