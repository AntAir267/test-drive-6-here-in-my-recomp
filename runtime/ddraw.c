/* DirectDraw 7 stand-in (DESIGN.md points 3 and 4): IDirectDraw7, the old IDirectDraw the
   intro-video player uses, surfaces (IDirectDrawSurface7 and IDirectDrawSurface), palettes,
   clippers and gamma control, with the DirectX 7 vtable layouts. We pretend to be a
   GeForce2-class card: 64 MB of video memory (+64 MB AGP for textures), 16/32-bit modes from
   640x480 to 1600x1200, hardware gamma. Nothing is shown yet: surfaces are memory in the
   program's address space, mode changes are only recorded, and Flip (or a Blt to the primary
   in windowed mode) counts a presented frame.

   What the game does (Game.exe 0x410060-0x412fe0, 0x415390-0x416080, 0x43df50):
   - DirectDrawEnumerateExA(cb, 0, 7); per adapter DirectDrawCreateEx, QI IDirect3D7, GetCaps
     (reads DDCAPS2_PRIMARYGAMMA), IDirect3D7::EnumDevices, GetDisplayMode, EnumDisplayModes.
   - Fullscreen: SetCooperativeLevel(FULLSCREEN|EXCLUSIVE|ALLOWREBOOT|FPUPRESERVE),
     SetDisplayMode(w, h, bpp, 0, 0), primary {COMPLEX|FLIP|3DDEVICE|VIDEOMEMORY|LOCALVIDMEM,
     1 back buffer}, GetAttachedSurface(BACKBUFFER), z-buffer {ZBUFFER|VIDEOMEMORY + the
     enumerated DDPIXELFORMAT} attached to the back buffer, present = GetFlipStatus loop + Flip.
   - Windowed: SetCooperativeLevel(NORMAL|FPUPRESERVE), primary + OFFSCREENPLAIN|3DDEVICE back
     buffer, present = IsLost/Restore + Blt(primary, client rect, back, rect, WAIT).
   - Textures: SYSTEMMEMORY mipmapped copies filled by Lock(WAIT|NOSYSLOCK) (needs DDSD_PITCH
     in the returned flags), then VIDEOMEMORY copies (CreateSurface failure = evict and retry)
     filled per level by Blt(NULL, src, NULL, WAIT) walking GetAttachedSurface(TEXTURE|MIPMAP)
     until it fails. 2D images: SYSTEMMEMORY offscreen surfaces Blt'ed to the back buffer;
     colour-fill Blts.
   - Intro videos (0x43df50): DirectDrawCreate (v1), SetCooperativeLevel, SetDisplayMode
     640x480x16, v1 primary, colour-fill Blt, then DirectShow's multimedia stream. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "gfx.h"
#include "vk.h"
#include "win.h"

/* ------------------------------------------------------------------ constants */
#define VIDMEM_LOCAL            (64u << 20)
#define VIDMEM_AGP              (64u << 20)
#define MAX_TEXTURE             2048u

#define DDERR_ALREADYINITIALIZED DDHR(5)
#define DDERR_NOEXCLUSIVEMODE   DDHR(225)
#define DDERR_TOOBIGSIZE        DDHR(480)
#define DDERR_NOTAOVERLAYSURFACE DDHR(580)
#define DDERR_NOOVERLAYHW       DDHR(260)
#define DDERR_INVALIDDIRECTDRAWGUID DDHR(561)
#define DDERR_MOREDATA          DDHR(690)
#define DDERR_VERTICALBLANKINPROGRESS DDHR(537)

#define DDBLT_COLORFILL         0x00000400u
#define DDBLT_DDFX              0x00000800u
#define DDBLT_DDROPS            0x00001000u
#define DDBLT_KEYDEST           0x00002000u
#define DDBLT_KEYDESTOVERRIDE   0x00004000u
#define DDBLT_KEYSRC            0x00008000u
#define DDBLT_KEYSRCOVERRIDE    0x00010000u
#define DDBLT_ROP               0x00020000u
#define DDBLT_DEPTHFILL         0x02000000u
#define DDBLTFAST_SRCCOLORKEY   0x00000001u
#define DDBLTFAST_DESTCOLORKEY  0x00000002u
#define SRCCOPY                 0x00cc0020u
#define BLACKNESS               0x00000042u
#define WHITENESS               0x00ff0062u
#define DDEDM_REFRESHRATES      0x00000001u
#define DDPCAPS_8BIT            0x00000004u

/* ------------------------------------------------------------------ log helpers */
const char *flag_str(uint32_t v, const FlagName *t, int n, char *buf, size_t sz)
{
    size_t k = 0;
    buf[0] = 0;
    for (int i = 0; i < n && v; i++)
        if ((v & t[i].bit) == t[i].bit && t[i].bit) {
            k += snprintf(buf + k, k < sz ? sz - k : 0, "%s%s", k ? "|" : "", t[i].name);
            v &= ~t[i].bit;
        }
    if (v || !k)
        k += snprintf(buf + k, k < sz ? sz - k : 0, "%s0x%x", k ? "|" : "", v);
    return buf;
}

static const FlagName ddsd_names[] = {
    {DDSD_CAPS, "CAPS"}, {DDSD_HEIGHT, "HEIGHT"}, {DDSD_WIDTH, "WIDTH"}, {DDSD_PITCH, "PITCH"},
    {DDSD_BACKBUFFERCOUNT, "BACKBUFFERCOUNT"}, {DDSD_ZBUFFERBITDEPTH, "ZBUFFERBITDEPTH"},
    {DDSD_ALPHABITDEPTH, "ALPHABITDEPTH"}, {DDSD_LPSURFACE, "LPSURFACE"},
    {DDSD_PIXELFORMAT, "PIXELFORMAT"}, {DDSD_CKDESTOVERLAY, "CKDESTOVERLAY"},
    {DDSD_CKDESTBLT, "CKDESTBLT"}, {DDSD_CKSRCOVERLAY, "CKSRCOVERLAY"}, {DDSD_CKSRCBLT, "CKSRCBLT"},
    {DDSD_MIPMAPCOUNT, "MIPMAPCOUNT"}, {DDSD_REFRESHRATE, "REFRESHRATE"},
    {DDSD_LINEARSIZE, "LINEARSIZE"}, {DDSD_TEXTURESTAGE, "TEXTURESTAGE"}};
static const FlagName caps_names[] = {
    {0x2, "ALPHA"}, {0x4, "BACKBUFFER"}, {0x8, "COMPLEX"}, {0x10, "FLIP"}, {0x20, "FRONTBUFFER"},
    {0x40, "OFFSCREENPLAIN"}, {0x80, "OVERLAY"}, {0x100, "PALETTE"}, {0x200, "PRIMARYSURFACE"},
    {0x800, "SYSTEMMEMORY"}, {0x1000, "TEXTURE"}, {0x2000, "3DDEVICE"}, {0x4000, "VIDEOMEMORY"},
    {0x8000, "VISIBLE"}, {0x10000, "WRITEONLY"}, {0x20000, "ZBUFFER"}, {0x40000, "OWNDC"},
    {0x80000, "LIVEVIDEO"}, {0x100000, "HWCODEC"}, {0x200000, "MODEX"}, {0x400000, "MIPMAP"},
    {0x4000000, "ALLOCONLOAD"}, {0x8000000, "VIDEOPORT"}, {0x10000000, "LOCALVIDMEM"},
    {0x20000000, "NONLOCALVIDMEM"}, {0x40000000, "STANDARDVGAMODE"}, {0x80000000u, "OPTIMIZED"}};
static const FlagName caps2_names[] = {
    {0x4, "HINTDYNAMIC"}, {0x8, "HINTSTATIC"}, {0x10, "TEXTUREMANAGE"}, {0x80, "OPAQUE"},
    {0x100, "HINTANTIALIASING"}, {0x200, "CUBEMAP"}, {0x10000, "MIPMAPSUBLEVEL"},
    {0x20000, "D3DTEXTUREMANAGE"}, {0x40000, "DONOTPERSIST"}};
static const FlagName scl_names[] = {
    {0x1, "FULLSCREEN"}, {0x2, "ALLOWREBOOT"}, {0x4, "NOWINDOWCHANGES"}, {0x8, "NORMAL"},
    {0x10, "EXCLUSIVE"}, {0x40, "ALLOWMODEX"}, {0x80, "SETFOCUSWINDOW"}, {0x100, "SETDEVICEWINDOW"},
    {0x200, "CREATEDEVICEWINDOW"}, {0x400, "MULTITHREADED"}, {0x800, "FPUSETUP"},
    {0x1000, "FPUPRESERVE"}};
static const FlagName blt_names[] = {
    {0x1, "ALPHADEST"}, {0x2, "ALPHADESTCONSTOVERRIDE"}, {0x4, "ALPHADESTNEG"},
    {0x8, "ALPHADESTSURFACEOVERRIDE"}, {0x10, "ALPHAEDGEBLEND"}, {0x20, "ALPHASRC"},
    {0x40, "ALPHASRCCONSTOVERRIDE"}, {0x80, "ALPHASRCNEG"}, {0x100, "ALPHASRCSURFACEOVERRIDE"},
    {0x200, "ASYNC"}, {0x400, "COLORFILL"}, {0x800, "DDFX"}, {0x1000, "DDROPS"},
    {0x2000, "KEYDEST"}, {0x4000, "KEYDESTOVERRIDE"}, {0x8000, "KEYSRC"},
    {0x10000, "KEYSRCOVERRIDE"}, {0x20000, "ROP"}, {0x40000, "ROTATIONANGLE"},
    {0x80000, "ZBUFFER"}, {0x1000000, "WAIT"}, {0x2000000, "DEPTHFILL"}, {0x8000000, "DONOTWAIT"}};
static const FlagName lock_names[] = {
    {0x1, "WAIT"}, {0x2, "EVENT"}, {0x10, "READONLY"}, {0x20, "WRITEONLY"}, {0x800, "NOSYSLOCK"},
    {0x1000, "NOOVERWRITE"}, {0x2000, "DISCARDCONTENTS"}, {0x4000, "DONOTWAIT"}};
static const FlagName ckey_names[] = {
    {0x1, "COLORSPACE"}, {0x2, "DESTBLT"}, {0x4, "DESTOVERLAY"}, {0x8, "SRCBLT"},
    {0x10, "SRCOVERLAY"}};

const char *pf_str(const uint32_t *pf, char *buf, size_t n)
{
    uint32_t fl = pf[1], bits = pf[3];
    if (fl & DDPF_ZBUFFER) {
        if (fl & DDPF_STENCILBUFFER)
            snprintf(buf, n, "Z%u(z %08x, stencil %08x)", bits, pf[5], pf[6]);
        else
            snprintf(buf, n, "Z%u(%08x)", bits, pf[5]);
    } else if (fl & DDPF_FOURCC) {
        snprintf(buf, n, "FOURCC '%c%c%c%c'", pf[2] & 0xff, pf[2] >> 8 & 0xff, pf[2] >> 16 & 0xff,
                 pf[2] >> 24);
    } else if (fl & DDPF_PALETTEINDEXED8) {
        snprintf(buf, n, "P8");
    } else if (fl & DDPF_RGB) {
        uint32_t a = fl & DDPF_ALPHAPIXELS ? pf[7] : 0;
        int nb[4];
        uint32_t m[4] = {pf[4], pf[5], pf[6], a};
        for (int i = 0; i < 4; i++)
            nb[i] = __builtin_popcount(m[i]);
        int pad = a ? nb[3] : (int)bits - nb[0] - nb[1] - nb[2];
        const char *order = pf[4] > pf[6] ? "RGB" : "BGR";
        int c0 = pf[4] > pf[6] ? nb[0] : nb[2], c2 = pf[4] > pf[6] ? nb[2] : nb[0];
        if (pad)
            snprintf(buf, n, "%c%s%d%d%d%d", a ? 'A' : 'X', order, pad, c0, nb[1], c2);
        else
            snprintf(buf, n, "%s%d%d%d", order, c0, nb[1], c2);
    } else {
        snprintf(buf, n, "pf(flags %x, %u bits)", fl, bits);
    }
    return buf;
}

const char *rect_str(uint32_t r, char *buf, size_t n)
{
    if (!r)
        return "NULL";
    snprintf(buf, n, "(%d,%d)-(%d,%d)", (int32_t)R32(r), (int32_t)R32(r + 4), (int32_t)R32(r + 8),
             (int32_t)R32(r + 12));
    return buf;
}

/* ------------------------------------------------------------------ GUIDs */
typedef struct { const char *name; uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } Guid;
static const Guid guids[G_COUNT] = {
    [G_IUNKNOWN] = {"IUnknown", 0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}},
    [G_IDIRECTDRAW] = {"IDirectDraw", 0x6c14db80, 0xa733, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}},
    [G_IDIRECTDRAW2] = {"IDirectDraw2", 0xb3a6f3e0, 0x2b43, 0x11cf, {0xa2, 0xde, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}},
    [G_IDIRECTDRAW4] = {"IDirectDraw4", 0x9c59509a, 0x39bd, 0x11d1, {0x8c, 0x4a, 0x00, 0xc0, 0x4f, 0xd9, 0x30, 0xc5}},
    [G_IDIRECTDRAW7] = {"IDirectDraw7", 0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}},
    [G_IDIRECTDRAWSURFACE] = {"IDirectDrawSurface", 0x6c14db81, 0xa733, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}},
    [G_IDIRECTDRAWSURFACE2] = {"IDirectDrawSurface2", 0x57805885, 0x6eec, 0x11cf, {0x94, 0x41, 0xa8, 0x23, 0x03, 0xc1, 0x0e, 0x27}},
    [G_IDIRECTDRAWSURFACE3] = {"IDirectDrawSurface3", 0xda044e00, 0x69b2, 0x11d0, {0xa1, 0xd5, 0x00, 0xaa, 0x00, 0xb8, 0xdf, 0xbb}},
    [G_IDIRECTDRAWSURFACE4] = {"IDirectDrawSurface4", 0x0b2b8630, 0xad35, 0x11d0, {0x8e, 0xa6, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}},
    [G_IDIRECTDRAWSURFACE7] = {"IDirectDrawSurface7", 0x06675a80, 0x3b9b, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}},
    [G_IDIRECTDRAWPALETTE] = {"IDirectDrawPalette", 0x6c14db84, 0xa733, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}},
    [G_IDIRECTDRAWCLIPPER] = {"IDirectDrawClipper", 0x6c14db85, 0xa733, 0x11ce, {0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60}},
    [G_IDIRECTDRAWCOLORCONTROL] = {"IDirectDrawColorControl", 0x4b9f0ee0, 0x0d7e, 0x11d0, {0x9b, 0x06, 0x00, 0xa0, 0xc9, 0x03, 0xa3, 0xb8}},
    [G_IDIRECTDRAWGAMMACONTROL] = {"IDirectDrawGammaControl", 0x69c11c3e, 0xb46b, 0x11d1, {0xad, 0x7a, 0x00, 0xc0, 0x4f, 0xc2, 0x9b, 0x4e}},
    [G_IDIRECT3D] = {"IDirect3D", 0x3bba0080, 0x2421, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}},
    [G_IDIRECT3D2] = {"IDirect3D2", 0x6aae1ec1, 0x662a, 0x11d0, {0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a}},
    [G_IDIRECT3D3] = {"IDirect3D3", 0xbb223240, 0xe72b, 0x11d0, {0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e}},
    [G_IDIRECT3D7] = {"IDirect3D7", 0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}},
    [G_IDIRECT3DDEVICE7] = {"IDirect3DDevice7", 0xf5049e79, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}},
    [G_IDIRECT3DVERTEXBUFFER7] = {"IDirect3DVertexBuffer7", 0xf5049e7d, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}},
    [G_IDIRECT3DTEXTURE] = {"IDirect3DTexture", 0x2cdcd9e0, 0x25a0, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}},
    [G_IDIRECT3DTEXTURE2] = {"IDirect3DTexture2", 0x93281502, 0x8cf8, 0x11d0, {0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}},
    [G_RGBDEVICE] = {"IID_IDirect3DRGBDevice", 0xa4665c60, 0x2673, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}},
    [G_HALDEVICE] = {"IID_IDirect3DHALDevice", 0x84e63de0, 0x46aa, 0x11cf, {0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e}},
    [G_TNLHALDEVICE] = {"IID_IDirect3DTnLHalDevice", 0xf5049e78, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}},
    [G_REFDEVICE] = {"IID_IDirect3DRefDevice", 0x50936643, 0x13e9, 0x11d1, {0x89, 0xaa, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}},
    [G_MMXDEVICE] = {"IID_IDirect3DMMXDevice", 0x881949a1, 0xd6f3, 0x11d0, {0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}},
    [G_NULLDEVICE] = {"IID_IDirect3DNullDevice", 0x8767df22, 0xbacc, 0x11d1, {0x89, 0x69, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}},
    [G_RAMPDEVICE] = {"IID_IDirect3DRampDevice", 0xf2086b20, 0x259f, 0x11cf, {0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56}},
};

int gfx_is_guid(uint32_t a, int id)
{
    const Guid *g = &guids[id];
    return guid_eq(a, g->d1, g->d2, g->d3, g->d4);
}

const char *gfx_guid_name(uint32_t a, char *buf, size_t n)
{
    if (!a)
        return "NULL";
    for (int i = 0; i < G_COUNT; i++)
        if (gfx_is_guid(a, i))
            return guids[i].name;
    snprintf(buf, n, "{" GUID_FMT "}", GUID_ARGS(a));
    return buf;
}

void gfx_write_guid(uint32_t dst, int id)
{
    const Guid *g = &guids[id];
    W32(dst, g->d1);
    W16(dst + 4, g->d2);
    W16(dst + 6, g->d3);
    memcpy(gptr(dst + 8), g->d4, 8);
}

/* ------------------------------------------------------------------ formats */
#define PF(fl, bits, r, g, b, a) {32, fl, 0, bits, r, g, b, a}
static const uint32_t fmt_565[8] = PF(DDPF_RGB, 16, 0xf800, 0x07e0, 0x001f, 0);
static const uint32_t fmt_x888[8] = PF(DDPF_RGB, 32, 0xff0000, 0xff00, 0xff, 0);
static const uint32_t fmt_p8[8] = PF(DDPF_RGB | DDPF_PALETTEINDEXED8, 8, 0, 0, 0, 0);
static const uint32_t fmt_888[8] = PF(DDPF_RGB, 24, 0xff0000, 0xff00, 0xff, 0);
/* what a GeForce2 offered through EnumTextureFormats (minus the DXTn FourCCs) */
const uint32_t gfx_tex_formats[][8] = {
    PF(DDPF_RGB, 16, 0x7c00, 0x03e0, 0x001f, 0),
    PF(DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7c00, 0x03e0, 0x001f, 0x8000),
    PF(DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0f00, 0x00f0, 0x000f, 0xf000),
    PF(DDPF_RGB, 16, 0xf800, 0x07e0, 0x001f, 0),
    PF(DDPF_RGB, 32, 0xff0000, 0xff00, 0xff, 0),
    PF(DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0xff0000, 0xff00, 0xff, 0xff000000u),
};
const int gfx_ntex_formats = COUNT(gfx_tex_formats);
/* The game keeps the deepest plain (no stencil) format, the last one wins: X8D24. */
const uint32_t gfx_z_formats[][8] = {
    {32, DDPF_ZBUFFER, 0, 16, 0, 0xffff, 0, 0},
    {32, DDPF_ZBUFFER, 0, 32, 0, 0xffffffffu, 0, 0},
    {32, DDPF_ZBUFFER, 0, 32, 0, 0x00ffffff, 0, 0},
    {32, DDPF_ZBUFFER | DDPF_STENCILBUFFER, 0, 32, 8, 0x00ffffff, 0xff000000u, 0},
};
const int gfx_nz_formats = COUNT(gfx_z_formats);

static const uint32_t *display_pf(uint32_t bpp)
{
    return bpp == 16 ? fmt_565 : bpp == 8 ? fmt_p8 : bpp == 24 ? fmt_888 : fmt_x888;
}

static const uint32_t mode_sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 1024}, {1600, 1200}};
static const uint32_t mode_bpps[] = {16, 32};
const uint32_t (*const ddraw_mode_sizes)[2] = mode_sizes;      /* for the Enhanced page */
const int ddraw_nmode_sizes = sizeof mode_sizes / sizeof mode_sizes[0];
#define DESKTOP_BPP 32u
#define DESKTOP_HZ 60u

/* ------------------------------------------------------------------ state */
struct DDraw {
    int id, refs, dead;
    uint32_t obj7, obj1;
    uint32_t hwnd, coop;
};

typedef struct Clipper { int refs; uint32_t obj, hwnd; } Clipper;
typedef struct Palette { int refs; uint32_t obj, caps; uint32_t entries[256]; } Palette;
typedef struct PrivData { struct PrivData *next; uint8_t guid[16]; uint32_t size, flags; uint8_t *data; } PrivData;

static pthread_mutex_t gfx_mutex;
static Surface *all_surfaces;
static int next_surface_id = 1, next_dd_id = 1;
static uint32_t vid_used, agp_used;
static unsigned long frames;
uint16_t gfx_gamma[3][256];
struct GfxMode gfx_mode;

static ComClass dd7_cls, dd1_cls, surf7_cls, surf1_cls, gamma_cls, clipper_cls, palette_cls;
static ComClass dead_dd7_cls, dead_dd1_cls, dead_surf7_cls, dead_surf1_cls, dead_gamma_cls,
    dead_clipper_cls, dead_palette_cls;
static uint32_t str_desc, str_name;

void gfx_lock(void) { pthread_mutex_lock(&gfx_mutex); }
void gfx_unlock(void) { pthread_mutex_unlock(&gfx_mutex); }

static void desktop_mode(uint32_t *w, uint32_t *h, uint32_t *bpp)
{
    int dw, dh;
    host_desktop_size(&dw, &dh);
    *w = (uint32_t)dw;
    *h = (uint32_t)dh;
    *bpp = DESKTOP_BPP;
}

static void current_mode(uint32_t *w, uint32_t *h, uint32_t *bpp, uint32_t *hz)
{
    if (gfx_mode.set) {
        *w = gfx_mode.w;
        *h = gfx_mode.h;
        *bpp = gfx_mode.bpp;
        *hz = gfx_mode.refresh ? gfx_mode.refresh : DESKTOP_HZ;
    } else {
        desktop_mode(w, h, bpp);
        *hz = DESKTOP_HZ;
    }
}

/* ------------------------------------------------------------------ DirectDraw objects */
uint32_t dd_coop_flags(DDraw *dd) { return dd->coop; }
void dd_addref(DDraw *dd) { dd->refs++; }

static DDraw *dd_new(void)
{
    DDraw *dd = calloc(1, sizeof *dd);
    if (!dd)
        fatal("out of memory");
    dd->id = next_dd_id++;
    dd->refs = 1;
    return dd;
}

static uint32_t dd_obj7(DDraw *dd)
{
    if (!dd->obj7)
        dd->obj7 = com_new(&dd7_cls, dd);
    return dd->obj7;
}

static uint32_t dd_obj1(DDraw *dd)
{
    if (!dd->obj1)
        dd->obj1 = com_new(&dd1_cls, dd);
    return dd->obj1;
}

void dd_release(DDraw *dd)
{
    if (--dd->refs > 0)
        return;
    dd->dead = 1;
    if (dd->obj7)
        com_set_class(dd->obj7, &dead_dd7_cls);
    if (dd->obj1)
        com_set_class(dd->obj1, &dead_dd1_cls);
    LOG(LOG_DDRAW, "DirectDraw object #%d destroyed", dd->id);
}

/* ------------------------------------------------------------------ surfaces */
const char *surface_name(const Surface *s, char *buf, size_t n)
{
    char pf[48];
    const char *kind = s->caps & DDSCAPS_PRIMARYSURFACE ? "primary"
                     : s->caps & DDSCAPS_BACKBUFFER ? "back"
                     : s->caps & DDSCAPS_ZBUFFER ? "z"
                     : s->caps & DDSCAPS_TEXTURE ? "tex"
                     : "plain";
    snprintf(buf, n, "surf#%d(%s%s %ux%u %s%s)", s->id, kind,
             s->caps & DDSCAPS_SYSTEMMEMORY ? " sys" : "", s->width, s->height,
             pf_str(s->pf, pf, sizeof pf), s->dead ? " RELEASED" : "");
    return buf;
}

Surface *surface_get(uint32_t obj)
{
    if (!obj)
        return NULL;
    return com_host(obj);
}

uint32_t surface_obj7(Surface *s)
{
    if (!s->obj7)
        s->obj7 = com_new(&surf7_cls, s);
    return s->obj7;
}

static uint32_t surface_obj1(Surface *s)
{
    if (!s->obj1)
        s->obj1 = com_new(&surf1_cls, s);
    return s->obj1;
}

void surface_addref(Surface *s)
{
    s->refs++;
}

Surface *surface_zbuffer(Surface *s)
{
    for (int i = 0; i < s->nattached; i++)
        if (s->attached[i]->caps & DDSCAPS_ZBUFFER)
            return s->attached[i];
    return NULL;
}

static void set_pixfmt(Surface *s, const uint32_t *pf)
{
    memcpy(s->pf, pf, 32);
    s->pf[0] = 32;
    s->bpp = (pf[3] + 7) / 8;
    memset(s->shift, 0, sizeof s->shift);
    memset(s->bits, 0, sizeof s->bits);
    s->zmask = s->stencilmask = 0;
    if (pf[1] & DDPF_ZBUFFER) {
        s->zmask = pf[5];
        s->stencilmask = pf[6];
        return;
    }
    uint32_t m[4] = {pf[4], pf[5], pf[6], pf[1] & DDPF_ALPHAPIXELS ? pf[7] : 0};
    if (pf[1] & (DDPF_PALETTEINDEXED8 | DDPF_FOURCC))
        return;
    for (int c = 0; c < 4; c++) {
        s->shift[c] = m[c] ? (uint8_t)__builtin_ctz(m[c]) : 0;
        s->bits[c] = (uint8_t)__builtin_popcount(m[c]);
    }
}

static void layout(Surface *s)
{
    s->pitch = (s->width * s->bpp + 3) & ~3u;
    s->memsize = s->pitch * s->height;
    if (!s->memsize)
        s->memsize = 4;
}

static int alloc_mem(Surface *s)
{
    if (s->memsize >= 0x10000) {
        s->mem = vm_alloc(0, s->memsize, 0);
        s->mem_kind = 2;
    } else {
        s->mem = galloc(s->memsize);
        s->mem_kind = 1;
    }
    return s->mem != 0;
}

static void free_mem(Surface *s)
{
    if (s->own_kind == 1)
        gfree(s->own_mem);
    else if (s->own_kind == 2)
        vm_free(s->own_mem, 0, 1);
    s->own_mem = 0;
    s->own_kind = 0;
    if (s->mem_kind == 1)
        gfree(s->mem);
    else if (s->mem_kind == 2)
        vm_free(s->mem, 0, 1);
    s->mem = 0;
    s->mem_kind = 0;
    if (s->vidmem) {
        if (s->nonlocal)
            agp_used -= s->vidmem;
        else
            vid_used -= s->vidmem;
        s->vidmem = 0;
    }
}

static Surface *surface_new(DDraw *dd)
{
    Surface *s = calloc(1, sizeof *s);
    if (!s)
        fatal("out of memory");
    s->id = next_surface_id++;
    s->dd = dd;
    s->uniq = 1;
    s->link = all_surfaces;
    all_surfaces = s;
    return s;
}

static void privdata_free(Surface *s)
{
    for (PrivData *p = s->privdata, *nx; p; p = nx) {
        nx = p->next;
        free(p->data);
        free(p);
    }
    s->privdata = NULL;
}

/* One member of a surface: its memory goes, its interfaces now only log and fail. */
static void surface_kill(Surface *s)
{
    char nm[96];
    LOG(LOG_DDRAW, "%s destroyed", surface_name(s, nm, sizeof nm));
    vk_surface_destroyed(s);
    free_mem(s);
    privdata_free(s);
    s->dead = 1;
    if (s->obj7)
        com_set_class(s->obj7, &dead_surf7_cls);
    if (s->obj1)
        com_set_class(s->obj1, &dead_surf1_cls);
    if (s->gamma_obj)
        com_set_class(s->gamma_obj, &dead_gamma_cls);
    for (Surface **pp = &all_surfaces; *pp; pp = &(*pp)->link)
        if (*pp == s) {
            *pp = s->link;
            break;
        }
}

/* Members created with a complex surface live until the root goes. */
void surface_release(Surface *s)
{
    if (--s->refs > 0)
        return;
    s->refs = 0;
    if (s->root || s->dead)
        return;
    Surface *chain[64];
    int n = 0;
    for (Surface *t = s; t && n < 64; t = t->next) {
        chain[n++] = t;
        if (t->next == s)
            break;
    }
    for (int i = 0; i < n; i++) {
        Surface *t = chain[i];
        for (int k = 0; k < t->nattached; k++)
            surface_release(t->attached[k]);
        t->nattached = 0;
        surface_kill(t);
    }
}

/* With the GPU drawing a render target, its program memory gets fills and copies too only in test
   runs (they compare memory; TD6_BENCH=1 measures without), as d3d.c's Clear does: otherwise a
   Lock reads the target back from the GPU (the GPU copy is the newer). Depth buffers, which are not
   read back, keep theirs. */
static int mirror_mem(const Surface *s)
{
    static int mirror = -1;
    if (mirror < 0)
        mirror = deterministic && !getenv("TD6_BENCH");
    return mirror || !vk_target(s);
}

/* GPU surfaces get the fill on the GPU too; the program's memory as above. */
void surface_fill(Surface *s, const int *rect, uint32_t raw)
{
    /* (s->gpu is the renderer's to look at: a depth buffer without a GPU image ignores the fill there) */
    if (vk_target(s) || ((s->caps & DDSCAPS_ZBUFFER) && vk_on()))
        vk_fill(s, rect, raw);
    else
        vk_cpu_access(s, 1);
    if (mirror_mem(s))
        surface_fill_mem(s, rect, raw);
}

void surface_fill_mem(Surface *s, const int *rect, uint32_t raw)
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
    for (int y = y0; y < y1; y++) {
        uint8_t *row = (uint8_t *)gptr(s->mem + (uint32_t)y * s->pitch);
        if (s->bpp == 1)
            memset(row + x0, (int)(raw & 0xff), (size_t)(x1 - x0));
        else if (s->bpp == 2) {
            uint16_t *p = (uint16_t *)(row + 2 * x0);
            for (int x = x0; x < x1; x++)
                *p++ = (uint16_t)raw;
        } else if (s->bpp == 4) {
            uint32_t *p = (uint32_t *)(row + 4 * x0);
            for (int x = x0; x < x1; x++)
                *p++ = raw;
        } else {
            for (int x = x0; x < x1; x++)
                surf_write(s, (uint32_t)x, (uint32_t)y, raw);
        }
    }
    s->dirty++;
}

static int same_format(const Surface *a, const Surface *b)
{
    return a->bpp == b->bpp && !memcmp(&a->pf[1], &b->pf[1], 7 * sizeof(uint32_t));
}

/* To a GPU surface: drawn there in call order, and copied in memory as without a GPU (in test runs;
   mirror_mem). */
void surface_copy(Surface *dst, const int *drect, Surface *src, const int *srect,
                  int srckey, uint32_t key_lo, uint32_t key_hi)
{
    int d[4] = {0, 0, (int)dst->width, (int)dst->height};
    int s[4] = {0, 0, (int)src->width, (int)src->height};
    if (drect)
        memcpy(d, drect, sizeof d);
    if (srect)
        memcpy(s, srect, sizeof s);
    if (d[2] <= d[0] || d[3] <= d[1] || s[2] <= s[0] || s[3] <= s[1] || !dst->mem || !src->mem)
        return;
    if (vk_target(dst)) {
        vk_blt(dst, d, src, s, srckey, key_lo, key_hi);
    } else {
        vk_cpu_access(src, 0);
        vk_cpu_access(dst, 1);
    }
    if (mirror_mem(dst))
        surface_copy_mem(dst, d, src, s, srckey, key_lo, key_hi);
}

void surface_copy_mem(Surface *dst, const int *drect, Surface *src, const int *srect,
                      int srckey, uint32_t key_lo, uint32_t key_hi)
{
    int d[4] = {0, 0, (int)dst->width, (int)dst->height};
    int s[4] = {0, 0, (int)src->width, (int)src->height};
    if (drect)
        memcpy(d, drect, sizeof d);
    if (srect)
        memcpy(s, srect, sizeof s);
    int dw = d[2] - d[0], dh = d[3] - d[1], sw = s[2] - s[0], sh = s[3] - s[1];
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || !dst->mem || !src->mem)
        return;
    int x0 = d[0] > 0 ? d[0] : 0, y0 = d[1] > 0 ? d[1] : 0;
    int x1 = d[2] < (int)dst->width ? d[2] : (int)dst->width;
    int y1 = d[3] < (int)dst->height ? d[3] : (int)dst->height;
    if (x0 >= x1 || y0 >= y1)
        return;
    /* the same surface on both sides: read from a snapshot */
    Surface tmp;
    uint8_t *snap = NULL;
    if (dst == src || (dst->mem < src->mem + src->memsize && src->mem < dst->mem + dst->memsize)) {
        snap = malloc(src->memsize);
        if (!snap)
            return;
        memcpy(snap, gptr(src->mem), src->memsize);
        tmp = *src;
        src = &tmp;
    }
    int same = same_format(dst, src);
    for (int y = y0; y < y1; y++) {
        int sy = s[1] + (int)((int64_t)(y - d[1]) * sh / dh);
        if (sy < 0 || sy >= (int)src->height)
            continue;
        const uint8_t *srow = snap ? snap + (uint32_t)sy * src->pitch
                                   : (const uint8_t *)gptr(src->mem + (uint32_t)sy * src->pitch);
        if (same && !srckey && dw == sw && s[0] + (x0 - d[0]) >= 0 && s[0] + (x1 - d[0]) <= (int)src->width) {
            memcpy(gptr(dst->mem + (uint32_t)y * dst->pitch + (uint32_t)x0 * dst->bpp),
                   srow + (uint32_t)(s[0] + (x0 - d[0])) * src->bpp, (size_t)(x1 - x0) * dst->bpp);
            continue;
        }
        for (int x = x0; x < x1; x++) {
            int sx = s[0] + (int)((int64_t)(x - d[0]) * sw / dw);
            if (sx < 0 || sx >= (int)src->width)
                continue;
            const uint8_t *sp = srow + (uint32_t)sx * src->bpp;
            uint32_t raw = src->bpp == 2 ? (uint32_t)(sp[0] | sp[1] << 8)
                         : src->bpp == 1 ? sp[0]
                         : src->bpp == 3 ? (uint32_t)(sp[0] | sp[1] << 8 | sp[2] << 16)
                         : (uint32_t)(sp[0] | sp[1] << 8 | sp[2] << 16 | (uint32_t)sp[3] << 24);
            if (srckey && raw >= key_lo && raw <= key_hi)
                continue;
            surf_write(dst, (uint32_t)x, (uint32_t)y, same ? raw : pix_encode(dst, pix_decode(src, raw)));
        }
    }
    free(snap);
    dst->dirty++;
}

/* Fill a DDSURFACEDESC / DDSURFACEDESC2 (by its dwSize) describing s. */
static void fill_desc(Surface *s, uint32_t d, uint32_t lpsurface)
{
    uint32_t size = R32(d) == SZ_DDSURFACEDESC ? SZ_DDSURFACEDESC : SZ_DDSURFACEDESC2;
    memset(gptr(d + 4), 0, size - 4);
    uint32_t fl = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    W32(d + 8, s->height);
    W32(d + 0xc, s->width);
    W32(d + 0x10, s->pitch);
    if (s->backbuffers) {
        fl |= DDSD_BACKBUFFERCOUNT;
        W32(d + 0x14, (uint32_t)s->backbuffers);
    }
    if (s->caps & DDSCAPS_MIPMAP) {
        fl |= DDSD_MIPMAPCOUNT;
        W32(d + 0x18, (uint32_t)(s->mip_count - s->mip_level));
    } else if ((s->caps & DDSCAPS_ZBUFFER) && size == SZ_DDSURFACEDESC) {
        fl |= DDSD_ZBUFFERBITDEPTH;
        W32(d + 0x18, (uint32_t)__builtin_popcount(s->zmask));
    }
    if (lpsurface) {
        fl |= DDSD_LPSURFACE;
        W32(d + 0x24, lpsurface);
    }
    static const uint32_t ckflag[4] = {DDSD_CKDESTBLT, DDSD_CKDESTOVERLAY, DDSD_CKSRCBLT, DDSD_CKSRCOVERLAY};
    static const uint32_t ckoff[4] = {0x30, 0x28, 0x40, 0x38};
    for (int i = 0; i < 4; i++)
        if (s->ck_set & (DDCKEY_DESTBLT << i)) {
            fl |= ckflag[i];
            W32(d + ckoff[i], s->ck[i][0]);
            W32(d + ckoff[i] + 4, s->ck[i][1]);
        }
    memcpy(gptr(d + 0x48), s->pf, 32);
    W32(d + 0x68, s->caps);
    if (size == SZ_DDSURFACEDESC2) {
        W32(d + 0x6c, s->caps2);
        W32(d + 0x70, s->caps3);
        W32(d + 0x74, s->caps4);
    }
    W32(d + 4, fl);
}

static int pf_valid(const uint32_t *pf)
{
    uint32_t fl = pf[1], bits = pf[3];
    if (fl & DDPF_ZBUFFER)
        return (bits == 16 || bits == 24 || bits == 32) && pf[5];
    if (fl & DDPF_FOURCC)
        return 0;
    if (fl & DDPF_PALETTEINDEXED8)
        return bits == 8;
    if (fl & DDPF_RGB)
        return bits == 8 || bits == 16 || bits == 24 || bits == 32;
    return 0;
}

static int charge(Surface *s, uint32_t bytes, int texture)
{
    if (!(s->caps & DDSCAPS_NONLOCALVIDMEM) && vid_used + bytes <= VIDMEM_LOCAL) {
        vid_used += bytes;
        s->caps |= DDSCAPS_LOCALVIDMEM;
        return 1;
    }
    if (texture && !(s->caps & DDSCAPS_LOCALVIDMEM) && agp_used + bytes <= VIDMEM_AGP) {
        agp_used += bytes;
        s->caps |= DDSCAPS_NONLOCALVIDMEM;
        s->nonlocal = 1;
        return 1;
    }
    return 0;
}

static uint32_t create_surface(DDraw *dd, uint32_t d, int v7, uint32_t pp, const char *who)
{
    char t1[256], t2[96], t3[48], t4[64];
    if (!d || !pp)
        return DDERR_INVALIDPARAMS;
    uint32_t size = R32(d);
    if (size != (v7 ? SZ_DDSURFACEDESC2 : SZ_DDSURFACEDESC)) {
        LOG(LOG_ALWAYS, "%s::CreateSurface: descriptor size %u", who, size);
        return DDERR_INVALIDPARAMS;
    }
    uint32_t fl = R32(d + 4);
    uint32_t caps = fl & DDSD_CAPS ? R32(d + 0x68) : 0;
    uint32_t caps2 = (fl & DDSD_CAPS) && v7 ? R32(d + 0x6c) : 0;
    uint32_t w = fl & DDSD_WIDTH ? R32(d + 0xc) : 0, h = fl & DDSD_HEIGHT ? R32(d + 8) : 0;
    uint32_t pf[8];
    int backbuffers = fl & DDSD_BACKBUFFERCOUNT ? (int)R32(d + 0x14) : 0;
    int mips = 1;
    W32(pp, 0);
    LOG(LOG_DDRAW, "%s::CreateSurface(%s, caps %s%s%s, %ux%u%s%s, backbuffers %d, mips %d)", who,
        FLAGS(fl, ddsd_names, t1), FLAGS(caps, caps_names, t2), caps2 ? " caps2 " : "",
        caps2 ? FLAGS(caps2, caps2_names, t3) : "", w, h, fl & DDSD_PIXELFORMAT ? " " : "",
        fl & DDSD_PIXELFORMAT ? pf_str((const uint32_t *)gptr(d + 0x48), t4, sizeof t4) : "",
        backbuffers, fl & DDSD_MIPMAPCOUNT ? (int)R32(d + 0x18) : 0);

    if (caps & DDSCAPS_PRIMARYSURFACE) {
        uint32_t bpp, hz;
        if (fl & (DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT))
            return DDERR_INVALIDPARAMS;
        if ((dd->coop & DDSCL_EXCLUSIVE) && gfx_mode.set)
            current_mode(&w, &h, &bpp, &hz);
        else
            desktop_mode(&w, &h, &bpp);
        memcpy(pf, display_pf(bpp), sizeof pf);
        if (caps & DDSCAPS_FLIP) {
            if (!(caps & DDSCAPS_COMPLEX) || backbuffers < 1 || backbuffers > 3)
                return DDERR_INVALIDCAPS;
            if (!(dd->coop & DDSCL_EXCLUSIVE))
                return DDERR_NOEXCLUSIVEMODE;
            caps |= DDSCAPS_FRONTBUFFER;
        } else {
            backbuffers = 0;
        }
        caps |= DDSCAPS_VISIBLE;
    } else {
        backbuffers = 0;
        if (!w || !h)
            return DDERR_INVALIDPARAMS;
        if (fl & DDSD_PIXELFORMAT) {
            memcpy(pf, gptr(d + 0x48), sizeof pf);
        } else if (caps & DDSCAPS_ZBUFFER) {
            uint32_t depth = !v7 && (fl & DDSD_ZBUFFERBITDEPTH) ? R32(d + 0x18) : 16;
            memcpy(pf, gfx_z_formats[depth == 32 ? 1 : depth == 24 ? 2 : 0], sizeof pf);
        } else {
            uint32_t mw, mh, bpp, hz;
            current_mode(&mw, &mh, &bpp, &hz);
            memcpy(pf, display_pf(bpp), sizeof pf);
        }
        if (!pf_valid(pf) || ((caps & DDSCAPS_ZBUFFER) != 0) != ((pf[1] & DDPF_ZBUFFER) != 0)) {
            LOG(LOG_ALWAYS, "%s::CreateSurface: unsupported pixel format %s", who, pf_str(pf, t1, sizeof t1));
            return DDERR_INVALIDPIXELFORMAT;
        }
        if ((caps & DDSCAPS_TEXTURE) && (w > MAX_TEXTURE || h > MAX_TEXTURE))
            return DDERR_TOOBIGSIZE;
        if ((caps & DDSCAPS_MIPMAP) && (caps & DDSCAPS_COMPLEX)) {
            int full = 1;
            for (uint32_t m = w > h ? w : h; m > 1; m >>= 1)
                full++;
            mips = fl & DDSD_MIPMAPCOUNT ? (int)R32(d + 0x18) : full;
            if (mips < 1 || mips > full)
                return DDERR_INVALIDPARAMS;
        }
    }
    int managed = (caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)) != 0;
    int video = !(caps & DDSCAPS_SYSTEMMEMORY) && !managed;
    if (video)
        caps |= DDSCAPS_VIDEOMEMORY;
    if ((fl & DDSD_LPSURFACE) && (video || backbuffers || mips > 1))
        return DDERR_INVALIDPARAMS;

    /* build the chain: front + back buffers, or the mipmap levels */
    int n = backbuffers ? 1 + backbuffers : mips;
    Surface *chain[16];
    if (n > 16)
        return DDERR_INVALIDPARAMS;
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        Surface *s = surface_new(dd);
        chain[i] = s;
        s->caps = caps;
        s->caps2 = caps2;
        s->width = w;
        s->height = h;
        if (backbuffers && i > 0) {
            s->caps &= ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE);
            if (i == 1)
                s->caps |= DDSCAPS_BACKBUFFER;
        }
        if (mips > 1) {
            s->width = w >> i ? w >> i : 1;
            s->height = h >> i ? h >> i : 1;
            s->mip_level = i;
            s->mip_count = mips;
            if (i > 0)
                s->caps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
        }
        set_pixfmt(s, pf);
        layout(s);
        if (i > 0)
            s->root = chain[0];
        total += s->memsize;
    }
    chain[0]->refs = 1;
    chain[0]->backbuffers = backbuffers;
    for (int i = 0; i + 1 < n; i++)
        chain[i]->next = chain[i + 1];
    if (backbuffers)
        chain[n - 1]->next = chain[0];
    if (video) {
        Surface probe = {.caps = chain[0]->caps};
        if (!charge(&probe, total, (caps & DDSCAPS_TEXTURE) != 0)) {
            LOG(LOG_DDRAW, "%s::CreateSurface: out of video memory (%u KB asked, %u/%u KB local and %u/%u KB AGP used)",
                who, total >> 10, vid_used >> 10, VIDMEM_LOCAL >> 10, agp_used >> 10, VIDMEM_AGP >> 10);
            for (int i = 0; i < n; i++)
                surface_kill(chain[i]);
            return DDERR_OUTOFVIDEOMEMORY;
        }
        for (int i = 0; i < n; i++) {
            chain[i]->caps |= probe.caps & (DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM);
            chain[i]->vidmem = chain[i]->memsize;
            chain[i]->nonlocal = probe.nonlocal;
        }
    }
    for (int i = 0; i < n; i++) {
        Surface *s = chain[i];
        if (i == 0 && (fl & DDSD_LPSURFACE)) {
            s->mem = R32(d + 0x24);
            s->mem_kind = 0;
            if (fl & DDSD_PITCH)
                s->pitch = R32(d + 0x10);
            continue;
        }
        if (!alloc_mem(s)) {
            for (int k = 0; k < n; k++)
                surface_kill(chain[k]);
            return DDERR_OUTOFMEMORY;
        }
    }
    static const uint32_t ckflag[4] = {DDSD_CKDESTBLT, DDSD_CKDESTOVERLAY, DDSD_CKSRCBLT, DDSD_CKSRCOVERLAY};
    static const uint32_t ckoff[4] = {0x30, 0x28, 0x40, 0x38};
    for (int i = 0; i < 4; i++)
        if (fl & ckflag[i]) {
            chain[0]->ck[i][0] = chain[0]->ck[i][1] = R32(d + ckoff[i]);
            if (R32(d + ckoff[i] + 4) > chain[0]->ck[i][0])
                chain[0]->ck[i][1] = R32(d + ckoff[i] + 4);
            chain[0]->ck_set |= DDCKEY_DESTBLT << i;
        }
    uint32_t obj = v7 ? surface_obj7(chain[0]) : surface_obj1(chain[0]);
    W32(pp, obj);
    for (int i = 0; i < n; i++)
        LOG(LOG_DDRAW, "  -> %s at %08x pitch %u%s", surface_name(chain[i], t2, sizeof t2), chain[i]->mem,
            chain[i]->pitch, i == 0 ? "" : " (in chain)");
    LOG(LOG_DDRAW, "  -> %08x; video memory used %u KB local, %u KB AGP", obj, vid_used >> 10, agp_used >> 10);
    return DD_OK;
}

/* ------------------------------------------------------------------ presenting */
static void present(Surface *src, const int *rect)
{
    frames++;
    if (vk_on())
        vk_present(src, rect);
    else
        swrast_present(src, rect);
    d3d_frame_stats(frames);
}

/* frame_presented() may end the process: never call it with the lock held */
static void presented_unlocked(void)
{
    gfx_unlock();
    frame_presented();
    gfx_lock();
}

/* ------------------------------------------------------------------ IDirectDraw7 / IDirectDraw */
#define DDOBJ(o) ((DDraw *)com_host(o))

static uint32_t dd_qi(CPU *c, const char *who)
{
    DDraw *dd = DDOBJ(ARG(0));
    uint32_t riid = ARG(1), ppv = ARG(2);
    char g[64];
    if (!ppv)
        return DDERR_INVALIDPARAMS;
    W32(ppv, 0);
    uint32_t r = 0;
    if (gfx_is_guid(riid, G_IUNKNOWN))
        r = ARG(0);
    else if (gfx_is_guid(riid, G_IDIRECTDRAW7))
        r = dd_obj7(dd);
    else if (gfx_is_guid(riid, G_IDIRECTDRAW))
        r = dd_obj1(dd);
    else if (gfx_is_guid(riid, G_IDIRECT3D7)) {
        r = d3d_create(dd);
        W32(ppv, r);
        LOG(LOG_DDRAW, "%s::QueryInterface(%s) -> %08x", who, gfx_guid_name(riid, g, sizeof g), r);
        return DD_OK;
    }
    if (!r) {
        LOG(LOG_ALWAYS, "%s::QueryInterface(%s): not implemented", who, gfx_guid_name(riid, g, sizeof g));
        return E_NOINTERFACE;
    }
    dd->refs++;
    W32(ppv, r);
    LOG(LOG_DDRAW, "%s::QueryInterface(%s) -> %08x", who, gfx_guid_name(riid, g, sizeof g), r);
    return DD_OK;
}
GMETHOD(dd7_QueryInterface, 3) { return dd_qi(c, "IDirectDraw7"); }
GMETHOD(dd1_QueryInterface, 3) { return dd_qi(c, "IDirectDraw"); }

GMETHOD(dd_AddRef, 1)
{
    DDraw *dd = DDOBJ(ARG(0));
    LOG(LOG_DDRAW, "IDirectDraw#%d::AddRef -> %d", dd->id, dd->refs + 1);
    return (uint32_t)++dd->refs;
}

GMETHOD(dd_Release, 1)
{
    DDraw *dd = DDOBJ(ARG(0));
    int r = dd->refs - 1;
    LOG(LOG_DDRAW, "IDirectDraw#%d::Release -> %d", dd->id, r);
    dd_release(dd);
    return (uint32_t)(r > 0 ? r : 0);
}

GMETHOD(dd_Compact, 1)
{
    LOG(LOG_DDRAW, "IDirectDraw::Compact");
    return DD_OK;
}

GMETHOD(dd_CreateClipper, 4)
{
    uint32_t flags = ARG(1), pp = ARG(2);
    if (!pp)
        return DDERR_INVALIDPARAMS;
    if (ARG(3))
        return CLASS_E_NOAGGREGATION;
    Clipper *cl = calloc(1, sizeof *cl);
    if (!cl)
        fatal("out of memory");
    cl->refs = 1;
    cl->obj = com_new(&clipper_cls, cl);
    W32(pp, cl->obj);
    LOG(LOG_DDRAW, "IDirectDraw::CreateClipper(%x) -> %08x", flags, cl->obj);
    return DD_OK;
}

GMETHOD(dd_CreatePalette, 5)
{
    uint32_t flags = ARG(1), entries = ARG(2), pp = ARG(3);
    if (!pp)
        return DDERR_INVALIDPARAMS;
    if (ARG(4))
        return CLASS_E_NOAGGREGATION;
    Palette *p = calloc(1, sizeof *p);
    if (!p)
        fatal("out of memory");
    p->refs = 1;
    p->caps = flags;
    int n = flags & DDPCAPS_8BIT ? 256 : flags & 2 ? 16 : flags & 1 ? 4 : flags & 0x800 ? 2 : 256;
    if (entries)
        for (int i = 0; i < n; i++)
            p->entries[i] = R32(entries + 4 * (uint32_t)i);
    p->obj = com_new(&palette_cls, p);
    W32(pp, p->obj);
    LOG(LOG_DDRAW, "IDirectDraw::CreatePalette(%x, %d entries) -> %08x", flags, n, p->obj);
    return DD_OK;
}

GMETHOD(dd7_CreateSurface, 4)
{
    if (ARG(3))
        return CLASS_E_NOAGGREGATION;
    return create_surface(DDOBJ(ARG(0)), ARG(1), 1, ARG(2), "IDirectDraw7");
}

GMETHOD(dd1_CreateSurface, 4)
{
    if (ARG(3))
        return CLASS_E_NOAGGREGATION;
    return create_surface(DDOBJ(ARG(0)), ARG(1), 0, ARG(2), "IDirectDraw");
}

GMETHOD(dd_DuplicateSurface, 3)
{
    LOG(LOG_ALWAYS, "IDirectDraw::DuplicateSurface(%08x): not implemented", ARG(1));
    if (ARG(2))
        W32(ARG(2), 0);
    return DDERR_UNSUPPORTED;
}

static void mode_desc(uint32_t d, uint32_t w, uint32_t h, uint32_t bpp, uint32_t hz)
{
    uint32_t size = R32(d) == SZ_DDSURFACEDESC ? SZ_DDSURFACEDESC : SZ_DDSURFACEDESC2;
    memset(gptr(d + 4), 0, size - 4);
    W32(d + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
    W32(d + 8, h);
    W32(d + 0xc, w);
    W32(d + 0x10, (w * (bpp / 8) + 3) & ~3u);
    W32(d + 0x18, hz);
    memcpy(gptr(d + 0x48), display_pf(bpp), 32);
}

static uint32_t enum_modes(CPU *c, int v7)
{
    uint32_t flags = ARG(1), filter = ARG(2), ctx = ARG(3), cb = ARG(4);
    if (!cb)
        return DDERR_INVALIDPARAMS;
    uint32_t ff = filter ? R32(filter + 4) : 0;
    static const uint32_t rates[] = {60, 75, 85};
    int nrates = flags & DDEDM_REFRESHRATES ? 3 : 1;
    LOG(LOG_DDRAW, "IDirectDraw%s::EnumDisplayModes(flags %x, filter %s, cb %08x)", v7 ? "7" : "", flags,
        filter ? "yes" : "none", cb);
    uint32_t d = galloc(SZ_DDSURFACEDESC2);
    int count = 0;
    for (int i = 0; i < COUNT(mode_sizes); i++)
        for (int b = 0; b < COUNT(mode_bpps); b++)
            for (int r = 0; r < nrates; r++) {
                uint32_t w = mode_sizes[i][0], h = mode_sizes[i][1], bpp = mode_bpps[b];
                uint32_t hz = nrates > 1 ? rates[r] : 0;
                if (((ff & DDSD_WIDTH) && R32(filter + 0xc) != w) || ((ff & DDSD_HEIGHT) && R32(filter + 8) != h) ||
                    ((ff & DDSD_PIXELFORMAT) && R32(filter + 0x54) != bpp) ||
                    ((ff & DDSD_REFRESHRATE) && R32(filter + 0x18) && R32(filter + 0x18) != hz))
                    continue;
                W32(d, v7 ? SZ_DDSURFACEDESC2 : SZ_DDSURFACEDESC);
                mode_desc(d, w, h, bpp, hz);
                count++;
                gfx_unlock();
                uint32_t ret = guest_call(c, cb, 2, d, ctx);
                gfx_lock();
                if (ret == DDENUMRET_CANCEL)
                    goto done;
            }
done:
    gfree(d);
    LOG(LOG_DDRAW, "  %d modes enumerated", count);
    return DD_OK;
}
GMETHOD(dd7_EnumDisplayModes, 5) { return enum_modes(c, 1); }
GMETHOD(dd1_EnumDisplayModes, 5) { return enum_modes(c, 0); }

GMETHOD(dd_EnumSurfaces, 5)
{
    LOG(LOG_ALWAYS, "IDirectDraw::EnumSurfaces(%x): not implemented", ARG(1));
    return DDERR_UNSUPPORTED;
}

GMETHOD(dd_FlipToGDISurface, 1)
{
    LOG(LOG_DDRAW, "IDirectDraw::FlipToGDISurface");
    return DD_OK;
}

static void fill_ddcaps(uint32_t p, int hel)
{
    uint32_t size = R32(p);
    uint8_t b[SZ_DDCAPS];
    memset(b, 0, sizeof b);
#define CAP(off, v) do { uint32_t _v = (v); memcpy(b + (off), &_v, 4); } while (0)
    uint32_t blt = 0x40u | 0x200u | 0x04000000u | 0x00400000u | 0x20000000u | 0x40000000u | 0x80000000u;
    uint32_t surfcaps = DDSCAPS_ALPHA | DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP |
                        DDSCAPS_FRONTBUFFER | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PRIMARYSURFACE |
                        DDSCAPS_SYSTEMMEMORY | DDSCAPS_VISIBLE | DDSCAPS_ZBUFFER;
    CAP(0, size);
    if (!hel) {
        /* 3D | BLT | BLTSTRETCH | GDI | READSCANLINE | ZBLTS | COLORKEY | COLORKEYHWASSIST |
           BLTCOLORFILL | BLTDEPTHFILL | CANCLIP | CANCLIPSTRETCHED | CANBLTSYSMEM */
        CAP(0x04, blt | 0x1u | 0x400u | 0x20000u | 0x100000u | 0x01000000u | 0x10000000u);
        /* CERTIFIED | NONLOCALVIDMEM | NONLOCALVIDMEMCAPS | NOPAGELOCKREQUIRED | WIDESURFACES |
           COPYFOURCC | PRIMARYGAMMA | CANRENDERWINDOWED | FLIPINTERVAL | FLIPNOVSYNC |
           CANMANAGETEXTURE */
        CAP(0x08, 0x1u | 0x200u | 0x400u | 0x800u | 0x1000u | 0x8000u | 0x20000u | 0x80000u |
                  0x200000u | 0x400000u | 0x800000u);
        surfcaps |= DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY | DDSCAPS_MIPMAP |
                    DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM;
        CAP(0x38, DDBD_16 | DDBD_24 | DDBD_32);
        CAP(0x3c, VIDMEM_LOCAL);
        CAP(0x40, VIDMEM_LOCAL - vid_used);
    } else {
        CAP(0x04, blt | 0x400u | 0x8000u);
        CAP(0x08, 0x1000u);
        CAP(0x38, DDBD_16 | DDBD_24 | DDBD_32);
    }
    CAP(0x0c, 0x200);                         /* DDCKEYCAPS_SRCBLT */
    /* DDFXCAPS: arithmetic stretch Y, mirrors, shrink/stretch X/Y (and the integer variants) */
    CAP(0x10, 0x20u | 0x40u | 0x80u | 0x2000u | 0x4000u | 0x8000u | 0x10000u | 0x20000u | 0x40000u |
              0x80000u | 0x100000u);
    CAP(0x64 + 4 * (0xcc / 32), 1u << (0xcc % 32));        /* SRCCOPY */
    CAP(0x84, surfcaps);
    CAP(0xac, blt);                           /* system -> video */
    CAP(0xb0, 0x200);
    CAP(0xb8 + 4 * (0xcc / 32), 1u << (0xcc % 32));
    CAP(0xd8, blt);                           /* video -> system */
    CAP(0xdc, 0x200);
    CAP(0xe4 + 4 * (0xcc / 32), 1u << (0xcc % 32));
    CAP(0x104, blt);                          /* system -> system */
    CAP(0x108, 0x200);
    CAP(0x110 + 4 * (0xcc / 32), 1u << (0xcc % 32));
    if (!hel) {
        CAP(0x13c, blt);                      /* AGP -> local */
        CAP(0x144, 0x200);
        CAP(0x14c + 4 * (0xcc / 32), 1u << (0xcc % 32));
    }
    CAP(0x16c, surfcaps);
#undef CAP
    uint32_t n = size < SZ_DDCAPS ? size : SZ_DDCAPS;
    memcpy(gptr(p), b, n);
}

GMETHOD(dd_GetCaps, 3)
{
    uint32_t drv = ARG(1), hel = ARG(2);
    if (!drv && !hel)
        return DDERR_INVALIDPARAMS;
    for (int i = 0; i < 2; i++) {
        uint32_t p = i ? hel : drv;
        if (p && R32(p) != SZ_DDCAPS && R32(p) != 316 && R32(p) != 364) {
            LOG(LOG_ALWAYS, "IDirectDraw::GetCaps: DDCAPS size %u", R32(p));
            return DDERR_INVALIDPARAMS;
        }
    }
    if (drv)
        fill_ddcaps(drv, 0);
    if (hel)
        fill_ddcaps(hel, 1);
    LOG(LOG_DDRAW, "IDirectDraw::GetCaps(%s%s) -> GeForce2-class, %u KB video memory free", drv ? "driver" : "",
        hel ? " HEL" : "", (VIDMEM_LOCAL - vid_used) >> 10);
    return DD_OK;
}

GMETHOD(dd_GetDisplayMode, 2)
{
    uint32_t d = ARG(1);
    if (!d || (R32(d) != SZ_DDSURFACEDESC && R32(d) != SZ_DDSURFACEDESC2))
        return DDERR_INVALIDPARAMS;
    uint32_t w, h, bpp, hz;
    current_mode(&w, &h, &bpp, &hz);
    mode_desc(d, w, h, bpp, hz);
    LOG(LOG_DDRAW, "IDirectDraw::GetDisplayMode -> %ux%ux%u %u Hz%s", w, h, bpp, hz, gfx_mode.set ? "" : " (desktop)");
    return DD_OK;
}

GMETHOD(dd_GetFourCCCodes, 3)
{
    if (ARG(1))
        W32(ARG(1), 0);
    LOG(LOG_DDRAW, "IDirectDraw::GetFourCCCodes -> none");
    return DD_OK;
}

static Surface *find_primary(DDraw *dd)
{
    for (Surface *s = all_surfaces; s; s = s->link)
        if (s->dd == dd && (s->caps & DDSCAPS_PRIMARYSURFACE) && !s->dead)
            return s;
    return NULL;
}

static uint32_t get_gdi_surface(CPU *c, int v7)
{
    DDraw *dd = DDOBJ(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    Surface *s = find_primary(dd);
    LOG(LOG_DDRAW, "IDirectDraw::GetGDISurface -> %s", s ? "primary" : "none");
    if (!s) {
        W32(ARG(1), 0);
        return DDERR_NOTFOUND;
    }
    s->refs++;
    W32(ARG(1), v7 ? surface_obj7(s) : surface_obj1(s));
    return DD_OK;
}
GMETHOD(dd7_GetGDISurface, 2) { return get_gdi_surface(c, 1); }
GMETHOD(dd1_GetGDISurface, 2) { return get_gdi_surface(c, 0); }

GMETHOD(dd_GetMonitorFrequency, 2)
{
    uint32_t w, h, bpp, hz;
    current_mode(&w, &h, &bpp, &hz);
    if (ARG(1))
        W32(ARG(1), hz);
    LOG(LOG_DDRAW, "IDirectDraw::GetMonitorFrequency -> %u", hz);
    return DD_OK;
}

GMETHOD(dd_GetScanLine, 2)
{
    uint32_t w, h, bpp, hz;
    current_mode(&w, &h, &bpp, &hz);
    uint32_t total = h + h / 20;
    uint32_t line = (uint32_t)(now_ms() * hz * total / 1000.0) % total;
    if (ARG(1))
        W32(ARG(1), line < h ? line : h - 1);
    LOG(LOG_DDRAW, "IDirectDraw::GetScanLine -> %u", line);
    return line < h ? DD_OK : DDERR_VERTICALBLANKINPROGRESS;
}

GMETHOD(dd_GetVerticalBlankStatus, 2)
{
    if (ARG(1))
        W32(ARG(1), 0);
    LOG(LOG_DDRAW, "IDirectDraw::GetVerticalBlankStatus -> FALSE");
    return DD_OK;
}

GMETHOD(dd_Initialize, 2)
{
    LOG(LOG_DDRAW, "IDirectDraw::Initialize -> already initialized");
    return DDERR_ALREADYINITIALIZED;
}

GMETHOD(dd_RestoreDisplayMode, 1)
{
    LOG(LOG_DDRAW, "IDirectDraw::RestoreDisplayMode (was %ux%ux%u)", gfx_mode.w, gfx_mode.h, gfx_mode.bpp);
    vk_sync();                          /* (the renderer reads the mode) */
    gfx_mode.set = 0;
    return DD_OK;
}

GMETHOD(dd_SetCooperativeLevel, 3)
{
    DDraw *dd = DDOBJ(ARG(0));
    uint32_t hwnd = ARG(1), flags = ARG(2);
    char f[160];
    LOG(LOG_DDRAW, "IDirectDraw#%d::SetCooperativeLevel(hwnd %08x, %s)", dd->id, hwnd, FLAGS(flags, scl_names, f));
    if (((flags & DDSCL_FULLSCREEN) != 0) != ((flags & DDSCL_EXCLUSIVE) != 0) ||
        ((flags & DDSCL_NORMAL) && (flags & DDSCL_EXCLUSIVE)) ||
        ((flags & DDSCL_FPUSETUP) && (flags & DDSCL_FPUPRESERVE)))
        return DDERR_INVALIDPARAMS;
    dd->hwnd = hwnd;
    dd->coop = flags;
    return DD_OK;
}

static uint32_t set_mode(CPU *c, uint32_t w, uint32_t h, uint32_t bpp, uint32_t hz, uint32_t flags, const char *who)
{
    int ok = 0;
    for (int i = 0; i < COUNT(mode_sizes); i++)
        for (int b = 0; b < COUNT(mode_bpps); b++)
            if (mode_sizes[i][0] == w && mode_sizes[i][1] == h && mode_bpps[b] == bpp)
                ok = 1;
    if (hz && hz != 60 && hz != 75 && hz != 85)
        ok = 0;
    LOG(LOG_DDRAW, "%s::SetDisplayMode(%ux%ux%u, %u Hz, flags %x)%s", who, w, h, bpp, hz, flags, ok ? "" : " -> unsupported");
    if (!ok)
        return DDERR_INVALIDMODE;
    vk_sync();                          /* (the renderer reads the mode) */
    gfx_mode.w = w;
    gfx_mode.h = h;
    gfx_mode.bpp = bpp;
    gfx_mode.refresh = hz;
    gfx_mode.set = 1;
    host_display_mode((int)w, (int)h);
    return DD_OK;
}
GMETHOD(dd7_SetDisplayMode, 6) { return set_mode(c, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), "IDirectDraw7"); }
GMETHOD(dd1_SetDisplayMode, 4) { return set_mode(c, ARG(1), ARG(2), ARG(3), 0, 0, "IDirectDraw"); }

GMETHOD(dd_WaitForVerticalBlank, 3)
{
    LOG(LOG_DDRAW, "IDirectDraw::WaitForVerticalBlank(%x)", ARG(1));
    return DD_OK;
}

GMETHOD(dd_GetAvailableVidMem, 4)
{
    uint32_t caps = ARG(1) ? R32(ARG(1)) : 0, total = ARG(2), fr = ARG(3);
    int agp = (caps & DDSCAPS_NONLOCALVIDMEM) != 0;
    uint32_t t = agp ? VIDMEM_AGP : VIDMEM_LOCAL, u = agp ? agp_used : vid_used;
    if (total)
        W32(total, t);
    if (fr)
        W32(fr, t - u);
    char f[200];
    LOG(LOG_DDRAW, "IDirectDraw7::GetAvailableVidMem(%s) -> total %u KB, free %u KB", FLAGS(caps, caps_names, f),
        t >> 10, (t - u) >> 10);
    return DD_OK;
}

GMETHOD(dd_GetSurfaceFromDC, 3)
{
    LOG(LOG_DDRAW, "IDirectDraw7::GetSurfaceFromDC(%08x) -> not found", ARG(1));
    if (ARG(2))
        W32(ARG(2), 0);
    return DDERR_NOTFOUND;
}

GMETHOD(dd_RestoreAllSurfaces, 1)
{
    LOG(LOG_DDRAW, "IDirectDraw7::RestoreAllSurfaces");
    return DD_OK;
}

GMETHOD(dd_TestCooperativeLevel, 1)
{
    LOG(LOG_DDRAW, "IDirectDraw7::TestCooperativeLevel -> OK");
    return DD_OK;
}

GMETHOD(dd_GetDeviceIdentifier, 3)
{
    uint32_t p = ARG(1);
    if (!p)
        return DDERR_INVALIDPARAMS;
    memset(gptr(p), 0, 1068);
    gstrncpy(p, "nv4_disp.dll", 512);
    gstrncpy(p + 512, "NVIDIA GeForce2 GTS", 512);
    W32(p + 1024, (10u << 16) | 700);         /* driver 6.13.10.700 */
    W32(p + 1028, (6u << 16) | 13);
    W32(p + 1032, 0x10de);
    W32(p + 1036, 0x0150);
    W32(p + 1048, 0xd7b71f83);
    W16(p + 1052, 0x6340);
    W16(p + 1054, 0x11cf);
    W32(p + 1056, 0x7d76ea4e);
    W32(p + 1060, 0xc2d6d311);
    W32(p + 1064, 1);
    LOG(LOG_DDRAW, "IDirectDraw7::GetDeviceIdentifier(%x) -> NVIDIA GeForce2 GTS", ARG(2));
    return DD_OK;
}

GMETHOD(dd_StartModeTest, 4)
{
    LOG(LOG_ALWAYS, "IDirectDraw7::StartModeTest: not implemented");
    return DDERR_UNSUPPORTED;
}

GMETHOD(dd_EvaluateMode, 3)
{
    LOG(LOG_ALWAYS, "IDirectDraw7::EvaluateMode: not implemented");
    return DDERR_UNSUPPORTED;
}

/* ------------------------------------------------------------------ surface methods */
#define SURF(o) ((Surface *)com_host(o))

static uint32_t gamma_obj(Surface *s)
{
    if (!s->gamma_obj)
        s->gamma_obj = com_new(&gamma_cls, s);
    return s->gamma_obj;
}

static uint32_t surf_qi(CPU *c, const char *who)
{
    Surface *s = SURF(ARG(0));
    uint32_t riid = ARG(1), ppv = ARG(2);
    char g[64], nm[96];
    if (!ppv)
        return DDERR_INVALIDPARAMS;
    W32(ppv, 0);
    uint32_t r = 0;
    if (gfx_is_guid(riid, G_IUNKNOWN))
        r = ARG(0);
    else if (gfx_is_guid(riid, G_IDIRECTDRAWSURFACE7))
        r = surface_obj7(s);
    else if (gfx_is_guid(riid, G_IDIRECTDRAWSURFACE))
        r = surface_obj1(s);
    else if (gfx_is_guid(riid, G_IDIRECTDRAWGAMMACONTROL))
        r = gamma_obj(s);
    if (!r) {
        LOG(LOG_ALWAYS, "%s::QueryInterface(%s) on %s: not implemented", who, gfx_guid_name(riid, g, sizeof g),
            surface_name(s, nm, sizeof nm));
        return E_NOINTERFACE;
    }
    s->refs++;
    W32(ppv, r);
    LOG(LOG_DDRAW, "%s::QueryInterface(%s) on %s -> %08x", who, gfx_guid_name(riid, g, sizeof g),
        surface_name(s, nm, sizeof nm), r);
    return DD_OK;
}
GMETHOD(surf_QueryInterface, 3) { return surf_qi(c, "IDirectDrawSurface"); }
GMETHOD(gamma_QueryInterface, 3) { return surf_qi(c, "IDirectDrawGammaControl"); }

GMETHOD(surf_AddRef, 1)
{
    Surface *s = SURF(ARG(0));
    char nm[96];
    LOG(LOG_DDRAW, "%s::AddRef -> %d", surface_name(s, nm, sizeof nm), s->refs + 1);
    return (uint32_t)++s->refs;
}

GMETHOD(surf_Release, 1)
{
    Surface *s = SURF(ARG(0));
    char nm[96];
    int r = s->refs - 1 > 0 ? s->refs - 1 : 0;
    LOG(LOG_DDRAW, "%s::Release -> %d", surface_name(s, nm, sizeof nm), r);
    surface_release(s);
    return (uint32_t)r;
}

GMETHOD(surf_AddAttachedSurface, 2)
{
    Surface *s = SURF(ARG(0)), *a = surface_get(ARG(1));
    char n1[96], n2[96];
    if (!a)
        return DDERR_INVALIDPARAMS;
    LOG(LOG_DDRAW, "%s::AddAttachedSurface(%s)", surface_name(s, n1, sizeof n1), surface_name(a, n2, sizeof n2));
    for (int i = 0; i < s->nattached; i++)
        if (s->attached[i] == a)
            return DDERR_SURFACEALREADYATTACHED;
    if (!(a->caps & DDSCAPS_ZBUFFER) || surface_zbuffer(s) || s->nattached >= COUNT(s->attached)) {
        LOG(LOG_DDRAW, "  cannot attach");
        return DDERR_CANNOTATTACHSURFACE;
    }
    if (a->width < s->width || a->height < s->height)
        return DDERR_CANNOTATTACHSURFACE;
    s->attached[s->nattached++] = a;
    a->refs++;
    return DD_OK;
}

GMETHOD(surf_AddOverlayDirtyRect, 2)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::AddOverlayDirtyRect -> not an overlay");
    return DDERR_NOTAOVERLAYSURFACE;
}

static int read_rect(uint32_t r, int *out, const Surface *s)
{
    if (!r) {
        out[0] = out[1] = 0;
        out[2] = (int)s->width;
        out[3] = (int)s->height;
        return 1;
    }
    for (int i = 0; i < 4; i++)
        out[i] = (int32_t)R32(r + 4 * (uint32_t)i);
    return out[0] < out[2] && out[1] < out[3];
}

GMETHOD(surf_Blt, 6)
{
    Surface *d = SURF(ARG(0)), *s = surface_get(ARG(2));
    uint32_t dr = ARG(1), sr = ARG(3), flags = ARG(4), fx = ARG(5);
    char n1[96], n2[96], r1[48], r2[48], f[200];
    LOG(LOG_DDRAW, "%s::Blt(%s <- %s %s, %s)", surface_name(d, n1, sizeof n1), rect_str(dr, r1, sizeof r1),
        s ? surface_name(s, n2, sizeof n2) : "NULL", rect_str(sr, r2, sizeof r2), FLAGS(flags, blt_names, f));
    int drc[4], src[4];
    if (!read_rect(dr, drc, d))
        return DDERR_INVALIDRECT;
    if (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) {
        if (!fx)
            return DDERR_INVALIDPARAMS;
        uint32_t v = R32(fx + 0x50);
        if ((flags & DDBLT_DEPTHFILL) && !(d->caps & DDSCAPS_ZBUFFER))
            return DDERR_INVALIDPARAMS;
        LOG(LOG_DDRAW, "  fill %08x", v);
        surface_fill(d, drc, v);
        return DD_OK;
    }
    if (flags & DDBLT_ROP) {
        uint32_t rop = fx ? R32(fx + 8) : SRCCOPY;
        if (rop == BLACKNESS || rop == WHITENESS) {
            surface_fill(d, drc, rop == BLACKNESS ? 0 : pix_encode(d, 0xffffffffu));
            return DD_OK;
        }
        if (rop != SRCCOPY)
            LOG(LOG_ALWAYS, "IDirectDrawSurface::Blt: raster op %08x not implemented, copying", rop);
    }
    if (!s)
        return DDERR_INVALIDPARAMS;
    if (!read_rect(sr, src, s))
        return DDERR_INVALIDRECT;
    if (flags & (DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE | DDBLT_DDFX | DDBLT_DDROPS))
        LOG(LOG_ALWAYS, "IDirectDrawSurface::Blt: flags %s not implemented, ignored", FLAGS(flags, blt_names, f));
    int key = 0;
    uint32_t lo = 0, hi = 0;
    if (flags & DDBLT_KEYSRCOVERRIDE) {
        key = 1;
        lo = R32(fx + 0x5c);
        hi = R32(fx + 0x60) > lo ? R32(fx + 0x60) : lo;
    } else if ((flags & DDBLT_KEYSRC) && (s->ck_set & DDCKEY_SRCBLT)) {
        key = 1;
        lo = s->ck[2][0];
        hi = s->ck[2][1];
    }
    if ((d->caps & DDSCAPS_PRIMARYSURFACE) && vk_on()) {
        surface_copy_mem(d, drc, s, src, key, lo, hi);
        present(s, src);
        presented_unlocked();
        return DD_OK;
    }
    surface_copy(d, drc, s, src, key, lo, hi);
    if (d->caps & DDSCAPS_PRIMARYSURFACE) {
        present(s, src);
        presented_unlocked();
    }
    return DD_OK;
}

GMETHOD(surf_BltBatch, 4)
{
    LOG(LOG_ALWAYS, "IDirectDrawSurface::BltBatch: not implemented");
    return DDERR_UNSUPPORTED;
}

GMETHOD(surf_BltFast, 6)
{
    Surface *d = SURF(ARG(0)), *s = surface_get(ARG(3));
    uint32_t x = ARG(1), y = ARG(2), sr = ARG(4), trans = ARG(5);
    char n1[96], n2[96], r2[48];
    LOG(LOG_DDRAW, "%s::BltFast(%d,%d <- %s %s, %x)", surface_name(d, n1, sizeof n1), (int)x, (int)y,
        s ? surface_name(s, n2, sizeof n2) : "NULL", rect_str(sr, r2, sizeof r2), trans);
    if (!s)
        return DDERR_INVALIDPARAMS;
    int src[4];
    if (!read_rect(sr, src, s))
        return DDERR_INVALIDRECT;
    int drc[4] = {(int)x, (int)y, (int)x + src[2] - src[0], (int)y + src[3] - src[1]};
    if (drc[2] > (int)d->width || drc[3] > (int)d->height || src[2] > (int)s->width || src[3] > (int)s->height)
        return DDERR_INVALIDRECT;
    if (trans & DDBLTFAST_DESTCOLORKEY)
        LOG(LOG_ALWAYS, "IDirectDrawSurface::BltFast: destination colour key not implemented");
    int key = (trans & DDBLTFAST_SRCCOLORKEY) && (s->ck_set & DDCKEY_SRCBLT);
    if ((d->caps & DDSCAPS_PRIMARYSURFACE) && vk_on()) {
        surface_copy_mem(d, drc, s, src, key, s->ck[2][0], s->ck[2][1]);
        present(s, src);
        presented_unlocked();
        return DD_OK;
    }
    surface_copy(d, drc, s, src, key, s->ck[2][0], s->ck[2][1]);
    if (d->caps & DDSCAPS_PRIMARYSURFACE) {
        present(s, src);
        presented_unlocked();
    }
    return DD_OK;
}

GMETHOD(surf_DeleteAttachedSurface, 3)
{
    Surface *s = SURF(ARG(0)), *a = surface_get(ARG(2));
    char n1[96], n2[96];
    LOG(LOG_DDRAW, "%s::DeleteAttachedSurface(%x, %s)", surface_name(s, n1, sizeof n1), ARG(1),
        a ? surface_name(a, n2, sizeof n2) : "all");
    if (!a) {
        for (int i = 0; i < s->nattached; i++)
            surface_release(s->attached[i]);
        s->nattached = 0;
        return DD_OK;
    }
    for (int i = 0; i < s->nattached; i++)
        if (s->attached[i] == a) {
            memmove(&s->attached[i], &s->attached[i + 1], (size_t)(s->nattached - i - 1) * sizeof *s->attached);
            s->nattached--;
            surface_release(a);
            return DD_OK;
        }
    return DDERR_SURFACENOTATTACHED;
}

static uint32_t enum_attached(CPU *c, int v7)
{
    Surface *s = SURF(ARG(0));
    uint32_t ctx = ARG(1), cb = ARG(2);
    char nm[96];
    if (!cb)
        return DDERR_INVALIDPARAMS;
    Surface *list[10];
    int n = 0;
    for (int i = 0; i < s->nattached; i++)
        list[n++] = s->attached[i];
    if (s->next && s->next != s)
        list[n++] = s->next;
    LOG(LOG_DDRAW, "%s::EnumAttachedSurfaces -> %d", surface_name(s, nm, sizeof nm), n);
    uint32_t d = galloc(SZ_DDSURFACEDESC2);
    for (int i = 0; i < n; i++) {
        list[i]->refs++;
        W32(d, v7 ? SZ_DDSURFACEDESC2 : SZ_DDSURFACEDESC);
        fill_desc(list[i], d, 0);
        uint32_t obj = v7 ? surface_obj7(list[i]) : surface_obj1(list[i]);
        gfx_unlock();
        uint32_t r = guest_call(c, cb, 3, obj, d, ctx);
        gfx_lock();
        if (r == DDENUMRET_CANCEL)
            break;
    }
    gfree(d);
    return DD_OK;
}
GMETHOD(surf7_EnumAttachedSurfaces, 3) { return enum_attached(c, 1); }
GMETHOD(surf1_EnumAttachedSurfaces, 3) { return enum_attached(c, 0); }

GMETHOD(surf_EnumOverlayZOrders, 4)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::EnumOverlayZOrders -> none");
    return DD_OK;
}

GMETHOD(surf_Flip, 3)
{
    Surface *s = SURF(ARG(0)), *t = surface_get(ARG(1));
    char nm[96];
    LOG(LOG_DDRAW, "%s::Flip(%s, %x) frame %lu", surface_name(s, nm, sizeof nm), t ? "target" : "NULL", ARG(2),
        frames + 1);
    if (!(s->caps & DDSCAPS_FLIP) || !(s->caps & DDSCAPS_FRONTBUFFER) || !s->next)
        return DDERR_NOTFLIPPABLE;
    if (t) {
        int found = 0;
        for (Surface *p = s->next; p && p != s; p = p->next)
            if (p == t)
                found = 1;
        if (!found)
            return DDERR_INVALIDPARAMS;
    }
    Surface *back = t ? t : s->next;
    present(back, NULL);
    /* the memory moves here; the GPU images (vk_flip) in order with the renderer's calls, which may
       still be on their way (vkthread.c) */
    if (t) {
        uint32_t m = s->mem;
        int k = s->mem_kind;
        s->mem = t->mem;
        s->mem_kind = t->mem_kind;
        t->mem = m;
        t->mem_kind = k;
        t->dirty++;
    } else {
        uint32_t m = s->mem;
        int k = s->mem_kind;
        Surface *p = s;
        for (; p->next != s; p = p->next) {
            p->mem = p->next->mem;
            p->mem_kind = p->next->mem_kind;
            p->dirty++;
        }
        p->mem = m;
        p->mem_kind = k;
        p->dirty++;
    }
    vk_flip(s, t);
    s->dirty++;
    presented_unlocked();
    return DD_OK;
}

static uint32_t get_attached(CPU *c, int v7)
{
    Surface *s = SURF(ARG(0));
    uint32_t pcaps = ARG(1), pp = ARG(2);
    char nm[96], n2[96], f[200];
    if (!pcaps || !pp)
        return DDERR_INVALIDPARAMS;
    uint32_t want = R32(pcaps), want2 = v7 ? R32(pcaps + 4) : 0;
    W32(pp, 0);
    Surface *found = NULL;
    for (int i = 0; i < s->nattached && !found; i++)
        if ((s->attached[i]->caps & want) == want && (s->attached[i]->caps2 & want2) == want2)
            found = s->attached[i];
    if (!found && s->next && s->next != s && (s->next->caps & want) == want && (s->next->caps2 & want2) == want2)
        found = s->next;
    LOG(LOG_DDRAW, "%s::GetAttachedSurface(%s) -> %s", surface_name(s, nm, sizeof nm), FLAGS(want, caps_names, f),
        found ? surface_name(found, n2, sizeof n2) : "not found");
    if (!found)
        return DDERR_NOTFOUND;
    found->refs++;
    W32(pp, v7 ? surface_obj7(found) : surface_obj1(found));
    return DD_OK;
}
GMETHOD(surf7_GetAttachedSurface, 3) { return get_attached(c, 1); }
GMETHOD(surf1_GetAttachedSurface, 3) { return get_attached(c, 0); }

GMETHOD(surf_GetBltStatus, 2)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::GetBltStatus(%x) -> done", ARG(1));
    return DD_OK;
}

static uint32_t get_caps(CPU *c, int v7)
{
    Surface *s = SURF(ARG(0));
    uint32_t p = ARG(1);
    char nm[96];
    if (!p)
        return DDERR_INVALIDPARAMS;
    W32(p, s->caps);
    if (v7) {
        W32(p + 4, s->caps2);
        W32(p + 8, s->caps3);
        W32(p + 12, s->caps4);
    }
    LOG(LOG_DDRAW, "%s::GetCaps -> %x", surface_name(s, nm, sizeof nm), s->caps);
    return DD_OK;
}
GMETHOD(surf7_GetCaps, 2) { return get_caps(c, 1); }
GMETHOD(surf1_GetCaps, 2) { return get_caps(c, 0); }

GMETHOD(surf_GetClipper, 2)
{
    Surface *s = SURF(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    W32(ARG(1), s->clipper);
    LOG(LOG_DDRAW, "IDirectDrawSurface::GetClipper -> %08x", s->clipper);
    if (!s->clipper)
        return DDERR_NOCLIPPERATTACHED;
    ((Clipper *)com_host(s->clipper))->refs++;
    return DD_OK;
}

static int ck_index(uint32_t flags)
{
    uint32_t k = flags & ~DDCKEY_COLORSPACE;
    return k == DDCKEY_DESTBLT ? 0 : k == DDCKEY_DESTOVERLAY ? 1 : k == DDCKEY_SRCBLT ? 2 : k == DDCKEY_SRCOVERLAY ? 3 : -1;
}

GMETHOD(surf_GetColorKey, 3)
{
    Surface *s = SURF(ARG(0));
    uint32_t flags = ARG(1), key = ARG(2);
    int i = ck_index(flags);
    char nm[96], f[64];
    if (i < 0 || !key)
        return DDERR_INVALIDPARAMS;
    LOG(LOG_DDRAW, "%s::GetColorKey(%s)", surface_name(s, nm, sizeof nm), FLAGS(flags, ckey_names, f));
    if (!(s->ck_set & (DDCKEY_DESTBLT << i)))
        return DDERR_NOCOLORKEY;
    W32(key, s->ck[i][0]);
    W32(key + 4, s->ck[i][1]);
    return DD_OK;
}

GMETHOD(surf_GetDC, 2)
{
    Surface *s = SURF(ARG(0));
    char nm[96];
    LOG(LOG_ALWAYS, "%s::GetDC: not implemented", surface_name(s, nm, sizeof nm));
    if (ARG(1))
        W32(ARG(1), 0);
    return DDERR_CANTCREATEDC;
}

GMETHOD(surf_GetFlipStatus, 2)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::GetFlipStatus(%x) -> ready", ARG(1));
    return DD_OK;
}

GMETHOD(surf_GetOverlayPosition, 3)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::GetOverlayPosition -> not an overlay");
    return DDERR_NOTAOVERLAYSURFACE;
}

GMETHOD(surf_GetPalette, 2)
{
    Surface *s = SURF(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    W32(ARG(1), s->palette);
    LOG(LOG_DDRAW, "IDirectDrawSurface::GetPalette -> %08x", s->palette);
    if (!s->palette)
        return DDERR_NOPALETTEATTACHED;
    ((Palette *)com_host(s->palette))->refs++;
    return DD_OK;
}

GMETHOD(surf_GetPixelFormat, 2)
{
    Surface *s = SURF(ARG(0));
    char nm[96];
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    memcpy(gptr(ARG(1)), s->pf, 32);
    LOG(LOG_DDRAW, "%s::GetPixelFormat", surface_name(s, nm, sizeof nm));
    return DD_OK;
}

GMETHOD(surf_GetSurfaceDesc, 2)
{
    Surface *s = SURF(ARG(0));
    uint32_t d = ARG(1);
    char nm[96];
    if (!d || (R32(d) != SZ_DDSURFACEDESC && R32(d) != SZ_DDSURFACEDESC2))
        return DDERR_INVALIDPARAMS;
    fill_desc(s, d, 0);
    LOG(LOG_DDRAW, "%s::GetSurfaceDesc", surface_name(s, nm, sizeof nm));
    return DD_OK;
}

GMETHOD(surf_Initialize, 3)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::Initialize -> already initialized");
    return DDERR_ALREADYINITIALIZED;
}

GMETHOD(surf_IsLost, 1)
{
    Surface *s = SURF(ARG(0));
    char nm[96];
    LOG(LOG_DDRAW, "%s::IsLost -> no", surface_name(s, nm, sizeof nm));
    return DD_OK;
}

GMETHOD(surf_Lock, 5)
{
    Surface *s = SURF(ARG(0));
    uint32_t rect = ARG(1), d = ARG(2), flags = ARG(3);
    char nm[96], r[48], f[120];
    if (!d || (R32(d) != SZ_DDSURFACEDESC && R32(d) != SZ_DDSURFACEDESC2))
        return DDERR_INVALIDPARAMS;
    uint32_t off = 0;
    if (rect) {
        int rc[4];
        if (!read_rect(rect, rc, s) || rc[0] < 0 || rc[1] < 0 || rc[2] > (int)s->width || rc[3] > (int)s->height)
            return DDERR_INVALIDRECT;
        off = (uint32_t)rc[1] * s->pitch + (uint32_t)rc[0] * s->bpp;
    }
    vk_cpu_access(s, !(flags & 0x10));              /* not DDLOCK_READONLY */
    fill_desc(s, d, s->mem + off);
    s->locks++;
    LOG(LOG_DDRAW, "%s::Lock(%s, %s) -> %08x pitch %u", surface_name(s, nm, sizeof nm), rect_str(rect, r, sizeof r),
        FLAGS(flags, lock_names, f), s->mem + off, s->pitch);
    return DD_OK;
}

GMETHOD(surf_ReleaseDC, 2)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::ReleaseDC(%08x) -> no DC", ARG(1));
    return DDERR_NODC;
}

GMETHOD(surf_Restore, 1)
{
    Surface *s = SURF(ARG(0));
    char nm[96];
    LOG(LOG_DDRAW, "%s::Restore", surface_name(s, nm, sizeof nm));
    return DD_OK;
}

GMETHOD(surf_SetClipper, 2)
{
    Surface *s = SURF(ARG(0));
    uint32_t cl = ARG(1);
    LOG(LOG_DDRAW, "IDirectDrawSurface::SetClipper(%08x)", cl);
    if (cl)
        ((Clipper *)com_host(cl))->refs++;
    if (s->clipper) {
        Clipper *old = com_host(s->clipper);
        if (--old->refs <= 0)
            com_set_class(old->obj, &dead_clipper_cls);
    }
    s->clipper = cl;
    return DD_OK;
}

GMETHOD(surf_SetColorKey, 3)
{
    Surface *s = SURF(ARG(0));
    uint32_t flags = ARG(1), key = ARG(2);
    int i = ck_index(flags);
    char nm[96], f[64];
    if (i < 0)
        return DDERR_INVALIDPARAMS;
    if (!key) {
        s->ck_set &= ~(DDCKEY_DESTBLT << i);
        LOG(LOG_DDRAW, "%s::SetColorKey(%s, none)", surface_name(s, nm, sizeof nm), FLAGS(flags, ckey_names, f));
        return DD_OK;
    }
    s->ck[i][0] = R32(key);
    s->ck[i][1] = (flags & DDCKEY_COLORSPACE) && R32(key + 4) > s->ck[i][0] ? R32(key + 4) : s->ck[i][0];
    s->ck_set |= DDCKEY_DESTBLT << i;
    s->dirty++;
    LOG(LOG_DDRAW, "%s::SetColorKey(%s, %08x-%08x)", surface_name(s, nm, sizeof nm), FLAGS(flags, ckey_names, f),
        s->ck[i][0], s->ck[i][1]);
    return DD_OK;
}

GMETHOD(surf_SetOverlayPosition, 3)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::SetOverlayPosition -> not an overlay");
    return DDERR_NOTAOVERLAYSURFACE;
}

GMETHOD(surf_SetPalette, 2)
{
    Surface *s = SURF(ARG(0));
    uint32_t p = ARG(1);
    LOG(LOG_DDRAW, "IDirectDrawSurface::SetPalette(%08x)", p);
    if (p)
        ((Palette *)com_host(p))->refs++;
    if (s->palette) {
        Palette *old = com_host(s->palette);
        if (--old->refs <= 0)
            com_set_class(old->obj, &dead_palette_cls);
    }
    s->palette = p;
    s->dirty++;
    return DD_OK;
}

GMETHOD(surf_Unlock, 2)
{
    Surface *s = SURF(ARG(0));
    char nm[96];
    LOG(LOG_DDRAW, "%s::Unlock(%08x)", surface_name(s, nm, sizeof nm), ARG(1));
    if (!s->locks)
        return DDERR_NOTLOCKED;
    s->locks--;
    s->dirty++;
    return DD_OK;
}

GMETHOD(surf_UpdateOverlay, 6)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::UpdateOverlay -> no overlay hardware");
    return DDERR_NOOVERLAYHW;
}

GMETHOD(surf_UpdateOverlayDisplay, 2)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::UpdateOverlayDisplay -> no overlay hardware");
    return DDERR_NOOVERLAYHW;
}

GMETHOD(surf_UpdateOverlayZOrder, 3)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface::UpdateOverlayZOrder -> not an overlay");
    return DDERR_NOTAOVERLAYSURFACE;
}

GMETHOD(surf_GetDDInterface, 2)
{
    Surface *s = SURF(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    s->dd->refs++;
    W32(ARG(1), dd_obj7(s->dd));
    LOG(LOG_DDRAW, "IDirectDrawSurface7::GetDDInterface -> %08x", s->dd->obj7);
    return DD_OK;
}

GMETHOD(surf_PageLock, 2)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface7::PageLock");
    return DD_OK;
}

GMETHOD(surf_PageUnlock, 2)
{
    LOG(LOG_DDRAW, "IDirectDrawSurface7::PageUnlock");
    return DD_OK;
}

/* DirectX 7: a system-memory surface (a mipmap chain's levels too) given the program's own pixels,
   optionally with a new size, pitch and pixel format. TD6 decodes each texture level into its
   own buffer and points a staging chain at it before the Blt into the real texture. The
   surface's own memory is kept until it goes. */
GMETHOD(surf_SetSurfaceDesc, 3)
{
    Surface *s = SURF(ARG(0));
    uint32_t d = ARG(1);
    char nm[96], f[200];
    if (!d || R32(d) != SZ_DDSURFACEDESC2)
        return DDERR_INVALIDPARAMS;
    uint32_t fl = R32(d + 4);
    LOG(LOG_DDRAW, "%s::SetSurfaceDesc(%s)", surface_name(s, nm, sizeof nm), FLAGS(fl, ddsd_names, f));
    const uint32_t allowed = DDSD_LPSURFACE | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_CAPS;
    if (!(fl & DDSD_LPSURFACE) || (fl & ~allowed) || !(s->caps & DDSCAPS_SYSTEMMEMORY) || !R32(d + 0x24)) {
        LOG(LOG_ALWAYS, "%s::SetSurfaceDesc(%s): not supported", surface_name(s, nm, sizeof nm), FLAGS(fl, ddsd_names, f));
        return DDERR_INVALIDPARAMS;
    }
    if (fl & DDSD_PIXELFORMAT) {
        uint32_t pf[8];
        for (int i = 0; i < 8; i++)
            pf[i] = R32(d + 0x48 + 4 * i);
        if (!pf_valid(pf))
            return DDERR_INVALIDPIXELFORMAT;
        set_pixfmt(s, pf);
    }
    if (fl & DDSD_WIDTH)
        s->width = R32(d + 0x0c);
    if (fl & DDSD_HEIGHT)
        s->height = R32(d + 0x08);
    if (fl & DDSD_PITCH)
        s->pitch = R32(d + 0x10);
    else if (fl & (DDSD_WIDTH | DDSD_PIXELFORMAT))
        s->pitch = (s->width * s->bpp + 3) & ~3u;
    if (s->mem_kind && !s->own_kind) {
        s->own_mem = s->mem;
        s->own_kind = s->mem_kind;
    } else if (s->mem_kind == 1)
        gfree(s->mem);
    else if (s->mem_kind == 2)
        vm_free(s->mem, 0, 1);
    s->mem = R32(d + 0x24);
    s->mem_kind = 0;
    s->memsize = s->pitch * s->height;
    s->dirty++;
    return DD_OK;
}

GMETHOD(surf_SetPrivateData, 5)
{
    Surface *s = SURF(ARG(0));
    uint32_t guid = ARG(1), data = ARG(2), size = ARG(3), flags = ARG(4);
    if (!guid || (size && !data))
        return DDERR_INVALIDPARAMS;
    PrivData **pp = (PrivData **)&s->privdata;
    for (; *pp; pp = &(*pp)->next)
        if (!memcmp((*pp)->guid, gptr(guid), 16))
            break;
    if (!*pp) {
        *pp = calloc(1, sizeof **pp);
        if (!*pp)
            fatal("out of memory");
        memcpy((*pp)->guid, gptr(guid), 16);
    }
    free((*pp)->data);
    (*pp)->data = malloc(size ? size : 1);
    if (!(*pp)->data)
        fatal("out of memory");
    if (size)
        memcpy((*pp)->data, gptr(data), size);
    (*pp)->size = size;
    (*pp)->flags = flags;
    LOG(LOG_DDRAW, "IDirectDrawSurface7::SetPrivateData(%u bytes, flags %x)", size, flags);
    return DD_OK;
}

GMETHOD(surf_GetPrivateData, 4)
{
    Surface *s = SURF(ARG(0));
    uint32_t guid = ARG(1), data = ARG(2), psize = ARG(3);
    if (!guid || !psize)
        return DDERR_INVALIDPARAMS;
    for (PrivData *p = s->privdata; p; p = p->next)
        if (!memcmp(p->guid, gptr(guid), 16)) {
            uint32_t have = R32(psize);
            W32(psize, p->size);
            if (!data || have < p->size)
                return DDERR_MOREDATA;
            memcpy(gptr(data), p->data, p->size);
            return DD_OK;
        }
    return DDERR_NOTFOUND;
}

GMETHOD(surf_FreePrivateData, 2)
{
    Surface *s = SURF(ARG(0));
    uint32_t guid = ARG(1);
    if (!guid)
        return DDERR_INVALIDPARAMS;
    for (PrivData **pp = (PrivData **)&s->privdata; *pp; pp = &(*pp)->next)
        if (!memcmp((*pp)->guid, gptr(guid), 16)) {
            PrivData *p = *pp;
            *pp = p->next;
            free(p->data);
            free(p);
            return DD_OK;
        }
    return DDERR_NOTFOUND;
}

GMETHOD(surf_GetUniquenessValue, 2)
{
    Surface *s = SURF(ARG(0));
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    W32(ARG(1), s->uniq);
    return DD_OK;
}

GMETHOD(surf_ChangeUniquenessValue, 1)
{
    SURF(ARG(0))->uniq++;
    return DD_OK;
}

GMETHOD(surf_SetPriority, 2)
{
    SURF(ARG(0))->priority = ARG(1);
    LOG(LOG_DDRAW, "IDirectDrawSurface7::SetPriority(%u)", ARG(1));
    return DD_OK;
}

GMETHOD(surf_GetPriority, 2)
{
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    W32(ARG(1), SURF(ARG(0))->priority);
    return DD_OK;
}

GMETHOD(surf_SetLOD, 2)
{
    SURF(ARG(0))->lod = ARG(1);
    LOG(LOG_DDRAW, "IDirectDrawSurface7::SetLOD(%u)", ARG(1));
    return DD_OK;
}

GMETHOD(surf_GetLOD, 2)
{
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    W32(ARG(1), SURF(ARG(0))->lod);
    return DD_OK;
}

/* ------------------------------------------------------------------ IDirectDrawGammaControl */
GMETHOD(gamma_GetGammaRamp, 3)
{
    uint32_t p = ARG(2);
    if (!p)
        return DDERR_INVALIDPARAMS;
    memcpy(gptr(p), gfx_gamma, sizeof gfx_gamma);
    LOG(LOG_DDRAW, "IDirectDrawGammaControl::GetGammaRamp(%x)", ARG(1));
    return DD_OK;
}

GMETHOD(gamma_SetGammaRamp, 3)
{
    uint32_t p = ARG(2);
    if (!p)
        return DDERR_INVALIDPARAMS;
    memcpy(gfx_gamma, gptr(p), sizeof gfx_gamma);
    LOG(LOG_DDRAW, "IDirectDrawGammaControl::SetGammaRamp(%x): red %04x %04x %04x, green %04x %04x %04x, blue %04x %04x %04x",
        ARG(1), gfx_gamma[0][0], gfx_gamma[0][128], gfx_gamma[0][255], gfx_gamma[1][0], gfx_gamma[1][128],
        gfx_gamma[1][255], gfx_gamma[2][0], gfx_gamma[2][128], gfx_gamma[2][255]);
    return DD_OK;
}

/* ------------------------------------------------------------------ IDirectDrawClipper */
#define CLIP(o) ((Clipper *)com_host(o))

GMETHOD(clip_QueryInterface, 3)
{
    uint32_t riid = ARG(1), ppv = ARG(2);
    char g[64];
    if (!ppv)
        return DDERR_INVALIDPARAMS;
    if (gfx_is_guid(riid, G_IUNKNOWN) || gfx_is_guid(riid, G_IDIRECTDRAWCLIPPER)) {
        CLIP(ARG(0))->refs++;
        W32(ppv, ARG(0));
        return DD_OK;
    }
    W32(ppv, 0);
    LOG(LOG_ALWAYS, "IDirectDrawClipper::QueryInterface(%s): not implemented", gfx_guid_name(riid, g, sizeof g));
    return E_NOINTERFACE;
}

GMETHOD(clip_AddRef, 1)
{
    return (uint32_t)++CLIP(ARG(0))->refs;
}

GMETHOD(clip_Release, 1)
{
    Clipper *cl = CLIP(ARG(0));
    int r = --cl->refs;
    LOG(LOG_DDRAW, "IDirectDrawClipper::Release -> %d", r);
    if (r <= 0) {
        com_set_class(cl->obj, &dead_clipper_cls);
        return 0;
    }
    return (uint32_t)r;
}

GMETHOD(clip_GetClipList, 4)
{
    LOG(LOG_DDRAW, "IDirectDrawClipper::GetClipList -> none");
    return DDERR_NOCLIPLIST;
}

GMETHOD(clip_GetHWnd, 2)
{
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    W32(ARG(1), CLIP(ARG(0))->hwnd);
    return DD_OK;
}

GMETHOD(clip_Initialize, 3)
{
    return DDERR_ALREADYINITIALIZED;
}

GMETHOD(clip_IsClipListChanged, 2)
{
    if (ARG(1))
        W32(ARG(1), 0);
    return DD_OK;
}

GMETHOD(clip_SetClipList, 3)
{
    LOG(LOG_DDRAW, "IDirectDrawClipper::SetClipList(%08x) (ignored)", ARG(1));
    return DD_OK;
}

GMETHOD(clip_SetHWnd, 3)
{
    CLIP(ARG(0))->hwnd = ARG(2);
    LOG(LOG_DDRAW, "IDirectDrawClipper::SetHWnd(%08x)", ARG(2));
    return DD_OK;
}

/* ------------------------------------------------------------------ IDirectDrawPalette */
#define PAL(o) ((Palette *)com_host(o))

GMETHOD(pal_QueryInterface, 3)
{
    uint32_t riid = ARG(1), ppv = ARG(2);
    char g[64];
    if (!ppv)
        return DDERR_INVALIDPARAMS;
    if (gfx_is_guid(riid, G_IUNKNOWN) || gfx_is_guid(riid, G_IDIRECTDRAWPALETTE)) {
        PAL(ARG(0))->refs++;
        W32(ppv, ARG(0));
        return DD_OK;
    }
    W32(ppv, 0);
    LOG(LOG_ALWAYS, "IDirectDrawPalette::QueryInterface(%s): not implemented", gfx_guid_name(riid, g, sizeof g));
    return E_NOINTERFACE;
}

GMETHOD(pal_AddRef, 1)
{
    return (uint32_t)++PAL(ARG(0))->refs;
}

GMETHOD(pal_Release, 1)
{
    Palette *p = PAL(ARG(0));
    int r = --p->refs;
    LOG(LOG_DDRAW, "IDirectDrawPalette::Release -> %d", r);
    if (r <= 0) {
        com_set_class(p->obj, &dead_palette_cls);
        return 0;
    }
    return (uint32_t)r;
}

GMETHOD(pal_GetCaps, 2)
{
    if (!ARG(1))
        return DDERR_INVALIDPARAMS;
    W32(ARG(1), PAL(ARG(0))->caps);
    return DD_OK;
}

GMETHOD(pal_GetEntries, 5)
{
    Palette *p = PAL(ARG(0));
    uint32_t base = ARG(2), n = ARG(3), out = ARG(4);
    if (!out || base + n > 256)
        return DDERR_INVALIDPARAMS;
    for (uint32_t i = 0; i < n; i++)
        W32(out + 4 * i, p->entries[base + i]);
    LOG(LOG_DDRAW, "IDirectDrawPalette::GetEntries(%u, %u)", base, n);
    return DD_OK;
}

GMETHOD(pal_Initialize, 4)
{
    return DDERR_ALREADYINITIALIZED;
}

GMETHOD(pal_SetEntries, 5)
{
    Palette *p = PAL(ARG(0));
    uint32_t base = ARG(2), n = ARG(3), in = ARG(4);
    if (!in || base + n > 256)
        return DDERR_INVALIDPARAMS;
    for (uint32_t i = 0; i < n; i++)
        p->entries[base + i] = R32(in + 4 * i);
    LOG(LOG_DDRAW, "IDirectDrawPalette::SetEntries(%u, %u)", base, n);
    return DD_OK;
}

/* ------------------------------------------------------------------ released objects */
static int dead_logged;
static uint32_t dead_call(CPU *c, int nargs)
{
    if (dead_logged < 20) {
        dead_logged++;
        LOG(LOG_ALWAYS, "ddraw: call with %d args on released object %08x (return address %08x)", nargs, ARG(0),
            R32(c->esp));
    }
    return nargs == 1 ? 0 : DDERR_INVALIDOBJECT;
}
METHOD(dead1, 1) { return dead_call(c, 1); }
METHOD(dead2, 2) { return dead_call(c, 2); }
METHOD(dead3, 3) { return dead_call(c, 3); }
METHOD(dead4, 4) { return dead_call(c, 4); }
METHOD(dead5, 5) { return dead_call(c, 5); }
METHOD(dead6, 6) { return dead_call(c, 6); }

/* ------------------------------------------------------------------ vtables */
static void (*const dd7_methods[])(CPU *) = {
    dd7_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette,
    dd7_CreateSurface, dd_DuplicateSurface, dd7_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface,
    dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes, dd7_GetGDISurface, dd_GetMonitorFrequency,
    dd_GetScanLine, dd_GetVerticalBlankStatus, dd_Initialize, dd_RestoreDisplayMode, dd_SetCooperativeLevel,
    dd7_SetDisplayMode, dd_WaitForVerticalBlank, dd_GetAvailableVidMem, dd_GetSurfaceFromDC,
    dd_RestoreAllSurfaces, dd_TestCooperativeLevel, dd_GetDeviceIdentifier, dd_StartModeTest,
    dd_EvaluateMode};
static const char *const dd7_names[] = {
    "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette",
    "CreateSurface", "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface",
    "GetCaps", "GetDisplayMode", "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency",
    "GetScanLine", "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode", "SetCooperativeLevel",
    "SetDisplayMode", "WaitForVerticalBlank", "GetAvailableVidMem", "GetSurfaceFromDC",
    "RestoreAllSurfaces", "TestCooperativeLevel", "GetDeviceIdentifier", "StartModeTest",
    "EvaluateMode"};
static void (*const dead_dd7_methods[])(CPU *) = {
    dead3, dead1, dead1, dead1, dead4, dead5, dead4, dead3, dead5, dead5, dead1, dead3, dead2, dead3,
    dead2, dead2, dead2, dead2, dead2, dead1, dead3, dead6, dead3, dead4, dead3, dead1, dead1, dead3,
    dead4, dead3};

static void (*const dd1_methods[])(CPU *) = {
    dd1_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette,
    dd1_CreateSurface, dd_DuplicateSurface, dd1_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface,
    dd_GetCaps, dd_GetDisplayMode, dd_GetFourCCCodes, dd1_GetGDISurface, dd_GetMonitorFrequency,
    dd_GetScanLine, dd_GetVerticalBlankStatus, dd_Initialize, dd_RestoreDisplayMode, dd_SetCooperativeLevel,
    dd1_SetDisplayMode, dd_WaitForVerticalBlank};
static void (*const dead_dd1_methods[])(CPU *) = {
    dead3, dead1, dead1, dead1, dead4, dead5, dead4, dead3, dead5, dead5, dead1, dead3, dead2, dead3,
    dead2, dead2, dead2, dead2, dead2, dead1, dead3, dead4, dead3};

static void (*const surf7_methods[])(CPU *) = {
    surf_QueryInterface, surf_AddRef, surf_Release, surf_AddAttachedSurface, surf_AddOverlayDirtyRect,
    surf_Blt, surf_BltBatch, surf_BltFast, surf_DeleteAttachedSurface, surf7_EnumAttachedSurfaces,
    surf_EnumOverlayZOrders, surf_Flip, surf7_GetAttachedSurface, surf_GetBltStatus, surf7_GetCaps,
    surf_GetClipper, surf_GetColorKey, surf_GetDC, surf_GetFlipStatus, surf_GetOverlayPosition,
    surf_GetPalette, surf_GetPixelFormat, surf_GetSurfaceDesc, surf_Initialize, surf_IsLost, surf_Lock,
    surf_ReleaseDC, surf_Restore, surf_SetClipper, surf_SetColorKey, surf_SetOverlayPosition,
    surf_SetPalette, surf_Unlock, surf_UpdateOverlay, surf_UpdateOverlayDisplay, surf_UpdateOverlayZOrder,
    surf_GetDDInterface, surf_PageLock, surf_PageUnlock, surf_SetSurfaceDesc, surf_SetPrivateData,
    surf_GetPrivateData, surf_FreePrivateData, surf_GetUniquenessValue, surf_ChangeUniquenessValue,
    surf_SetPriority, surf_GetPriority, surf_SetLOD, surf_GetLOD};
static const char *const surf7_names[] = {
    "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect",
    "Blt", "BltBatch", "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces",
    "EnumOverlayZOrders", "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps",
    "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus", "GetOverlayPosition",
    "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
    "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition",
    "SetPalette", "Unlock", "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder",
    "GetDDInterface", "PageLock", "PageUnlock", "SetSurfaceDesc", "SetPrivateData",
    "GetPrivateData", "FreePrivateData", "GetUniquenessValue", "ChangeUniquenessValue",
    "SetPriority", "GetPriority", "SetLOD", "GetLOD"};
static void (*const dead_surf7_methods[])(CPU *) = {
    dead3, dead1, dead1, dead2, dead2, dead6, dead4, dead6, dead3, dead3, dead4, dead3, dead3, dead2,
    dead2, dead2, dead3, dead2, dead2, dead3, dead2, dead2, dead2, dead3, dead1, dead5, dead2, dead1,
    dead2, dead3, dead3, dead2, dead2, dead6, dead2, dead3, dead2, dead2, dead2, dead3, dead5, dead4,
    dead2, dead2, dead1, dead2, dead2, dead2, dead2};

static void (*const surf1_methods[])(CPU *) = {
    surf_QueryInterface, surf_AddRef, surf_Release, surf_AddAttachedSurface, surf_AddOverlayDirtyRect,
    surf_Blt, surf_BltBatch, surf_BltFast, surf_DeleteAttachedSurface, surf1_EnumAttachedSurfaces,
    surf_EnumOverlayZOrders, surf_Flip, surf1_GetAttachedSurface, surf_GetBltStatus, surf1_GetCaps,
    surf_GetClipper, surf_GetColorKey, surf_GetDC, surf_GetFlipStatus, surf_GetOverlayPosition,
    surf_GetPalette, surf_GetPixelFormat, surf_GetSurfaceDesc, surf_Initialize, surf_IsLost, surf_Lock,
    surf_ReleaseDC, surf_Restore, surf_SetClipper, surf_SetColorKey, surf_SetOverlayPosition,
    surf_SetPalette, surf_Unlock, surf_UpdateOverlay, surf_UpdateOverlayDisplay, surf_UpdateOverlayZOrder};

static void (*const gamma_methods[])(CPU *) = {
    gamma_QueryInterface, surf_AddRef, surf_Release, gamma_GetGammaRamp, gamma_SetGammaRamp};
static const char *const gamma_names[] = {"QueryInterface", "AddRef", "Release", "GetGammaRamp", "SetGammaRamp"};
static void (*const dead_gamma_methods[])(CPU *) = {dead3, dead1, dead1, dead3, dead3};

static void (*const clipper_methods[])(CPU *) = {
    clip_QueryInterface, clip_AddRef, clip_Release, clip_GetClipList, clip_GetHWnd, clip_Initialize,
    clip_IsClipListChanged, clip_SetClipList, clip_SetHWnd};
static const char *const clipper_names[] = {
    "QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd", "Initialize",
    "IsClipListChanged", "SetClipList", "SetHWnd"};
static void (*const dead_clipper_methods[])(CPU *) = {dead3, dead1, dead1, dead4, dead2, dead3, dead2, dead3, dead3};

static void (*const palette_methods[])(CPU *) = {
    pal_QueryInterface, pal_AddRef, pal_Release, pal_GetCaps, pal_GetEntries, pal_Initialize, pal_SetEntries};
static const char *const palette_names[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries", "Initialize", "SetEntries"};
static void (*const dead_palette_methods[])(CPU *) = {dead3, dead1, dead1, dead2, dead5, dead4, dead5};

static const char *const dead_names[] = {
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released", "released", "released", "released", "released", "released", "released", "released",
    "released"};

_Static_assert(COUNT(dd7_methods) == 30 && COUNT(dd7_names) == 30 && COUNT(dead_dd7_methods) == 30,
               "IDirectDraw7 has 30 methods");
_Static_assert(COUNT(dd1_methods) == 23 && COUNT(dead_dd1_methods) == 23, "IDirectDraw has 23 methods");
_Static_assert(COUNT(surf7_methods) == 49 && COUNT(surf7_names) == 49 && COUNT(dead_surf7_methods) == 49,
               "IDirectDrawSurface7 has 49 methods");
_Static_assert(COUNT(surf1_methods) == 36, "IDirectDrawSurface has 36 methods");
_Static_assert(COUNT(gamma_methods) == 5 && COUNT(dead_gamma_methods) == 5, "gamma control has 5 methods");
_Static_assert(COUNT(clipper_methods) == 9 && COUNT(dead_clipper_methods) == 9, "clipper has 9 methods");
_Static_assert(COUNT(palette_methods) == 7 && COUNT(dead_palette_methods) == 7, "palette has 7 methods");
_Static_assert(COUNT(dead_names) >= 49, "names for released vtables");

/* ------------------------------------------------------------------ exports */
static uint32_t create_dd(uint32_t guid, uint32_t pp, uint32_t outer, int v7, const char *who)
{
    char g[64];
    if (!pp)
        return DDERR_INVALIDPARAMS;
    W32(pp, 0);
    if (outer)
        return CLASS_E_NOAGGREGATION;
    /* NULL, DDCREATE_HARDWAREONLY (1), DDCREATE_EMULATIONONLY (2): the one display device */
    if (guid > 2) {
        LOG(LOG_ALWAYS, "%s: no display device %s", who, gfx_guid_name(guid, g, sizeof g));
        return DDERR_INVALIDDIRECTDRAWGUID;
    }
    gfx_lock();
    DDraw *dd = dd_new();
    uint32_t obj = v7 ? dd_obj7(dd) : dd_obj1(dd);
    gfx_unlock();
    W32(pp, obj);
    LOG(LOG_DDRAW, "%s(%s) -> DirectDraw object #%d %08x", who, guid == 0 ? "primary" : guid == 1 ? "HARDWAREONLY" : "EMULATIONONLY",
        dd->id, obj);
    return DD_OK;
}

API(DDRAW, DirectDrawCreate, 3)
{
    return create_dd(ARG(0), ARG(1), ARG(2), 0, "DirectDrawCreate");
}

API(DDRAW, DirectDrawCreateEx, 4)
{
    char g[64];
    if (!gfx_is_guid(ARG(2), G_IDIRECTDRAW7)) {
        LOG(LOG_ALWAYS, "DirectDrawCreateEx(%s): only IDirectDraw7", gfx_guid_name(ARG(2), g, sizeof g));
        if (ARG(1))
            W32(ARG(1), 0);
        return DDERR_INVALIDPARAMS;
    }
    return create_dd(ARG(0), ARG(1), ARG(3), 1, "DirectDrawCreateEx");
}

API(DDRAW, DirectDrawEnumerateExA, 3)
{
    uint32_t cb = ARG(0), ctx = ARG(1), flags = ARG(2);
    LOG(LOG_DDRAW, "DirectDrawEnumerateExA(cb %08x, flags %x) -> the primary display driver", cb, flags);
    if (!cb)
        return DDERR_INVALIDPARAMS;
    guest_call(c, cb, 5, 0, str_desc, str_name, ctx, 0);
    return DD_OK;
}

API(DDRAW, DirectDrawEnumerateA, 2)
{
    uint32_t cb = ARG(0), ctx = ARG(1);
    LOG(LOG_DDRAW, "DirectDrawEnumerateA(cb %08x) -> the primary display driver", cb);
    if (!cb)
        return DDERR_INVALIDPARAMS;
    guest_call(c, cb, 4, 0, str_desc, str_name, ctx);
    return DD_OK;
}

/* ------------------------------------------------------------------ init */
void ddraw_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&gfx_mutex, &a);
    pthread_mutexattr_destroy(&a);
    for (int i = 0; i < 256; i++)
        gfx_gamma[0][i] = gfx_gamma[1][i] = gfx_gamma[2][i] = (uint16_t)(i * 257);
    com_class_init(&dd7_cls, "IDirectDraw7", 30, dd7_methods, dd7_names);
    com_class_init(&dd1_cls, "IDirectDraw", 23, dd1_methods, dd7_names);
    com_class_init(&surf7_cls, "IDirectDrawSurface7", 49, surf7_methods, surf7_names);
    com_class_init(&surf1_cls, "IDirectDrawSurface", 36, surf1_methods, surf7_names);
    com_class_init(&gamma_cls, "IDirectDrawGammaControl", 5, gamma_methods, gamma_names);
    com_class_init(&clipper_cls, "IDirectDrawClipper", 9, clipper_methods, clipper_names);
    com_class_init(&palette_cls, "IDirectDrawPalette", 7, palette_methods, palette_names);
    com_class_init(&dead_dd7_cls, "IDirectDraw7(released)", 30, dead_dd7_methods, dead_names);
    com_class_init(&dead_dd1_cls, "IDirectDraw(released)", 23, dead_dd1_methods, dead_names);
    com_class_init(&dead_surf7_cls, "IDirectDrawSurface7(released)", 49, dead_surf7_methods, dead_names);
    com_class_init(&dead_surf1_cls, "IDirectDrawSurface(released)", 36, dead_surf7_methods, dead_names);
    com_class_init(&dead_gamma_cls, "IDirectDrawGammaControl(released)", 5, dead_gamma_methods, dead_names);
    com_class_init(&dead_clipper_cls, "IDirectDrawClipper(released)", 9, dead_clipper_methods, dead_names);
    com_class_init(&dead_palette_cls, "IDirectDrawPalette(released)", 7, dead_palette_methods, dead_names);
    str_desc = gstrdup("Primary Display Driver");
    str_name = gstrdup("display");
    d3d_init();
    swrast_init();
}
