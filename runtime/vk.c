/* Vulkan renderer (DESIGN.md point 4). Direct3D state comes from d3d.c's record; vertices are
   transformed and lit on the CPU (d3d.c, the same code ProcessVertices and swrast use), so the
   GPU only rasterizes: the vertex shader passes clip-space positions through and one
   fragment "uber-shader" does the two texture stages, specular, fog, alpha test and colour key.
   Blending is per pipeline (small cache); depth test/write/func, cull and z-bias are dynamic
   state (Vulkan 1.3 core). Textures are one bindless array, refreshed from the surface's
   program memory when its dirty counter moves (16-bit formats converted, all mip levels).

   Render targets (3D-capable surfaces) live on the GPU; their program memory is only brought up
   to date when the program reads it (Lock, a Blt from it), and re-uploaded after the program
   wrote it. Everything is recorded in call order into one command buffer per frame, two frames
   in flight; texture uploads in the middle of a frame break the rendering instance and resume
   it. Flip draws the frame into the window (aspect-correct, through the gamma ramp, FIFO).

   Coordinates: D3D7 puts pixel centres on integer coordinates, Vulkan on half-integers, so
   every position moves by +0.5 pixel; depth is 0..1 in both; flat shading uses the first
   vertex in both.

   TD6_GPU_OFFSCREEN=1 renders without a window (any Vulkan device: TD6_GPU_DEVICE=<part
   of the name> picks one, VK_ICD_FILENAMES works too); with TD6_DUMP_FRAMES=<dir>
   (TD6_DUMP_EVERY=<n>, TD6_DUMP_FROM=<frame>) the presented frames are read back and
   written as PPM like swrast does, and TD6_DUMP_PRESENT=1 also writes the scaled,
   gamma-corrected window picture. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "runtime.h"
#include "gfx.h"
#include "vk.h"
#include "font.h"
#include "win.h"
#include "options.h"
#include "widescreen.h"
#include "shaders/draw.vert.spv.h"
#include "shaders/draw.frag.spv.h"
#include "shaders/draw.frag.shadow.spv.h"
#include "shaders/draw.frag.refl.spv.h"
#include "shaders/tnl.vert.spv.h"
#include "shaders/present.vert.spv.h"
#include "shaders/present.frag.spv.h"
#include "shaders/resolve.frag.spv.h"
#include "shaders/ao.frag.spv.h"
#include "shaders/draw.vert.motion.spv.h"
#include "shaders/tnl.vert.motion.spv.h"
#include "shaders/draw.frag.motion.spv.h"
#include "shaders/draw.frag.refl.motion.spv.h"
#include "shaders/draw.frag.shadow.motion.spv.h"
#include "shaders/taa.frag.spv.h"
#include "shaders/taau.frag.spv.h"
#include "shaders/smoke.vert.spv.h"
#include "shaders/smoke.frag.spv.h"
#include "shaders/smoke.frag.motion.spv.h"
#include "shaders/smokevis.frag.spv.h"
#if defined(HAVE_FSR2) || defined(HAVE_DLSS)
#include "fsr2.h"
#endif
#ifdef HAVE_DLSS
#include "dlss.h"
#endif
#include "vkr.h"

/* The render side: vk.c's functions (vkr_*) run where vkthread.c has them run, the render thread or
   the caller's, and read what the game side had set at each call from vkr (vkthread.c sets it before
   each one), not the game side's variables, which the game thread is changing meanwhile. */
VkrCtx vkr;
#define vk_fade (vkr.fade)
#define vk_nocast (vkr.nocast)
#define vk_fog_end (vkr.fog_end)
#define vk_fog_start (vkr.fog_start)
#define vk_smoke_parts (vkr.smoke_parts)
#define vk_puff (vkr.puff)
#define ws_race_depth (vkr.ws_race_depth)
#define ws_hud_depth (vkr.ws_hud_depth)
#define ws_menu_depth (vkr.ws_menu_depth)
#define gfx_gamma (vkr.gamma)

#define FRAMES 2
#define MAX_TEX 4096
#define MAX_SMP 256
#define MAX_CHUNKS 32
#define CHUNK_SIZE (8u << 20)
#define RT_FORMAT VK_FORMAT_B8G8R8A8_UNORM
#define AUX_FORMAT VK_FORMAT_R8G8B8A8_UNORM   /* the shadows' extra attachments: the sun's part of the colour, */
#define GEO_FORMAT VK_FORMAT_R8G8B8A8_UNORM   /* ... and the surface's class, n.l, normal and how much AO it
                                                takes (water: its distance instead of the normal; draw.frag) */
#define SH_FORMAT VK_FORMAT_R8G8B8A8_UNORM    /* the shadows' images T and U (resolve.frag sh_enc): shadow,
                                                penumbra, eye distance */
#define AO_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT   /* ambient occlusion's small images (ao.frag): darkening, eye
                                                    distance, normal */
#define VEL_FORMAT VK_FORMAT_R16G16_SFLOAT        /* temporal anti-aliasing: the scene's motion vectors, */
#define MASK_FORMAT VK_FORMAT_R8G8_UNORM          /* ... its reactive and transparency masks, */
#define UPS_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT  /* ... FSR 2's output (the back buffer's size) */
#define ZH_FORMAT VK_FORMAT_R32G32_SFLOAT        /* volumetric smoke: the smaller image's eye depths (the scene;
                                                    the nearest see-through surface, its alpha in the fraction);
                                                    the puffs' shadow (the sun let through; the eye distance) */
#define HP_FORMAT VK_FORMAT_R16G16B16A16_UNORM   /* "colour_precision = high": the scene's colour: the same
                                                    gamma-space values as RT_FORMAT's, with 16 bits each, clamped to
                                                    0..1 as 8-bit UNORM is, so every blend is the game's (a float
                                                    target would keep what additive passes take over 1 and below 0);
                                                    hp_format (the sun's part beside it: hp_sun, 10 bits) */
#define NOISE_FORMAT VK_FORMAT_R16G16_UNORM      /* the dither's blue noise (r: a pixel's rank, 64 x 64) */

#define VKC(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) fatal("Vulkan: %s failed (%d) at line %d", #x, _r, __LINE__); } while (0)

enum { K_TEX = 1, K_RT, K_DEPTH, K_AUX };    /* K_AUX: the shadows' extra attachments */

typedef struct GpuImg {
    int kind, slot;
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkImageView view2;            /* another view of it (the scene's: FSR 2's masks, depth alone), freed with it */
    VkFormat format;
    VkImageAspectFlags aspect;
    VkImageLayout layout;
    uint32_t w, h, levels;
    uint32_t lw, lh;              /* render targets: the program's size; w, h are the GPU's */
    float sx, sy;                 /* w / lw, h / lh */
    float ox;                     /* widescreen: where the program's 4:3 picture starts (GPU pixels) */
    unsigned long dirty[16];      /* textures: the levels' dirty counters as uploaded */
    int uploaded[16];
    int gpu_newer, cpu_newer;     /* render targets: which copy of the pixels is current */
    uint64_t cutout;              /* textures: which of level 0's 8 x 8 tiles have a texel with alpha
                                     under 1/2 (bit y * 8 + x; ray-traced shadows' cut-outs) */
    int cut_known;                /* ... worked out (when a ray-traced view first needs it after an upload) */
    float cut_frac;               /* ... the share of level 0's texels with alpha under 1/2 (foliage) */
    uint32_t game_levels;         /* textures: the program's own mip levels (levels may have more: made here) */
    int made_mips;                /* textures: levels 1.. worked out here from level 0 (taa_mips) */
    int mips_taa;                 /* textures: made while temporal anti-aliasing was chosen (else none) */
} GpuImg;

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    VkDeviceSize size, used;
    VkDeviceAddress addr;         /* with ray tracing: its device address */
} Chunk;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkSemaphore acquired;
    Chunk chunks[MAX_CHUNKS];
    int nchunks, cur, recording;
    GpuImg **garbage;
    int ngarbage, capgarbage;
    TnlConst *tnl;                /* this frame's TnlConst blocks (GPU transform and lighting) */
    uint32_t ntnl;
    uint32_t ninst;               /* instance records used (vk_draw_tnl_inst) */
    int timed;                    /* TD6_VK_STATS: its timestamps were written */
    int sh_timed;                 /* ... ray tracing's too (start, built, ambient occlusion, shadows) */
    int taa_timed;                /* ... temporal anti-aliasing's (FSR 2 and its copy) */
    int sm_timed;                 /* ... the volumetric smoke's (its first view's: 1 first passes, 2 the rest; 4 the
                                     two in one, at the view's end: no figure for the first) */
    int hp;                       /* ... "colour_precision = high" was on (TD6_VK_STATS=2's frame lines) */
} Frame;
#define TNL_MAX 4096              /* GPU-lit draws per frame; more go the CPU way */

/* the GPU vertex (40 bytes) */
typedef struct {
    float pos[4];
    uint32_t diffuse, specular;   /* R8G8B8A8 */
    float tc0[2], tc1[2];
} GV;

typedef struct {
    uint32_t stage[4];
    int32_t texsmp[4];
    uint32_t misc[4];
    float fog[4];
    uint32_t flags[4];
    uint32_t tnl[4];              /* GPU transform and lighting: draw, frame, texcoord sets s0, s1 */
    uint32_t shadow[4];           /* ray-traced shadows: mode | 0x100, class, frame (draw.frag) */
    uint32_t inst[4];             /* instanced draws (vk_draw_tnl_inst): x the first record, y 1 */
} PushDraw;
_Static_assert(sizeof(PushDraw) <= 128, "the push constants every device has");

/* Instanced GPU-lit draws (vk_draw_tnl_inst: drawdist.c's map copies): each copy's record in the
   frame's block (binding 7, shaders/tnl.vert): its world matrix (3 rows of the column-vector 3x4),
   then its sun's scale and its fade (VK_FADE bits) */
#define INST_MAX 32768
static float *inst_map;           /* FRAMES * INST_MAX records of 16 floats */
static const struct InstBind { uint32_t base, count; } *inst_bind;

typedef struct {
    float uvrect[4];
    int32_t idx[4];
} PushPresent;

/* Distance fade and far terrain (drawdist.c): the dither fade and edge fog of the next draws, and
   the far terrain's mesh (one buffer of the game's 40-byte vertices and 32-bit indices) with the
   range of it the next draw takes. (vk_fade, vk_fog_end, vk_fog_start: vkthread.c's, vkr's here) */
static Chunk far_mesh;
static VkDeviceSize far_ioff;
static const struct FarBind { uint32_t first, count; int32_t vbase; } *far_bind;

/* Ray-traced shadows (shadows.c; DESIGN.md "Ray-traced sun shadows"). During the race's main view
   (vk_shadow_begin .. vk_shadow_end) the draws into its render target also write two extra
   attachments: the sun's part of their colour (draw.frag) and the face normal with a class
   (terrain, object). The depth-writing triangle draws of objects and cars are gathered as the
   geometries of one bottom-level acceleration structure, built at the view's end from their
   vertices in this frame's upload chunks, each with its world matrix; the map's terrain is a
   static one (vk_shadow_terrain), repeated where the map wraps. Then shaders/resolve.frag sends
   one ray per pixel towards the sun and takes the sun's part away where something is in the
   way. Ambient occlusion (VkShadowView.ao) uses the same structures and attachments, attachment 2
   then RGBA8 with the face normal and how much ambient occlusion the surface takes:
   shaders/ao.frag traces short rays into a small image (about 400 rows), blurs them and darkens
   the colour less its sun and fog parts, before the shadows' pass (vk_shadow_end, ao_passes).
   Reflections (VkShadowView.refl) draw the game's reflective draws without their texture's colour
   and again after the shadows with what rays meet in its place (refl_gather, refl_replay,
   shaders/draw.frag RT_REFLECT, refl.glsl); those that meet nothing look for the sky dome in a top
   level of its own (descriptor SH_SLOTS + the view's slot). */
#define FLARES 12                 /* headlights: lamps' flares a view (two a beam, the first 6 beams) */
typedef struct {
    float inv_vp[16];             /* as shaders/shadow.glsl Globals (std430, 432 bytes) */
    float place[4], place2[4], sun[4], cam[4], fade[4];
    int32_t slots[4], slots2[4];
    float ray0[4], rayx[4], rayy[4];
    int32_t tiles[16], rslots[4];
    float terr[4], tavg[4], refl[4], fogp[4], fogcol[4], fogedge[4], refl2[4], sunc[4];
    int32_t beamn[4];
    float hl[4], hl2[4], hl3[4];
    float beams[VK_BEAMS][16];
    int32_t flaren[4];
    float flare[2 * FLARES][4];
} ShadowGlobals;
typedef struct {
    uint64_t vaddr, iaddr;
    int32_t tex, smp;
    float aref;
    uint32_t flags;
    uint32_t tnl, misc;
} ShadowGeom;
_Static_assert(sizeof(ShadowGlobals) == 1920 && sizeof(ShadowGeom) == 40, "std430 layouts");
#define SH_GEOMS 4096             /* geometries a view (its draws; the last SH_SKY the sky's) */
#define SH_SKY 8
#define SKY_BASE (SH_GEOMS - SH_SKY)
#define INST_TERRAIN 0x800000u    /* the terrain's instances' custom index (shaders/refl.glsl) */
#define REFL_MAX 1024             /* reflective draws a view (drawn again after it) */
#define SH_VIEWS 4                /* views a frame with shadows (the race's main view, its mirrors) */
#define SH_SLOTS (FRAMES * SH_VIEWS)   /* a view's slot: frame * SH_VIEWS + view */
_Static_assert(SH_SLOTS == 8, "shaders/shadow.glsl and resolve.frag have 8 slots");
#define SH_INST 32                /* instances in the top level: the frame's objects, map copies */
typedef struct {                  /* a buffer with a device address */
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    VkDeviceAddress addr;
    uint8_t *map;
} DBuf;
typedef struct {                  /* an acceleration structure and its buffer */
    VkAccelerationStructureKHR as;
    DBuf b;
    VkDeviceAddress addr;
} Accel;
static int rt_ok;                 /* the device traces rays (VK_KHR_ray_query) */
static int rt_trace = 1;          /* TD6_RT_TRACE=0: all but the rays (to time the parts) */
static int rt_opaque;             /* TD6_RT_OPAQUE=1: no cut-outs (tests) */
static int rt_full;               /* TD6_RT_FULL=1: a ray per pixel with 4X anti-aliasing too */
static int rt_debug;              /* TD6_RT_DEBUG=n: resolve.frag's debug views */
static int rt_log;                /* TD6_SH_LOG=2: every view's ray tracing logged */
static PFN_vkCreateAccelerationStructureKHR p_create_as;
static PFN_vkDestroyAccelerationStructureKHR p_destroy_as;
static PFN_vkGetAccelerationStructureBuildSizesKHR p_build_sizes;
static PFN_vkCmdBuildAccelerationStructuresKHR p_build_as;
static PFN_vkGetAccelerationStructureDeviceAddressKHR p_as_addr;
static VkShaderModule sm_rf;      /* shaders/resolve.frag */
static DBuf sh_buf;               /* ShadowGlobals[SH_SLOTS], then ShadowGeom[SH_SLOTS][SH_GEOMS] (binding 3) */
static DBuf sh_scratch[FRAMES];   /* the frames' build scratch (their views' builds one after another) */
static struct ShFrame {           /* a view slot's structures and gathered geometries */
    Accel blas, tlas;
    DBuf inst, xform;             /* top-level instances, the geometries' 3x4 matrices */
    VkAccelerationStructureGeometryKHR *geo;
    VkAccelerationStructureBuildRangeInfoKHR *range;
    uint32_t *maxprim;
    uint32_t ngeo;
    unsigned long tris;
    /* reflections: the sky dome's geometries (a bottom level of their own, in a top level of its own:
       in the frame's it would lie round every other instance) */
    Accel sky, sky_tlas;
    DBuf sky_inst;
    VkAccelerationStructureGeometryKHR sky_geo[SH_SKY];
    VkAccelerationStructureBuildRangeInfoKHR sky_range[SH_SKY];
    uint32_t sky_maxprim[SH_SKY], nsky;
    uint64_t sky_hash, sky_built;  /* the view's sky mesh (round the camera: the same frame to frame), the
                                     built one's (0: none) */
} shf[SH_SLOTS];
/* Ray-traced reflections: the view's reflective draws, drawn in it without their texture's colour
   and again after it with what rays meet (draw.frag RT_REFLECT) */
typedef struct {
    PushDraw pc;
    VkBuffer vbuf, ibuf;
    VkDeviceSize voff, ioff;
    uint32_t nidx, first;
    int32_t vbase;
    int sc[4], topo, ztest;
    VkCullModeFlags cull;
    VkCompareOp zf;
    float bias;
    VkFormat zfmt;
} ReflDraw;
static ReflDraw refl_draws[REFL_MAX];
static int nrefl;
static int refl_trace = 1;        /* TD6_REFL_TRACE=0: the reflective draws again as the game drew them (timing) */
static int refl_share = 1;        /* TD6_REFL_SHARE=0: a ray for every pixel (no half-size pass; tests) */
static int refl_debug;            /* TD6_REFL_DEBUG=1: draw.frag RT_REFLECT's debug view */
static int refl_half;             /* TD6_REFL_HALF=1: FULL's glass and chrome at half size too (their reflections blur) */
static GpuImg *att_rs[3];         /* reflections' rays at a half, a quarter, an eighth of the size each way: what
                                     they meet (rgb), the eye distance (AO_FORMAT) */
static float refl_sd[2] = {0.03f, 0.10f};   /* TD6_REFL_SPREAD=a,b: LIGHT's game look from a to b (refl2.zw) */
static uint32_t refl_lrate[3] = {3, 3, 3};   /* TD6_REFL_LRATE=g,p,w: LIGHT's rates (log2) for glass and chrome, paint, water */
static float refl_fresnel = -1, refl_darken = -1, refl_gain = -1;   /* TD6_REFL_FRESNEL, _DARKEN, _GAIN: override
                                     the view's */
/* the terrain's colour map as its colour pass draws it: 16 tiles' textures (z / 64 * 4 + x / 64),
   their sampler, the pass's vertex grey, each tile's mean colour */
static GpuImg *tile_img[16];
static int tile_smp = -1;
static float tile_grey = 1.0f, tile_avg[16][3];
static struct {                   /* the view being drawn */
    int active, broken, cls, have_cam;
    int slot;                     /* its slot (frame * SH_VIEWS + view) */
    GpuImg *c, *z;                /* its render target and depth buffer */
    int cleared;                  /* the extra attachments were cleared this view */
    int sc[4];                    /* where it drew (GPU pixels) */
    VkShadowView v;
    double vp[16];                /* its camera: view * projection (D3D row vectors) */
    float place[6];               /* ax bx ay by mnz dz */
    uint32_t fog;                 /* its terrain's fog (ambient occlusion leaves fog alone): colour (ARGB), */
    float fogp[4];                /* ... start, end, density, how (ao.frag: mode | range << 2 | edge << 3) */
    float fog_edge[2];            /* ... drawdist.c's full fog at the view's edge: end, start */
    float fwd[4];                 /* its camera's depth axis: view z = p . fwd + fwd[3] */
    float focal;                  /* its focal length in GPU pixels (vertical) */
    int beams;                    /* headlights: beams light it (resolve.frag pass 6) */
    int hl_rect[4];               /* ... the GPU pixels their lit room can cover (x0 y0 x1 y1) */
    float lamp[VK_BEAMS][2];      /* ... each one's car's lamps either side of it, its fade (G's order) */
    int have_amb, have_tcol;      /* ... the light its lit objects and its terrain were drawn with is known */
} sv;
/* Volumetric smoke and dust (the section "volumetric smoke and dust" below): the last view whose
   acceleration structures were built with the sun shadows traced (its puffs' sun grids use them) */
static struct { int ok, slot; GpuImg *c; unsigned long frame; } sm_rt;
#define SMOKE_MAX 512             /* puffs a view draws (the nearest) */
#define SMOKE_SLOTS 4             /* views a frame with puffs */
#define SMOKE_HDR 16              /* a view's block (shaders/smoke.glsl): its own vec4s, */
#define SMOKE_PV 6                /* ... a puff's */
#define SMOKE_BLOCK (SMOKE_HDR + SMOKE_PV * SMOKE_MAX)
static DBuf sm_buf;               /* the frames' views' blocks (binding 6) */
static int sm_slots_used;         /* this frame's blocks */
static int sm_debug;              /* TD6_SMOKE_DEBUG: 1 the volumes alone, over grey; 3 the puffs' rectangles;
                                     7 the puffs taken, none drawn; 8 the sun's share and self-shadow; 9 in
                                     front of glass red, behind it blue */
static int sm_steps;              /* TD6_SMOKE_STEPS: every puff's steps (0: by its size) */
static int sm_scale;              /* TD6_SMOKE_SCALE: the render target's pixels a march pixel each way (0: 2, 4X: 4) */
static float sm_budget = 10.0f;   /* TD6_SMOKE_BUDGET: the march's work at most (steps over the smaller image's
                                     area, so many times) before the puffs' steps are made coarser */
static float sm_density = 1.0f;   /* TD6_SMOKE_DENSITY: scales the puffs' density */
static int sm_log;                /* TD6_SMOKE_LOG=1 */
static int sm_glass = 1;          /* TD6_SMOKE_GLASS=0: puffs behind glass drawn over it (timing) */
static int sm_shadows = 1;        /* TD6_SMOKE_SHADOWS=0: the puffs cast no shadows */
static int sm_clip = 1;           /* TD6_SMOKE_CLIP=0: no cabins behind car glass, no edge share (to compare) */
#define SMOKE_SHADOWS 64          /* puffs a view casting shadows (the nearest) */
static float sm_widen = 1.4f;     /* TD6_SMOKE_WIDEN: a puff's ball against its sprite's blob (as faint in all) */
/* (vk_smoke_parts, smoke.c's "the see-through parts are being drawn" (0x419280): vkthread.c's, vkr's here) */
static float vk_smoke_early(GpuImg *c, GpuImg *z, int slot);
static void sm_glass_add(GpuImg *c, const PushDraw *pc, int topo, int tnl, VkBuffer vbuf, VkBuffer ibuf, VkDeviceSize voff,
                         VkDeviceSize ioff, uint32_t nidx, uint32_t first, int32_t vbase, const int *sc,
                         VkCullModeFlags cull, VkFormat zfmt, const int *bb, float zmin);
static double st_gpu_sm_ms, st_gpu_sm_max, st_gpu_sm1_ms;   /* TD6_VK_STATS: the smoke's GPU ms (the frame's first
                                     view with puffs), the most in a frame, its first passes' (sm_first) */
/* Temporal anti-aliasing's scene (the race's main view drawn into images of its own; the section
   "temporal anti-aliasing" below) */
typedef struct { float prev[16]; float jit[4]; } MotionConst;     /* shaders/motion.glsl Motion */
_Static_assert(sizeof(MotionConst) == 80, "std430 layout");
static struct {
    int active;                   /* inside the main view */
    int fsr;                      /* ... resolved by FSR 2 (else "colour_precision = high" alone: its colour in 16 bits
                                     at the back buffer's size, no jitter, vectors or masks; copied with the dither) */
    int drawn;                    /* its images are in use (its first draw or clear came) */
    GpuImg *dst, *dz;             /* the back buffer it is resolved into, and its depth buffer */
    GpuImg *c, *z, *vel, *mask;   /* its colour, depth, motion vectors, masks (render size) */
    GpuImg *out;                  /* FSR 2's output (the back buffer's size) */
    GpuImg *hist[2];              /* ANTI-ALIASING TAA's history, this frame's and the last (the back buffer's size) */
    int hcur;                     /* ... which is this frame's */
    VkImageView zview;            /* its depth's depth-only view (FSR 2 samples it) */
    int32_t cam[3], pcam[3];      /* the camera (16.16 units), the previous scene's */
    double V[16], P[16];          /* this scene's view and projection (D3D, its first perspective draw) */
    double pV[16], pP[16];        /* ... the previous scene's */
    double d[3];                  /* the camera's move since the previous scene (game units) */
    int have_vp, prev_ok;         /* this scene's camera is known; the previous scene's can be reprojected */
    int reset;                    /* no history (a camera cut, new sizes, the first scene) */
    uint32_t phase;               /* the jitter sequence's next */
    float jx, jy;                 /* this scene's jitter (scene pixels) and in Vulkan clip space per w */
    float jndc[2];
    int cls;                      /* VK_SC_* of the next draws */
    int sc[4];                    /* where it drew (scene pixels) */
    unsigned long last;           /* nframes of the last scene resolved */
    double t_last;                /* ... its time (FSR 2's frame time) */
    uint32_t fw, fh, fdw, fdh;    /* the sizes FSR 2's context has */
    int backend;                  /* its images are for DLSS (1), FSR 2 (0) or TAA (2) */
    int dyn;                      /* ... its context for RENDER SCALE AUTO (up to the back buffer's size) */
    int clear_set;                /* the back buffer's colour was cleared this frame before the scene: */
    unsigned long clear_frame;    /* ... (nframes then) */
    VkClearColorValue clear_col;  /* ... to this */
} scn;
static int r_mot, want_mot;       /* the rendering has the scene's attachments 3, 4; begin_rendering may add them */
static int taa_ok;                /* FSR 2 is built in and the device can run it */
static int dlss_ok;               /* DLSS is built in and the device can run it (dlss.c: NVIDIA RTX) */
static int aa_dlss;               /* "antialiasing = dlss": the scene resolved by DLSS instead of FSR 2 */
static int aa_own;                /* "antialiasing = taa": ... by the port's own upscaler (taau.frag), lighter */
static int own_ok;                /* the scene's images can be made (all TAA needs) */
static int taa_can(void) { return aa_own ? own_ok : aa_dlss ? dlss_ok : taa_ok; }   /* the chosen one can run here */
static int aa_backend(void) { return aa_own ? 2 : aa_dlss; }      /* (scn.backend) */
static MotionConst *motmap;       /* the frames' Motion blocks (TNL_MAX each, at the draws' TnlConst index) */
static int taa_debug;             /* TD6_TAA_DEBUG */
static int taa_jitter = 1;        /* TD6_TAA_JITTER=0: none (tests) */
static int taa_match = 1;         /* TD6_TAA_MATCH=0: no draws matched (tests) */
static float taa_sharp = 0.25f;   /* TD6_TAA_SHARPEN: RCAS's sharpness, 0..1 (below 0 none) */
static float taa_react = 1.0f;    /* TD6_TAA_REACTIVE: the reactive mask's scale */
static int taa_log;               /* TD6_TAA_LOG=1 */
static float taa_mip;             /* the scene's textures' level of detail bias: log2(render / display) - 1 (FSR 2's
                                     advice; TD6_TAA_MIPBIAS adds to it) */
static float taa_mip_add;
static VkImageView tc_view;       /* the masks' image seen as its g channel (FSR 2's transparency mask) */
static unsigned long st_taa_scenes, st_taa_draws, st_taa_matched, st_taa_moving;   /* TD6_VK_STATS */
static double st_taa_cpu_ms, st_gpu_taa_ms;
static uint32_t draw_mot;         /* the next draw pipeline's: 1 | attachment 3 written (1 replaced, 2 by alpha) << 1 |
                                     attachment 4 written << 3 (0: no motion attachments) */
static GpuImg *att_sun, *att_geo; /* the extra attachments (the size of sv.c) */
static int hl_debug;              /* TD6_HL_DEBUG: 1 the beams' shadows red, 2 no rays, 3 the pass without its
                                     ray, 4 the ray without cut-outs (timing, look) */
static int hl_terrain;            /* TD6_HL_TERRAIN=1: the terrain casts the beams' shadows too */
static int hl_max = 6;            /* TD6_HL_MAX: beams at most a view (the nearest the camera in it) */
static GpuImg *att_hlc, *att_hlh; /* ... the view's colour copied for their pass (resolve.frag pass 7), their
                                     light at half the size (pass 6) */
static int hl_log, hl_logged;     /* TD6_HL_LOG=1: the light the terrain and objects were drawn with, once */
static void beams_cull(uint32_t cw, uint32_t ch);
static GpuImg *sm_noise;          /* (the volumetric smoke's tiled 3D noise: VOLUMETRIC's air takes it too) */
static void sm_noise_make(void);
static void sm_noise_start(void);
static GpuImg *att_sh[3];         /* the shadows' images (resolve.frag): T, a pixel per 1 x 1 (2 x 2 with 4X AA);
                                     U and its filtered copy, half T's size */
static int sh_soft = 1;           /* TD6_SH_SOFT=0: hard edges */
static float sh_sun = 1.0f;       /* TD6_SH_SUN: the sun's angular size for the penumbrae, degrees */
static float sh_keep = 0.5f;      /* TD6_SH_KEEP: how much of the hard shadow a soft one keeps (thin casters) */
static int sh_rmax = 8;           /* TD6_SH_RMAX: the widest penumbra half-width at 1600 rows, pixels */
static int sh_nearest;            /* TD6_SH_NEAREST=1: the nearest blocker (else the first found) */
static int rt_nofade;             /* TD6_RT_NOFADE=1: objects in the distance fade block in full (tests) */
static int sh_time;               /* TD6_SH_TIME=1: without ambient occlusion, its timestamps time the shadows' passes */
static GpuImg *att_ao[2];         /* ambient occlusion: the small images (traced, blurred across) */
static VkShaderModule sm_ao;      /* shaders/ao.frag */
static int ao_rays = -1;          /* TD6_AO_RAYS: rays per small-image pixel (-1: the view's, 3 FULL, 2 LIGHT) */
static int ao_scale;              /* TD6_AO_SCALE: view pixels per small-image pixel each way (0: the view's rows,
                                     about 400 FULL, 267 LIGHT) */
static int ao_blur = 1;           /* TD6_AO_BLUR=0: no blur (tests) */
static int ao_debug;              /* TD6_AO_DEBUG=n: ao.frag's debug views (1 the darkening alone, grey) */
static int ao_flags;              /* TD6_AO_FLAGS: 1 first hit, 2 all opaque, 4 no terrain (timing) */
static VkDeviceSize tlas_scratch; /* what a top level's build needs */
static Accel terr_blas;           /* the map's terrain */
static DBuf terr_vb, terr_ib;
static GpuImg *lm_img;            /* the game's object light map (R8, 256 x 256) */
static unsigned long st_rt_geo, st_rt_tris, st_rt_views;   /* TD6_VK_STATS */
static unsigned long st_rt_cut, st_rt_cut_tris;            /* ... the geometries a hit is tested on (not opaque) */
static double st_rt_ms, st_rt_sizes_ms, st_rt_draw_ms;   /* ... the CPU at the views' ends, the sizes, the draws' part */

static int state;                 /* 0 not tried, 1 running, -1 off */
static int offscreen;
static SDL_Window *window;
static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkSurfaceKHR surface;
static VkPhysicalDeviceMemoryProperties memprops;
static VkFormat depth24;
static int depth24_stencil;
static VkDescriptorSetLayout dsl;
static VkDescriptorPool dpool;
static VkDescriptorSet dset;
static VkPipelineLayout playout;
static VkBuffer tnlbuf;            /* GPU transform and lighting constants (TnlConst) */
static VkDeviceMemory tnlmem;
static VkShaderModule sm_dv, sm_df, sm_pv, sm_pf, sm_tv, sm_dfs;   /* sm_dfs: draw.frag with the shadow outputs */
static VkShaderModule sm_dfr;     /* draw.frag RT_REFLECT: reflective draws again after their view */
static VkShaderModule sm_dfrm;     /* draw.frag RT_REFLECT with MOTION: the reflections drawn again in the scene */
static VkShaderModule sm_tvm, sm_dvm, sm_dfm, sm_dfsm;   /* tnl.vert, draw.vert, draw.frag (and with the shadow outputs)
                                     with MOTION: temporal anti-aliasing's scene */
static VkShaderModule sm_taa;     /* shaders/taa.frag: FSR 2's output into the back buffer */
static VkShaderModule sm_taau;    /* shaders/taau.frag: ANTI-ALIASING TAA, the port's own upscaler */
static VkShaderModule sm_smv, sm_smf, sm_smfm, sm_smvis;   /* volumetric smoke: smoke.vert, smoke.frag (and with
                                     MOTION), smokevis.frag (its sun grid, with ray queries) */
static uint32_t ntex_slots;
static int *free_slots, nfree_slots;

static Frame fr[FRAMES];
static int fi;
static VkCommandBuffer cb;

static GpuImg *r_color, *r_depth;
static int bg_phase = 1;          /* widescreen: still in the frame's leading background tiles */

/* Render resolution (options.txt "resolution"): "original" draws at the program's display
   mode and scales the picture to the window; "native" draws render targets at the size the
   picture has in the window, so the 3D world and the menus come out at the screen's own
   resolution; a number draws at that multiple of the mode. The program never sees the
   difference: coordinates stay in its own pixels. */
static int res_native;
static float res_fixed = 1.0f, res_scale = 1.0f;
static float res_aa = 1.0f;          /* "antialiasing = 4x": render at twice the size each way */
static int aa_temporal;              /* "antialiasing = fsr": temporal anti-aliasing (the scene: vk_scene_begin) */
static int hp_want;                  /* "colour_precision = high": the scene drawn with 16 bits a channel (hp_format),
                                        dithered into the 8-bit back buffer at its end (the section "colour precision") */
static VkFormat hp_format;           /* ... its colour format (VK_FORMAT_UNDEFINED: the device cannot blend one) */
static int hp_dither = 1;            /* TD6_HP_DITHER=0: rounded without the dither (tests) */
static VkFormat hp_sun = AUX_FORMAT; /* ... the sun's part of the colour in its scene (aux_format): 10 bits a channel
                                        (TD6_HP_SUN=8 or 16: 8 or the colour's 16, to compare) */
static int hp_flip;                  /* TD6_HP_FLIP=n: ORIGINAL and HIGH in turn, n frames each (timing them side by
                                        side on a busy machine, with TD6_VK_STATS=2) */
static GpuImg *noise_img;            /* the dither's blue noise (made when first needed) */
static float taa_ratio = 1.0f;       /* "render_scale": the scene drawn at 1 / this of the back buffer's size */
static int want_vsync = 1;           /* "vsync = off": mailbox or immediate presentation */
static int tex_smooth;               /* "textures = smooth": trilinear + anisotropic on mipmapped textures */
static int aniso_ok;                 /* the device can filter anisotropically */
static float aniso_max = 1.0f;

static int rendering;
static VkPipeline bound_pipe;

/* swapchain or the offscreen picture */
static VkSwapchainKHR swapchain;
static VkFormat sc_format;
static uint32_t sc_w, sc_h, sc_count;
static VkImage sc_images[8];
static VkImageView sc_views[8];
static VkSemaphore sc_done[8];
static int sc_stale;
static VkPresentModeKHR sc_mode = VK_PRESENT_MODE_FIFO_KHR;
static GpuImg *out_img;
static GpuImg *gamma_img;
static uint16_t gamma_seen[3][256];
static int gamma_valid;

/* readback */
static VkBuffer rb_buf;
static VkDeviceMemory rb_mem;
static uint8_t *rb_map;
static VkDeviceSize rb_size;

/* frame dumps */
static int dumping, dump_present;
static char dumpdir[900];
static unsigned long dump_every = 30, dump_from, nframes, readbacks;
/* per-frame counters, logged every 100 frames with --log d3d or TD6_VK_STATS=1 */
static unsigned long st_draws, st_uploads, st_upload_bytes, st_passes;
static int st_print;
/* TD6_VK_STATS: the GPU's time per frame, from timestamps at the start and the end of each
   frame's command buffer (read back when the frame's fence is waited for) */
static VkQueryPool st_queries;
#define TS 15                     /* timestamps a frame: start, end, ray tracing start, built, ambient occlusion
                                     traced, blurred, applied, shadows traced, reflections drawn; temporal
                                     anti-aliasing start, end; volumetric smoke's first passes start, end, its
                                     march and applying start, end */
static double st_tick_ns, st_gpu_ms, st_gpu_sh_ms, st_gpu_build_ms, st_gpu_ao_ms[3], st_gpu_refl_ms;   /* GPU ms
                                     per frame; ray tracing's (structures, ambient occlusion, shadows and
                                     reflections); the builds'; AO's trace, blur and apply; the reflections' */
static unsigned long st_refl_draws;   /* reflective draws drawn again */
static int st_gpu_n;
static long hud_log_frame;
static double st_draw_ms, st_present_ms, st_last, st_max, st_over, st_cpu0;
static double st_gpu_gap_ms, st_gpu_gap_n;          /* ... the GPU idle between frames (timestamps) */
static double st_barriers;                          /* ... image barriers recorded */
static uint64_t st_gpu_end;
static unsigned long st_gpu_end_frame;
static double thread_cpu_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void res_update(void)
{
    int w = 0, h = 0;
    if (offscreen && out_img) { w = (int)out_img->w; h = (int)out_img->h; }
    else if (window) SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w > 0 && h > 0)
        widescreen_screen(w, h);          /* "widescreen = on" follows the window's shape */
    float sc = res_fixed;
    if (res_native && gfx_mode.set && gfx_mode.w && gfx_mode.h && w > 0 && h > 0) {
        float aspect = widescreen_on() ? ws_aspect : (float)gfx_mode.w / (float)gfx_mode.h;
        float a = (float)w / ((float)gfx_mode.h * aspect), b = (float)h / (float)gfx_mode.h;
        sc = a < b ? a : b;
    }
    if (sc < 1.0f) sc = 1.0f;
    sc *= res_aa;
    if (sc > 8.0f) sc = 8.0f;
    res_scale = sc;
}

/* ------------------------------------------------------------------ automatic render scale
   RENDER SCALE AUTO (render_scale = auto, with FSR 2 or DLSS): the scene's size follows the GPU's time
   a frame against the frame's time: FRAME LIMIT's rate, else the screen's refresh (TD6_RS_TARGET=
   <fps> sets it, for tests). It starts at QUALITY's size. Measured over RS_WIN frames that drew the
   scene (menus and loading left out), their median: on a laptop's APU the CPU and the GPU share the
   power and the GPU's time swings by milliseconds from frame to frame (the Linux session's 890M). The
   GPU's time is taken as F + P / ratio^2: a part that does not shrink with the scene (the upscaler's
   own pass: FSR 2 about 3.7 ms at 1440p on the 890M, whatever the scale) and one that does, worked out
   from the last two measurements at ratios at least 0.15 apart, F kept within 20-85 % of the time (the
   scene itself changes between them too; one alone: F a third of it). Over 85 % of the frame's time:
   straight to the ratio that model puts at 75 %; if even the smallest size would not reach it, the
   smallest (no sitting at native for nothing). Under 65 % twice running: up to the ratio it puts at
   65 %, by 0.25 at most. Ratios in steps of 0.05 from 1 (native) to 2 (PERFORMANCE's: smaller costs
   the picture more than it wins back past the upscaler's own pass), never under 540 rows, with DLSS
   inside its mode's range. The scene's images
   are made again at each size (scene_images: no wait, the history kept). TD6_RS_LOG=1 logs each
   measurement and change. */
#define SCALE_STEP      0.05f
#define SCALE_MAX       2.0f            /* PERFORMANCE's: the smallest it goes (and where it stays when the
                                           frame's time is out of reach: no sitting at a smaller size for little) */
#define SCALE_START     1.5f            /* QUALITY's: where it starts */
#define SCALE_DLSS_MODE 1.5f            /* DLSS's mode for its context with AUTO (QUALITY) */
#define RS_WIN          90              /* scene frames a measurement */
#define RS_SETTLE       8               /* frames after a change before its measurement starts */
static int rs_auto, rs_log;
static float rs_cur = SCALE_START;      /* the ratio now (scene = back buffer / this) */
static double rs_target_ms = 1000.0 / 60.0, rs_gpu;     /* the frame's time; the last measurement */
static double rs_noise;                 /* TD6_RS_NOISE (tests) */
static unsigned long rs_changed;        /* nframes at the last change */
static float rs_win[RS_WIN];            /* this measurement's frames */
static int rs_nwin, rs_room;            /* ... how many; measurements running with room to grow */
static struct { float r; double ms; unsigned long at; } rs_pt[2];     /* the last two at different ratios */
static int rs_npt;
static uint32_t rs_rows;                /* the back buffer's rows (the 540-row floor) */

static void rs_options(int was_auto)    /* (the game thread: vk_options) */
{
    const char *lim = option_str("frame_limit", "off");
    int fps = !strcasecmp(lim, "screen") ? frame_limit_screen() : atoi(lim);
    if (fps < 10)
        fps = host_refresh_hz();
    const char *e = getenv("TD6_RS_TARGET");
    if (e && atoi(e) >= 10)
        fps = atoi(e);
    rs_target_ms = 1000.0 / (fps >= 10 ? fps : 60);
    rs_log = getenv("TD6_RS_LOG") != NULL;
    rs_noise = getenv("TD6_RS_NOISE") ? atof(getenv("TD6_RS_NOISE")) : 0;
    if (rs_auto && !was_auto)
        rs_cur = SCALE_START;
    rs_nwin = rs_npt = rs_room = 0;     /* (other options cost differently: measured anew) */
    rs_changed = nframes;
    if (rs_auto && rs_log)
        LOG(LOG_ALWAYS, "vk: render scale auto: the frame's time %.2f ms (%d fps)", rs_target_ms, fps);
}
/* the ratios the scene may have for back buffer c: 1 to 3 (never under 540 rows); with DLSS, inside
   the sizes its mode takes (dlss_ranges: the mode taking the most, native included if one does) */
static float rs_lo = 1.0f, rs_hi = SCALE_MAX, rs_dlss_mode;
static void rs_bounds(const GpuImg *c)
{
    static uint32_t bw, bh;
    static int bdlss = -1;
    rs_rows = c->h;
    if (c->w == bw && c->h == bh && aa_dlss == bdlss)
        return;
    bw = c->w;
    bh = c->h;
    bdlss = aa_dlss;
    rs_lo = 1.0f;
    rs_hi = (float)c->h / 540.0f;
    rs_dlss_mode = 0;
#ifdef HAVE_DLSS
    if (aa_dlss && dlss_ok) {
        static const float modes[4] = {1.0f, 1.5f, 1.7f, 2.0f};
        float lo[4], hi[4];
        dlss_ranges(c->w, c->h, lo, hi);
        int best = -1;
        for (int m = 0; m < 4; m++) {
            if (!lo[m] || !hi[m])
                continue;
            int nat = lo[m] < 1.01f, bnat = best >= 0 && lo[best] < 1.01f;
            if (best < 0 || (nat && !bnat) || (nat == bnat && hi[m] > hi[best]))
                best = m;
        }
        if (best >= 0) {
            rs_lo = lo[best] < 1.0f ? 1.0f : lo[best];
            rs_hi = hi[best] < rs_hi ? hi[best] : rs_hi;
            rs_dlss_mode = modes[best];
        } else
            rs_dlss_mode = SCALE_DLSS_MODE;
        LOG(LOG_ALWAYS, "vk: render scale auto with DLSS at %ux%u: ratios DLAA %.2f-%.2f, QUALITY %.2f-%.2f, BALANCED "
            "%.2f-%.2f, PERFORMANCE %.2f-%.2f; scene 1 / %.2f to 1 / %.2f", c->w, c->h, lo[0], hi[0], lo[1], hi[1], lo[2],
            hi[2], lo[3], hi[3], rs_lo, rs_hi < SCALE_MAX ? rs_hi : SCALE_MAX);
    }
#endif
    rs_hi = rs_hi < rs_lo ? rs_lo : rs_hi > SCALE_MAX ? SCALE_MAX : rs_hi;
}

static float rs_ratio(const GpuImg *c)  /* the scene's ratio for back buffer c */
{
    if (!rs_auto)
        return taa_ratio;
    rs_bounds(c);
    return rs_cur < rs_lo ? rs_lo : rs_cur > rs_hi ? rs_hi : rs_cur;
}


static void rs_set(float r)
{
    rs_cur = r;
    rs_changed = nframes;
    rs_nwin = 0;
}

static int rs_cmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* a frame's GPU time (frame_begin, as its slot comes round); scene: it drew the scene with the upscaler */
static void rs_sample(double ms, int scene)
{
    if (!rs_auto || !aa_temporal || !scene || nframes - rs_changed < RS_SETTLE)
        return;
    if (rs_noise > 0)                   /* (TD6_RS_NOISE=<ms>: each frame's time off by up to that, for tests) */
        ms += rs_noise * (2.0 * rand() / RAND_MAX - 1.0);
    rs_win[rs_nwin++] = (float)ms;
    if (rs_nwin < RS_WIN)
        return;
    qsort(rs_win, RS_WIN, sizeof *rs_win, rs_cmp);
    double m = rs_win[RS_WIN / 2];
    rs_nwin = 0;
    rs_gpu = m;
    float r = rs_cur;
    if (rs_npt && fabsf(rs_pt[0].r - r) < 0.01f)
        rs_pt[0].ms = m, rs_pt[0].at = nframes;
    else {
        rs_pt[1] = rs_pt[0];
        rs_pt[0].r = r, rs_pt[0].ms = m, rs_pt[0].at = nframes;
        rs_npt = rs_npt < 2 ? rs_npt + 1 : 2;
    }
    /* the model: m = F + P / r^2 */
    double F, P, x0 = 1.0 / ((double)r * r);
    if (rs_npt == 2 && fabsf(rs_pt[1].r - r) >= 0.15f && nframes - rs_pt[1].at < 3600) {
        double x1 = 1.0 / ((double)rs_pt[1].r * rs_pt[1].r);
        P = (m - rs_pt[1].ms) / (x0 - x1);
        if (P < 0.1 * m / x0)           /* (no gain seen, or noise the other way: a little) */
            P = 0.1 * m / x0;
        F = m - P * x0;
        F = F < 0.2 * m ? 0.2 * m : F > 0.85 * m ? 0.85 * m : F;   /* (two windows apart the scene itself changes too) */
        P = (m - F) / x0;
    } else {
        F = m / 3.0;
        P = (m - F) / x0;
    }
    double T = rs_target_ms;
    float want = r;
    if (m > 0.85 * T) {
        rs_room = 0;
        if (F + P / ((double)rs_hi * rs_hi) > 0.85 * T)
            want = rs_hi;               /* (out of reach) */
        else
            want = (float)sqrt(P / (0.75 * T - F));
        want = ceilf(want / SCALE_STEP - 0.01f) * SCALE_STEP;
    } else if (m < 0.65 * T && ++rs_room >= 2) {
        want = 0.65 * T > F ? (float)sqrt(P / (0.65 * T - F)) : r;
        want = ceilf(want / SCALE_STEP - 0.01f) * SCALE_STEP;
        if (want < r - 0.25f)
            want = r - 0.25f;
        if (want > r)
            want = r;
    } else if (m >= 0.65 * T)
        rs_room = 0;
    want = want < rs_lo ? rs_lo : want > rs_hi ? rs_hi : want;
    if (rs_log)
        LOG(LOG_ALWAYS, "vk: render scale auto: 1 / %.2f GPU %.2f ms (median of %d) against %.2f: fixed %.2f, scaled %.2f%s",
            r, m, RS_WIN, T, F, P * x0, fabsf(want - r) >= SCALE_STEP / 2 ? "" : " (stays)");
    if (fabsf(want - r) >= SCALE_STEP / 2) {
        if (want < r)
            rs_room = 0;
        rs_set(want);
    }
}

/* Enhanced options: read at start-up and again whenever the in-game page changes one */
void vkr_options(void)
{
    const char *res = option_str("resolution", "original");
    res_native = 0;
    res_fixed = 1.0f;
    if (!strcasecmp(res, "native"))
        res_native = 1;
    else if (atof(res) >= 1.0)
        res_fixed = (float)atof(res);
    else if (strcasecmp(res, "original"))
        LOG(LOG_ALWAYS, "options: resolution \"%s\" is not original, native or a number", res);
    const char *aa = option_str("antialiasing", "off");
    res_aa = !strcasecmp(aa, "4x") ? 2.0f : 1.0f;
    aa_dlss = !strcasecmp(aa, "dlss");
    aa_own = !strcasecmp(aa, "taa");
    aa_temporal = !strcasecmp(aa, "fsr") || aa_dlss || aa_own;
    if (!aa_temporal && res_aa < 1.5f && strcasecmp(aa, "off"))
        LOG(LOG_ALWAYS, "options: antialiasing \"%s\" is not off, 4x, taa, fsr or dlss", aa);
    const char *rsc = option_str("render_scale", "native");
    int was_auto = rs_auto;
    rs_auto = !strcasecmp(rsc, "auto");
    taa_ratio = !strcasecmp(rsc, "quality") ? 1.5f : !strcasecmp(rsc, "balanced") ? 1.7f
              : !strcasecmp(rsc, "performance") ? 2.0f : 1.0f;
    if (taa_ratio == 1.0f && strcasecmp(rsc, "native") && !rs_auto)
        LOG(LOG_ALWAYS, "options: render_scale \"%s\" is not native, quality, balanced, performance or auto", rsc);
    rs_options(was_auto);
    const char *vs = getenv("TD6_VSYNC");
    int v = vs ? strcmp(vs, "0") != 0 : option_bool("vsync", 1);
    if (v != want_vsync) {
        want_vsync = v;
        sc_stale = 1;
    }
    tex_smooth = !strcasecmp(option_str("textures", "original"), "smooth");
    const char *cp = option_str("colour_precision", "original");
    hp_want = !strcasecmp(cp, "high");
    if (!hp_want && strcasecmp(cp, "original"))
        LOG(LOG_ALWAYS, "options: colour_precision \"%s\" is not original or high", cp);
    res_update();
}

static char gpu_name[256];

/* "Vulkan: 1920x1080, <GPU>" for the Video Options page (kept short: it shares the title's row) */
void vk_render_info(char *out, int n)
{
    char gpu[40];
    snprintf(gpu, sizeof gpu, "%s", gpu_name);
    size_t k = strlen(gpu);
    if (k > 9 && !strcmp(gpu + k - 9, " Graphics")) gpu[k - 9] = 0;
    if (!gfx_mode.set || !gfx_mode.w) {
        snprintf(out, (size_t)n, "Vulkan, %s", gpu);
        return;
    }
    uint32_t ph = (uint32_t)lroundf((float)gfx_mode.h * res_scale);
    uint32_t pw = widescreen_on() && ws_aspect * gfx_mode.h > gfx_mode.w ? (uint32_t)lroundf((float)ph * ws_aspect)
                                                                           : (uint32_t)lroundf((float)gfx_mode.w * res_scale);
    float ratio = rs_auto ? rs_cur : taa_ratio;
    if (aa_temporal && taa_can() && (ratio > 1.0f || rs_auto))   /* temporal anti-aliasing: the 3D world from a smaller size */
        snprintf(out, (size_t)n, "Vulkan: %ux%u, %s from %ux%u%s, %s", pw, ph, aa_own ? "TAA" : aa_dlss ? "DLSS" : "FSR",
                 (unsigned)lroundf((float)pw / ratio), (unsigned)lroundf((float)ph / ratio), rs_auto ? " (auto)" : "", gpu);
    else if (aa_temporal && aa_dlss && taa_can())
        snprintf(out, (size_t)n, "Vulkan: %ux%u, DLAA, %s", pw, ph, gpu);
    else
        snprintf(out, (size_t)n, "Vulkan: %ux%u, %s", pw, ph, gpu);
}

/* the picture area in pixels (window or off-screen image), for the menu's NATIVE text */
int vk_resolution_native(void) { return res_native; }

void vk_native_size(int *w, int *h)
{
    *w = *h = 0;
    if (offscreen && out_img) { *w = (int)out_img->w; *h = (int)out_img->h; }
    else if (window) SDL_GetWindowSizeInPixels(window, w, h);
}
double vk_tnl_ms;                 /* d3d.c: time building the vertices for the GPU */
static double wall_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static double st_t0;

/* pipelines and samplers */
typedef struct { uint32_t key; VkFormat depth, color; VkPipeline p; } Pipe;
static Pipe pipes[512];
static int npipes;
typedef struct { uint32_t key; float bias; VkSampler s; } Smp;
static Smp smps[MAX_SMP];
static int nsmps;
static int smp_linear, smp_nearest;

/* ------------------------------------------------------------------ helpers */
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice)
{
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags;
            if ((bits & (1u << i)) && (f & want) == want && (pass || (f & nice) == nice))
                return i;
        }
    fatal("Vulkan: no memory type %x for %x", want, bits);
}

static int slot_alloc(void)
{
    if (!nfree_slots)
        fatal("Vulkan: out of texture slots (%u)", ntex_slots);
    return free_slots[--nfree_slots];
}

static void slot_write(int slot, VkImageView view)
{
    VkDescriptorImageInfo ii = {.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
                              .dstArrayElement = (uint32_t)slot, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii};
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
}

static void chunk_new(Chunk *c, VkDeviceSize size)
{
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    if (rt_ok)                    /* the shadows build acceleration structures from the draws' vertices */
        bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    VKC(vkCreateBuffer(dev, &bi, NULL, &c->buf));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, c->buf, &r);
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = rt_ok ? &fi : NULL,
                               .allocationSize = r.size,
                               .memoryTypeIndex = memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
    VKC(vkAllocateMemory(dev, &ai, NULL, &c->mem));
    VKC(vkBindBufferMemory(dev, c->buf, c->mem, 0));
    VKC(vkMapMemory(dev, c->mem, 0, size, 0, (void **)&c->map));
    c->size = size;
    c->used = 0;
    c->addr = 0;
    if (rt_ok) {
        VkBufferDeviceAddressInfo da = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = c->buf};
        c->addr = vkGetBufferDeviceAddress(dev, &da);
    }
}

/* A buffer with a device address (with ray queries): host-visible and mapped, or device-local */
static DBuf dbuf_new(VkDeviceSize size, VkBufferUsageFlags usage, int host)
{
    DBuf d;
    memset(&d, 0, sizeof d);
    if (rt_ok)
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VKC(vkCreateBuffer(dev, &bi, NULL, &d.buf));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, d.buf, &r);
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = rt_ok ? &fi : NULL,
                               .allocationSize = r.size,
                               .memoryTypeIndex = host ? memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)
                                                       : memtype(r.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VKC(vkAllocateMemory(dev, &ai, NULL, &d.mem));
    VKC(vkBindBufferMemory(dev, d.buf, d.mem, 0));
    if (host)
        VKC(vkMapMemory(dev, d.mem, 0, size, 0, (void **)&d.map));
    d.size = size;
    if (rt_ok) {
        VkBufferDeviceAddressInfo da = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = d.buf};
        d.addr = vkGetBufferDeviceAddress(dev, &da);
    }
    return d;
}

static void dbuf_free(DBuf *d)
{
    if (!d->buf)
        return;
    if (d->map)
        vkUnmapMemory(dev, d->mem);
    vkDestroyBuffer(dev, d->buf, NULL);
    vkFreeMemory(dev, d->mem, NULL);
    memset(d, 0, sizeof *d);
}

/* Buffers and acceleration structures that recorded commands may still use: freed once this
   frame's fence passed (by then the earlier frames are done too) */
#define TRASH 32
static struct { DBuf b[TRASH]; VkAccelerationStructureKHR as[TRASH]; int nb, nas; } trash[FRAMES];
static void dbuf_retire(DBuf *d)
{
    if (!d->buf)
        return;
    if (trash[fi].nb == TRASH) {
        vkDeviceWaitIdle(dev);        /* (never in practice) */
        dbuf_free(d);
        return;
    }
    trash[fi].b[trash[fi].nb++] = *d;
    memset(d, 0, sizeof *d);
}

static void accel_retire(Accel *a)
{
    if (a->as) {
        if (trash[fi].nas == TRASH) {
            vkDeviceWaitIdle(dev);
            p_destroy_as(dev, a->as, NULL);
        } else
            trash[fi].as[trash[fi].nas++] = a->as;
    }
    dbuf_retire(&a->b);
    memset(a, 0, sizeof *a);
}

static void trash_empty(int f)
{
    for (int i = 0; i < trash[f].nas; i++)
        p_destroy_as(dev, trash[f].as[i], NULL);
    for (int i = 0; i < trash[f].nb; i++)
        dbuf_free(&trash[f].b[i]);
    trash[f].nas = trash[f].nb = 0;
}

/* An acceleration structure of at least `size` bytes (a bigger one replaces it) */
static void accel_reserve(Accel *a, VkAccelerationStructureTypeKHR type, VkDeviceSize size)
{
    if (a->as && a->b.size >= size)
        return;
    accel_retire(a);
    size = (size + 65535) & ~(VkDeviceSize)65535;     /* room to grow */
    a->b = dbuf_new(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0);
    VkAccelerationStructureCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                               .buffer = a->b.buf, .size = size, .type = type};
    VKC(p_create_as(dev, &ci, NULL, &a->as));
    VkAccelerationStructureDeviceAddressInfoKHR ai = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = a->as};
    a->addr = p_as_addr(dev, &ai);
}

static void scratch_reserve(DBuf *s, VkDeviceSize size)
{
    if (s->buf && s->size >= size)
        return;
    dbuf_retire(s);
    *s = dbuf_new((size + 65535) & ~(VkDeviceSize)65535, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0);
}

/* The two frames' top levels (a fixed size, so their descriptors are written once), instance and
   matrix buffers, the gathering arrays */
static void rt_init(void)
{
    for (int i = 0; i < SH_SLOTS; i++) {
        struct ShFrame *s = &shf[i];
        s->inst = dbuf_new(SH_INST * sizeof(VkAccelerationStructureInstanceKHR),
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1);
        s->xform = dbuf_new(SH_GEOMS * 48, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1);
        s->geo = calloc(SH_GEOMS, sizeof *s->geo);
        s->range = calloc(SH_GEOMS, sizeof *s->range);
        s->maxprim = calloc(SH_GEOMS, sizeof *s->maxprim);
        if (!s->geo || !s->range || !s->maxprim)
            fatal("out of memory");
        VkAccelerationStructureGeometryKHR g = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                                   .data.deviceAddress = s->inst.addr}};
        VkAccelerationStructureBuildGeometryInfoKHR bi = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = &g};
        uint32_t n = SH_INST;
        VkAccelerationStructureBuildSizesInfoKHR sz = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        p_build_sizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &n, &sz);
        accel_reserve(&s->tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sz.accelerationStructureSize);
        tlas_scratch = sz.buildScratchSize;
        /* the sky's top level: one instance */
        s->sky_inst = dbuf_new(sizeof(VkAccelerationStructureInstanceKHR),
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1);
        g.geometry.instances.data.deviceAddress = s->sky_inst.addr;
        n = 1;
        p_build_sizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &n, &sz);
        accel_reserve(&s->sky_tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sz.accelerationStructureSize);
        if (sz.buildScratchSize > tlas_scratch)
            tlas_scratch = sz.buildScratchSize;
    }
    for (int i = 0; i < FRAMES; i++)
        scratch_reserve(&sh_scratch[i], tlas_scratch);
    VkAccelerationStructureKHR as[2 * SH_SLOTS];      /* the frames' views', then their skies' */
    for (int i = 0; i < SH_SLOTS; i++) {
        as[i] = shf[i].tlas.as;
        as[SH_SLOTS + i] = shf[i].sky_tlas.as;
    }
    VkWriteDescriptorSetAccelerationStructureKHR wa = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 2 * SH_SLOTS, .pAccelerationStructures = as};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &wa, .dstSet = dset,
                              .dstBinding = 4, .descriptorCount = 2 * SH_SLOTS,
                              .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    const char *t = getenv("TD6_RT_TRACE"), *o = getenv("TD6_RT_OPAQUE");
    rt_trace = !(t && !strcmp(t, "0"));
    rt_opaque = o && *o && strcmp(o, "0");
    const char *full = getenv("TD6_RT_FULL"), *dbg = getenv("TD6_RT_DEBUG");
    rt_full = full && *full && strcmp(full, "0");
    rt_debug = dbg ? atoi(dbg) : 0;
    const char *shl = getenv("TD6_SH_LOG");
    rt_log = shl && atoi(shl) >= 2;
    const char *ar = getenv("TD6_AO_RAYS"), *asc = getenv("TD6_AO_SCALE"), *ab = getenv("TD6_AO_BLUR"),
               *ad = getenv("TD6_AO_DEBUG");
    if (ar && atoi(ar) >= 0 && atoi(ar) <= 64)       /* (0: no rays, for timing) */
        ao_rays = atoi(ar);
    if (asc && atoi(asc) >= 1 && atoi(asc) <= 8)
        ao_scale = atoi(asc);
    ao_blur = !(ab && !strcmp(ab, "0"));
    ao_debug = ad ? atoi(ad) : 0;
    const char *ss = getenv("TD6_SH_SOFT"), *su = getenv("TD6_SH_SUN"), *sk = getenv("TD6_SH_KEEP"),
               *sr = getenv("TD6_SH_RMAX"), *sn = getenv("TD6_SH_NEAREST");
    sh_soft = !(ss && !strcmp(ss, "0"));
    const char *hd = getenv("TD6_HL_DEBUG"), *hm = getenv("TD6_HL_MAX");
    hl_debug = hd ? atoi(hd) : 0;
    hl_log = getenv("TD6_HL_LOG") != NULL;
    hl_terrain = getenv("TD6_HL_TERRAIN") && atoi(getenv("TD6_HL_TERRAIN"));
    if (hm && atoi(hm) >= 0)
        hl_max = atoi(hm) > VK_BEAMS ? VK_BEAMS : atoi(hm);
    if (su && atof(su) > 0 && atof(su) < 10)
        sh_sun = (float)atof(su);
    if (sk)
        sh_keep = (float)atof(sk);
    if (sr && atoi(sr) >= 1 && atoi(sr) <= 8)
        sh_rmax = atoi(sr);
    sh_nearest = sn && atoi(sn) != 0;
    const char *nf = getenv("TD6_RT_NOFADE");
    rt_nofade = nf && atoi(nf) != 0;
    sh_time = getenv("TD6_SH_TIME") != NULL;
    const char *af = getenv("TD6_AO_FLAGS");
    ao_flags = af ? atoi(af) & 7 : 0;
    const char *rt = getenv("TD6_REFL_TRACE"), *rf = getenv("TD6_REFL_FRESNEL"), *rd = getenv("TD6_REFL_DARKEN");
    refl_trace = !(rt && !strcmp(rt, "0"));
    if (rf)
        refl_fresnel = fminf(fmaxf((float)atof(rf), 0.0f), 1.0f);
    if (rd)
        refl_darken = fminf(fmaxf((float)atof(rd), 0.0f), 1.0f);
    const char *rs = getenv("TD6_REFL_SHARE"), *rdb = getenv("TD6_REFL_DEBUG");
    refl_share = !(rs && !strcmp(rs, "0"));
    refl_debug = rdb ? atoi(rdb) : 0;
    const char *rfu = getenv("TD6_REFL_HALF");
    refl_half = rfu && atoi(rfu) != 0;
    const char *rsd = getenv("TD6_REFL_SPREAD");
    if (rsd)
        sscanf(rsd, "%f,%f", &refl_sd[0], &refl_sd[1]);
    const char *rlr = getenv("TD6_REFL_LRATE");
    if (rlr && sscanf(rlr, "%u,%u,%u", &refl_lrate[0], &refl_lrate[1], &refl_lrate[2]) == 3)
        for (int i = 0; i < 3; i++)
            refl_lrate[i] = refl_lrate[i] < 1 ? 1 : refl_lrate[i] > 3 ? 3 : refl_lrate[i];
    const char *rg = getenv("TD6_REFL_GAIN");
    if (rg)
        refl_gain = fminf(fmaxf((float)atof(rg), 0.0f), 4.0f);
}

/* Space for this frame's vertices, indices and uploads. */
static VkDeviceAddress ring_addr;   /* the device address of the last space's buffer (ray tracing) */
static VkDeviceSize ring(VkDeviceSize size, VkDeviceSize align, VkBuffer *buf, uint8_t **ptr)
{
    Frame *f = &fr[fi];
    for (;;) {
        if (f->cur < f->nchunks) {
            Chunk *ch = &f->chunks[f->cur];
            VkDeviceSize off = (ch->used + align - 1) & ~(align - 1);
            if (off + size <= ch->size) {
                ch->used = off + size;
                *buf = ch->buf;
                *ptr = ch->map + off;
                ring_addr = ch->addr;
                return off;
            }
            f->cur++;
            continue;
        }
        if (f->nchunks == MAX_CHUNKS)
            fatal("Vulkan: more than %d staging chunks in one frame", MAX_CHUNKS);
        chunk_new(&f->chunks[f->nchunks++], size > CHUNK_SIZE ? size : CHUNK_SIZE);
    }
}

static void readback_reserve(VkDeviceSize size)
{
    if (size <= rb_size)
        return;
    if (rb_buf) {
        vkDeviceWaitIdle(dev);
        vkDestroyBuffer(dev, rb_buf, NULL);
        vkFreeMemory(dev, rb_mem, NULL);
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKC(vkCreateBuffer(dev, &bi, NULL, &rb_buf));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, rb_buf, &r);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
                               .memoryTypeIndex = memtype(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT)};
    VKC(vkAllocateMemory(dev, &ai, NULL, &rb_mem));
    VKC(vkBindBufferMemory(dev, rb_buf, rb_mem, 0));
    VKC(vkMapMemory(dev, rb_mem, 0, size, 0, (void **)&rb_map));
    rb_size = size;
}

/* ------------------------------------------------------------------ images */
static GpuImg *img_new(int kind, uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, VkImageUsageFlags usage)
{
    GpuImg *g = calloc(1, sizeof *g);
    if (!g)
        fatal("out of memory");
    g->kind = kind;
    g->w = w;
    g->h = h;
    g->lw = w;
    g->lh = h;
    g->sx = g->sy = 1.0f;
    g->levels = levels;
    g->format = fmt;
    g->slot = -1;
    g->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    g->aspect = kind == K_DEPTH ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (kind == K_DEPTH && (fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT))
        g->aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt, .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VKC(vkCreateImage(dev, &ci, NULL, &g->image));
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, g->image, &r);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size,
                               .memoryTypeIndex = memtype(r.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VKC(vkAllocateMemory(dev, &ai, NULL, &g->mem));
    VKC(vkBindImageMemory(dev, g->image, g->mem, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {g->aspect, 0, levels, 0, 1}};
    if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))      /* not for copy-only staging images */
        VKC(vkCreateImageView(dev, &vi, NULL, &g->view));
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        g->slot = slot_alloc();
        slot_write(g->slot, g->view);
    }
    return g;
}

static void img_free(GpuImg *g)
{
    vkDestroyImageView(dev, g->view, NULL);
    if (g->view2)
        vkDestroyImageView(dev, g->view2, NULL);
    vkDestroyImage(dev, g->image, NULL);
    vkFreeMemory(dev, g->mem, NULL);
    if (g->slot >= 0)
        free_slots[nfree_slots++] = g->slot;
    free(g);
}

/* freed once the frames that may use it are done */
static void frame_begin(void);
static void img_discard(GpuImg *g)
{
    frame_begin();                /* so it belongs to a frame that is not submitted yet */
    Frame *f = &fr[fi];
    for (int i = 0; i < 16; i++)
        if (tile_img[i] == g)
            tile_img[i] = NULL;
    if (g == r_color || g == r_depth) {
        if (rendering)
            vkCmdEndRendering(cb);
        rendering = 0;
        r_color = r_depth = NULL;
    }
    if (f->ngarbage == f->capgarbage) {
        f->capgarbage = f->capgarbage ? 2 * f->capgarbage : 64;
        f->garbage = realloc(f->garbage, (size_t)f->capgarbage * sizeof *f->garbage);
        if (!f->garbage)
            fatal("out of memory");
    }
    f->garbage[f->ngarbage++] = g;
}

static void end_rendering(void)
{
    if (rendering) {
        vkCmdEndRendering(cb);
        rendering = 0;
    }
}

/* What an image in a layout is used by: the stages and accesses a barrier out of it waits for (src) and
   into it holds back (dst). A layout that tells nothing (GENERAL: storage images; UNDEFINED as the old
   one, whose last readers are not known) waits for and holds back everything. TD6_VK_FULL_BARRIERS=1:
   every barrier the whole pipeline (as before). */
static int full_barriers = -1;
static void layout_use(VkImageLayout l, int src, VkPipelineStageFlags2 *stage, VkAccessFlags2 *access)
{
    const VkPipelineStageFlags2 shaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    switch (full_barriers ? VK_IMAGE_LAYOUT_GENERAL : l) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | (src ? 0 : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
        return;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        *stage = tests;
        *access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | (src ? 0 : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        return;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        *stage = tests | shaders;
        *access = src ? 0 : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = shaders;
        *access = src ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        *access = src ? 0 : VK_ACCESS_2_TRANSFER_READ_BIT;
        return;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        return;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:   /* (out of it: after the acquire's wait, at colour output) */
        *stage = src ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_NONE;
        *access = 0;
        return;
    default:
        *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        *access = src ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    }
}

static void raw_barrier(VkImage image, VkImageAspectFlags aspect, uint32_t levels, VkImageLayout from, VkImageLayout to)
{
    end_rendering();
    st_barriers++;
    if (full_barriers < 0) {
        const char *e = getenv("TD6_VK_FULL_BARRIERS");
        full_barriers = e && *e == '1';
    }
    VkPipelineStageFlags2 ss, ds;
    VkAccessFlags2 sa, da;
    layout_use(from, 1, &ss, &sa);
    layout_use(to, 0, &ds, &da);
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = ss, .srcAccessMask = sa, .dstStageMask = ds, .dstAccessMask = da,
                               .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
                               .subresourceRange = {aspect, 0, levels, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static void barrier(GpuImg *g, VkImageLayout to)
{
    raw_barrier(g->image, g->aspect, g->levels, g->layout, to);
    g->layout = to;
}

/* ------------------------------------------------------------------ frames */
static void free_garbage(Frame *f)
{
    for (int i = 0; i < f->ngarbage; i++)
        img_free(f->garbage[i]);
    f->ngarbage = 0;
}

static void cb_begin(Frame *f)
{
    VKC(vkResetCommandPool(dev, f->pool, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKC(vkBeginCommandBuffer(f->cb, &bi));
    cb = f->cb;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &dset, 0, NULL);
    for (int i = 0; i < f->nchunks; i++)
        f->chunks[i].used = 0;
    f->cur = 0;
    f->ntnl = 0;
    f->ninst = 0;
    rendering = 0;
    r_color = r_depth = NULL;
    bound_pipe = VK_NULL_HANDLE;
    f->recording = 1;
    f->sh_timed = 0;
    f->taa_timed = 0;
    f->sm_timed = 0;
    if (st_queries) {
        vkCmdResetQueryPool(cb, st_queries, TS * (uint32_t)fi, TS);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, st_queries, TS * (uint32_t)fi);
    }
}

static void frame_begin(void)
{
    Frame *f = &fr[fi];
    if (f->recording)
        return;
    VKC(vkWaitForFences(dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
    VKC(vkResetFences(dev, 1, &f->fence));
    free_garbage(f);
    trash_empty(fi);
    uint64_t ts[TS];
    if (st_queries && f->timed &&
        vkGetQueryPoolResults(dev, st_queries, TS * (uint32_t)fi, TS, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS && ts[1] > ts[0]) {
        st_gpu_ms += (double)(ts[1] - ts[0]) * st_tick_ns / 1e6;
        st_gpu_n++;
        /* the GPU idle between the frame before's end and this one's start (the slots complete in turn) */
        if (st_gpu_end && ts[0] > st_gpu_end && st_gpu_end_frame + 1 == nframes) {
            st_gpu_gap_ms += (double)(ts[0] - st_gpu_end) * st_tick_ns / 1e6;
            st_gpu_gap_n++;
        }
        st_gpu_end = ts[1];
        st_gpu_end_frame = nframes;
        rs_sample((double)(ts[1] - ts[0]) * st_tick_ns / 1e6, f->taa_timed);
        if (st_print > 1)         /* TD6_VK_STATS=2: every frame's (the frame this slot drew last) */
            logmsg(LOG_D3D, "vk: gpu frame %lu %.3f ms%s", nframes, (double)(ts[1] - ts[0]) * st_tick_ns / 1e6,
                   f->hp ? " high" : "");
        int ok = 1;
        for (int q = 3; q < 9; q++)
            ok &= ts[q] >= ts[q - 1];
        if (ok && ts[2] >= ts[0]) {
            st_gpu_sh_ms += (double)(ts[8] - ts[2]) * st_tick_ns / 1e6;
            st_gpu_build_ms += (double)(ts[3] - ts[2]) * st_tick_ns / 1e6;
            st_gpu_refl_ms += (double)(ts[8] - ts[7]) * st_tick_ns / 1e6;
            for (int q = 0; q < 3; q++)
                st_gpu_ao_ms[q] += (double)(ts[4 + q] - ts[3 + q]) * st_tick_ns / 1e6;
        }
        if (ts[10] >= ts[9] && ts[9] >= ts[0])
            st_gpu_taa_ms += (double)(ts[10] - ts[9]) * st_tick_ns / 1e6;
        if (ts[12] >= ts[11] && ts[11] >= ts[0] && ts[14] >= ts[13] && ts[13] >= ts[0]) {
            double ms = (double)(ts[12] - ts[11] + ts[14] - ts[13]) * st_tick_ns / 1e6;
            st_gpu_sm_ms += ms;
            st_gpu_sm_max = ms > st_gpu_sm_max ? ms : st_gpu_sm_max;
            st_gpu_sm1_ms += f->sm_timed & 4 ? 0.0 : (double)(ts[12] - ts[11]) * st_tick_ns / 1e6;
        }
    }
    cb_begin(f);
}

static void submit(VkSemaphore wait, VkSemaphore signal)
{
    Frame *f = &fr[fi];
    end_rendering();
    if (st_queries) {
        if (!f->sh_timed)         /* no ray tracing this time: its timestamps are empty */
            for (uint32_t q = 2; q < 9; q++)
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, TS * (uint32_t)fi + q);
        if (!f->taa_timed)        /* ... no temporal anti-aliasing */
            for (uint32_t q = 9; q < 11; q++)
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, TS * (uint32_t)fi + q);
        for (uint32_t q = 11; q < 15; q++)  /* ... no volumetric smoke (or a part of it) */
            if (!(f->sm_timed & (q < 13 ? 1 : 2)))
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, TS * (uint32_t)fi + q);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, TS * (uint32_t)fi + 1);
    }
    f->timed = st_queries != VK_NULL_HANDLE;
    f->hp = hp_want && hp_format;
    VKC(vkEndCommandBuffer(cb));
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = wait ? 1 : 0,
                       .pWaitSemaphores = &wait, .pWaitDstStageMask = &ws, .commandBufferCount = 1,
                       .pCommandBuffers = &cb, .signalSemaphoreCount = signal ? 1 : 0, .pSignalSemaphores = &signal};
    VKC(vkQueueSubmit(queue, 1, &si, f->fence));
    f->recording = 0;
}

/* Run everything recorded so far and wait (readbacks); recording goes on afterwards. */
static void flush_wait(void)
{
    Frame *f = &fr[fi];
    if (sv.active)                /* the gathered vertices' space is about to be reused: no shadows this view */
        sv.broken = 1;
    submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    VKC(vkWaitForFences(dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
    VKC(vkResetFences(dev, 1, &f->fence));
    cb_begin(f);
}

/* ------------------------------------------------------------------ samplers */
static int sampler_get(uint32_t mag, uint32_t min, uint32_t mip, uint32_t au, uint32_t av, uint32_t border,
                       float bias, uint32_t maxmip, uint32_t levels, int aniso)
{
    float maxlod = mip <= 1 ? (float)maxmip : (float)(levels ? levels - 1 : 0);
    uint32_t key = (mag & 7) | (min & 7) << 3 | (mip & 3) << 6 | (au & 7) << 8 | (av & 7) << 11 |
                   (border & 3) << 14 | (maxmip & 15) << 16 | ((uint32_t)maxlod & 15) << 20 | (uint32_t)!!aniso << 24;
    for (int i = 0; i < nsmps; i++)
        if (smps[i].key == key && smps[i].bias == bias)
            return i;
    if (nsmps == MAX_SMP) {
        LOG(LOG_ALWAYS, "vk: out of samplers");
        return 0;
    }
    static const VkSamplerAddressMode am[] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                              VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                              VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    static const VkBorderColor bc[] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
                                       VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                              .minFilter = min == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
                              .mipmapMode = mip == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = am[au <= 4 ? au : 1], .addressModeV = am[av <= 4 ? av : 1],
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .mipLodBias = bias,
                              .minLod = (float)maxmip, .maxLod = maxlod > (float)maxmip ? maxlod : (float)maxmip,
                              .borderColor = bc[border < 3 ? border : 0],
                              .anisotropyEnable = aniso && aniso_ok, .maxAnisotropy = aniso_max};
    VKC(vkCreateSampler(dev, &si, NULL, &smps[nsmps].s));
    smps[nsmps].key = key;
    smps[nsmps].bias = bias;
    VkDescriptorImageInfo ii = {.sampler = smps[nsmps].s};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1,
                              .dstArrayElement = (uint32_t)nsmps, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    return nsmps++;
}

/* D3D7 border colour -> the closest of Vulkan's fixed ones */
static uint32_t border_class(uint32_t c)
{
    uint32_t a = c >> 24, l = ((c >> 16 & 0xff) + (c >> 8 & 0xff) + (c & 0xff)) / 3;
    return a < 128 ? 0 : l < 128 ? 1 : 2;
}

static float samp_bias;           /* temporal anti-aliasing's scene: its textures' level of detail moved by this */
static int samp_scene;            /* ... a draw of that scene */
/* A texture stage's sampler; levels: the texture's mip levels as this draw may use them. In temporal
   anti-aliasing's scene (samp_scene) a texture with levels is filtered trilinear and anisotropic,
   as SMOOTH does everywhere: the game's nearest-level mipmaps would step, and its jittered samples flicker */
static int stage_sampler(const uint32_t *t, uint32_t levels)
{
    uint32_t maxmip = t[TSS_MAXMIPLEVEL];
    if (levels && maxmip > levels - 1)
        maxmip = levels - 1;
    uint32_t bias;
    memcpy(&bias, &t[TSS_MIPMAPLODBIAS], 4);
    float fb;
    memcpy(&fb, &bias, 4);
    if (samp_scene && levels > 1)
        fb = (isfinite(fb) ? fb : 0) + samp_bias;
    if ((tex_smooth || samp_scene) && levels > 1)  /* the 3D world's textures: trilinear, anisotropic */
        return sampler_get(2, 2, 3, t[TSS_ADDRESSU], t[TSS_ADDRESSV], border_class(t[TSS_BORDERCOLOR]),
                           isfinite(fb) ? fb : 0, maxmip, levels, 1);
    return sampler_get(t[TSS_MAGFILTER], t[TSS_MINFILTER], t[TSS_MIPFILTER], t[TSS_ADDRESSU], t[TSS_ADDRESSV],
                       border_class(t[TSS_BORDERCOLOR]), isfinite(fb) ? fb : 0, maxmip, levels, 0);
}

/* ------------------------------------------------------------------ pipelines */
/* mrt: the shadows' two extra attachments too (attachment 1 blended like the colour, attachment 2
   written only when geo_write; geo_write 2: only attachment 2's alpha, times 1 - the output's
   alpha: see-through parts mark where they cover the surface, which ambient occlusion then leaves
   alone; 3: blended by the output's alpha, 0 or 1: solid see-through parts replace it where
   solid); resolve: 1
   shaders/resolve.frag's colour - output, 2 ao.frag's colour * output alpha + output */
static int spec_pass = -1;        /* >= 0: the fragment shader's constant 0 (resolve.frag, ao.frag: the pass; draw.frag RT_REFLECT: no rays) */
static uint32_t pipe_mot;         /* temporal anti-aliasing's scene: draw_mot's bits (attachments 3, 4) */

/* the sun's part of the colour (the shadows' attachment 1, blended like the colour; its rgb is what the shadows take
   away and the base of the ambient occlusion's darkening): 8 bits beside the 8-bit colour; beside the 16-bit scene
   ("colour_precision = high") 10 bits (hp_sun): its rounding then a quarter of a level, far under what shows,
   where 8 bits put their own steps into the shadows and the darkening, at half the 16 bits' cost (its alpha, 2
   bits there, is never read: the back buffer has none, so no blend reads it) */
static VkFormat aux_format(VkFormat color)
{
    return hp_format && color == hp_format ? hp_sun : AUX_FORMAT;
}
/* The pipelines' cache, kept in <user>/cache/pipelines.bin from run to run: a pipeline is made the first time
   its draw state is needed, and from the cache the driver need not compile its shaders again (the cache's
   header names the device and driver: Vulkan ignores one from another). At exit the log says how many were
   made and how long that took. */
static VkPipelineCache pcache;
static double pipe_ms, pipe_max_ms;
static unsigned pipe_n;
static size_t pcache_loaded;

double vkr_pipe_ms(void) { return pipe_ms; }      /* (vkthread.c's frame trace) */

static void pcache_path(char *p, size_t n)
{
    snprintf(p, n, "%s/cache/pipelines.bin", user_dir);
}

static void pcache_save(void)
{
    if (!pcache)
        return;
    LOG(LOG_ALWAYS, "vk: %u pipelines made, %.1f ms in all (slowest %.1f ms); the cache had %lu bytes at start",
        pipe_n, pipe_ms, pipe_max_ms, (unsigned long)pcache_loaded);
    size_t n = 0;
    if (vkGetPipelineCacheData(dev, pcache, &n, NULL) != VK_SUCCESS || !n)
        return;
    void *d = malloc(n);
    char p[1200], tmp[1210];
    pcache_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = d && vkGetPipelineCacheData(dev, pcache, &n, d) == VK_SUCCESS ? fopen(tmp, "wb") : NULL;
    if (f) {
        int ok = fwrite(d, 1, n, f) == n;
        if (fclose(f) == 0 && ok)
            rename(tmp, p);
    }
    free(d);
}

static void pcache_open(void)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/cache", user_dir);
    mkdir(p, 0755);
    pcache_path(p, sizeof p);
    void *d = NULL;
    FILE *f = fopen(p, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && (d = malloc((size_t)n)) && fread(d, 1, (size_t)n, f) == (size_t)n)
            pcache_loaded = (size_t)n;
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = pcache_loaded, .pInitialData = pcache_loaded ? d : NULL};
    if (vkCreatePipelineCache(dev, &ci, NULL, &pcache) != VK_SUCCESS) {
        pcache_loaded = 0;
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        if (vkCreatePipelineCache(dev, &ci, NULL, &pcache) != VK_SUCCESS)
            pcache = VK_NULL_HANDLE;
    }
    free(d);
    if (pcache)
        atexit(pcache_save);
}

static VkPipeline pipeline_create(VkShaderModule vs, VkShaderModule fs, VkPrimitiveTopology topo, int draw,
                                  int blend, VkBlendFactor sb, VkBlendFactor db, VkFormat color, VkFormat depth,
                                  int tnl, int mrt, int geo_write, int resolve)
{
    int32_t spec_value = spec_pass;
    VkSpecializationMapEntry spec_entry = {0, 0, 4};
    VkSpecializationInfo spec = {1, &spec_entry, 4, &spec_value};
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs, .pName = "main", .pSpecializationInfo = spec_pass >= 0 ? &spec : NULL}};
    VkVertexInputBindingDescription vb = {0, sizeof(GV), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[5] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GV, pos)},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GV, diffuse)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(GV, specular)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GV, tc0)},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GV, tc1)}};
    if (tnl) {                    /* the game's raw 40-byte vertices (shaders/tnl.vert) */
        vb.stride = 40;
        VkVertexInputAttributeDescription ta[5] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
            {2, 0, VK_FORMAT_B8G8R8A8_UNORM, 24}, {3, 0, VK_FORMAT_B8G8R8A8_UNORM, 28},
            {4, 0, VK_FORMAT_R32G32_SFLOAT, 32}};
        memcpy(va, ta, sizeof va);
    }
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = draw ? 1 : 0,
                                               .pVertexBindingDescriptions = &vb,
                                               .vertexAttributeDescriptionCount = draw ? 5 : 0,
                                               .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthCompareOp = VK_COMPARE_OP_ALWAYS};
    if (!draw && depth != VK_FORMAT_UNDEFINED) {    /* (temporal anti-aliasing's copy: only where the depth is
                                                       as cleared, taa_copy) */
        ds.depthTestEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    }
    VkPipelineColorBlendAttachmentState ba[5] = {
        {.blendEnable = blend ? VK_TRUE : VK_FALSE, .srcColorBlendFactor = sb, .dstColorBlendFactor = db,
         .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = sb, .dstAlphaBlendFactor = db,
         .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf}};
    ba[1] = ba[0];
    ba[2] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = geo_write ? 0xf : 0};
    if (geo_write == 3)                 /* solid see-through parts: their pixels (output alpha 1) replace it */
        ba[2] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    if (geo_write == 2)
        ba[2] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ZERO, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_A_BIT};
    if (mrt == 3 || mrt == 4)           /* reflective draws again (draw.frag RT_REFLECT): added, and the
                                           colour below kept by 1 - alpha */
        ba[0] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    if (resolve)
        ba[0] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = resolve == 2 ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE,
            .colorBlendOp = resolve == 2 ? VK_BLEND_OP_ADD : VK_BLEND_OP_REVERSE_SUBTRACT,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    if (resolve == 4)                   /* (headlights, resolve.frag pass 7: added) */
        ba[0] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE, .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    if (resolve == 3)                   /* (the volumetric smoke's march: its light added over what is behind
                                           by T, and T kept by T) */
        ba[0] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    uint32_t natt = mrt == 1 ? 3 : 1;
    VkFormat formats[5] = {color, aux_format(color), GEO_FORMAT, VEL_FORMAT, MASK_FORMAT};
    if (pipe_mot & 1) {
        /* the scene's motion vectors (replaced, or by alpha: solid see-through parts) and masks (the
           largest); without the shadows' attachments 1 and 2 none there */
        natt = 5;
        if (mrt != 1) {
            formats[1] = formats[2] = VK_FORMAT_UNDEFINED;
            ba[1] = ba[2] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = 0};
        }
        uint32_t velw = pipe_mot >> 1 & 3;
        ba[3] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = velw ? 3 : 0};
        if (velw == 2)
            ba[3] = (VkPipelineColorBlendAttachmentState){
                .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 3};
        ba[4] = (VkPipelineColorBlendAttachmentState){.colorWriteMask = 0};
        if (pipe_mot & 8)
            ba[4] = (VkPipelineColorBlendAttachmentState){
                .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
                .colorBlendOp = VK_BLEND_OP_MAX, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .alphaBlendOp = VK_BLEND_OP_MAX, .colorWriteMask = 3};
    }
    VkPipelineColorBlendStateCreateInfo cbs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = natt, .pAttachments = ba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                            VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = draw ? COUNT(dyn) : 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = natt, .pColorAttachmentFormats = formats,
                                        .depthAttachmentFormat = depth};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cbs,
                                       .pDynamicState = &dy, .layout = playout};
    VkPipeline p;
    double t0 = wall_ms();
    VKC(vkCreateGraphicsPipelines(dev, pcache, 1, &pi, NULL, &p));
    double t = wall_ms() - t0;
    pipe_ms += t;
    pipe_max_ms = t > pipe_max_ms ? t : pipe_max_ms;
    pipe_n++;
    return p;
}

static VkBlendFactor blend_factor(uint32_t f, int dst_alpha)
{
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return dst_alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_ONE;
    case 8: return dst_alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ZERO;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static unsigned long st_binds, st_scans;   /* TD6_VK_STATS: pipeline binds, textures' cut-outs worked out */
static void bind_pipeline(VkPipeline p)
{
    if (p != bound_pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        bound_pipe = p;
        st_binds++;
    }
}

/* topo: 0 triangles, 1 lines, 2 points; mrt: 1 with the shadows' extra attachments (geo_write:
   attachment 2 written, 2 marked: pipeline_create), 3 a reflective draw again after its view
   (draw.frag RT_REFLECT, its own blending), 4 the same tracing nothing at full size (its ray code
   left out: REFL_NO_RAYS), 2 the same into a smaller rays' image (no blending), 5 a see-through
   surface drawn again for the volumetric smoke (draw.frag as it is: its alpha and its depth) */
static VkPipeline draw_pipeline(int topo, int blend, VkBlendFactor sb, VkBlendFactor db, VkFormat depth, int tnl,
                                int mrt, int geo_write)
{
    if (mrt == 2)
        blend = 0;
    if (!blend || mrt >= 3)
        sb = VK_BLEND_FACTOR_ONE, db = VK_BLEND_FACTOR_ZERO;
    /* (the rendering's colour: the back buffer, the scene's 8 or 16 bits, the smoke's glass image) */
    VkFormat cf = mrt == 2 ? AO_FORMAT : rendering && r_color ? r_color->format : RT_FORMAT;
    geo_write = mrt != 1 ? 0 : geo_write >= 2 ? geo_write & 3 : !!geo_write;
    uint32_t mot = mrt != 2 ? draw_mot & 15 : 0;     /* (temporal anti-aliasing's scene: its draws' motion vectors;
                                                        the reflections drawn again: masks) */
    uint32_t key = (uint32_t)topo | (uint32_t)blend << 2 | (uint32_t)sb << 3 | (uint32_t)db << 8 | (uint32_t)!!tnl << 13 |
                   (uint32_t)(mrt & 7) << 14 | (uint32_t)geo_write << 17 | mot << 19;   /* (bits 0-22) */
    for (int i = 0; i < npipes; i++)
        if (pipes[i].key == key && pipes[i].depth == depth && pipes[i].color == cf)
            return pipes[i].p;
    if (npipes == COUNT(pipes))
        fatal("Vulkan: too many pipelines");
    static const VkPrimitiveTopology tp[] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                             VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    Pipe *p = &pipes[npipes++];
    p->key = key;
    p->depth = depth;
    p->color = cf;
    pipe_mot = mot;
    spec_pass = mrt == 4 ? 1 : -1;
    if (mot)
        p->p = pipeline_create(tnl ? sm_tvm : sm_dvm, mrt == 1 ? sm_dfsm : mrt == 3 || mrt == 4 ? sm_dfrm : sm_dfm, tp[topo], 1, blend,
                               sb, db, cf, depth, tnl, mrt, geo_write, 0);
    else
        p->p = pipeline_create(tnl ? sm_tv : sm_dv, mrt == 1 ? sm_dfs : mrt >= 2 && mrt <= 4 ? sm_dfr : sm_df, tp[topo], 1,
                               blend, sb,
                               db, cf, depth, tnl, mrt, geo_write, 0);
    spec_pass = -1;
    pipe_mot = 0;
    LOG(LOG_D3D, "vk: pipeline %d (topology %d, blend %d %d/%d, depth format %d, shadows %d %d, motion %u)", npipes, topo,
        blend, sb, db, depth, mrt, geo_write, mot);
    return p->p;
}

/* shaders/ao.frag's pass `pass`: into a small image (0-2) or applied to the render target (3; 4 LIGHT's), of
   format color */
static VkPipeline ao_pipeline(int pass, VkFormat color)
{
    int apply = pass >= 3;
    uint32_t key = 0x20000000u | (uint32_t)pass;
    for (int i = 0; i < npipes; i++)
        if (pipes[i].key == key && pipes[i].color == color)
            return pipes[i].p;
    if (npipes == COUNT(pipes))
        fatal("Vulkan: too many pipelines");
    Pipe *p = &pipes[npipes++];
    p->key = key;
    p->depth = VK_FORMAT_UNDEFINED;
    p->color = color;
    spec_pass = pass;
    p->p = pipeline_create(sm_pv, sm_ao, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, VK_BLEND_FACTOR_ONE,
                           VK_BLEND_FACTOR_ZERO, p->color, VK_FORMAT_UNDEFINED, 0, 0, 0, apply ? 2 : 0);
    spec_pass = -1;
    return p->p;
}

/* shaders/resolve.frag's pass `pass`: over the render target (0, 2; 7 the headlights' light, added: no depth)
   or into an image; color: the target's format */
static VkPipeline resolve_pipeline(int pass, VkFormat color)
{
    int small = pass != 0 && pass != 2 && pass != 7 && pass != 8;
    uint32_t key = 0x40000000u | (uint32_t)pass;
    for (int i = 0; i < npipes; i++)
        if (pipes[i].key == key && pipes[i].color == color)
            return pipes[i].p;
    if (npipes == COUNT(pipes))
        fatal("Vulkan: too many pipelines");
    Pipe *p = &pipes[npipes++];
    p->key = key;
    p->depth = VK_FORMAT_UNDEFINED;
    p->color = color;
    spec_pass = pass;
    p->p = pipeline_create(sm_pv, sm_rf, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, VK_BLEND_FACTOR_ONE,
                           VK_BLEND_FACTOR_ZERO, p->color, VK_FORMAT_UNDEFINED, 0, 0, 0,
                           pass == 7 || pass == 8 ? 4 : !small);
    spec_pass = -1;
    return p->p;
}

static VkPipeline present_pipeline(VkFormat color)
{
    uint32_t key = 0x80000000u;
    for (int i = 0; i < npipes; i++)
        if (pipes[i].key == key && pipes[i].color == color)
            return pipes[i].p;
    if (npipes == COUNT(pipes))
        fatal("Vulkan: too many pipelines");
    Pipe *p = &pipes[npipes++];
    p->key = key;
    p->depth = VK_FORMAT_UNDEFINED;
    p->color = color;
    p->p = pipeline_create(sm_pv, sm_pf, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, VK_BLEND_FACTOR_ONE,
                           VK_BLEND_FACTOR_ZERO, color, VK_FORMAT_UNDEFINED, 0, 0, 0, 0);
    return p->p;
}

/* ------------------------------------------------------------------ surfaces on the GPU */
/* Convert program pixels to B8G8R8A8 (0xAARRGGBB little-endian). */
/* 16-bit formats: every possible pixel decoded once (a table per channel layout) */
typedef struct { uint8_t shift[4], bits[4]; uint32_t *lut; } Lut16;
static Lut16 luts[8];
static int nluts;

static const uint32_t *lut16(const Surface *s)
{
    for (int i = 0; i < nluts; i++)
        if (!memcmp(luts[i].shift, s->shift, 4) && !memcmp(luts[i].bits, s->bits, 4))
            return luts[i].lut;
    if (nluts == 8)
        return NULL;
    Lut16 *l = &luts[nluts];
    l->lut = malloc(65536 * 4);
    if (!l->lut)
        return NULL;
    memcpy(l->shift, s->shift, 4);
    memcpy(l->bits, s->bits, 4);
    for (uint32_t v = 0; v < 65536; v++)
        l->lut[v] = pix_decode(s, v);
    nluts++;
    return l->lut;
}

/* a pixel of s, from the render thread's copy of it when the game has written it since (vkr.h vk_snap_pixels) */
static uint32_t snap_read(const Surface *s, const uint8_t *snap, uint32_t x, uint32_t y)
{
    if (!snap)
        return surf_read(s, x, y);
    const uint8_t *p = snap + (size_t)y * s->pitch + (size_t)x * s->bpp;
    switch (s->bpp) {
    case 1: return p[0];
    case 2: return (uint32_t)(p[0] | p[1] << 8);
    case 3: return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16);
    default: return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
    }
}

static void convert_rows(const Surface *s, uint8_t *dst)
{
    const uint8_t *snap = vk_snap_pixels(s, NULL);
    int direct = s->bpp == 4 && s->pf[4] == 0xff0000 && s->pf[5] == 0xff00 && s->pf[6] == 0xff &&
                 (PF_FLAGS(s) & DDPF_ALPHAPIXELS) && s->pf[7] == 0xff000000u;
    int xrgb = s->bpp == 4 && s->pf[4] == 0xff0000 && s->pf[5] == 0xff00 && s->pf[6] == 0xff &&
               !(PF_FLAGS(s) & DDPF_ALPHAPIXELS);
    const uint32_t *lut = s->bpp == 2 ? lut16(s) : NULL;
    for (uint32_t y = 0; y < s->height; y++) {
        uint32_t *o = (uint32_t *)(dst + (size_t)y * s->width * 4);
        const uint8_t *row = snap ? snap + (size_t)y * s->pitch : (const uint8_t *)gptr(s->mem + y * s->pitch);
        if (direct) {
            memcpy(o, row, (size_t)s->width * 4);
        } else if (lut) {
            const uint16_t *p = (const uint16_t *)row;
            for (uint32_t x = 0; x < s->width; x++)
                o[x] = lut[p[x]];
        } else if (xrgb) {
            const uint32_t *p = (const uint32_t *)row;
            for (uint32_t x = 0; x < s->width; x++)
                o[x] = p[x] | 0xff000000u;
        } else {
            for (uint32_t x = 0; x < s->width; x++)
                o[x] = pix_decode(s, snap_read(s, snap, x, y));
        }
    }
}

/* Which of the 8 x 8 tiles of surface s have a texel with alpha under 1/2 (bit y * 8 + x), from
   the program's memory (the upload space is write-combined: slow to read) */
static uint64_t cut_tiles(const Surface *s, float *frac)
{
    *frac = 0;
    if (!(PF_FLAGS(s) & DDPF_ALPHAPIXELS) || !s->width || !s->height)
        return 0;
    uint32_t ncut = 0;
    const uint8_t *snap = vk_snap_pixels(s, NULL);
    const uint32_t *lut = s->bpp == 2 ? lut16(s) : NULL;
    int direct = s->bpp == 4 && s->pf[7] == 0xff000000u;
    uint32_t w = s->width, h = s->height;
    uint64_t m = 0;
    for (uint32_t ty = 0; ty < 8; ty++) {
        uint32_t y0 = ty * h / 8, y1 = (ty + 1) * h / 8;
        y1 = y1 > y0 ? y1 : y0 + 1;
        for (uint32_t tx = 0; tx < 8; tx++) {
            uint32_t x0 = tx * w / 8, x1 = (tx + 1) * w / 8;
            x1 = x1 > x0 ? x1 : x0 + 1;
            uint32_t acc = 0xffffffffu;   /* alpha's top bit stays only if every texel has it */
            for (uint32_t y = y0; y < y1 && y < h; y++) {
                const uint8_t *row = snap ? snap + (size_t)y * s->pitch : (const uint8_t *)gptr(s->mem + y * s->pitch);
                for (uint32_t x = x0; x < x1 && x < w; x++) {
                    uint32_t v = direct ? ((const uint32_t *)row)[x] : lut ? lut[((const uint16_t *)row)[x]]
                                                                             : pix_decode(s, snap_read(s, snap, x, y));
                    acc &= v;
                    ncut += !(v & 0x80000000u);
                }
            }
            if (!(acc & 0x80000000u))
                m |= 1ull << (ty * 8 + tx);
        }
    }
    *frac = fminf((float)ncut / (float)(w * h), 1.0f);
    return m;
}

static void upload_level(GpuImg *g, uint32_t level, const Surface *s)
{
    VkBuffer buf;
    uint8_t *p;
    VkDeviceSize size = (VkDeviceSize)s->width * s->height * 4;
    VkDeviceSize off = ring(size, 16, &buf, &p);
    convert_rows(s, p);
    if (level == 0 && g->kind == K_TEX)     /* its cut-outs: worked out again when a ray-traced view needs them */
        g->cut_known = 0;
    VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                           .imageExtent = {s->width, s->height, 1}};
    vkCmdCopyBufferToImage(cb, buf, g->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    st_uploads++;
    st_upload_bytes += size;
}

static int is_rt(const GpuImg *g) { return g && g->kind == K_RT; }

/* Temporal anti-aliasing: mip levels for the game's textures that have none (most of its terrain's:
   bilinear with no levels, so where the ground is seen at a slant a pixel's sample lands on any texel
   of its footprint, and as the jitter moves it frame to frame the ground boils: a heat haze over the
   screen). A texture that is not a cut-out (no colour key; alpha under 1/2 in at most 2% of its
   texels: the terrain's have alpha, nearly all opaque) gets a chain, each level the alpha-weighted
   2 x 2 average of the one above, while FSR 2 is chosen; only the scene's draws use it (stage_sampler),
   so OFF and 4X are as they were. Cut-outs keep their edges as drawn (an averaged alpha would thin
   them in the distance). */
static int taa_mips(const Surface *s)
{
    if ((s->ck_set & DDCKEY_SRCBLT) || s->width < 8 || s->height < 8 || s->width > 4096 || s->height > 4096)
        return 0;
    float frac = 0;
    if (PF_FLAGS(s) & DDPF_ALPHAPIXELS)
        cut_tiles(s, &frac);
    return frac <= 0.02f;
}

static uint32_t mip_count(uint32_t w, uint32_t h)
{
    uint32_t n = 1;
    while ((w > 1 || h > 1) && n < 16) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        n++;
    }
    return n;
}

static void upload_bytes(GpuImg *g, uint32_t level, const uint8_t *px, uint32_t w, uint32_t h)
{
    VkBuffer buf;
    uint8_t *p;
    VkDeviceSize size = (VkDeviceSize)w * h * 4;
    VkDeviceSize off = ring(size, 16, &buf, &p);
    memcpy(p, px, (size_t)size);
    VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                           .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cb, buf, g->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    st_uploads++;
    st_upload_bytes += size;
}

/* level 0 from s and the levels below it, each the 2 x 2 average of the one above */
static void upload_made_mips(GpuImg *g, const Surface *s)
{
    uint32_t w = s->width, h = s->height;
    uint8_t *a = malloc((size_t)w * h * 4), *b = malloc((size_t)((w + 1) / 2) * ((h + 1) / 2) * 4);
    if (!a || !b)
        fatal("out of memory");
    convert_rows(s, a);
    g->cut_known = 0;
    upload_bytes(g, 0, a, w, h);
    for (uint32_t level = 1; level < g->levels; level++) {
        uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        for (uint32_t y = 0; y < nh; y++)
            for (uint32_t x = 0; x < nw; x++) {
                uint32_t x0 = 2 * x < w ? 2 * x : w - 1, x1 = 2 * x + 1 < w ? 2 * x + 1 : w - 1;
                uint32_t y0 = 2 * y < h ? 2 * y : h - 1, y1 = 2 * y + 1 < h ? 2 * y + 1 : h - 1;
                const uint8_t *p00 = a + ((size_t)y0 * w + x0) * 4, *p01 = a + ((size_t)y0 * w + x1) * 4;
                const uint8_t *p10 = a + ((size_t)y1 * w + x0) * 4, *p11 = a + ((size_t)y1 * w + x1) * 4;
                uint8_t *o = b + ((size_t)y * nw + x) * 4;
                uint32_t sa = (uint32_t)p00[3] + p01[3] + p10[3] + p11[3];
                for (int c = 0; c < 3; c++)       /* (by alpha: a clear texel's colour does not bleed in) */
                    o[c] = sa ? (uint8_t)((p00[c] * p00[3] + p01[c] * p01[3] + p10[c] * p10[3] + p11[c] * p11[3] + sa / 2) / sa)
                              : (uint8_t)((p00[c] + p01[c] + p10[c] + p11[c] + 2) / 4);
                o[3] = (uint8_t)((sa + 2) / 4);
            }
        upload_bytes(g, level, b, nw, nh);
        uint8_t *t = a;
        a = b;
        b = t;
        w = nw;
        h = nh;
    }
    free(a);
    free(b);
}

/* s's dirty counter as its pixels the render thread reads were (vkr.h vk_snap_pixels) */
static unsigned long snap_dirty(const Surface *s)
{
    unsigned long d;
    return vk_snap_pixels(s, &d) ? d : s->dirty;
}

/* The GPU copy of a texture (or a 2D source surface), refreshed from program memory. */
static GpuImg *tex_sync(Surface *s)
{
    static int warned;
    if (!s || s->dead || !s->mem || s->bpp < 1 || s->bpp > 4 || (PF_FLAGS(s) & DDPF_ZBUFFER))
        return NULL;
    GpuImg *g = s->gpu;
    if (is_rt(g) || (g && g->kind == K_DEPTH)) {
        if (!warned++)
            LOG(LOG_ALWAYS, "vk: a render target used as a texture is not supported yet");
        return NULL;
    }
    Surface *lv[16];
    uint32_t n = 1;
    lv[0] = s;
    if (s->caps & DDSCAPS_MIPMAP)
        for (Surface *m = s->next; m && m != s && !m->dead && n < 16; m = m->next)
            lv[n++] = m;
    int taa = n == 1 && vk_taa_on();
    if (g && (g->w != s->width || g->h != s->height || g->game_levels != n || g->mips_taa != taa)) {
        img_discard(g);
        g = NULL;
    }
    if (!g) {
        int made = taa && taa_mips(s);
        g = img_new(K_TEX, s->width, s->height, made ? mip_count(s->width, s->height) : n, RT_FORMAT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        g->game_levels = n;
        g->made_mips = made;
        g->mips_taa = taa;
        s->gpu = g;
    }
    if (g->made_mips) {
        unsigned long d0 = snap_dirty(s);
        if (g->uploaded[0] && g->dirty[0] == d0)
            return g;
        barrier(g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        upload_made_mips(g, s);
        g->dirty[0] = d0;
        g->uploaded[0] = 1;
        barrier(g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return g;
    }
    int need = 0;
    unsigned long dv[16];
    for (uint32_t i = 0; i < n; i++) {
        dv[i] = snap_dirty(lv[i]);
        if (!g->uploaded[i] || g->dirty[i] != dv[i])
            need = 1;
    }
    if (!need)
        return g;
    barrier(g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (uint32_t i = 0; i < n; i++)
        if (!g->uploaded[i] || g->dirty[i] != dv[i]) {
            upload_level(g, i, lv[i]);
            g->dirty[i] = dv[i];
            g->uploaded[i] = 1;
        }
    barrier(g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return g;
}

/* The letters drawn from the chosen font (font.c), used in place of the game's picture of them: its
   shade as the colour, the coverage as alpha, with a chain of 2 x 2 averaged levels (the menus shrink it about 3 times at
   1440p); uploaded again when font.c draws it anew. NULL with the game's own letters. */
static GpuImg *font_img;
static uint32_t font_img_gen;

static GpuImg *font_sync(void)
{
    int n;
    uint32_t gen;
    const uint8_t *a = font_atlas(&n, &gen);
    if (!a)
        return NULL;
    if (font_img && font_img_gen == gen)
        return font_img;
    uint32_t levels = mip_count((uint32_t)n, (uint32_t)n);
    if (!font_img)
        font_img = img_new(K_TEX, (uint32_t)n, (uint32_t)n, levels, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    barrier(font_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    uint8_t *buf2[2] = {malloc((size_t)n * n * 2), malloc((size_t)n * n * 2)};   /* alpha, then shade */
    memcpy(buf2[0], a, (size_t)n * n * 2);
    for (uint32_t l = 0, w = (uint32_t)n, cur = 0; l < levels; l++, w /= 2, cur ^= 1) {
        const uint8_t *lv = buf2[cur];
        VkBuffer buf;
        uint8_t *p;
        VkDeviceSize size = (VkDeviceSize)w * w * 4;
        VkDeviceSize off = ring(size, 16, &buf, &p);
        for (size_t k = 0; k < (size_t)w * w; k++) {
            p[4 * k] = p[4 * k + 1] = p[4 * k + 2] = lv[(size_t)w * w + k];
            p[4 * k + 3] = lv[k];
        }
        VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1},
                               .imageExtent = {w, w, 1}};
        vkCmdCopyBufferToImage(cb, buf, font_img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        st_uploads++;
        st_upload_bytes += size;
        if (w < 2)
            break;
        uint8_t *next = buf2[cur ^ 1];
        for (int plane = 0; plane < 2; plane++)
            for (uint32_t y = 0; y < w / 2; y++)
                for (uint32_t x = 0; x < w / 2; x++) {
                    const uint8_t *q = lv + (size_t)plane * w * w + (size_t)(2 * y) * w + 2 * x;
                    next[(size_t)plane * (w / 2) * (w / 2) + (size_t)y * (w / 2) + x] =
                        (uint8_t)((q[0] + q[1] + q[w] + q[w + 1] + 2) / 4);
                }
    }
    free(buf2[0]);
    free(buf2[1]);
    barrier(font_img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    font_img_gen = gen;
    return font_img;
}

static void blit(GpuImg *src, const int *sr, GpuImg *dst, const int *dr, VkFilter filter)
{
    end_rendering();
    barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{sr[0], sr[1], 0}, {sr[2], sr[3], 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{dr[0], dr[1], 0}, {dr[2], dr[3], 1}}};
    vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, filter);
}

static GpuImg *rt_new(const Surface *s, uint32_t pw, uint32_t ph)
{
    GpuImg *g = img_new(K_RT, pw, ph, 1, RT_FORMAT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    g->lw = s->width;
    g->lh = s->height;
    g->sx = (float)pw / (float)s->width;
    g->sy = (float)ph / (float)s->height;
    g->ox = ((float)pw - (float)s->width * g->sy) / 2.0f;
    return g;
}

/* Is s the screen (front/back buffer of the display mode)? Widescreen widens only those. */
static int screen_sized(const Surface *s)
{
    return gfx_mode.set && s->width == gfx_mode.w && s->height == gfx_mode.h;
}

/* Program x (its pixels) -> GPU x for things that keep their 4:3 shape (centred). */
static float ui_x(const GpuImg *c, float x) { return x * c->sy + c->ox; }

/* The GPU image of a render target, res_scale times the surface's size; created from (and
   refreshed after CPU writes to) memory; resized with its picture kept when the scale changes. */
static GpuImg *rt_sync(Surface *s)
{
    GpuImg *g = s->gpu;
    if (g && !is_rt(g)) {
        img_discard(g);
        g = NULL;
    }
    uint32_t ph = (uint32_t)lroundf((float)s->height * res_scale);
    uint32_t pw = widescreen_on() && screen_sized(s) && ws_aspect * s->height > s->width
                      ? (uint32_t)lroundf((float)ph * ws_aspect)
                      : (uint32_t)lroundf((float)s->width * res_scale);
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    if (g && (g->w != pw || g->h != ph)) {
        GpuImg *n = rt_new(s, pw, ph);
        if (!g->cpu_newer) {
            int a[4] = {0, 0, (int)g->w, (int)g->h}, b[4] = {0, 0, (int)pw, (int)ph};
            blit(g, a, n, b, VK_FILTER_LINEAR);
            n->gpu_newer = g->gpu_newer;
        } else {
            n->cpu_newer = 1;
        }
        img_discard(g);
        g = n;
        s->gpu = g;
    }
    if (!g) {
        g = rt_new(s, pw, ph);
        g->cpu_newer = 1;
        s->gpu = g;
    }
    if (g->cpu_newer) {
        if (g->w == s->width && g->h == s->height) {
            end_rendering();
            barrier(g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            upload_level(g, 0, s);
        } else {
            /* upload at the program's size, then scale up */
            GpuImg *t = img_new(K_TEX, s->width, s->height, 1, RT_FORMAT,
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            end_rendering();
            barrier(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            upload_level(t, 0, s);
            int a[4] = {0, 0, (int)s->width, (int)s->height}, b[4] = {0, 0, (int)g->w, (int)g->h};
            blit(t, a, g, b, VK_FILTER_LINEAR);
            img_discard(t);
        }
        g->cpu_newer = 0;
    }
    return g;
}

static GpuImg *depth_sync(Surface *z, const GpuImg *c)
{
    if (!z || z->dead || !(z->caps & DDSCAPS_ZBUFFER) || z->width < c->lw || z->height < c->lh)
        return NULL;
    uint32_t pw = (uint32_t)lroundf((float)z->width * c->sx), ph = (uint32_t)lroundf((float)z->height * c->sy);
    GpuImg *g = z->gpu;
    int sampled = rt_ok || vk_smoke_depth;      /* the shadows read it, the volumetric smoke too */
    if (g && g->kind == K_DEPTH && g->w == pw && g->h == ph && (!sampled || g->slot >= 0))
        return g;
    if (g)
        img_discard(g);
    VkFormat f = z->zmask == 0xffff ? VK_FORMAT_D16_UNORM : depth24;
    g = img_new(K_DEPTH, pw, ph, 1, f,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0));
    barrier(g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue v = {1.0f, 0};
    VkImageSubresourceRange r = {g->aspect, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cb, g->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    z->gpu = g;
    return g;
}

/* The image in holder *a, made at w x h in format f if it is not: views of two sizes in one frame
   (temporal anti-aliasing's scene and a mirror at the screen's size) each keep theirs, the other
   one in *spare, instead of making them again at every view */
static GpuImg *aux_img(GpuImg **a, GpuImg **spare, uint32_t w, uint32_t h, VkFormat f)
{
    if (*a && (*a)->w == w && (*a)->h == h && (*a)->format == f)
        return *a;
    if (*spare && (*spare)->w == w && (*spare)->h == h && (*spare)->format == f) {
        GpuImg *t = *a;
        *a = *spare;
        *spare = t;
        return *a;
    }
    if (*spare)
        img_discard(*spare);
    *spare = *a;
    *a = img_new(K_AUX, w, h, 1, f, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    return *a;
}

/* the shadows' extra attachments for render target c (made or remade at its size; attachment 2 in
   format geo) */
static void aux_sync(GpuImg *c, VkFormat geo)
{
    static GpuImg *spare[2];
    aux_img(&att_sun, &spare[0], c->w, c->h, aux_format(c->format));
    aux_img(&att_geo, &spare[1], c->w, c->h, geo);
}

static int r_mrt;                 /* the rendering has the shadows' extra attachments */

static void begin_rendering(GpuImg *c, GpuImg *d)
{
    int mrt = sv.active && c == sv.c;
    int mot = want_mot && scn.active && scn.fsr && scn.drawn && c == scn.c;   /* temporal anti-aliasing's scene:
                                                                                attachments 3, 4 */
    if (rendering && r_color == c && r_depth == d && r_mrt == mrt && r_mot == mot)
        return;
    end_rendering();
    if (mrt) {
        aux_sync(c, GEO_FORMAT);
        barrier(att_sun, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        barrier(att_geo, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (mot) {
        barrier(scn.vel, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        barrier(scn.mask, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    barrier(c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (d)
        barrier(d, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca[5] = {{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = c->view,
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE}};
    if (mrt) {
        /* cleared once a view: no sun to take away, no class */
        for (int i = 1; i < 3; i++)
            ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                .imageView = (i == 1 ? att_sun : att_geo)->view,
                                                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                .loadOp = sv.cleared ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        sv.cleared = 1;
    }
    if (mot) {                    /* (cleared as the scene starts: scene_target) */
        for (int i = 1; i < 5; i++)
            if (i >= 3 || !mrt)
                ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                    .imageView = i == 3 ? scn.vel->view : i == 4 ? scn.mask->view
                                                                                                : VK_NULL_HANDLE,
                                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    }
    VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = d ? d->view : VK_NULL_HANDLE,
                                    .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {c->w, c->h}},
                          .layerCount = 1, .colorAttachmentCount = mot ? 5 : mrt ? 3 : 1, .pColorAttachments = ca,
                          .pDepthAttachment = d ? &da : NULL};
    vkCmdBeginRendering(cb, &ri);
    st_passes++;
    rendering = 1;
    r_color = c;
    r_depth = d;
    r_mrt = mrt;
    r_mot = mot;
}

static void set_state(const GpuImg *c, const int *sc, VkCullModeFlags cull, int ztest, int zwrite, VkCompareOp op,
                      float bias)
{
    VkViewport vp = {0, 0, (float)c->w, (float)c->h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    int x0 = sc[0], y0 = sc[1], x1 = sc[2], y1 = sc[3];   /* GPU pixels */
    x0 = x0 > 0 ? x0 : 0;
    y0 = y0 > 0 ? y0 : 0;
    x1 = x1 < (int)c->w ? x1 : (int)c->w;
    y1 = y1 < (int)c->h ? y1 : (int)c->h;
    VkRect2D r = {{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}};
    vkCmdSetScissor(cb, 0, 1, &r);
    vkCmdSetCullMode(cb, cull);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetDepthTestEnable(cb, ztest);
    vkCmdSetDepthWriteEnable(cb, zwrite);
    vkCmdSetDepthCompareOp(cb, op);
    vkCmdSetDepthBiasEnable(cb, bias != 0);
    vkCmdSetDepthBias(cb, bias, 0, 0);
}

static uint32_t pack_rgba(const float *c)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        r |= (uint32_t)(v * 255.0f + 0.5f) << (8 * i);
    }
    return r;
}

/* Does a draw's texture have cut-outs where the draw uses it? The texture coordinates' extent of
   the vertices it uses (40-byte vertices: u, v at 32; its indices, or all nv) against the
   texture's tiles with cut-outs. (Program memory only: the upload space is slow to read.) */
static int cut_used(uint64_t tiles, const void *raw, int nv, const uint16_t *ix, int ni)
{
    if (!tiles)
        return 0;
    if (!raw || tiles == ~0ull)
        return 1;
    float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
    const uint8_t *b = raw;
    int n = ix ? ni : nv;
    for (int i = 0; i < n; i++) {
        int k = ix ? ix[i] : i;
        if (k >= nv)
            continue;
        float t[2];
        memcpy(t, b + 40 * (size_t)k + 32, 8);
        u0 = t[0] < u0 ? t[0] : u0;
        u1 = t[0] > u1 ? t[0] : u1;
        v0 = t[1] < v0 ? t[1] : v0;
        v1 = t[1] > v1 ? t[1] : v1;
    }
    if (!(u1 - u0 < 1.0f && v1 - v0 < 1.0f) || !isfinite(u0) || !isfinite(v0))
        return 1;                     /* repeats: all of it */
    float fu = floorf(u0), fv = floorf(v0);
    u0 -= fu, u1 -= fu, v0 -= fv, v1 -= fv;
    if (u1 > 1.0f || v1 > 1.0f)       /* across the wrap */
        return 1;
    int x0 = (int)(u0 * 8), x1 = (int)(u1 * 8), y0 = (int)(v0 * 8), y1 = (int)(v1 * 8);
    x1 = x1 > 7 ? 7 : x1;
    y1 = y1 > 7 ? 7 : y1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (tiles >> (y * 8 + x) & 1)
                return 1;
    return 0;
}

/* ------------------------------------------------------------------ init */
static int pick_device(void)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice list[16];
    if (n > 16)
        n = 16;
    vkEnumeratePhysicalDevices(inst, &n, list);
    const char *want = getenv("TD6_GPU_DEVICE");
    int best = -1, best_score = 0;
    uint32_t best_q = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(list[i], &p);
        VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                                .pNext = &f12};
        VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        vkGetPhysicalDeviceFeatures2(list[i], &f);
        if (!f13.dynamicRendering || !f13.synchronization2 || !f12.runtimeDescriptorArray ||
            !f12.descriptorBindingPartiallyBound || !f12.descriptorBindingSampledImageUpdateAfterBind)
            continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(list[i], &nq, NULL);
        VkQueueFamilyProperties qp[16];
        if (nq > 16)
            nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(list[i], &nq, qp);
        int q = -1;
        for (uint32_t k = 0; k < nq && q < 0; k++) {
            VkBool32 pres = VK_TRUE;
            if (surface)
                vkGetPhysicalDeviceSurfaceSupportKHR(list[i], k, surface, &pres);
            if ((qp[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres)
                q = (int)k;
        }
        if (q < 0)
            continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 5
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 4
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 3 : 2;
        if (want && *want && strcasestr(p.deviceName, want))
            score += 100;
        if (score > best_score) {
            best_score = score;
            best = (int)i;
            best_q = (uint32_t)q;
        }
    }
    if (best < 0)
        return 0;
    phys = list[best];
    qfam = best_q;
    return 1;
}

static int pick_depth_format(void)
{
    static const VkFormat cand[] = {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
                                    VK_FORMAT_D32_SFLOAT_S8_UINT};
    for (int i = 0; i < COUNT(cand); i++) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(phys, cand[i], &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            depth24 = cand[i];
            depth24_stencil = i >= 2;
            return 1;
        }
    }
    return 0;
}

/* Can the picked device trace rays from a fragment shader (the shadows)? Not on a CPU device
   (llvmpipe: possible but far too slow) unless TD6_RT_ANY=1; TD6_RT=0 says no anyway. */
static int rt_supported(VkPhysicalDeviceType type)
{
    const char *e = getenv("TD6_RT"), *any = getenv("TD6_RT_ANY");
    if (e && !strcmp(e, "0"))
        return 0;
    if (type == VK_PHYSICAL_DEVICE_TYPE_CPU && !(any && *any && strcmp(any, "0")))
        return 0;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL);
    VkExtensionProperties *ep = calloc(n ? n : 1, sizeof *ep);
    if (!ep)
        return 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &n, ep);
    int have = 0;
    for (uint32_t i = 0; i < n; i++) {
        have |= !strcmp(ep[i].extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        have |= !strcmp(ep[i].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) << 1;
        have |= !strcmp(ep[i].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) << 2;
    }
    free(ep);
    if (have != 7)
        return 0;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fas = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR frq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
                                               .pNext = &fas};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &frq};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(phys, &f);
    return fas.accelerationStructure && frq.rayQuery && f12.bufferDeviceAddress &&
           f12.shaderSampledImageArrayNonUniformIndexing;   /* resolve.frag: the hit geometry's texture */
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m;
    VKC(vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}

static void swapchain_destroy(void)
{
    for (uint32_t i = 0; i < sc_count; i++) {
        vkDestroyImageView(dev, sc_views[i], NULL);
        vkDestroySemaphore(dev, sc_done[i], NULL);
    }
    sc_count = 0;
}

/* The swapchain's format (the output step, present.frag's encode()): an 8-bit UNORM one in sRGB_NONLINEAR,
   the game's gamma-space values shown as they are. What else the surface offers for HDR is logged once (the
   HDR path, DESIGN.md decision 4 "Colour precision": scRGB is R16G16B16A16_SFLOAT in EXTENDED_SRGB_LINEAR,
   HDR10 A2B10G10R10 in HDR10_ST2084; their colour spaces are listed with VK_EXT_swapchain_colorspace). */
static VkFormat surface_format(void)
{
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, NULL);
    VkSurfaceFormatKHR *fm = calloc(nf ? nf : 1, sizeof *fm);
    if (!fm)
        fatal("out of memory");
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, fm);
    VkFormat f = nf ? fm[0].format : VK_FORMAT_B8G8R8A8_UNORM;
    for (uint32_t i = 0; i < nf; i++)
        if ((fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fm[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            f = fm[i].format;
            break;
        }
    static int told;
    if (!told++) {
        int ten = 0, half = 0, scrgb = 0, hdr10 = 0, nsp = 0;
        VkColorSpaceKHR sp[16];
        for (uint32_t i = 0; i < nf; i++) {
            int a2 = fm[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || fm[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            int fp = fm[i].format == VK_FORMAT_R16G16B16A16_SFLOAT;
            ten |= a2 && fm[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            half |= fp && fm[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            scrgb |= fp && fm[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
            hdr10 |= a2 && fm[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
            int k = 0;
            while (k < nsp && sp[k] != fm[i].colorSpace)
                k++;
            if (k == nsp && nsp < 16)
                sp[nsp++] = fm[i].colorSpace;
        }
        logmsg(LOG_ALWAYS, "Vulkan: the window offers %u formats in %d colour spaces: 10-bit %s, half float %s, "
               "HDR scRGB (R16G16B16A16_SFLOAT, EXTENDED_SRGB_LINEAR) %s, HDR10 (A2B10G10R10, ST 2084) %s",
               nf, nsp, ten ? "yes" : "no", half ? "yes" : "no", scrgb ? "yes" : "no", hdr10 ? "yes" : "no");
    }
    free(fm);
    return f;
}

/* (Re)creates the swapchain for the window's current size; 0 while minimized. */
static int swapchain_ok(void)
{
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0)
        return 0;
    if (swapchain && !sc_stale && (uint32_t)w == sc_w && (uint32_t)h == sc_h)
        return 1;
    vkDeviceWaitIdle(dev);
    VkSurfaceCapabilitiesKHR caps;
    VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xffffffffu) {
        ext.width = (uint32_t)w;
        ext.height = (uint32_t)h;
    }
    if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
    if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (!ext.width || !ext.height)
        return 0;
    if (!sc_format)
        sc_format = surface_format();
    sc_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!want_vsync) {
        uint32_t nm = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, NULL);
        VkPresentModeKHR md[16];
        if (nm > 16)
            nm = 16;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, md);
        for (uint32_t i = 0; i < nm; i++)
            if (md[i] == VK_PRESENT_MODE_MAILBOX_KHR ||
                (md[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && sc_mode == VK_PRESENT_MODE_FIFO_KHR))
                sc_mode = md[i];
    }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;
    if (count > 8)
        count = 8;
    VkCompositeAlphaFlagBitsKHR ca = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ca))
        for (uint32_t b = 1; b; b <<= 1)
            if (caps.supportedCompositeAlpha & b) {
                ca = (VkCompositeAlphaFlagBitsKHR)b;
                break;
            }
    VkSwapchainKHR old = swapchain;
    VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
                                   .minImageCount = count, .imageFormat = sc_format,
                                   .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = ext,
                                   .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                   .preTransform = caps.currentTransform, .compositeAlpha = ca,
                                   .presentMode = sc_mode, .clipped = VK_TRUE, .oldSwapchain = old};
    VKC(vkCreateSwapchainKHR(dev, &ci, NULL, &swapchain));
    swapchain_destroy();
    if (old)
        vkDestroySwapchainKHR(dev, old, NULL);
    VKC(vkGetSwapchainImagesKHR(dev, swapchain, &sc_count, NULL));
    if (sc_count > 8)
        fatal("Vulkan: %u swapchain images", sc_count);
    VKC(vkGetSwapchainImagesKHR(dev, swapchain, &sc_count, sc_images));
    for (uint32_t i = 0; i < sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sc_format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKC(vkCreateImageView(dev, &vi, NULL, &sc_views[i]));
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKC(vkCreateSemaphore(dev, &si, NULL, &sc_done[i]));
    }
    sc_w = (uint32_t)w;
    sc_h = (uint32_t)h;
    sc_stale = 0;
    LOG(LOG_D3D, "vk: swapchain %ux%u, %u images, format %d, present mode %d", ext.width, ext.height, sc_count,
        sc_format, sc_mode);
    return 1;
}

static int init(void)
{
    const char *off = getenv("TD6_GPU_OFFSCREEN");
    int want_off = off && *off && strcmp(off, "0");
    window = host_sdl_window();
    if (!window && !want_off) {
        if (!headless)
            logmsg(LOG_ALWAYS, "Vulkan: no window to draw into");
        return 0;
    }
    offscreen = !window;
    const char *exts[16];
    uint32_t next = 0;
    if (window) {
        Uint32 n = 0;
        const char *const *e = SDL_Vulkan_GetInstanceExtensions(&n);
        for (Uint32 i = 0; e && i < n && next < 15; i++)
            exts[next++] = e[i];
        /* the surface's colour spaces beyond sRGB (HDR: surface_format logs what the window offers) */
        uint32_t ne = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
        VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
        if (ep && vkEnumerateInstanceExtensionProperties(NULL, &ne, ep) == VK_SUCCESS)
            for (uint32_t i = 0; i < ne; i++)
                if (!strcmp(ep[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
                    exts[next++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
                    break;
                }
        free(ep);
    }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "1NSANE native",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = next, .ppEnabledExtensionNames = exts};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *val = getenv("TD6_VK_VALIDATE");
    if (val && *val && strcmp(val, "0")) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
    }
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        logmsg(LOG_ALWAYS, "Vulkan: cannot create an instance (%d)", r);
        return 0;
    }
    if (window && !SDL_Vulkan_CreateSurface(window, inst, NULL, &surface)) {
        logmsg(LOG_ALWAYS, "Vulkan: cannot create a window surface: %s", SDL_GetError());
        return 0;
    }
    if (!pick_device()) {
        logmsg(LOG_ALWAYS, "Vulkan: no device with Vulkan 1.3, dynamic rendering and descriptor indexing");
        return 0;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    st_tick_ns = props.limits.timestampComputeAndGraphics ? props.limits.timestampPeriod : 0;
    aniso_max = props.limits.maxSamplerAnisotropy < 16.0f ? props.limits.maxSamplerAnisotropy : 16.0f;
    snprintf(gpu_name, sizeof gpu_name, "%s", props.deviceName);
    char *paren = strstr(gpu_name, " (");      /* "AMD Radeon 890M Graphics (RADV GFX1150)" */
    if (paren) *paren = 0;
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    if (!pick_depth_format()) {
        logmsg(LOG_ALWAYS, "Vulkan: no usable depth format");
        return 0;
    }
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(phys, &p2);
    ntex_slots = MAX_TEX;
    if (p12.maxPerStageDescriptorUpdateAfterBindSampledImages < ntex_slots)
        ntex_slots = p12.maxPerStageDescriptorUpdateAfterBindSampledImages;
    if (p12.maxDescriptorSetUpdateAfterBindSampledImages < ntex_slots)
        ntex_slots = p12.maxDescriptorSetUpdateAfterBindSampledImages;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .runtimeDescriptorArray = VK_TRUE,
                                            .descriptorBindingPartiallyBound = VK_TRUE,
                                            .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = &f12, .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE};   /* discard in draw.frag */
    const char *dext[16];
    uint32_t ndext = 0;
    if (window)
        dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    /* ray-traced shadows: ray queries on a real GPU (llvmpipe has them too, far too slowly) */
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fas = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .accelerationStructure = VK_TRUE};
    VkPhysicalDeviceRayQueryFeaturesKHR frq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
                                               .pNext = &fas, .rayQuery = VK_TRUE};
    VkPhysicalDeviceFeatures pf;
    vkGetPhysicalDeviceFeatures(phys, &pf);
    rt_ok = rt_supported(props.deviceType) && pf.independentBlend;   /* (the extra attachments blend apart) */
    /* temporal anti-aliasing (FSR 2): the scene's attachments blend apart and are of these formats; FSR 2's
       shaders write format-less storage images and use 16-bit maths and subgroup sizes where there */
    VkPhysicalDeviceVulkan11Features q11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features q12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &q11};
    VkPhysicalDeviceVulkan13Features q13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &q12};
    VkPhysicalDeviceFeatures2 qf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &q13};
    vkGetPhysicalDeviceFeatures2(phys, &qf);
#if defined(HAVE_FSR2) || defined(HAVE_DLSS)
    int scene_fmt;                /* the scene's images can be made (FSR 2 and DLSS) */
    {
        const char *te = getenv("TD6_TAA");
        VkFormatProperties fv, fm, fu;
        vkGetPhysicalDeviceFormatProperties(phys, VEL_FORMAT, &fv);
        vkGetPhysicalDeviceFormatProperties(phys, MASK_FORMAT, &fm);
        vkGetPhysicalDeviceFormatProperties(phys, UPS_FORMAT, &fu);
        VkFormatFeatureFlags att = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        scene_fmt = !(te && !strcmp(te, "0")) && pf.independentBlend && (fv.optimalTilingFeatures & att) == att &&
                    (fm.optimalTilingFeatures & att) == att &&
                    (fu.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    }
#endif
#ifdef HAVE_FSR2
    taa_ok = scene_fmt && pf.shaderStorageImageWriteWithoutFormat && pf.shaderStorageImageReadWithoutFormat;
#endif
#if defined(HAVE_FSR2) || defined(HAVE_DLSS)
    own_ok = scene_fmt;
#endif
    int dlss_want = 0;
#ifdef HAVE_DLSS
    /* DLSS (dlss.c): NGX's device extensions, and buffer device addresses */
    int dlss_bda = 0, ndlss = scene_fmt ? dlss_device_extensions(phys, dext + 8, 8, &dlss_bda) : -1;
    if (dlss_bda && !q12.bufferDeviceAddress)
        ndlss = -1;
    for (int k = 0; k < ndlss; k++)
        dext[ndext + k] = dext[8 + k];      /* (after the swapchain's; the ray queries' follow) */
    if (ndlss > 0)
        ndext += (uint32_t)ndlss;
    if (ndlss >= 0 && dlss_bda)
        f12.bufferDeviceAddress = VK_TRUE;
    dlss_want = ndlss >= 0;
#endif
    {   /* "colour_precision = high": a 16-bit colour the device blends and filters; UNORM (clamped as the 8-bit
           colour is) where it can, else half floats (TD6_HP_FORMAT=float: those, to compare) */
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        const char *e = getenv("TD6_HP_FORMAT");
        VkFormat cand[2] = {HP_FORMAT, VK_FORMAT_R16G16B16A16_SFLOAT};
        if (e && !strcmp(e, "10"))      /* (10 bits a channel, the 8-bit colour's bandwidth: to compare) */
            cand[0] = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys, NOISE_FORMAT, &fp);
        int noise = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        for (int i = e && !strcmp(e, "float") ? 1 : 0; i < 2 && !hp_format && noise; i++) {
            vkGetPhysicalDeviceFormatProperties(phys, cand[i], &fp);
            if ((fp.optimalTilingFeatures & need) == need)
                hp_format = cand[i];
        }
        if ((e = getenv("TD6_HP_DITHER")))
            hp_dither = atoi(e) != 0;
        /* the sun's part beside it: 10 bits (blended and sampled by every Vulkan device) */
        hp_sun = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        vkGetPhysicalDeviceFormatProperties(phys, hp_sun, &fp);
        if ((fp.optimalTilingFeatures & need) != need)
            hp_sun = hp_format;
        if ((e = getenv("TD6_HP_SUN")))
            hp_sun = atoi(e) == 8 ? AUX_FORMAT : atoi(e) == 16 ? hp_format : hp_sun;
        if ((e = getenv("TD6_HP_FLIP")))
            hp_flip = atoi(e) > 0 ? atoi(e) : 0;
    }
    if (rt_ok) {
        dext[ndext++] = VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME;
        dext[ndext++] = VK_KHR_RAY_QUERY_EXTENSION_NAME;
        dext[ndext++] = VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME;
        f12.bufferDeviceAddress = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        f12.pNext = &frq;
    }
    if (taa_ok) {                 /* FSR 2's shaders: what the device has of these (after the ray queries' features,
                                     which f12 leads to) */
        f12.shaderFloat16 = q12.shaderFloat16;
        f11.storageBuffer16BitAccess = q11.storageBuffer16BitAccess;
        f11.uniformAndStorageBuffer16BitAccess = q11.uniformAndStorageBuffer16BitAccess;
        f13.subgroupSizeControl = q13.subgroupSizeControl;
        f13.computeFullSubgroups = q13.computeFullSubgroups;
        f11.pNext = f12.pNext;
        f12.pNext = &f11;
    }
    aniso_ok = pf.samplerAnisotropy;
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13,
                                    .features = {.samplerAnisotropy = pf.samplerAnisotropy,
                                                 .independentBlend = rt_ok || taa_ok || dlss_want ? VK_TRUE : VK_FALSE,
                                                 .shaderInt16 = taa_ok ? pf.shaderInt16 : VK_FALSE,
                                                 .shaderStorageImageReadWithoutFormat = taa_ok ? VK_TRUE : VK_FALSE,
                                                 .shaderStorageImageWriteWithoutFormat = taa_ok ? VK_TRUE : VK_FALSE}};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = ndext,
                              .ppEnabledExtensionNames = dext};
    r = vkCreateDevice(phys, &dci, NULL, &dev);
    if (r != VK_SUCCESS && rt_ok) {
        logmsg(LOG_ALWAYS, "Vulkan: cannot create the device with ray queries (%d); shadows stay as shipped", r);
        rt_ok = 0;
        dlss_want = 0;                          /* (its extensions go too) */
        dci.enabledExtensionCount = window ? 1 : 0;
        f12.bufferDeviceAddress = f12.shaderSampledImageArrayNonUniformIndexing = VK_FALSE;
        f12.pNext = taa_ok ? &f11 : NULL;
        f11.pNext = NULL;
        f2.features.independentBlend = taa_ok ? VK_TRUE : VK_FALSE;
        r = vkCreateDevice(phys, &dci, NULL, &dev);
    }
    if (r != VK_SUCCESS) {
        logmsg(LOG_ALWAYS, "Vulkan: cannot create the device (%d)", r);
        return 0;
    }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    pcache_open();
    sm_noise_start();
    if (rt_ok) {
        p_create_as = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr(dev, "vkCreateAccelerationStructureKHR");
        p_destroy_as = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr(dev, "vkDestroyAccelerationStructureKHR");
        p_build_sizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(
            dev, "vkGetAccelerationStructureBuildSizesKHR");
        p_build_as = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(dev, "vkCmdBuildAccelerationStructuresKHR");
        p_as_addr = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(
            dev, "vkGetAccelerationStructureDeviceAddressKHR");
        rt_ok = p_create_as && p_destroy_as && p_build_sizes && p_build_as && p_as_addr;
    }
#ifdef HAVE_DLSS
    dlss_ok = dlss_want && dlss_init(inst, phys, dev, user_dir);
#endif

    /* bindless textures and samplers; the T&L constants; the shadows' block (and with ray queries
       the two frames' top-level acceleration structures) */
    /* (binding 5: temporal anti-aliasing's Motion blocks, shaders/motion.glsl) */
    /* (binding 6: the volumetric smoke's puffs, shaders/smoke.glsl) */
    /* (binding 7: the instanced draws' records, shaders/tnl.vert) */
    uint32_t nb = rt_ok ? 8 : 7;
    VkDescriptorSetLayoutBinding b[8] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ntex_slots, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, MAX_SMP, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2 * SH_SLOTS, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorBindingFlags bf[8] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0, 0, 0, 0, 0, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = nb,
        .pBindingFlags = bf};
    VkDescriptorSetLayoutCreateInfo dli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .pNext = &bfi,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                           .bindingCount = nb, .pBindings = b};
    VKC(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, ntex_slots}, {VK_DESCRIPTOR_TYPE_SAMPLER, MAX_SMP},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5},
                                  {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2 * SH_SLOTS}};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 1,
                                      .poolSizeCount = rt_ok ? 4 : 3, .pPoolSizes = ps};
    VKC(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VKC(vkAllocateDescriptorSets(dev, &dai, &dset));
    {   /* the frames' TnlConst blocks (frame i at TNL_MAX * i), host-written */
        VkDeviceSize size = (VkDeviceSize)FRAMES * TNL_MAX * sizeof(TnlConst);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        VKC(vkCreateBuffer(dev, &bi, NULL, &tnlbuf));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, tnlbuf, &mr);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                   .memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0)};
        VKC(vkAllocateMemory(dev, &ai, NULL, &tnlmem));
        VKC(vkBindBufferMemory(dev, tnlbuf, tnlmem, 0));
        TnlConst *map;
        VKC(vkMapMemory(dev, tnlmem, 0, size, 0, (void **)&map));
        for (int i = 0; i < FRAMES; i++)
            fr[i].tnl = map + (size_t)TNL_MAX * i;
        VkDescriptorBufferInfo bufi = {tnlbuf, 0, size};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 2,
                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &bufi};
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    }
    {   /* temporal anti-aliasing: the frames' Motion blocks (a draw's at its TnlConst index), host-written */
        DBuf mb = dbuf_new((VkDeviceSize)FRAMES * TNL_MAX * sizeof(MotionConst), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
        motmap = (MotionConst *)mb.map;
        memset(motmap, 0, (size_t)FRAMES * TNL_MAX * sizeof(MotionConst));
        VkDescriptorBufferInfo bufi = {mb.buf, 0, mb.size};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 5,
                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &bufi};
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
        const char *e;
        if ((e = getenv("TD6_TAA_DEBUG")))
            taa_debug = atoi(e);
        if ((e = getenv("TD6_TAA_JITTER")))
            taa_jitter = atoi(e) != 0;
        if ((e = getenv("TD6_TAA_MATCH")))
            taa_match = atoi(e) != 0;
        if ((e = getenv("TD6_TAA_SHARPEN")))
            taa_sharp = fminf((float)atof(e), 1.0f);
        if ((e = getenv("TD6_TAA_REACTIVE")))
            taa_react = fmaxf((float)atof(e), 0.0f);
        if ((e = getenv("TD6_TAA_MIPBIAS")))
            taa_mip_add = (float)atof(e);
        taa_log = getenv("TD6_TAA_LOG") != NULL;
    }
    {   /* the instanced draws' records: the frames' blocks, host-written */
        DBuf ib = dbuf_new((VkDeviceSize)FRAMES * INST_MAX * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
        inst_map = (float *)ib.map;
        memset(inst_map, 0, (size_t)FRAMES * INST_MAX * 64);
        VkDescriptorBufferInfo bufi = {ib.buf, 0, ib.size};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 7,
                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &bufi};
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    }
    {   /* the shadows' block: ShadowGlobals per frame, then the frames' geometries */
        VkDeviceSize size = SH_SLOTS * sizeof(ShadowGlobals) + (VkDeviceSize)SH_SLOTS * SH_GEOMS * sizeof(ShadowGeom);
        sh_buf = dbuf_new(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
        memset(sh_buf.map, 0, size);
        VkDescriptorBufferInfo bufi = {sh_buf.buf, 0, size};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 3,
                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &bufi};
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    }
    {   /* the volumetric smoke's puffs: the frames' views' blocks, host-written */
        sm_buf = dbuf_new((VkDeviceSize)FRAMES * SMOKE_SLOTS * SMOKE_BLOCK * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
        memset(sm_buf.map, 0, (size_t)sm_buf.size);
        VkDescriptorBufferInfo bufi = {sm_buf.buf, 0, sm_buf.size};
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 6,
                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &bufi};
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
        const char *e;
        if ((e = getenv("TD6_SMOKE_DEBUG")))
            sm_debug = atoi(e);
        if ((e = getenv("TD6_SMOKE_STEPS")))
            sm_steps = atoi(e);
        if ((e = getenv("TD6_SMOKE_BUDGET")) && atof(e) > 0)
            sm_budget = (float)atof(e);
        if ((e = getenv("TD6_SMOKE_DENSITY")) && atof(e) > 0)
            sm_density = (float)atof(e);
        sm_log = getenv("TD6_SMOKE_LOG") != NULL;
        if ((e = getenv("TD6_SMOKE_GLASS")))
            sm_glass = atoi(e) != 0;
        if ((e = getenv("TD6_SMOKE_SHADOWS")))
            sm_shadows = atoi(e) != 0;
        if ((e = getenv("TD6_SMOKE_CLIP")))
            sm_clip = atoi(e) != 0;
        if ((e = getenv("TD6_SMOKE_WIDEN")) && atof(e) > 0)
            sm_widen = (float)atof(e);
        if ((e = getenv("TD6_SMOKE_SCALE")) && atoi(e) > 0)
            sm_scale = atoi(e);
    }
    if (rt_ok)
        rt_init();
    free_slots = malloc(ntex_slots * sizeof *free_slots);
    if (!free_slots)
        fatal("out of memory");
    for (uint32_t i = 0; i < ntex_slots; i++)
        free_slots[i] = (int)(ntex_slots - 1 - i);
    nfree_slots = (int)ntex_slots;
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushDraw)};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VKC(vkCreatePipelineLayout(dev, &pli, NULL, &playout));
    sm_dv = shader(spv_draw_vert, sizeof spv_draw_vert);
    sm_tv = shader(spv_tnl_vert, sizeof spv_tnl_vert);
    sm_df = shader(spv_draw_frag, sizeof spv_draw_frag);
    sm_dfs = shader(spv_draw_frag_shadow, sizeof spv_draw_frag_shadow);
    sm_dfr = shader(spv_draw_frag_refl, sizeof spv_draw_frag_refl);
    sm_tvm = shader(spv_tnl_vert_motion, sizeof spv_tnl_vert_motion);
    sm_dvm = shader(spv_draw_vert_motion, sizeof spv_draw_vert_motion);
    sm_dfm = shader(spv_draw_frag_motion, sizeof spv_draw_frag_motion);
    sm_dfsm = shader(spv_draw_frag_shadow_motion, sizeof spv_draw_frag_shadow_motion);
    sm_dfrm = shader(spv_draw_frag_refl_motion, sizeof spv_draw_frag_refl_motion);
    sm_taa = shader(spv_taa_frag, sizeof spv_taa_frag);
    sm_taau = shader(spv_taau_frag, sizeof spv_taau_frag);
    sm_pv = shader(spv_present_vert, sizeof spv_present_vert);
    sm_pf = shader(spv_present_frag, sizeof spv_present_frag);
    sm_smv = shader(spv_smoke_vert, sizeof spv_smoke_vert);
    sm_smf = shader(spv_smoke_frag, sizeof spv_smoke_frag);
    sm_smfm = shader(spv_smoke_frag_motion, sizeof spv_smoke_frag_motion);
    if (rt_ok) {
        sm_rf = shader(spv_resolve_frag, sizeof spv_resolve_frag);
        sm_ao = shader(spv_ao_frag, sizeof spv_ao_frag);
        sm_smvis = shader(spv_smokevis_frag, sizeof spv_smokevis_frag);
    }
    smp_linear = sampler_get(2, 2, 1, 3, 3, 0, 0, 0, 1, 0);
    smp_nearest = sampler_get(1, 1, 1, 3, 3, 0, 0, 0, 1, 0);
    vkr_options();

    for (int i = 0; i < FRAMES; i++) {
        Frame *f = &fr[i];
        VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam};
        VKC(vkCreateCommandPool(dev, &cpi, NULL, &f->pool));
        VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                           .commandPool = f->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                           .commandBufferCount = 1};
        VKC(vkAllocateCommandBuffers(dev, &cai, &f->cb));
        VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VKC(vkCreateFence(dev, &fci, NULL, &f->fence));
        VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKC(vkCreateSemaphore(dev, &sci, NULL, &f->acquired));
    }

    const char *d = getenv("TD6_DUMP_FRAMES");
    if (d && *d) {
        snprintf(dumpdir, sizeof dumpdir, "%s", d);
        mkdir(dumpdir, 0755);
        const char *e = getenv("TD6_DUMP_EVERY");
        if (e && atol(e) > 0)
            dump_every = (unsigned long)atol(e);
        const char *p = getenv("TD6_DUMP_PRESENT");
        dump_present = p && *p && strcmp(p, "0");
        const char *from = getenv("TD6_DUMP_FROM");     /* only from that frame on */
        dump_from = from ? (unsigned long)atol(from) : 0;
        dumping = 1;
    }
    if (getenv("TD6_HUD_LOG")) hud_log_frame = atol(getenv("TD6_HUD_LOG"));
    const char *stv = getenv("TD6_VK_STATS");
    st_print = stv && *stv && strcmp(stv, "0") ? (atoi(stv) > 1 ? 2 : 1) : 0;
    if (st_tick_ns > 0) {         /* (always: RENDER SCALE AUTO reads the frames' GPU time) */
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = TS * FRAMES};
        VKC(vkCreateQueryPool(dev, &qi, NULL, &st_queries));
    }
    st_t0 = wall_ms();
    state = 1;                    /* from here on the helpers may record commands */
    frame_begin();
    gamma_img = img_new(K_TEX, 256, 1, 1, VK_FORMAT_R16G16B16A16_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (offscreen) {
        uint32_t w = 1280, h = 720;
        const char *sz = getenv("TD6_GPU_OFFSCREEN_SIZE");
        if (sz && sscanf(sz, "%ux%u", &w, &h) != 2)
            w = 1280, h = 720;
        out_img = img_new(K_RT, w, h, 1, RT_FORMAT,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
    logmsg(LOG_ALWAYS, "Vulkan: %s (%s), depth format %d, %u texture slots, ray queries %s, FSR 2 %s, DLSS %s, "
           "16-bit colour %s", props.deviceName, offscreen ? "offscreen" : "window", depth24, ntex_slots,
           rt_ok ? "yes" : "no", taa_ok ? "yes" : "no", dlss_ok ? "yes" : "no", hp_format == HP_FORMAT ? "yes" :
           hp_format == VK_FORMAT_R16G16B16A16_SFLOAT ? "half float" : hp_format ? "10-bit" : "no");
    return 1;
}

int vk_on(void)
{
    if (!state) {
        state = -1;
        if (init())
            state = 1;
        else
            state = -1;
    }
    return state > 0;
}

int vk_target(const Surface *s)
{
    return s && !s->dead && (s->caps & DDSCAPS_3DDEVICE) && !(s->caps & (DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER)) &&
           vk_on();
}

/* ------------------------------------------------------------------ Direct3D */
static int expand(int prim, int n, const uint16_t *idx, int nv, uint32_t *out)
{
    int k = 0;
#define V(i) (idx ? (uint32_t)idx[i] : (uint32_t)(i))
#define OK(i) (V(i) < (uint32_t)nv)
    switch (prim) {
    case PT_TRIANGLELIST:
        for (int i = 0; i + 2 < n; i += 3)
            if (OK(i) && OK(i + 1) && OK(i + 2)) {
                out[k++] = V(i);
                out[k++] = V(i + 1);
                out[k++] = V(i + 2);
            }
        break;
    case PT_TRIANGLESTRIP:            /* odd triangles turned, first vertex kept for flat shading */
        for (int i = 0; i + 2 < n; i++)
            if (OK(i) && OK(i + 1) && OK(i + 2)) {
                out[k++] = V(i);
                out[k++] = V(i + 1 + (i & 1));
                out[k++] = V(i + 2 - (i & 1));
            }
        break;
    case PT_TRIANGLEFAN:              /* same winding, first vertex = D3D's (the second of the fan) */
        for (int i = 1; i + 1 < n; i++)
            if (OK(0) && OK(i) && OK(i + 1)) {
                out[k++] = V(i);
                out[k++] = V(i + 1);
                out[k++] = V(0);
            }
        break;
    case PT_LINELIST:
        for (int i = 0; i + 1 < n; i += 2)
            if (OK(i) && OK(i + 1)) {
                out[k++] = V(i);
                out[k++] = V(i + 1);
            }
        break;
    case PT_LINESTRIP:
        for (int i = 0; i + 1 < n; i++)
            if (OK(i) && OK(i + 1)) {
                out[k++] = V(i);
                out[k++] = V(i + 1);
            }
        break;
    case PT_POINTLIST:
        for (int i = 0; i < n; i++)
            if (OK(i))
                out[k++] = V(i);
        break;
    }
#undef V
#undef OK
    return k;
}

static uint32_t stage_word(uint32_t op, uint32_t a1, uint32_t a2)
{
    return (op & 0xff) | (a1 & 0xff) << 8 | (a2 & 0xff) << 16;
}

static uint32_t key_rgb(Surface *t, int colorkey)
{
    if (!colorkey || !t || !(t->ck_set & DDCKEY_SRCBLT))
        return 0;
    return 0x1000000u | (pix_decode(t, t->ck[2][0]) & 0xffffff);
}

static float zbias_units(const GpuImg *z, uint32_t zbias)
{
    if (!z || !zbias)
        return 0;
    return -(float)zbias * (z->format == VK_FORMAT_D16_UNORM ? 1.0f : 256.0f);
}

static void draw_impl(const D3DState *st, int prim, const SwVertex *v, int nv, const uint16_t *idx, int ni,
                      int pretransformed, const void *raw, const TnlConst *k);
/* temporal anti-aliasing (its section below) */
static int scene_target(GpuImg **c, GpuImg **z, const D3DState *st);
static void scene_motion(const D3DState *st, PushDraw *pc, const TnlConst *k, const void *raw, int nv, const uint16_t *idx,
                         int ni, int pretransformed, const float *pl0, int blend, uint32_t sbf, uint32_t dbf, int zwrite,
                         Surface *t0, const int *sc);
void vkr_draw(const D3DState *st, int prim, const SwVertex *v, int nv, const uint16_t *idx, int ni,
             int pretransformed)
{
    double t = wall_ms();
    draw_impl(st, prim, v, nv, idx, ni, pretransformed, NULL, NULL);
    st_draw_ms += wall_ms() - t;
}

/* Name labels in widescreen. The game draws the frame's names over the cars together, in one draw
   of a quad per letter; each name (a run of letters side by side in a row) moves from its place in
   the 4:3 picture to the widened view's, keeping its shape: out[i] the shift of vertex i (GPU
   pixels). 0 if the draw is not quads like that (one shift for the draw then). */
static int ws_runs(const D3DState *st, const SwVertex *v, int nv, int pretransformed, float k, const GpuImg *c,
                   float *out)
{
    if (nv < 8 || nv % 4 || nv > 4096)
        return 0;
    float vx = (float)st->vp.x, vw = (float)st->vp.w, vy = (float)st->vp.y, vh = (float)st->vp.h;
    float vc = vx + vw / 2.0f;
    float rx0 = 0, rx1 = 0, ry = 0, rh = 0;
    int start = 0, runs = 0;
    for (int q = 0; q <= nv / 4; q++) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i = 4 * q; i < 4 * q + 4 && q < nv / 4; i++) {
            float x = v[i].x, y = v[i].y;
            if (!pretransformed) {
                if (!(v[i].w > 0))
                    return 0;
                x = vc + v[i].x / v[i].w * vw / 2.0f * k;
                y = vy + (1.0f - v[i].y / v[i].w) * vh / 2.0f;
            }
            x0 = x < x0 ? x : x0;
            x1 = x > x1 ? x : x1;
            y0 = y < y0 ? y : y0;
            y1 = y > y1 ? y : y1;
        }
        float h = y1 - y0, yc = (y0 + y1) * 0.5f;
        if (q < nv / 4 && (!(h > 0.5f) || x1 - x0 > 3.0f * h))
            return 0;                 /* not a letter */
        /* the letter after a run's last in its row, at most a letter's height on: the same name */
        if (q < nv / 4 && q > start && fabsf(yc - ry) < 0.25f * rh && x0 >= rx1 - 0.5f * rh && x0 <= rx1 + rh) {
            rx1 = x1 > rx1 ? x1 : rx1;
            continue;
        }
        if (q > start) {              /* the run of quads start .. q - 1 */
            float cx = (rx0 + rx1) * 0.5f, s = cx * c->sx - (cx * c->sy + c->ox);
            for (int i = 4 * start; i < 4 * q; i++)
                out[i] = s;
            runs++;
        }
        start = q;
        rx0 = x0;
        rx1 = x1;
        ry = yc;
        rh = h;
    }
    return runs > 0;
}

/* A camera the game built widened (widescreen.c): its projection's y/x scale is the wide aspect */
static int proj_widened(const D3DState *st)
{
    const float *P = &st->xf[XF_PROJ].m[0][0];
    return P[0] != 0.0f && ws_world_aspect > 1.34f && fabsf(P[5] / P[0] - ws_world_aspect) < 0.01f;
}

/* (vk_tnl_ok is vkthread.c's: it is the game side's question, with the game side's state) */

void vkr_draw_tnl(const D3DState *st, int prim, const void *raw, int nv, const uint16_t *idx, int ni,
                 const TnlConst *k)
{
    double t = wall_ms();
    draw_impl(st, prim, NULL, nv, idx, ni, 0, raw, k);
    st_draw_ms += wall_ms() - t;
}

/* The same mesh n times (drawdist.c's map copies): k with the identity as its world matrix, each
   copy's world matrix, sun and fade in recs (16 floats each, as tnl.vert reads them). Not ray-traced
   casters nor reflective (they are past the shadows' and reflections' reach), static to the motion
   vectors. */
void vkr_draw_tnl_inst(const D3DState *st, int prim, const void *raw, int nv, const uint16_t *idx, int ni,
                       const TnlConst *k, const float *recs, int n)
{
    if (!vk_on() || n <= 0)
        return;
    frame_begin();
    Frame *f = &fr[fi];
    if (f->ninst + (uint32_t)n > INST_MAX)
        return;
    struct InstBind b = {(uint32_t)INST_MAX * (uint32_t)fi + f->ninst, (uint32_t)n};
    memcpy(inst_map + (size_t)b.base * 16, recs, (size_t)n * 64);
    f->ninst += (uint32_t)n;
    double t = wall_ms();
    inst_bind = &b;
    draw_impl(st, prim, NULL, nv, idx, ni, 0, raw, k);
    inst_bind = NULL;
    st_draw_ms += wall_ms() - t;
}

/* A texture's mean colour (a 32 x 32 grid of its texels) */
static void tex_mean(const Surface *s, float *out)
{
    double a[3] = {0, 0, 0};
    int n = 0;
    const uint8_t *snap = vk_snap_pixels(s, NULL);
    for (uint32_t j = 0; j < 32 && s->width && s->height; j++)
        for (uint32_t i = 0; i < 32; i++) {
            uint32_t v = pix_decode(s, snap_read(s, snap, (2 * i + 1) * s->width / 64, (2 * j + 1) * s->height / 64));
            a[0] += (v >> 16) & 255;
            a[1] += (v >> 8) & 255;
            a[2] += v & 255;
            n++;
        }
    for (int c = 0; c < 3; c++)
        out[c] = n ? (float)(a[c] / n / 255.0) : 0.5f;
}

/* Ray-traced reflections, at a draw of a view (vk.c draw_impl, GPU transform and lighting): the
   terrain's colour pass says which texture is which tile of the colour map; the sky dome's draws
   are what rays meeting nothing show; a reflective draw (the game's reflection pass over cars and
   objects: SRCALPHA / ONE with z bias and no depth writes, its texture the map's env map; the
   water's colour pass, its texture the sky's) is drawn now without its texture's colour (pc's
   shadow.w bit 16) and recorded to be drawn again after the view with what rays meet */
static void refl_gather(const D3DState *st, const TnlConst *k, const void *raw, int nv, PushDraw *pc, uint32_t sbf,
                        uint32_t dbf, int blend, int zwrite, int ztest, GpuImg *z, int water, GpuImg *t0, const int *sc,
                        uint32_t cull, VkBuffer vbuf, VkBuffer ibuf, VkDeviceSize voff, VkDeviceSize ioff,
                        VkDeviceAddress vaddr, VkDeviceAddress iaddr, uint32_t nidx, uint32_t first, int32_t vbase)
{
    const uint32_t *r = st->rs;
    struct ShFrame *s = &shf[sv.slot];
    if (sv.cls == VK_SH_TERRAIN && raw && nv > 0 && t0) {
        /* the colour pass (grey vertices, the tile's texture coordinates 0..1; the detail pass has
           black ones, a repeat per cell): u = (x * 3.984375 + 0.5) / 256 for x = 0..64 cells into
           the tile, v the same with z */
        float p[3], tc[2];
        uint32_t dc;
        memcpy(p, raw, 12);
        memcpy(&dc, (const uint8_t *)raw + 24, 4);
        memcpy(tc, (const uint8_t *)raw + 32, 8);
        if ((dc & 0xffffffu) && tc[0] >= 0.0f && tc[0] <= 1.01f && tc[1] >= 0.0f && tc[1] <= 1.01f) {
            double cx = (k->world[0][0] * p[0] + k->world[0][1] * p[1] + k->world[0][2] * p[2] + k->world[0][3] +
                         sv.v.cam[0]) / 256.0,
                   cz = (k->world[2][0] * p[0] + k->world[2][1] * p[1] + k->world[2][2] * p[2] + k->world[2][3] +
                         sv.v.cam[2]) / 256.0;
            int tx = (int)lround((cx - (tc[0] * 256.0 - 0.5) / 3.984375) / 64.0) & 3,
                tz = (int)lround((cz - (tc[1] * 256.0 - 0.5) / 3.984375) / 64.0) & 3;
            if (tile_img[tz * 4 + tx] != t0) {
                tile_img[tz * 4 + tx] = t0;
                tex_mean(st->tex[0], tile_avg[tz * 4 + tx]);
            }
            tile_smp = pc->texsmp[2];
            tile_grey = (float)((dc >> 16) & 255) / 255.0f;
        }
        return;
    }
    if (sv.cls == VK_SH_SKY && nidx >= 3 && s->nsky < SH_SKY) {
        uint32_t i = s->nsky++, g = SKY_BASE + i;
        memcpy(s->xform.map + 48 * g, k->world, 48);
        ShadowGeom *sg = (ShadowGeom *)(sh_buf.map + SH_SLOTS * sizeof(ShadowGlobals)) + (size_t)sv.slot * SH_GEOMS + g;
        *sg = (ShadowGeom){vaddr, iaddr, pc->texsmp[0], pc->texsmp[2], 0.5f, 0, pc->tnl[0], 0};
        s->sky_geo[i] = (VkAccelerationStructureGeometryKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
            .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                   .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress = vaddr,
                                   .vertexStride = 40, .maxVertex = (uint32_t)nv - 1,
                                   .indexType = VK_INDEX_TYPE_UINT32, .indexData.deviceAddress = iaddr,
                                   .transformData.deviceAddress = s->xform.addr + 48 * g}};
        s->sky_range[i] = (VkAccelerationStructureBuildRangeInfoKHR){.primitiveCount = nidx / 3};
        s->sky_maxprim[i] = nidx / 3;
        /* (its places round the camera to a quarter unit: unchanged, the structures stay) */
        uint64_t h = s->sky_hash ^ ((uint64_t)nv << 32 | nidx);
        for (int j = 0; j < nv && raw; j++) {
            float q[3];
            memcpy(q, (const uint8_t *)raw + 40 * (size_t)j, 12);
            for (int c = 0; c < 3; c++) {
                float w = k->world[c][0] * q[0] + k->world[c][1] * q[1] + k->world[c][2] * q[2] + k->world[c][3];
                h = (h ^ (uint64_t)(int64_t)lroundf(w * 4.0f)) * 1099511628211ull;
            }
        }
        s->sky_hash = h * 1099511628211ull;
        return;
    }
    int env = (sv.cls == VK_SH_OBJECT || sv.cls == VK_SH_PARTS) && blend && sbf == 5 && dbf == 2 && !zwrite &&
              r[RS_ZBIAS] > 0;
    if (!(env || water) || pc->texsmp[0] < 0 || nrefl >= REFL_MAX)
        return;
    pc->shadow[3] |= 1u << 16;
    /* its rays' rate (log2 of how much smaller their image is each way; draw.frag RT_REFLECT): FULL,
       glass and chrome (strength from 0.5: their vertices' alpha) a ray a pixel, paint at a half, the
       water at a quarter; LIGHT each one step smaller and nothing at full size; 4X anti-aliasing
       draws at twice the size: FULL's glass and chrome at a half there too */
    uint32_t a0 = 255;
    if (raw && nv > 0) {
        memcpy(&a0, (const uint8_t *)raw + 24, 4);
        a0 >>= 24;
    }
    uint32_t ls = water ? 2 : a0 >= 128 ? (res_aa > 1.5f || refl_half ? 1 : 0) : 1;
    if (sv.v.refl == 1)
        ls = refl_lrate[water ? 2 : a0 >= 128 ? 0 : 1];
    ReflDraw *d = &refl_draws[nrefl++];
    d->pc = *pc;
    d->pc.shadow[3] = 1u << 17 | (sbf == 5 ? 1u << 18 : 0) | (env ? 1u : 2u) << 19 | ls << 23 |
                      (sv.v.refl == 1 ? 1u << 25 : 0);
    d->vbuf = vbuf;
    d->ibuf = ibuf;
    d->voff = voff;
    d->ioff = ioff;
    d->nidx = nidx;
    d->first = first;
    d->vbase = vbase;
    memcpy(d->sc, sc, sizeof d->sc);
    d->topo = 0;
    d->ztest = ztest;
    d->cull = cull == 2 ? VK_CULL_MODE_FRONT_BIT : cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    d->zf = (VkCompareOp)(r[RS_ZFUNC] >= 1 && r[RS_ZFUNC] <= 8 ? r[RS_ZFUNC] - 1 : VK_COMPARE_OP_LESS_OR_EQUAL);
    d->bias = ztest ? zbias_units(z, r[RS_ZBIAS]) : 0;
    d->zfmt = z ? z->format : VK_FORMAT_UNDEFINED;
}

/* Whether a draw's vertices span the whole viewport (to within a pixel) */
static int covers_view(const D3DState *st, const SwVertex *v, int nv, int pretransformed)
{
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < nv; i++) {
        float x = v[i].x, y = v[i].y;
        if (!pretransformed) {
            if (!(v[i].w > 0))
                return 0;
            x = st->vp.x + (x / v[i].w + 1) * st->vp.w / 2;
            y = st->vp.y + (1 - y / v[i].w) * st->vp.h / 2;
        }
        x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1;
        y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
    }
    return x0 <= st->vp.x + 1 && x1 >= st->vp.x + st->vp.w - 1 && y0 <= st->vp.y + 1 && y1 >= st->vp.y + st->vp.h - 1;
}

static void draw_impl(const D3DState *st, int prim, const SwVertex *v, int nv, const uint16_t *idx, int ni,
                      int pretransformed, const void *raw, const TnlConst *k)
{
    static int warned_stage2;
    Surface *rs = st->rt;
    if (!vk_on() || nv <= 0 || !rs || rs->dead || !rs->mem)
        return;
    frame_begin();
    GpuImg *c = rt_sync(rs);
    GpuImg *z = depth_sync(st->zb, c);
    int in_scene = scn.active && scene_target(&c, &z, st);     /* temporal anti-aliasing: the scene's images */
    int in_fsr = in_scene && scn.fsr;                          /* (the 16-bit scene alone: textures as they were) */
    samp_bias = in_fsr ? taa_mip : 0.0f;
    samp_scene = in_fsr;
    if (sv.active && !sv.c && k) {    /* ray-traced shadows: the view's first draw says where it goes */
        sv.c = c;
        sv.z = z;
    }
    const uint32_t *r = st->rs;
    PushDraw pc;
    memset(&pc, 0, sizeof pc);
    int nst = st->tss[0][TSS_COLOROP] > 1 ? (st->tss[1][TSS_COLOROP] > 1 ? 2 : 1) : 0;
    if (nst == 2 && st->tss[2][TSS_COLOROP] > 1 && !warned_stage2++)
        LOG(LOG_ALWAYS, "vk: texture stages beyond 2 are ignored");
    int tci[2] = {0, 1};
    for (int s = 0; s < 2; s++) {
        const uint32_t *t = st->tss[s];
        pc.stage[2 * s] = s < nst ? stage_word(t[TSS_COLOROP], t[TSS_COLORARG1], t[TSS_COLORARG2]) : 1;
        pc.stage[2 * s + 1] = s < nst ? stage_word(t[TSS_ALPHAOP], t[TSS_ALPHAARG1], t[TSS_ALPHAARG2]) : 1;
        pc.texsmp[s] = -1;
        tci[s] = (int)(t[TSS_TEXCOORDINDEX] & 7);
        if (s < nst && st->tex[s]) {
            GpuImg *g = __atomic_load_n(&st->tex[s]->font_letters, __ATOMIC_ACQUIRE) ? font_sync() : NULL;
            if (g) {                            /* the letters drawn from the chosen font: trilinear */
                pc.texsmp[s] = g->slot;
                pc.texsmp[2 + s] = sampler_get(2, 2, 3, t[TSS_ADDRESSU], t[TSS_ADDRESSV], border_class(t[TSS_BORDERCOLOR]),
                                               -0.5f, 0, g->levels, 0);
            } else if ((g = tex_sync(st->tex[s]))) {
                pc.texsmp[s] = g->slot;
                pc.texsmp[2 + s] = stage_sampler(t, in_fsr || !g->made_mips ? g->levels : g->game_levels);
                pc.misc[s] = key_rgb(st->tex[s], r[RS_COLORKEYENABLE] != 0);
            }
        }
    }
    pc.misc[2] = r[RS_TEXTUREFACTOR];
    pc.misc[3] = r[RS_FOGCOLOR];
    memcpy(&pc.fog[0], &r[RS_FOGSTART], 4);
    memcpy(&pc.fog[1], &r[RS_FOGEND], 4);
    memcpy(&pc.fog[2], &r[RS_FOGDENSITY], 4);
    pc.fog[3] = (float)(r[RS_ALPHAREF] & 0xff);
    uint32_t fl = 0;
    if (r[RS_ALPHATESTENABLE] && r[RS_ALPHAFUNC] >= 1 && r[RS_ALPHAFUNC] <= 8)
        fl |= r[RS_ALPHAFUNC];
    if (r[RS_SPECULARENABLE])
        fl |= 16;
    if (r[RS_SHADEMODE] == 1)
        fl |= 32;
    if (r[RS_FOGENABLE]) {
        if (r[RS_FOGTABLEMODE] >= 1 && r[RS_FOGTABLEMODE] <= 3)
            fl |= 2u << 6 | r[RS_FOGTABLEMODE] << 8;
        else
            fl |= 1u << 6;
    }
    if (vk_fog_end > 0 && (fl >> 6 & 3)) {    /* drawdist.c: full fog at the view's edge */
        fl |= 1u << 10;
        memcpy(&pc.flags[2], &vk_fog_end, 4);
        memcpy(&pc.flags[3], &vk_fog_start, 4);
    }
    pc.flags[0] = fl;
    pc.flags[1] = inst_bind ? 0 : vk_fade;   /* (instanced: each copy's own, from tnl.vert) */
    if (inst_bind) {
        pc.inst[0] = inst_bind->base;
        pc.inst[1] = 1;
    }
    static int fog_log = -1;      /* (read once: getenv per draw cost 2 ms a frame on Windows) */
    if (fog_log < 0)
        fog_log = getenv("TD6_FOG_LOG") != NULL;
    if (fog_log && nframes % 200 == 0 && r[RS_FOGENABLE]) {
        static unsigned long last;
        if (last != nframes) {
            last = nframes;
            logmsg(LOG_ALWAYS, "fog: table %u vertex %u start %g end %g density %g colour %08x", r[RS_FOGTABLEMODE],
                   r[RS_FOGVERTEXMODE], pc.fog[0], pc.fog[1], pc.fog[2], r[RS_FOGCOLOR]);
        }
    }

    /* vertices: D3D clip space or screen space -> Vulkan clip space with the half-pixel offset */
    VkBuffer vbuf, ibuf;
    uint8_t *vp = NULL, *ip;
    VkDeviceSize voff, ioff;
    VkDeviceAddress vaddr = 0, iaddr = 0;
    uint32_t nidx, first = 0;
    int32_t vbase = 0;
    if (far_bind) {                               /* the far terrain's mesh (vk_far_draw) */
        vbuf = ibuf = far_mesh.buf;
        voff = 0;
        ioff = far_ioff;
        nidx = far_bind->count;
        first = far_bind->first;
        vbase = far_bind->vbase;
    } else {
        voff = ring((VkDeviceSize)nv * (k ? 40 : sizeof(GV)), 16, &vbuf, &vp);
        vaddr = ring_addr + voff;
        int n = idx ? ni : nv;
        ioff = ring((VkDeviceSize)(3 * n + 6) * 4, 16, &ibuf, &ip);
        iaddr = ring_addr + ioff;
        nidx = (uint32_t)expand(prim, n, idx, nv, (uint32_t *)ip);
        if (!nidx)
            return;
    }
    /* Horizontal placement (GPU pixels). Normally the program's picture fills the image. With
       widescreen: a camera the game built widened (widescreen.c) spreads its viewport by the
       widening around the viewport's centre; anything else keeps its 4:3 shape in the middle. */
    float H = (float)c->lh, PW = (float)c->w;
    float vx = (float)st->vp.x, vw = (float)st->vp.w;
    float mid = ui_x(c, vx + vw / 2.0f), half = vw * c->sy / 2.0f, hp = 0.5f * c->sy;
    int place = 0;                                /* 0 centred, 1 widened camera, 2 background */
    if (c->ox > 0.5f) {
        if (!pretransformed && proj_widened(st))
            place = 1;
        else if (bg_phase && nv == 4 && st->tex[0] && (!r[RS_ZENABLE] || covers_view(st, v, nv, pretransformed)))
            place = 2;                            /* a background tile at the start of the frame (TD6's
                                                     menus: one quad over the whole view, depth on) */
        if (place != 2)
            bg_phase = 0;
    }
    if (place == 1)
        half *= ws_aspect / (4.0f / 3.0f);
    else if (place == 2) {
        mid = (vx + vw / 2.0f) * c->sx;
        half = vw * c->sx / 2.0f;
        hp = 0.5f * c->sx;
    }
    /* Race HUD in widescreen: a piece drawn by the HUD routines moves to the screen edge it is
       nearest in the 4:3 picture (left or right half; one across the middle stays centred). Other
       flat drawings of a race frame (name labels over the cars) keep their shape but take their
       position from the widened view. Menus, including the pause menu, keep their layout. */
    float shift = 0.0f;
    static float vshift[4096];    /* (name labels: each name's own shift, by vertex) */
    int by_vertex = 0;
    if (!k && c->ox > 0.5f && place != 2 && ws_race_depth > 0 && ws_menu_depth == 0 && (ws_hud_depth > 0 || place == 0)) {
        float x0 = 1e9f, x1 = -1e9f, k = place == 1 ? ws_aspect / (4.0f / 3.0f) : 1.0f, vc = vx + vw / 2.0f;
        for (int i = 0; i < nv; i++) {
            float x = v[i].x;
            if (!pretransformed) {
                if (!(v[i].w > 0)) continue;
                x = vc + v[i].x / v[i].w * vw / 2.0f * k;       /* where it would be in the 4:3 picture */
            }
            x0 = x < x0 ? x : x0;
            x1 = x > x1 ? x : x1;
        }
        if (x0 <= x1) {
            float half_w = (float)c->lw * 0.5f;
            if (ws_hud_depth > 0)     /* (TD6: the middle 30 % stays, as its time and countdown digits) */
                shift = x1 <= 0.7f * half_w ? -c->ox : x0 >= 1.3f * half_w ? c->ox : 0.0f;
            else if ((by_vertex = ws_runs(st, v, nv, pretransformed, k, c, vshift)) == 0) {
                float cx = (x0 + x1) * 0.5f;
                shift = cx * c->sx - (cx * c->sy + c->ox);
            }
        }
        mid += shift;
    }
    /* Front end in widescreen (inside 0x41d100, ws_menu_depth): its frame - the top banner and
       its title, the bottom bar with the rev counter - reaches the screen's sides. In 640 x 480
       units, pieces left of 160 move with the left edge, right of 480 with the right one, and the
       part between is stretched to join them: wide pieces (the bar's tiles) by vertex; small ones
       keep their shape, moved with their side (the needle) or left in place in the middle (the
       title's letters). Everything else - the menus, panels,
       previews - keeps its 4:3 place in the middle. */
    if (!k && c->ox > 0.5f && place == 0 && !pretransformed && ws_menu_depth > 0 && ws_race_depth == 0 && nv <= 4096) {
        float vc = vx + vw / 2.0f, vy = (float)st->vp.y, vh = (float)st->vp.h;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        int ok = 1;
        for (int i = 0; i < nv && ok; i++) {
            if (!(v[i].w > 0)) { ok = 0; break; }
            float x = vc + v[i].x / v[i].w * vw / 2.0f, y = vy + (1.0f - v[i].y / v[i].w) * vh / 2.0f;
            x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1;
            y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
        }
        /* (the bar's own tiles reach its bottom, y 478; what is drawn over its end tiles - the left
           one, from y 350, holds the rev counter, whose needle need not reach the bottom - goes with
           them: the needle stays on its dial at every angle) */
        const float L = 160.0f, R = 480.0f, cx = (x0 + x1) * 0.5f;
        if (ok && x0 <= x1 && (y1 <= 80.5f || y1 >= 476.0f || (cx < L && y0 >= 350.0f) || (cx > R && y0 >= 430.0f))) {
            const float e = c->ox / c->sy;
#define FRONT_X(x) ((x) < L ? (x) - e : (x) > R ? (x) + e : L - e + ((x) - L) * (R - L + 2.0f * e) / (R - L))
            if (x1 - x0 >= 150.0f) {
                for (int i = 0; i < nv; i++) {
                    float x = vc + v[i].x / v[i].w * vw / 2.0f;
                    vshift[i] = (FRONT_X(x) - x) * c->sy;
                }
                by_vertex = 1;
            } else {
                shift = cx < L ? -c->ox : cx > R ? c->ox : 0.0f;
                mid += shift;
            }
#undef FRONT_X
        }
    }
    if (!k && hud_log_frame && nframes == (unsigned long)hud_log_frame) {     /* TD6_HUD_LOG=<frame> */
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i = 0; i < nv; i++) {
            float x = pretransformed ? v[i].x : v[i].x / (v[i].w ? v[i].w : 1), y = pretransformed ? v[i].y : v[i].y / (v[i].w ? v[i].w : 1);
            x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1; y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
        }
        if (!pretransformed) {      /* clip space -> the viewport's pixels */
            x0 = st->vp.x + (x0 + 1) * st->vp.w / 2; x1 = st->vp.x + (x1 + 1) * st->vp.w / 2;
            float a = st->vp.y + (1 - y1) * st->vp.h / 2, b = st->vp.y + (1 - y0) * st->vp.h / 2;
            y0 = a; y1 = b;
        }
        const float *P = &st->xf[XF_PROJ].m[0][0];
        char chain[256] = "";
        {
            void *bt[64];
            int nb = backtrace(bt, 64), k = 0;
            for (int i = 0; i < nb && k < 200; i++) {
                Dl_info di;
                if (dladdr(bt[i], &di) && di.dli_sname && di.dli_sname[0] == 'F' && di.dli_sname[1] == '_')
                    k += snprintf(chain + k, sizeof chain - (size_t)k, " %s", di.dli_sname + 4);
            }
        }
        logmsg(LOG_ALWAYS, "hud:%s |", chain);
        logmsg(LOG_ALWAYS, "hud: %s place %d nv %d x %.1f..%.1f y %.1f..%.1f vp %d,%d %dx%d tex %ux%u z %u P %g %g %g %g",
               pretransformed ? "2D" : "3D", place, nv, x0, x1, y0, y1, st->vp.x, st->vp.y, st->vp.w, st->vp.h,
               st->tex[0] ? st->tex[0]->width : 0, st->tex[0] ? st->tex[0]->height : 0, r[RS_ZENABLE], P[0], P[5], P[10], P[15]);
    }
    float ax = 2.0f * half / PW, bx = 2.0f * (mid + hp) / PW - 1.0f;
    float ay = -(float)st->vp.h / H, by = (2.0f * (float)st->vp.y + 1.0f + (float)st->vp.h) / H - 1.0f;
    float mnz = st->vp.minz, dz = st->vp.maxz - st->vp.minz;
    const float pl0[6] = {ax, bx, ay, by, mnz, dz};     /* (without the scene's jitter: its motion vectors) */
    if (in_scene) {               /* temporal anti-aliasing: the scene drawn moved by the frame's jitter */
        bx += scn.jndc[0];
        by += scn.jndc[1];
    }
    if (k) {
        /* GPU transform and lighting: the raw vertices, the constants with the clip-space mapping */
        Frame *f = &fr[fi];
        TnlConst *kc = &f->tnl[f->ntnl];
        memcpy(kc, k, sizeof *kc);
        kc->place[0] = ax;
        kc->place[1] = bx;
        kc->place[2] = ay;
        kc->place[3] = by;
        kc->place2[0] = mnz;
        kc->place2[1] = dz;
        pc.tnl[0] = (uint32_t)TNL_MAX * (uint32_t)fi + f->ntnl++;
        pc.tnl[1] = 0;
        pc.tnl[2] = (uint32_t)tci[0];
        pc.tnl[3] = (uint32_t)tci[1];
        if (!far_bind)
            memcpy(vp, raw, (size_t)nv * 40);
    }
    GV *gv = (GV *)vp;
    for (int i = 0; i < nv && !k; i++) {
        const SwVertex *s = &v[i];
        GV *o = &gv[i];
        if (pretransformed) {
            float w = s->w > 0 && isfinite(s->w) ? s->w : 1.0f;
            float px = place == 2 ? (s->x + 0.5f) * c->sx : ui_x(c, s->x + 0.5f) + shift + (by_vertex ? vshift[i] : 0.0f);
            o->pos[0] = (2.0f * px / PW - 1.0f) * w;
            o->pos[1] = (2.0f * (s->y + 0.5f) / H - 1.0f) * w;
            o->pos[2] = s->z * w;
            o->pos[3] = w;
            if (in_scene) {
                o->pos[0] += scn.jndc[0] * w;
                o->pos[1] += scn.jndc[1] * w;
            }
        } else {
            o->pos[0] = ax * s->x + (bx + (by_vertex ? 2.0f * vshift[i] / PW : 0.0f)) * s->w;
            o->pos[1] = ay * s->y + by * s->w;
            o->pos[2] = mnz * s->w + dz * s->z;
            o->pos[3] = s->w;
        }
        if (s->packed) {
            o->diffuse = s->rgba[0];
            o->specular = s->rgba[1];
        } else {
            o->diffuse = pack_rgba(s->diffuse);
            o->specular = pack_rgba(s->specular);
        }
        o->tc0[0] = s->tc[tci[0]][0];
        o->tc0[1] = s->tc[tci[0]][1];
        o->tc1[0] = s->tc[tci[1]][0];
        o->tc1[1] = s->tc[tci[1]][1];
    }

    want_mot = in_scene;
    begin_rendering(c, z);
    want_mot = 0;
    c->gpu_newer = 1;
    int topo = prim <= PT_POINTLIST ? 2 : prim <= PT_LINESTRIP ? 1 : 0;
    int blend = r[RS_ALPHABLENDENABLE] != 0;
    uint32_t sbf = r[RS_SRCBLEND], dbf = r[RS_DESTBLEND];
    if (sbf == 12)
        sbf = 5, dbf = 6;
    else if (sbf == 13)
        sbf = 6, dbf = 5;
    int dst_alpha = rs->bits[3] != 0;
    int sc[4] = {(int)lroundf(mid - half), (int)lroundf((float)st->vp.y * c->sy), (int)lroundf(mid + half),
                 (int)lroundf((float)(st->vp.y + st->vp.h) * c->sy)};
    if (by_vertex) {              /* (names moved out over the widened view) */
        sc[0] = 0;
        sc[2] = (int)c->w;
    }
    uint32_t cull = r[RS_CULLMODE];
    int ztest = z && r[RS_ZENABLE];
    int zwrite = ztest && r[RS_ZWRITEENABLE];
    int geo_write = zwrite;           /* attachment 2: written by the depth-writing draws (or marked) */
    if (r_mrt) {
        double t_sh = st_print ? wall_ms() : 0;
        /* ray-traced shadows: what the sun gives this draw's colour and what its surface is */
        uint32_t mode = 0, cls = 0;
        int water = 0;                /* the water surface's colour pass (draws no depth: its own class) */
        if (k && sv.cls == VK_SH_TERRAIN) {
            mode = blend && sbf == 5 && dbf == 2 ? 3 : 2;   /* the shine pass is all sun */
            cls = 1;
            uint32_t dc;
            if (sv.beams && !sv.have_tcol && raw && nv > 0 && !(blend && dbf == 2) &&
                ((dc = *(const uint32_t *)((const uint8_t *)raw + 24)) & 0xffffffu)) {
                /* headlights: the terrain's colour pass's vertex colour (its first draw's), the light
                   its colour map is drawn with */
                ShadowGlobals *G = (ShadowGlobals *)sh_buf.map + sv.slot;
                for (int j = 0; j < 3; j++)
                    G->hl2[j] = (float)(dc >> (16 - 8 * j) & 255) / 255.0f;
                sv.have_tcol = 1;
                if (hl_log && !hl_logged++)
                    LOG(LOG_ALWAYS, "vk: headlights: the terrain drawn with %.3f %.3f %.3f, objects' ambient %.3f %.3f %.3f",
                        G->hl2[0], G->hl2[1], G->hl2[2], G->hl[0], G->hl[1], G->hl[2]);
            }
        } else if (k && (sv.cls == VK_SH_OBJECT || sv.cls == VK_SH_PARTS)) {
            mode = r[RS_LIGHTING] ? 1 : 0;
            cls = 2;
            if (sv.beams && !sv.have_amb && r[RS_LIGHTING] && sv.cls == VK_SH_OBJECT) {
                /* headlights: the ambient light the lit objects take (resolve.frag's estimate of a
                   surface's own colour) */
                ShadowGlobals *G = (ShadowGlobals *)sh_buf.map + sv.slot;
                for (int j = 0; j < 3; j++)
                    G->hl[j] = (float)(r[RS_AMBIENT] >> (16 - 8 * j) & 255) / 255.0f;
                sv.have_amb = 1;
            }
        } else if (k && sv.cls == VK_SH_WATER) {
            /* the water (0x406370): a colour pass blended over the ground below, then a wave pass
               added. The colour pass is the surface rays start from (attachment 2: class 3 with its
               distance); both take a shadow as the terrain does, by water_dark of the object
               light map */
            mode = 4 | (uint32_t)lroundf(sv.v.water_dark * 255.0f) << 16;
            if (blend && sbf == 5 && dbf == 6) {
                cls = 3;
                water = 1;
            }
        }
        pc.shadow[0] = 0x100 | mode;
        pc.shadow[1] = zwrite || water ? cls : 0;
        pc.shadow[2] = (uint32_t)sv.slot;
        if (water)
            geo_write = 1;
        if (k && sv.cls == VK_SH_TERRAIN && !sv.have_cam) {     /* the view's camera, for the rays */
            const Mat4 *V = &st->xf[XF_VIEW], *P = &st->xf[XF_PROJ];
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) {
                    double a = 0;
                    for (int m = 0; m < 4; m++)
                        a += (double)V->m[i][m] * P->m[m][j];
                    sv.vp[4 * i + j] = a;
                }
            float pl[6] = {ax, bx, ay, by, mnz, dz};
            memcpy(sv.place, pl, sizeof pl);
            /* its fog as draw.frag (table fog) or tnl.vert (vertex fog) works it out */
            uint32_t fmode = 0, frange = 0;
            if (r[RS_FOGENABLE] && r[RS_FOGTABLEMODE] >= 1 && r[RS_FOGTABLEMODE] <= 3)
                fmode = r[RS_FOGTABLEMODE];
            else if (r[RS_FOGENABLE] && r[RS_FOGVERTEXMODE] >= 1 && r[RS_FOGVERTEXMODE] <= 3) {
                fmode = r[RS_FOGVERTEXMODE];
                frange = r[RS_RANGEFOGENABLE] != 0;
            }
            sv.fog = r[RS_FOGCOLOR];
            memcpy(&sv.fogp[0], &r[RS_FOGSTART], 4);
            memcpy(&sv.fogp[1], &r[RS_FOGEND], 4);
            memcpy(&sv.fogp[2], &r[RS_FOGDENSITY], 4);
            int edge = fmode && vk_fog_end > 0;
            sv.fogp[3] = (float)(fmode | frange << 2 | (uint32_t)edge << 3);
            sv.fog_edge[0] = vk_fog_end;
            sv.fog_edge[1] = vk_fog_start;
            for (int i = 0; i < 4; i++)
                sv.fwd[i] = V->m[i][2];
            sv.focal = 0.5f * fabsf(P->m[1][1]) * (float)st->vp.h * c->sy;
            sv.have_cam = 1;
            if (sv.beams)
                beams_cull(c->w, c->h);
        }
        sv.sc[0] = sc[0] < sv.sc[0] ? sc[0] : sv.sc[0];
        sv.sc[1] = sc[1] < sv.sc[1] ? sc[1] : sv.sc[1];
        sv.sc[2] = sc[2] > sv.sc[2] ? sc[2] : sv.sc[2];
        sv.sc[3] = sc[3] > sv.sc[3] ? sc[3] : sv.sc[3];
        /* the depth-writing triangles of objects and cars cast shadows, and the see-through parts
           of models blended as cut-outs (lattices, fences, glass: by alpha, their colours' too) */
        struct ShFrame *s = &shf[sv.slot];
        int parts = sv.cls == VK_SH_PARTS && ztest && !zwrite && blend && sbf == 5 && dbf == 6;
        GpuImg *t0 = st->tex[0] && st->tex[0]->gpu && st->tex[0]->gpu->kind == K_TEX ? st->tex[0]->gpu : NULL;
        /* a depth-writing draw of an object or a car, or a see-through part: it casts, and the
           texture's cut-outs (worked out from the program's memory once after each upload; only
           for textures these draws use, so a texture painted every frame for a decal, as the
           cars' shadows with SHADOWS ORIGINAL, is never read) say how */
        int caster = k && !far_bind && !inst_bind && !vk_nocast && (sv.cls == VK_SH_OBJECT || sv.cls == VK_SH_PARTS) && (zwrite || parts) &&
                     topo == 0 && nidx >= 3;
        if (t0 && !t0->cut_known && caster) {
            st_scans++;
            t0->cutout = cut_tiles(st->tex[0], &t0->cut_frac);
            t0->cut_known = 1;
        }
        {
            /* ambient occlusion: how much of it the surface takes (attachment 2's alpha): foliage, a
               draw whose texture is mostly cut out, a little, so trees and bushes are not darkened
               through by their own leaves (they still darken what is round them); the water its
               own share */
            int foliage = caster && zwrite && sv.cls == VK_SH_OBJECT && t0 && t0->cut_frac >= 0.25f;
            float w = water ? sv.v.water_ao : foliage ? sv.v.ao_foliage : 1.0f;
            pc.shadow[3] = (uint32_t)lroundf(w * 255.0f) << 8;
            if (parts && t0 && t0->cut_frac >= 0.1f) {
                /* see-through parts cut out of their texture (lattice towers and gates, fences):
                   where solid, the surface rays start from (attachment 2 from its eye distance) */
                geo_write = 3;
                pc.shadow[3] = 2;
            } else if (parts) {         /* see-through parts that add light (glass, reflections) keep it off */
                geo_write = 2;
                pc.shadow[3] = 1;
            }
        }
        if (caster && !sv.v.shadows && !sv.v.refl && !sv.beams && raw) {
            /* ambient occlusion alone: what is out of its reach (its first vertex further than its
               fade-out, a ray and 100 m for the object's size) casts nothing anyone sees */
            float p[3], rr = sv.v.ao_fade_end + sv.v.ao_radius + 6667.0f;
            memcpy(p, raw, 12);
            float x = k->world[0][0] * p[0] + k->world[0][1] * p[1] + k->world[0][2] * p[2] + k->world[0][3];
            float y = k->world[1][0] * p[0] + k->world[1][1] * p[1] + k->world[1][2] * p[2] + k->world[1][3];
            float zz = k->world[2][0] * p[0] + k->world[2][1] * p[1] + k->world[2][2] * p[2] + k->world[2][3];
            if (x * x + y * y + zz * zz > rr * rr)
                caster = 0;
        }
        if (caster && s->ngeo < SKY_BASE) {
            uint32_t g = s->ngeo++;
            memcpy(s->xform.map + 48 * g, k->world, 48);
            int tex = st->tex[0] && st->tex[0]->gpu && st->tex[0]->gpu->kind == K_TEX && pc.texsmp[0] >= 0;
            int cut = tex && (parts || cut_used(st->tex[0]->gpu->cutout, raw, nv, idx, ni)) &&
                      !rt_opaque;
            /* what a candidate hit tests (rt.glsl blocks): 1 the colours' alpha, 2 the texture's; 4
               a draw in the distance fade's dither range lo, hi (it blocks that share of the rays) */
            uint32_t gfl = (parts && !rt_opaque ? 1u : 0u) | (cut ? 2u : 0u);
            if (vk_fade && !rt_nofade)
                gfl |= 4u | ((vk_fade >> 9) & 511u) << 8 | (vk_fade & 511u) << 20;
            ShadowGeom *sg = (ShadowGeom *)(sh_buf.map + SH_SLOTS * sizeof(ShadowGlobals)) + (size_t)sv.slot * SH_GEOMS + g;
            float cov = cut && t0 ? 1.0f - t0->cut_frac : 0.0f;     /* (reflections: a cut-out seen from afar) */
            uint32_t covb;
            memcpy(&covb, &cov, 4);
            *sg = (ShadowGeom){vaddr, iaddr, tex ? pc.texsmp[0] : -1, pc.texsmp[2], 0.5f, gfl, pc.tnl[0], covb};
            s->geo[g] = (VkAccelerationStructureGeometryKHR){
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                .flags = gfl ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR,
                .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                       .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress = vaddr,
                                       .vertexStride = 40, .maxVertex = (uint32_t)nv - 1,
                                       .indexType = VK_INDEX_TYPE_UINT32, .indexData.deviceAddress = iaddr,
                                       .transformData.deviceAddress = s->xform.addr + 48 * g}};
            s->range[g] = (VkAccelerationStructureBuildRangeInfoKHR){.primitiveCount = nidx / 3};
            if (gfl) {
                st_rt_cut++;
                st_rt_cut_tris += nidx / 3;
            }
            s->maxprim[g] = nidx / 3;
            s->tris += nidx / 3;
        }
        if (sv.v.refl && k && !far_bind && !inst_bind && topo == 0)
            refl_gather(st, k, raw, nv, &pc, sbf, dbf, blend, zwrite, ztest, z, water, t0, sc, cull, vbuf, ibuf, voff, ioff,
                        vaddr, iaddr, nidx, first, vbase);
        if (st_print)
            st_rt_draw_ms += wall_ms() - t_sh;
    }
    if (in_scene) {               /* the scene's area (what its end copies into the back buffer) */
        scn.sc[0] = sc[0] < scn.sc[0] ? sc[0] : scn.sc[0];
        scn.sc[1] = sc[1] < scn.sc[1] ? sc[1] : scn.sc[1];
        scn.sc[2] = sc[2] > scn.sc[2] ? sc[2] : scn.sc[2];
        scn.sc[3] = sc[3] > scn.sc[3] ? sc[3] : scn.sc[3];
    }
    if (in_scene && r_mot)        /* temporal anti-aliasing: where its vertices were, what it marks */
        scene_motion(st, &pc, k, raw, nv, idx, ni, pretransformed, pl0, blend, sbf, dbf, zwrite,
                     st->tex[0] && st->tex[0]->gpu && st->tex[0]->gpu->kind == K_TEX ? st->tex[0] : NULL, sc);
    if (vk_smoke_parts && !vk_puff && blend && sbf == 5 && dbf == 6 && ztest && !zwrite && topo == 0 && !far_bind &&
        !inst_bind && z) {
        /* volumetric smoke: a see-through surface (glass, a lattice) over what is behind it, with where it
           is on screen (GPU pixels; its vertices placed as drawn) */
        float b[4] = {1e9f, 1e9f, -1e9f, -1e9f}, zmin = 1e30f;   /* (zmin: its nearest eye depth, clip w) */
        int whole = 0;
        for (int i = 0; i < nv && !whole; i++) {
            float x, y, w;
            if (k) {
                const float *p = (const float *)((const uint8_t *)raw + 40 * (size_t)i), *m = k->wvp;
                x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
                y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
                w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
                if (!(w > 1e-4f)) {
                    whole = 1;
                    break;
                }
                x = ax * x / w + bx;
                y = ay * y / w + by;
            } else {
                w = gv[i].pos[3];
                if (!(w > 1e-4f)) {
                    whole = 1;
                    break;
                }
                x = gv[i].pos[0] / w;
                y = gv[i].pos[1] / w;
            }
            x = (x + 1.0f) * 0.5f * (float)c->w;
            y = (y + 1.0f) * 0.5f * (float)c->h;
            zmin = fminf(zmin, w);
            b[0] = fminf(b[0], x);
            b[1] = fminf(b[1], y);
            b[2] = fmaxf(b[2], x);
            b[3] = fmaxf(b[3], y);
        }
        int bb[4] = {whole ? sc[0] : (int)floorf(b[0]) - 1, whole ? sc[1] : (int)floorf(b[1]) - 1,
                     whole ? sc[2] : (int)ceilf(b[2]) + 1, whole ? sc[3] : (int)ceilf(b[3]) + 1};
        sm_glass_add(c, &pc, topo, k != NULL, vbuf, ibuf, voff, ioff, nidx, first, vbase, sc,
                     cull == 2 ? VK_CULL_MODE_FRONT_BIT : cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE, z->format,
                     bb, whole ? 0.0f : zmin);
    }
    bind_pipeline(draw_pipeline(topo, blend, blend_factor(sbf, dst_alpha), blend_factor(dbf, dst_alpha),
                                z ? z->format : VK_FORMAT_UNDEFINED, k != NULL, r_mrt, geo_write));
    draw_mot = 0;
    uint32_t zf = r[RS_ZFUNC] >= 1 && r[RS_ZFUNC] <= 8 ? r[RS_ZFUNC] - 1 : VK_COMPARE_OP_LESS_OR_EQUAL;
    set_state(c, sc, cull == 2 ? VK_CULL_MODE_FRONT_BIT : cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE, ztest,
              zwrite, (VkCompareOp)zf, ztest ? zbias_units(z, r[RS_ZBIAS]) : 0);
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &voff);
    vkCmdBindIndexBuffer(cb, ibuf, ioff, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, nidx, inst_bind ? inst_bind->count : 1, first, vbase, 0);
    st_draws++;
}

/* The far terrain's mesh: replaces the last one (a new map; rare, so the GPU is let finish first) */
int vkr_far_mesh(const void *verts, uint32_t nv, const uint32_t *idx, uint32_t ni)
{
    if (!vk_on())
        return 0;
    if (far_mesh.buf) {
        vkDeviceWaitIdle(dev);
        vkUnmapMemory(dev, far_mesh.mem);
        vkDestroyBuffer(dev, far_mesh.buf, NULL);
        vkFreeMemory(dev, far_mesh.mem, NULL);
        memset(&far_mesh, 0, sizeof far_mesh);
    }
    if (!nv || !ni)
        return 0;
    far_ioff = ((VkDeviceSize)nv * 40 + 255) & ~(VkDeviceSize)255;
    chunk_new(&far_mesh, far_ioff + (VkDeviceSize)ni * 4);
    memcpy(far_mesh.map, verts, (size_t)nv * 40);
    memcpy(far_mesh.map + far_ioff, idx, (size_t)ni * 4);
    return 1;
}

/* Indices first..first+count-1 of the far mesh (vertex numbers + vbase), with st's render state
   and the transform k (GPU transform and lighting) */
void vkr_far_draw(const D3DState *st, const TnlConst *k, uint32_t first, uint32_t count, int32_t vbase)
{
    if (!far_mesh.buf || !count || !vk_tnl_ok(st, 16))
        return;
    struct FarBind b = {first, count, vbase};
    double t = wall_ms();
    far_bind = &b;
    draw_impl(st, PT_TRIANGLELIST, NULL, 1, NULL, 0, 0, k, k);
    far_bind = NULL;
    st_draw_ms += wall_ms() - t;
}

/* ------------------------------------------------------------------ ray-traced shadows */
int vk_rt_available(void)
{
    return vk_on() && rt_ok;
}

/* the acceleration structures just built are read by the next builds and by fragment shaders */
static void as_barrier(void)
{
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                           .srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}

/* The map's terrain: one static bottom level (map units), replacing the last map's */
int vkr_shadow_terrain(const float *xyz, uint32_t nv, const uint32_t *idx, uint32_t ni)
{
    if (!vk_on() || !rt_ok)
        return 0;
    frame_begin();                    /* (the retired ones belong to this frame) */
    accel_retire(&terr_blas);
    dbuf_retire(&terr_vb);
    dbuf_retire(&terr_ib);
    if (!nv || !ni)
        return 0;
    end_rendering();
    terr_vb = dbuf_new((VkDeviceSize)nv * 12, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1);
    terr_ib = dbuf_new((VkDeviceSize)ni * 4, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1);
    memcpy(terr_vb.map, xyz, (size_t)nv * 12);
    memcpy(terr_ib.map, idx, (size_t)ni * 4);
    VkAccelerationStructureGeometryKHR g = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress = terr_vb.addr,
                               .vertexStride = 12, .maxVertex = nv - 1, .indexType = VK_INDEX_TYPE_UINT32,
                               .indexData.deviceAddress = terr_ib.addr}};
    VkAccelerationStructureBuildGeometryInfoKHR bi = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = &g};
    uint32_t np = ni / 3;
    VkAccelerationStructureBuildSizesInfoKHR sz = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    p_build_sizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &np, &sz);
    accel_reserve(&terr_blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sz.accelerationStructureSize);
    scratch_reserve(&sh_scratch[fi], sz.buildScratchSize);
    bi.dstAccelerationStructure = terr_blas.as;
    bi.scratchData.deviceAddress = sh_scratch[fi].addr;
    VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = np};
    const VkAccelerationStructureBuildRangeInfoKHR *pr = &range;
    p_build_as(cb, 1, &bi, &pr);
    as_barrier();
    LOG(LOG_ALWAYS, "vk: shadows: terrain %u triangles, %.1f MB", np, sz.accelerationStructureSize / 1048576.0);
    return 1;
}

/* the game's object light map (256 x 256 bytes, z * 256 + x) */
int vkr_shadow_lightmap(const uint8_t *lm)
{
    if (!vk_on())
        return 0;
    frame_begin();
    if (!lm_img)
        lm_img = img_new(K_TEX, 256, 256, 1, VK_FORMAT_R8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    end_rendering();
    barrier(lm_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBuffer buf;
    uint8_t *p;
    VkDeviceSize off = ring(256 * 256, 16, &buf, &p);
    memcpy(p, lm, 256 * 256);
    VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {256, 256, 1}};
    vkCmdCopyBufferToImage(cb, buf, lm_img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier(lm_img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return 1;
}

static int sh_views;               /* views with shadows in this frame */

void vkr_shadow_begin(const VkShadowView *v)
{
    if (!vk_on() || !rt_ok || sh_views >= SH_VIEWS)
        return;
    memset(&sv, 0, sizeof sv);
    sv.active = 1;
    sv.slot = fi * SH_VIEWS + sh_views++;
    shf[sv.slot].ngeo = 0;
    shf[sv.slot].tris = 0;
    shf[sv.slot].nsky = 0;
    shf[sv.slot].sky_hash = 14695981039346656037ull;
    nrefl = 0;
    sv.v = *v;
    sv.cls = VK_SH_OBJECT;
    sv.sc[0] = sv.sc[1] = 1 << 30;
    sv.sc[2] = sv.sc[3] = -(1 << 30);
    /* the parts draw.frag reads during the view (the sun, the terrain's light map) */
    ShadowGlobals *G = (ShadowGlobals *)sh_buf.map + sv.slot;
    for (int i = 0; i < 3; i++) {
        G->cam[i] = (float)v->cam[i];
        G->sun[i] = v->sun[i];
    }
    G->sun[3] = v->ray_len;
    G->fade[3] = v->terrain_dark;
    G->cam[3] = v->water_shadow;
    G->slots[0] = lm_img ? lm_img->slot : -1;
    G->slots[1] = smp_nearest;
    /* headlights: the beams (shaders/shadow.glsl Beam), the nearest first; culled to the view's camera
       and cut to hl_max when it is known (beams_cull) */
    int nb = v->nbeams < 0 ? 0 : v->nbeams > VK_BEAMS ? VK_BEAMS : v->nbeams;
    sv.beams = nb > 0;
    sv.hl_rect[0] = sv.hl_rect[1] = -(1 << 30);  /* (all of it, until the camera is known) */
    sv.hl_rect[2] = sv.hl_rect[3] = 1 << 30;
    G->beamn[0] = nb;
    G->beamn[1] = hl_debug;
    G->beamn[2] = hl_terrain;
    G->beamn[3] = 0;
    for (int i = 0; i < nb; i++) {
        const VkBeam *b = &v->beams[i];
        float *o = G->beams[i];
        for (int k = 0; k < 3; k++) {
            o[k] = b->pos[k];
            o[4 + k] = b->dir[k];
            o[8 + k] = b->up[k];
            o[12 + k] = b->color[k];
        }
        o[3] = b->range;
        o[7] = b->spread[0];
        o[11] = b->spread[1];
        o[15] = b->half;
        sv.lamp[i][0] = b->lamp_dx;
        sv.lamp[i][1] = b->fade;
    }
    G->flaren[0] = G->flaren[1] = G->flaren[2] = G->flaren[3] = 0;
    G->hl[0] = G->hl[1] = G->hl[2] = 0.2f;      /* (the objects' ambient light until a lit one says, */
    G->hl[3] = v->beam_cap;
    G->hl2[0] = G->hl2[1] = G->hl2[2] = 1.0f;   /* ... the terrain's until its colour pass) */
    G->hl2[3] = 0;
    for (int k = 0; k < 3; k++)
        G->hl3[k] = v->beam_tint[k];
    G->hl3[3] = v->beam_haze / 66.67f;          /* (per game unit) */
}

/* Headlights: the beams whose lit room (a ball round the middle of its reach) is outside the view's
   camera (sv.vp, camera-relative world -> D3D clip) dropped, and those past the first hl_max (the
   nearest the camera), now it is known (its first terrain draw; nothing before it reads them) */
static void beams_cull(uint32_t cw, uint32_t ch)
{
    ShadowGlobals *G = (ShadowGlobals *)sh_buf.map + sv.slot;
    double rx0 = 1e30, ry0 = 1e30, rx1 = -1e30, ry1 = -1e30;
    double pl[6][4];
    for (int j = 0; j < 4; j++) {
        double c0 = sv.vp[4 * j], c1 = sv.vp[4 * j + 1], c2 = sv.vp[4 * j + 2], c3 = sv.vp[4 * j + 3];
        pl[0][j] = c3 + c0;
        pl[1][j] = c3 - c0;
        pl[2][j] = c3 + c1;
        pl[3][j] = c3 - c1;
        pl[4][j] = c2;
        pl[5][j] = c3 - c2;
    }
    int n = 0;
    for (int i = 0; i < G->beamn[0] && n < hl_max; i++) {
        const float *b = G->beams[i];
        double r = 0.5 * b[3], cen[3], rad = r * sqrt(1.0 + (double)b[7] * b[7]);
        for (int k = 0; k < 3; k++)
            cen[k] = b[k] + b[4 + k] * r;
        int in = 1;
        for (int p = 0; p < 6 && in; p++) {
            double l = sqrt(pl[p][0] * pl[p][0] + pl[p][1] * pl[p][1] + pl[p][2] * pl[p][2]);
            in = pl[p][0] * cen[0] + pl[p][1] * cen[1] + pl[p][2] * cen[2] + pl[p][3] >= -rad * l;
        }
        if (in) {
            /* the pyramid holding its lit room on the screen: the lamp and the corners of the beam's
               width at its reach (one behind the camera: all of it) */
            /* (beam_light measures up along the car's up, which is not square to the axis: by the axis's
               own up, square to it, the edge's height above the axis is (s - dir.up) / (1 - dir.up^2)) */
            double side[3] = {b[5] * b[10] - b[6] * b[9], b[6] * b[8] - b[4] * b[10], b[4] * b[9] - b[5] * b[8]};
            double du = b[4] * b[8] + b[5] * b[9] + b[6] * b[10], up2[3];
            for (int j = 0; j < 3; j++)
                up2[j] = b[8 + j] - du * b[4 + j];
            for (int k = 0; k < 5; k++) {
                double sx = k == 0 ? 0 : (k & 1 ? 1 : -1) * b[7] * 1.05;
                double sy = k == 0 ? 0 : ((k & 2 ? 1.0 : -3.0) * b[11] * 1.05 - du) / fmax(1.0 - du * du, 1e-3);
                double a = k == 0 ? 0 : b[3], q[3], h[4];
                for (int j = 0; j < 3; j++)
                    q[j] = b[j] + a * (b[4 + j] + sx * side[j] + sy * up2[j]);
                for (int j = 0; j < 4; j++)
                    h[j] = q[0] * sv.vp[j] + q[1] * sv.vp[4 + j] + q[2] * sv.vp[8 + j] + sv.vp[12 + j];
                if (h[3] <= 1.0) {
                    rx0 = ry0 = -1e30;
                    rx1 = ry1 = 1e30;
                    break;
                }
                double px = ((sv.place[0] * h[0] + sv.place[1] * h[3]) / h[3] * 0.5 + 0.5) * cw,
                       py = ((sv.place[2] * h[1] + sv.place[3] * h[3]) / h[3] * 0.5 + 0.5) * ch;
                rx0 = fmin(rx0, px);
                rx1 = fmax(rx1, px);
                ry0 = fmin(ry0, py);
                ry1 = fmax(ry1, py);
            }
            if (n != i) {
                memcpy(G->beams[n], b, sizeof G->beams[0]);
                memcpy(sv.lamp[n], sv.lamp[i], sizeof sv.lamp[0]);
            }
            n++;
        }
    }
    G->beamn[0] = n;
    sv.beams = n > 0;
    sv.hl_rect[0] = (int)fmax(floor(rx0) - 2, -1e9);
    sv.hl_rect[1] = (int)fmax(floor(ry0) - 2, -1e9);
    sv.hl_rect[2] = (int)fmin(ceil(rx1) + 2, 1e9);
    sv.hl_rect[3] = (int)fmin(ceil(ry1) + 2, 1e9);
}

void vkr_shadow_class(int cls)
{
    sv.cls = cls;
}

/* 4 x 4 inverse (Gauss-Jordan); 0 if singular */
static int inverse4(const double *m, double *out)
{
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            a[i][j] = j < 4 ? m[4 * i + j] : (j - 4 == i);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int i = c + 1; i < 4; i++)
            if (fabs(a[i][c]) > fabs(a[p][c]))
                p = i;
        if (fabs(a[p][c]) < 1e-30)
            return 0;
        for (int j = 0; j < 8; j++) {
            double t = a[c][j];
            a[c][j] = a[p][j];
            a[p][j] = t;
        }
        double d = a[c][c];
        for (int j = 0; j < 8; j++)
            a[c][j] /= d;
        for (int i = 0; i < 4; i++)
            if (i != c) {
                double f = a[i][c];
                for (int j = 0; j < 8; j++)
                    a[i][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = a[i][j + 4];
    return 1;
}

/* resolve.frag's pass `pass` into image g (0 and 2 the render target, 1 the small image), over
   x0, y0 .. x1, y1 */
typedef struct {
    uint32_t a[4];                /* the view's slot, debug view, pass, the input image's texture */
    float b[4];                   /* T's view pixels per pixel, the widest half-width, how much of the hard
                                     shadow stays, tan of the sun's angular size */
    float c[4];                   /* the focal length in T's pixels, 1 the nearest blocker */
    int32_t r[4];                 /* the view in T's pixels */
} PushSh;
_Static_assert(sizeof(PushSh) <= sizeof(PushDraw), "push constant range");

static void resolve_pass(GpuImg *g, uint32_t pass, PushSh *pcv, GpuImg *in, int x0, int y0, int x1, int y1)
{
    begin_rendering(g, NULL);
    g->gpu_newer = 1;
    bind_pipeline(resolve_pipeline((int)pass, g->format));
    VkViewport vp = {0, 0, (float)g->w, (float)g->h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D r = {{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}};
    vkCmdSetScissor(cb, 0, 1, &r);
    pcv->a[2] = pass;
    pcv->a[3] = in ? (uint32_t)in->slot : 0;
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *pcv, pcv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    end_rendering();
}

/* ao.frag's pass `pass` into image g (a small image, or the render target: apply), over x0, y0 ..
   x1, y1 */
typedef struct {
    uint32_t a[4];                /* the view's slot, debug view | rays per pixel << 8, pass, the input image's texture */
    int32_t r[4];                 /* the view in small-image pixels: x0 | y0 << 16, x1 | y1 << 16; z: how much
                                     ambient occlusion solid see-through parts take, w: the most it darkens
                                     (float bits) */
    float k[4];                   /* ray length, strength on objects, on the terrain, falloff power */
    float m[4];                   /* the fog colour; view pixels per small-image pixel */
    float f[4];                   /* fades out from x to y (eye distance); full fog at the view's edge: z end, w start */
    float g[4];                   /* the fog: start, end, density, how (mode | range << 2 | edge << 3) */
    float v[4];                   /* the camera's depth axis (view z = p . v.xyz + v.w) */
} PushAo;
_Static_assert(sizeof(PushAo) <= sizeof(PushDraw), "push constant range");

static void ao_pass(GpuImg *g, PushAo *pc, uint32_t pass, GpuImg *in, int x0, int y0, int x1, int y1)
{
    int apply = pass >= 3;
    begin_rendering(g, NULL);
    if (apply)
        g->gpu_newer = 1;
    bind_pipeline(ao_pipeline((int)pass, g->format));
    VkViewport vp = {0, 0, (float)g->w, (float)g->h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D r = {{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}};
    vkCmdSetScissor(cb, 0, 1, &r);
    pc->a[2] = pass;
    pc->a[3] = in ? (uint32_t)in->slot : 0;
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    end_rendering();
}

/* Ambient occlusion over the view's x0, y0 .. x1, y1 of render target c: traced into a small image
   (a pixel for each S x S), blurred across and down, applied to the colour (ao.frag) */
static void ao_passes(GpuImg *c, int x0, int y0, int x1, int y1, int timed, uint32_t tsb)
{
    /* about 400 rows whatever the size (2560 x 1600: S = 4; 4X anti-aliasing there: 8), so the rays
       cost about the same at every resolution; LIGHT about 267 (S = 6; 12) */
    int S = ao_scale ? ao_scale : (int)lroundf((float)c->h / sv.v.ao_rows), smax = sv.v.ao == 1 ? 12 : 8;
    S = S < 1 ? 1 : S > smax ? smax : S;
    uint32_t w = (c->w + (uint32_t)S - 1) / (uint32_t)S, h = (c->h + (uint32_t)S - 1) / (uint32_t)S;
    static GpuImg *spare[2];
    for (int i = 0; i < 2; i++)
        aux_img(&att_ao[i], &spare[i], w, h, AO_FORMAT);
    int q0 = x0 / S, q1 = y0 / S, q2 = (x1 + S - 1) / S, q3 = (y1 + S - 1) / S;
    uint32_t fog = sv.fog;
    int rays = ao_rays >= 0 ? ao_rays : sv.v.ao_rays;
    PushAo pc = {{(uint32_t)sv.slot, (uint32_t)(ao_debug & 255) | (uint32_t)rays << 8 | (uint32_t)ao_flags << 16, 0, 0},
                 {q0 | q1 << 16, q2 | q3 << 16, 0, 0},
                 {sv.v.ao_radius, sv.v.ao_strength, sv.v.ao_terrain, sv.v.ao_falloff},
                 {(float)(fog >> 16 & 255) / 255.0f, (float)(fog >> 8 & 255) / 255.0f, (float)(fog & 255) / 255.0f,
                  (float)S}, {sv.v.ao_fade_start, sv.v.ao_fade_end, sv.fog_edge[0], sv.fog_edge[1]},
                 {sv.fogp[0], sv.fogp[1], sv.fogp[2], sv.fogp[3]}, {sv.fwd[0], sv.fwd[1], sv.fwd[2], sv.fwd[3]}};
    memcpy(&pc.r[2], &sv.v.ao_parts, 4);
    memcpy(&pc.r[3], &sv.v.ao_max, 4);
    ao_pass(att_ao[0], &pc, 0, NULL, q0, q1, q2, q3);
    barrier(att_ao[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (timed)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 4);
    if (ao_blur) {
        ao_pass(att_ao[1], &pc, 1, att_ao[0], q0, q1, q2, q3);
        barrier(att_ao[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ao_pass(att_ao[0], &pc, 2, att_ao[1], q0, q1, q2, q3);
        barrier(att_ao[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (timed)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 5);
    ao_pass(c, &pc, sv.v.ao == 1 ? 4 : 3, att_ao[0], x0, y0, x1, y1);
}

/* The view's reflective draws again (draw.frag RT_REFLECT, after its ambient occlusion and
   shadows): what rays meet added where the game added its reflection texture, by the same draws'
   own colours, fog and alpha; trace 0: the texture itself, as the game drew it */
static void refl_replay(GpuImg *c, GpuImg *z, int trace)
{
    if (!nrefl || !c)
        return;
    for (int i = 0; i < nrefl && !(trace && refl_share && z); i++)
        refl_draws[i].pc.shadow[3] &= ~(3u << 23);     /* (no smaller images: a ray a pixel) */
    for (int k = 0; k < 3 && trace && refl_share && z; k++) {
        /* the draws whose rays are at a half, a quarter, an eighth of the size: into a smaller image
           first (draw.frag RT_REFLECT, shadow.w bit 22) */
        int n = 0, S = 2 << k;
        for (int i = 0; i < nrefl; i++)
            n += (int)((refl_draws[i].pc.shadow[3] >> 23) & 3) == k + 1;
        if (!n)
            continue;
        uint32_t w = (c->w + (uint32_t)S - 1) / (uint32_t)S, h = (c->h + (uint32_t)S - 1) / (uint32_t)S;
        GpuImg **a = &att_rs[k];
        static GpuImg *spare[3];
        aux_img(a, &spare[k], w, h, AO_FORMAT);
        begin_rendering(*a, NULL);
        (*a)->gpu_newer = 1;
        VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {0, 0, 0, 0}}}};
        VkClearRect cr = {{{0, 0}, {w, h}}, 0, 1};
        vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        for (int i = 0; i < nrefl; i++) {
            ReflDraw *d = &refl_draws[i];
            if ((int)((d->pc.shadow[3] >> 23) & 3) != k + 1)
                continue;
            d->pc.tnl[1] = (uint32_t)(*a)->slot;      /* (the full-size pass reads it) */
            PushDraw pc = d->pc;
            pc.shadow[3] |= 1u << 22;
            int sc[4] = {d->sc[0] / S, d->sc[1] / S, (d->sc[2] + S - 1) / S, (d->sc[3] + S - 1) / S};
            bind_pipeline(draw_pipeline(d->topo, 0, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_FORMAT_UNDEFINED, 1, 2, 0));
            set_state(*a, sc, d->cull, 0, 0, VK_COMPARE_OP_ALWAYS, 0);
            vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            vkCmdBindVertexBuffers(cb, 0, 1, &d->vbuf, &d->voff);
            vkCmdBindIndexBuffer(cb, d->ibuf, d->ioff, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, d->nidx, 1, d->first, d->vbase, 0);
            st_draws++;
        }
        end_rendering();
        barrier(*a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    want_mot = 1;                       /* (in temporal anti-aliasing's scene: its masks too) */
    begin_rendering(c, z);
    want_mot = 0;
    c->gpu_newer = 1;
    draw_mot = r_mot ? 1u | 8u : 0;     /* (the masks only: the surfaces have their vectors) */
    for (int i = 0; i < nrefl; i++) {
        ReflDraw *d = &refl_draws[i];
        if (!trace)
            d->pc.shadow[3] |= 1u << 21;
        /* (LIGHT's draws with a smaller image trace nothing here: the pipeline without the ray code) */
        int none = trace && (d->pc.shadow[3] >> 25 & 1) && (d->pc.shadow[3] >> 23 & 3);
        bind_pipeline(draw_pipeline(d->topo, 1, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, z ? d->zfmt : VK_FORMAT_UNDEFINED,
                                    1, none ? 4 : 3, 0));
        set_state(c, d->sc, d->cull, z && d->ztest, 0, d->zf, z && d->ztest ? d->bias : 0);
        vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof d->pc, &d->pc);
        vkCmdBindVertexBuffers(cb, 0, 1, &d->vbuf, &d->voff);
        vkCmdBindIndexBuffer(cb, d->ibuf, d->ioff, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, d->nidx, 1, d->first, d->vbase, 0);
        st_draws++;
    }
    draw_mot = 0;
    st_refl_draws += (unsigned long)nrefl;
    nrefl = 0;
}

/* The view is drawn: build the acceleration structures, then ambient occlusion (ao.frag), a
   shadow ray per pixel (resolve.frag) and the reflections (refl_replay), as the view asked */
/* Headlights: the lamps' flares (resolve.frag pass 8, added after the sun's shadows). Each beam's car
   has two lamps either side of its beam's start; one facing the camera shows as a glow round its
   place on the screen: a core about the lamp's size (15 cm) and a halo and a sideways streak sized by
   the view (glare), as strong as the lamp faces the camera (its heading against the way to the
   camera, to the 4th power) and the beam's fade, hidden where the depth buffer has something nearer
   than the lamp at its place (pass 8 looks there). */
static void flares(ShadowGlobals *G, GpuImg *c, int x0, int y0, int x1, int y1)
{
    int nf = 0;
    double rx0 = 1e30, ry0 = 1e30, rx1 = -1e30, ry1 = -1e30;
    for (int i = 0; i < G->beamn[0] && nf < FLARES; i++) {
        const float *b = G->beams[i];
        double side[3] = {b[5] * b[10] - b[6] * b[9], b[6] * b[8] - b[4] * b[10], b[4] * b[9] - b[5] * b[8]};
        double sl = sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        for (int s = -1; s <= 1 && sl > 0; s += 2) {
            double q[3], h[4], eye = 0;
            for (int k = 0; k < 3; k++) {
                q[k] = b[k] + s * sv.lamp[i][0] * side[k] / sl;
                eye += q[k] * q[k];
            }
            eye = sqrt(eye);
            if (eye < 50.0)
                continue;
            /* facing: its heading against the way to the camera; a little to the 4th */
            double f = -(b[4] * q[0] + b[5] * q[1] + b[6] * q[2]) / eye;
            if (f <= 0.15)
                continue;
            double I = pow((f - 0.15) / 0.85, 4.0) * sv.lamp[i][1];
            for (int j = 0; j < 4; j++)
                h[j] = q[0] * sv.vp[j] + q[1] * sv.vp[4 + j] + q[2] * sv.vp[8 + j] + sv.vp[12 + j];
            if (h[3] <= 1.0 || I < 0.01)
                continue;
            double px = ((sv.place[0] * h[0] + sv.place[1] * h[3]) / h[3] * 0.5 + 0.5) * c->w,
                   py = ((sv.place[2] * h[1] + sv.place[3] * h[3]) / h[3] * 0.5 + 0.5) * c->h;
            double core = fmax(sv.focal * 10.0 / eye, 1.5), halo = sv.focal * 0.045 * sqrt(I) + core;
            double reach = fmax(halo * 4.0, core * 4.0);
            if (px + reach < x0 || px - reach > x1 || py + reach < y0 || py - reach > y1)
                continue;
            float *o = G->flare[2 * nf];
            o[0] = (float)px;
            o[1] = (float)py;
            o[2] = (float)eye;
            o[3] = (float)I;
            o[4] = (float)core;
            o[5] = (float)halo;
            o[6] = (float)reach;
            o[7] = 0;
            rx0 = fmin(rx0, px - reach);
            rx1 = fmax(rx1, px + reach);
            ry0 = fmin(ry0, py - halo * 2.0);
            ry1 = fmax(ry1, py + halo * 2.0);
            nf++;
        }
    }
    G->flaren[0] = nf;
    if (hl_log && nframes % 100 == 0)
        LOG(LOG_ALWAYS, "vk: headlights: frame %lu, view %d: %d beams, %d flares", nframes, sv.slot, G->beamn[0], nf);
    /* (pass 8's pipeline made with passes 6 and 7, the first view with beams: no hitch the first time a lamp
       faces the camera) */
    static VkFormat warm;
    if (warm != c->format) {
        resolve_pipeline(8, c->format);
        warm = c->format;
    }
    int r[4] = {(int)fmax(rx0, x0), (int)fmax(ry0, y0), (int)fmin(ceil(rx1), x1), (int)fmin(ceil(ry1), y1)};
    if (!nf || r[2] <= r[0] || r[3] <= r[1])
        return;
    PushSh pcv = {{(uint32_t)sv.slot, (uint32_t)rt_debug, 0, 0}, {1, 0, 0, 0}, {sv.focal, 0, 0, 0}, {x0, y0, x1, y1}};
    resolve_pass(c, 8, &pcv, NULL, r[0], r[1], r[2], r[3]);
}

void vkr_shadow_end(void)
{
    if (!sv.active)
        return;
    sv.active = 0;
    end_rendering();                  /* the view's rendering had the extra attachments */
    GpuImg *c = sv.c, *z = sv.z;
    double inv[16];
    if (!c || !z || sv.broken || !sv.have_cam || !sv.cleared || sv.sc[2] <= sv.sc[0] || sv.sc[3] <= sv.sc[1] ||
        !inverse4(sv.vp, inv) || z->slot < 0) {
        static int told;
        if (!told++ || rt_log)
            LOG(LOG_ALWAYS, "vk: shadows: a view without them (target %d, depth %d, broken %d, camera %d, drawn %d, "
                "area %d %d %d %d)", !!c, z ? z->slot : -2, sv.broken, sv.have_cam, sv.cleared, sv.sc[0], sv.sc[1],
                sv.sc[2], sv.sc[3]);
        refl_replay(c, z, 0);
        return;
    }
    double t0 = wall_ms();
    Frame *f = &fr[fi];
    struct ShFrame *s = &shf[sv.slot];
    DBuf *scr = &sh_scratch[fi];
    int timed = st_queries && !f->sh_timed;    /* (the frame's first view) */
    uint32_t tsb = TS * (uint32_t)fi;
    if (timed) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 2);
        f->sh_timed = 1;
    }
    /* the frame's objects and cars */
    VkAccelerationStructureInstanceKHR *inst = (VkAccelerationStructureInstanceKHR *)s->inst.map;
    uint32_t ninst = 0;
    VkTransformMatrixKHR ident = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    if (s->ngeo) {
        VkAccelerationStructureBuildGeometryInfoKHR bi = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = s->ngeo, .pGeometries = s->geo};
        VkAccelerationStructureBuildSizesInfoKHR sz = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        double tq = wall_ms();
        p_build_sizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, s->maxprim, &sz);
        st_rt_sizes_ms += wall_ms() - tq;
        accel_reserve(&s->blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sz.accelerationStructureSize);
        scratch_reserve(scr, sz.buildScratchSize > tlas_scratch ? sz.buildScratchSize : tlas_scratch);
        bi.dstAccelerationStructure = s->blas.as;
        bi.scratchData.deviceAddress = scr->addr;
        const VkAccelerationStructureBuildRangeInfoKHR *pr = s->range;
        p_build_as(cb, 1, &bi, &pr);
        as_barrier();
        inst[ninst++] = (VkAccelerationStructureInstanceKHR){
            .transform = ident, .mask = 0x02, .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
            .accelerationStructureReference = s->blas.addr};
    }
    /* reflections: the sky dome round the camera (its own bottom and top levels: only reflections that
       meet nothing look for it, from the camera) */
    int sky = s->nsky && sv.v.refl;
    if (sky && (s->sky_hash != s->sky_built || !s->sky.as)) {
        VkAccelerationStructureBuildGeometryInfoKHR bi = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = s->nsky, .pGeometries = s->sky_geo};
        VkAccelerationStructureBuildSizesInfoKHR sz = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        p_build_sizes(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, s->sky_maxprim, &sz);
        accel_reserve(&s->sky, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sz.accelerationStructureSize);
        scratch_reserve(scr, sz.buildScratchSize > tlas_scratch ? sz.buildScratchSize : tlas_scratch);
        bi.dstAccelerationStructure = s->sky.as;
        bi.scratchData.deviceAddress = scr->addr;
        const VkAccelerationStructureBuildRangeInfoKHR *pr = s->sky_range;
        p_build_as(cb, 1, &bi, &pr);
        as_barrier();
        *(VkAccelerationStructureInstanceKHR *)s->sky_inst.map = (VkAccelerationStructureInstanceKHR){
            .transform = ident, .instanceCustomIndex = SKY_BASE, .mask = 0xff,
            .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, .accelerationStructureReference = s->sky.addr};
        VkAccelerationStructureGeometryKHR g = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                                   .data.deviceAddress = s->sky_inst.addr}};
        VkAccelerationStructureBuildGeometryInfoKHR tb = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .dstAccelerationStructure = s->sky_tlas.as,
            .geometryCount = 1, .pGeometries = &g, .scratchData.deviceAddress = scr->addr};
        VkAccelerationStructureBuildRangeInfoKHR tr = {.primitiveCount = 1};
        const VkAccelerationStructureBuildRangeInfoKHR *ptr = &tr;
        p_build_as(cb, 1, &tb, &ptr);
        as_barrier();
        s->sky_built = s->sky_hash;
    }
    /* the terrain, where the map repeats within reach of the shadows */
    if (terr_blas.as && sv.v.terrain) {
        double R = sv.v.fade_end + sv.v.ray_len, cx = sv.v.cam[0], cy = sv.v.cam[1], cz = sv.v.cam[2];
        for (int oz = (int)floor((cz - R) / 65536.0); oz <= (int)floor((cz + R) / 65536.0); oz++)
            for (int ox = (int)floor((cx - R) / 65536.0); ox <= (int)floor((cx + R) / 65536.0); ox++)
                if (ninst < SH_INST) {
                    VkTransformMatrixKHR t = {{{1, 0, 0, (float)(ox * 65536.0 - cx)},
                                               {0, 1, 0, (float)-cy},
                                               {0, 0, 1, (float)(oz * 65536.0 - cz)}}};
                    inst[ninst++] = (VkAccelerationStructureInstanceKHR){
                        .transform = t, .instanceCustomIndex = INST_TERRAIN, .mask = 0x01,
                        .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                                 VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR,
                        .accelerationStructureReference = terr_blas.addr};
                }
    }
    if (ninst) {
        scratch_reserve(scr, tlas_scratch);
        VkAccelerationStructureGeometryKHR g = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                                   .data.deviceAddress = s->inst.addr}};
        VkAccelerationStructureBuildGeometryInfoKHR tb = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .dstAccelerationStructure = s->tlas.as,
            .geometryCount = 1, .pGeometries = &g, .scratchData.deviceAddress = scr->addr};
        VkAccelerationStructureBuildRangeInfoKHR tr = {.primitiveCount = ninst};
        const VkAccelerationStructureBuildRangeInfoKHR *ptr = &tr;
        p_build_as(cb, 1, &tb, &ptr);
        as_barrier();
        if (timed)
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 3);
        /* the rest of the block for the rays */
        ShadowGlobals *G = (ShadowGlobals *)sh_buf.map + sv.slot;
        for (int i = 0; i < 16; i++)
            G->inv_vp[i] = (float)inv[i];
        memcpy(G->place, sv.place, 4 * sizeof(float));
        G->place2[0] = sv.place[4];
        G->place2[1] = sv.place[5];
        G->place2[2] = 1.0f / (float)c->w;
        G->place2[3] = 1.0f / (float)c->h;
        G->fade[0] = sv.v.fade_start;
        G->fade[1] = sv.v.fade_end;
        G->fade[2] = sv.v.offset;
        G->slots[2] = att_sun->slot;
        G->slots[3] = att_geo->slot;
        G->slots2[0] = z->slot;
        G->slots2[1] = (int32_t)s->ngeo;
        G->slots2[2] = smp_nearest;
        G->slots2[3] = SH_GEOMS;
        /* the camera's rays: points of a plane of constant depth (shaders/rt.glsl position) */
        double rp[3][3];
        for (int k = 0; k < 3; k++) {
            double px = 0.5 + (k == 1), py = 0.5 + (k == 2), v[4], w[4];
            v[0] = ((px * 2.0 / c->w - 1.0) - sv.place[1]) / sv.place[0];
            v[1] = ((py * 2.0 / c->h - 1.0) - sv.place[3]) / sv.place[2];
            v[2] = (0.5 - sv.place[4]) / sv.place[5];
            v[3] = 1.0;
            for (int j = 0; j < 4; j++)
                w[j] = v[0] * inv[j] + v[1] * inv[4 + j] + v[2] * inv[8 + j] + v[3] * inv[12 + j];
            for (int j = 0; j < 3; j++)
                rp[k][j] = w[j] / w[3];
        }
        for (int j = 0; j < 3; j++) {
            G->ray0[j] = (float)rp[0][j];
            G->rayx[j] = (float)(rp[1][j] - rp[0][j]);
            G->rayy[j] = (float)(rp[2][j] - rp[0][j]);
        }
        G->ray0[3] = G->rayx[3] = G->rayy[3] = 0.0f;
        /* reflections: the terrain's colour map, the sky, the tuning, the fog */
        float avg[3] = {0, 0, 0};
        int na = 0;
        for (int i = 0; i < 16; i++) {
            G->tiles[i] = tile_img[i] ? tile_img[i]->slot : -1;
            for (int j = 0; j < 3 && tile_img[i]; j++)
                avg[j] += tile_avg[i][j];
            na += tile_img[i] != NULL;
        }
        G->rslots[0] = tile_smp >= 0 ? tile_smp : smp_linear;
        G->rslots[1] = sky ? SKY_BASE : -1;
        G->rslots[2] = G->rslots[3] = 0;
        G->terr[0] = tile_grey;
        G->terr[1] = sv.v.terrain_detail;
        G->terr[2] = G->terr[3] = 0;
        for (int j = 0; j < 3; j++) {
            G->fogcol[j] = (float)(sv.fog >> (16 - 8 * j) & 255) / 255.0f;
            G->tavg[j] = na ? avg[j] / (float)na : G->fogcol[j];
        }
        G->tavg[3] = G->fogcol[3] = 0;
        G->refl[0] = refl_fresnel >= 0 ? refl_fresnel : sv.v.refl_fresnel;
        G->refl[1] = refl_darken >= 0 ? refl_darken : sv.v.refl_darken;
        G->refl[2] = sv.v.refl_len;
        G->refl[3] = sv.focal > 0 ? 1.0f / sv.focal : 1.0e-3f;
        memcpy(G->fogp, sv.fogp, sizeof G->fogp);
        G->fogedge[0] = ((int)sv.fogp[3] & 8) ? sv.fog_edge[0] : 0.0f;
        G->fogedge[1] = sv.fog_edge[1];
        G->fogedge[2] = G->fogedge[3] = 0;
        G->refl2[0] = refl_gain >= 0 ? refl_gain : sv.v.refl_gain;
        G->refl2[1] = (float)refl_debug;
        G->refl2[2] = refl_sd[0];
        G->refl2[3] = refl_sd[1];
        G->rslots[2] = sampler_get(2, 2, 3, 1, 1, 0, 0.0f, 0, 16, 0);
        for (int j = 0; j < 3; j++)
            G->sunc[j] = sv.v.sun_color[j];
        G->sunc[3] = 0;
        barrier(att_sun, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(att_geo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(z, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        /* volumetric smoke: the view's puffs' first passes, their shadow for the resolve below */
        float smoke_sh = rt_trace && sv.v.shadows ? vk_smoke_early(c, z, sv.slot) : 0.0f;
        int x0 = sv.sc[0] > 0 ? sv.sc[0] : 0, y0 = sv.sc[1] > 0 ? sv.sc[1] : 0;
        int x1 = sv.sc[2] < (int)c->w ? sv.sc[2] : (int)c->w, y1 = sv.sc[3] < (int)c->h ? sv.sc[3] : (int)c->h;
        /* ambient occlusion first: it darkens the colour less the sun's part, which the shadows
           then take away where the sun is blocked */
        if (rt_trace && sv.v.ao)
            ao_passes(c, x0, y0, x1, y1, timed, tsb);
        /* headlights: the beams' light on each surface, its shadow traced, into an image half the size
           each way (resolve.frag pass 6), then added over the view (7), before the sun's shadows take
           their part away: the colour there is still the surface's lit as drawn */
        if (sv.beams) {
            /* (the colour as it is now, for pass 7 to read: its target can only add to itself) */
            static GpuImg *spare[2];
            /* (only where the beams' lit room can be on the screen: beams_cull) */
            int r4[4] = {x0 > sv.hl_rect[0] ? x0 : sv.hl_rect[0], y0 > sv.hl_rect[1] ? y0 : sv.hl_rect[1],
                         x1 < sv.hl_rect[2] ? x1 : sv.hl_rect[2], y1 < sv.hl_rect[3] ? y1 : sv.hl_rect[3]};
            aux_img(&att_hlc, &spare[0], c->w, c->h, c->format);
            aux_img(&att_hlh, &spare[1], (c->w + 1) / 2, (c->h + 1) / 2, VK_FORMAT_R16G16B16A16_SFLOAT);
            if (r4[2] > r4[0] && r4[3] > r4[1]) {
                if (hl_debug != 5)      /* (5: no copy, timing) */
                    blit(c, r4, att_hlc, r4, VK_FILTER_NEAREST);
                barrier(att_hlc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                G->beamn[3] = att_hlc->slot;
                if (G->hl3[3] > 0) {    /* VOLUMETRIC: the air's noise (resolve.frag air_light) */
                    if (!sm_noise)
                        sm_noise_make();
                    G->flaren[1] = sm_noise->slot;
                    G->flaren[2] = smp_linear;
                }
                PushSh pcv = {{(uint32_t)sv.slot, (uint32_t)rt_debug, 0, 0}, {1, 0, 0, 0},
                              {sv.focal, (float)sh_nearest, 0, 0}, {x0, y0, x1, y1}};
                /* (a pixel of it more round: pass 7's 4 nearest at the edges) */
                resolve_pass(att_hlh, 6, &pcv, NULL, r4[0] > 2 ? r4[0] / 2 - 1 : 0, r4[1] > 2 ? r4[1] / 2 - 1 : 0,
                             (r4[2] + 3) / 2, (r4[3] + 3) / 2);
                barrier(att_hlh, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                resolve_pass(c, 7, &pcv, att_hlh, r4[0], r4[1], r4[2], r4[3]);
            }
        }
        /* (TD6_SH_TIME=1 without ambient occlusion: its three timestamps time the shadows' trace,
           filters and taking away instead) */
        int sh_ts = timed && !sv.v.ao && sh_time;
        if (timed && !sh_ts)
            for (uint32_t q = rt_trace && sv.v.ao ? 6 : 4; q <= 6; q++)
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + q);
        if (rt_trace && sv.v.shadows) {
            /* 4X anti-aliasing draws at twice the size each way: a ray for each 2 x 2 pixels */
            int S = res_aa > 1.5f && !rt_full ? 2 : 1;
            uint32_t w2 = (c->w + (uint32_t)S - 1) / (uint32_t)S, h2 = (c->h + (uint32_t)S - 1) / (uint32_t)S;
            int q0 = x0 / S, q1 = y0 / S, q2 = (x1 + S - 1) / S, q3 = (y1 + S - 1) / S;
            int rmax = (int)lroundf((float)sh_rmax * (float)h2 / 6400.0f);     /* (in U's pixels) */
            PushSh pcv = {{(uint32_t)sv.slot, (uint32_t)rt_debug, 0, 0},
                          {(float)S, (float)(rmax < 1 ? 1 : rmax > 4 ? 4 : rmax), sh_keep,
                           tanf(sh_sun * (float)M_PI / 180.0f)},
                          {sv.focal / (float)S, (float)sh_nearest, 0, smoke_sh}, {q0, q1, q2, q3}};
            if (S == 1 && !sh_soft)
                resolve_pass(c, 0, &pcv, NULL, x0, y0, x1, y1);
            else {
                static GpuImg *spare[3];
                for (int i = 0; i < 3; i++)
                    aux_img(&att_sh[i], &spare[i], i ? (w2 + 3) / 4 : w2, i ? (h2 + 3) / 4 : h2, SH_FORMAT);
                /* trace into T; soft edges: T down into U, filtered across and down; take away */
                resolve_pass(att_sh[0], 1, &pcv, NULL, q0, q1, q2, q3);
                barrier(att_sh[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (sh_ts)
                    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 4);
                if (sh_soft) {
                    int u0 = q0 / 4, u1 = q1 / 4, u2 = (q2 + 3) / 4, u3 = (q3 + 3) / 4;
                    resolve_pass(att_sh[1], 5, &pcv, att_sh[0], u0, u1, u2, u3);
                    barrier(att_sh[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    resolve_pass(att_sh[2], 3, &pcv, att_sh[1], u0, u1, u2, u3);
                    barrier(att_sh[2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    resolve_pass(att_sh[1], 4, &pcv, att_sh[2], u0, u1, u2, u3);
                    barrier(att_sh[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    pcv.c[2] = (float)att_sh[1]->slot;
                }
                if (sh_ts) {
                    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 5);
                    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 6);
                }
                resolve_pass(c, 2, &pcv, att_sh[0], x0, y0, x1, y1);
            }
        }
        if (sv.beams)
            flares(G, c, x0, y0, x1, y1);
        if (timed)
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 7);
        refl_replay(c, z, rt_trace && refl_trace);
    }
    refl_replay(c, z, 0);               /* (no structures: as the game drew them) */
    sm_rt.ok = ninst && rt_trace && sv.v.shadows;   /* (volumetric smoke: its puffs' sun grids) */
    sm_rt.slot = sv.slot;
    sm_rt.c = c;
    sm_rt.frame = nframes;
    if (timed)
        for (uint32_t q = ninst ? 8 : 3; q < 9; q++)       /* (9, 10: temporal anti-aliasing's) */
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + q);
    if (rt_log)
        logmsg(LOG_ALWAYS, "vk: rt view frame %lu slot %d: %u geometries, %u instances, area %d %d %d %d, ao %d shadows %d, "
               "camera at cell %.1f %.1f looking %.2f %.2f %.2f", nframes, sv.slot, s->ngeo, ninst, sv.sc[0], sv.sc[1],
               sv.sc[2], sv.sc[3], sv.v.ao, sv.v.shadows, fmod(sv.v.cam[0] / 256.0 + 512.0, 256.0),
               fmod(sv.v.cam[2] / 256.0 + 512.0, 256.0), sv.fwd[0], sv.fwd[1], sv.fwd[2]);
    st_rt_views++;
    st_rt_geo += s->ngeo;
    st_rt_tris += s->tris;
    st_rt_ms += wall_ms() - t0;
}

/* ------------------------------------------------------------------ volumetric smoke and dust */
/* (DESIGN.md decision 4, "Volumetric smoke and dust"; smoke.c has how the game makes its puffs.) In
   a view of the world (vk_smoke_view_begin .. _end) each puff's sprite draw is taken (vk_smoke_take,
   from d3d.c while smoke.c has vk_puff set) instead of drawn: its quad's corners give its centre and
   size in view space; its atlas cell (worked out once a cell from its texture: how much of the cell
   its blob fills and how opaque the blob is) the ball's radius and density, which go on smoothly
   from phase to phase (sm_hist, by the object's number and how far it is into its phase). The
   nearest SMOKE_MAX are drawn back to front as volumes into an image S times smaller each way
   (shaders/smoke.frag) and brought up onto the view: first (sm_first) its depth (pass 0, the view's
   glass drawn again for it) and, with the sun shadows ray traced, each puff's sun grid
   (smokevis.frag) and the nearest SMOKE_SHADOWS' shadow (pass 4), which vk_shadow_end calls before
   its resolve (that takes the shadow); then at the view's end (sm_last) the march (pass 1) and
   bringing it up (pass 2). Without the ray-traced sun shadows both run at the view's end.
   A puff marches N steps by its size on screen; if the view's steps over the puffs' areas would
   pass sm_budget times the smaller image's area, all are made coarser (2 at least). */
int vk_smoke_depth;                     /* (vk_puff: vkthread.c's, vkr's here) */

typedef struct {
    float c[3], r;                /* view space, game units */
    float col[3], sigma;          /* colour as drawn (0..1); extinction at the densest (per game unit) */
    float dist;                   /* from the camera */
    float rect[4];                /* the smaller image's pixels */
    float srect[4];               /* where its shadow can fall (GPU pixels) */
    int steps;
    VkPuff m;
} SmPuff;
typedef struct {
    uint32_t a[4];                /* the view's block, puffs, the depth's texture, the input image */
    uint32_t b[4];                /* the smaller depth image, the noise, linear sampler, nearest */
    float c[4];                   /* the size drawn into, S, debug */
    float d[4];                   /* the masks' strengths, the ray-traced view's slot, 1: the whole image */
    uint32_t e[4];                /* the see-through surfaces' depth and alpha (pass 0), 1: there are some; 1 the
                                     cabins kept clear (pass 1), the edges' share (pass 2) */
} PushSmoke;
_Static_assert(sizeof(PushSmoke) <= sizeof(PushDraw), "push constant range");
#define SMOKE_TAKE 1024           /* puffs a view takes */
#define SMOKE_GLASS 256           /* see-through surfaces a view keeps */
typedef struct {
    int has;                      /* its first puff set the rest */
    VkSmokeView v;
    GpuImg *c, *z;                /* where its draws went */
    int in_scene;                 /* temporal anti-aliasing's scene */
    float pl[6];                  /* D3D clip -> Vulkan clip: ax bx ay by (with the jitter), mnz dz */
    int sc[4];                    /* the view's area (GPU pixels) */
    float P[4][4], V[4][4];       /* its projection and view (D3D) */
    uint32_t fog_col, ambient;
    float fogp[4], fog_edge[2];   /* start, end, density, how (mode | range << 2 | edge << 3); full fog at the edge */
    int n;
    SmPuff p[SMOKE_TAKE];
    int stage;                    /* 0 nothing drawn yet; 1 sm_first done (from vk_shadow_end with the sun shadows
                                     traced: their resolve takes the puffs' shadow), 2 nothing to draw */
    SmPuff *p0;                   /* sm_first's: the puffs drawn (back to front), */
    int np, S, grid;              /* ... how many, the smaller image's scale, sun grids, */
    uint32_t w2, h2;              /* ... its size, */
    int vr[4], q[4];              /* ... the view's area (GPU pixels), the puffs' (smaller image pixels), */
    PushSmoke pc;                 /* ... the passes' constants, */
    int ts_open;                  /* ... its timestamp window left open for sm_last (TD6_VK_STATS) */
    int nglass;                   /* its see-through surfaces (0x419280's draws but the puffs) */
    struct SmGlass {
        PushDraw pc;
        GpuImg *c;
        VkBuffer vbuf, ibuf;
        VkDeviceSize voff, ioff;
        uint32_t nidx, first;
        int32_t vbase;
        int sc[4], topo, tnl;
        int bb[4];                /* where it is (GPU pixels) */
        float zmin;               /* its nearest eye depth (clip w; 0 reaching the camera) */
        VkCullModeFlags cull;
        VkFormat zfmt;
    } glass[SMOKE_GLASS];
} SmView;
static SmView *smv[4];            /* views inside views (the in-car view's mirrors) */
static int smv_depth;
static GpuImg *sm_noise;          /* the tiled 3D noise (smoke.frag noise3) */
static GpuImg *att_smz, *att_sma, *att_smg;   /* the smaller image's depth, the march, the sun grids */
static GpuImg *att_sms, *att_smsz;   /* the see-through surfaces drawn again: their alpha, their depth */
static GpuImg *att_smsh;             /* the puffs' shadow */
static unsigned long st_sm_views, st_sm_taken, st_sm_drawn, st_sm_grid;
static double st_sm_steps, st_sm_area, st_sm_cpu_ms;
static unsigned long st_sm_kind[4];   /* puffs taken by kind (exhaust, the ground's smoke, dust, splashes) */
static unsigned long st_sm_glass;     /* see-through surfaces drawn again */
static unsigned long st_sm_casters;   /* puffs casting shadows */

/* A cell of a puff's texture: how much of its half-size its blob fills (the radius of a disk with
   the alpha's second moment round its centre) and the blob's mean alpha; worked out once */
typedef struct { const Surface *s; float u[4]; float fill, alpha; } SmCell;
static SmCell sm_cells[512];
static int sm_ncells, sm_cell_next;

static void sm_cells_drop(const Surface *s)
{
    for (int i = 0; i < sm_ncells; i++)
        if (sm_cells[i].s == s)
            sm_cells[i].s = NULL;
}

static const SmCell *sm_cell(const Surface *s, const float *u)
{
    for (int i = 0; i < sm_ncells; i++)
        if (sm_cells[i].s == s && fabsf(sm_cells[i].u[0] - u[0]) < 1e-4f && fabsf(sm_cells[i].u[1] - u[1]) < 1e-4f &&
            fabsf(sm_cells[i].u[2] - u[2]) < 1e-4f && fabsf(sm_cells[i].u[3] - u[3]) < 1e-4f)
            return &sm_cells[i];
    SmCell *e = &sm_cells[sm_ncells < 512 ? sm_ncells++ : (sm_cell_next++ & 511)];
    e->s = s;
    memcpy(e->u, u, sizeof e->u);
    e->fill = 0.6f;
    e->alpha = 0.4f;
    if (!s || !s->mem || !s->width || !s->height)
        return e;
    int x0 = (int)floorf(u[0] * (float)s->width), x1 = (int)ceilf(u[2] * (float)s->width);
    int y0 = (int)floorf(u[1] * (float)s->height), y1 = (int)ceilf(u[3] * (float)s->height);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > (int)s->width ? (int)s->width : x1;
    y1 = y1 > (int)s->height ? (int)s->height : y1;
    if (x1 - x0 < 2 || y1 - y0 < 2)
        return e;
    const uint8_t *snap = vk_snap_pixels(s, NULL);
    int sx = (x1 - x0 + 47) / 48, sy = (y1 - y0 + 47) / 48;
    double m0 = 0, mx = 0, my = 0, mr = 0, hw = (x1 - x0) * 0.5, hh = (y1 - y0) * 0.5;
    for (int pass = 0; pass < 3; pass++) {
        double cx = m0 > 0 ? mx / m0 : x0 + hw, cy = m0 > 0 ? my / m0 : y0 + hh, rho = m0 > 0 ? sqrt(2.0 * mr / m0) : 1.0;
        double sum = 0;
        int cnt = 0;
        for (int y = y0; y < y1; y += sy)
            for (int x = x0; x < x1; x += sx) {
                double a = (double)(pix_decode(s, snap_read(s, snap, (uint32_t)x, (uint32_t)y)) >> 24) / 255.0;
                double dx = (x + 0.5 - cx) / hw, dy = (y + 0.5 - cy) / hh;
                if (pass == 0) {
                    m0 += a;
                    mx += a * (x + 0.5);
                    my += a * (y + 0.5);
                } else if (pass == 1) {
                    mr += a * (dx * dx + dy * dy);
                } else if (dx * dx + dy * dy <= rho * rho) {
                    sum += a;
                    cnt++;
                }
            }
        if (pass == 0 && m0 <= 0)
            return e;
        if (pass == 2) {
            e->fill = (float)fmin(fmax(rho, 0.15), 1.0);
            e->alpha = cnt ? (float)(sum / cnt) : 0.0f;
            if (e->alpha < 0.2f)                /* (a fading blob's specks spread less) */
                e->fill = fmaxf(e->fill, 0.62f);
        }
    }
    return e;
}

/* the optical depth through the centre of a ball of density (1 - r^2)^2 whose disk is as opaque
   on average as a */
static float sm_tau(float a)
{
    static float tab[65];
    static int made;
    if (!made) {
        for (int k = 0; k <= 64; k++) {
            double tau = k * 0.125, m = 0;
            for (int i = 0; i < 256; i++) {
                double b = (i + 0.5) / 256.0;
                m += (1.0 - exp(-tau * pow(1.0 - b * b, 2.5))) * 2.0 * b / 256.0;
            }
            tab[k] = (float)m;
        }
        made = 1;
    }
    if (!(a > 0.0f))
        return 0.0f;
    for (int k = 1; k <= 64; k++)
        if (tab[k] >= a)
            return 0.125f * ((float)(k - 1) + (a - tab[k - 1]) / (tab[k] - tab[k - 1]));
    return 8.0f;
}

/* A puff's size and opacity, by its object's number: the phase before's going over to this one's */
typedef struct { uint32_t serial; float w, r0, r1, a0, a1; unsigned long frame; } SmHist;
static SmHist sm_hist[4096];

static SmHist *sm_hist_get(uint32_t serial, int *fresh)
{
    uint32_t h = (serial * 2654435761u) >> 20;
    SmHist *spare = NULL;
    for (uint32_t k = 0; k < 8; k++) {
        SmHist *e = &sm_hist[(h + k) & 4095];
        int live = e->frame && nframes - e->frame < 120;
        if (live && e->serial == serial) {
            *fresh = 0;
            return e;
        }
        if (!spare && !live)
            spare = e;
    }
    if (!spare)
        spare = &sm_hist[h & 4095];
    memset(spare, 0, sizeof *spare);
    spare->serial = serial;
    *fresh = 1;
    return spare;
}

void vkr_smoke_take(const D3DState *st, const float (*pos)[3], int n, uint32_t diffuse, const float (*uv)[2])
{
    SmView *s = smv_depth > 0 && smv_depth <= 4 ? smv[smv_depth - 1] : NULL;
    if (!s || !vk_puff || n < 4 || !st->rt || st->rt->dead || !st->rt->mem || !vk_on())
        return;
    frame_begin();
    GpuImg *c = rt_sync(st->rt), *z = depth_sync(st->zb, c);
    int in_scene = scn.active && scene_target(&c, &z, st);
    if (!z || z->slot < 0)
        return;
    const uint32_t *r = st->rs;
    if (!s->has) {
        /* the view: where its draws go (as draw_impl places a perspective draw) */
        s->has = 1;
        s->c = c;
        s->z = z;
        s->in_scene = in_scene;
        float H = (float)c->lh, PW = (float)c->w, vx = (float)st->vp.x, vw = (float)st->vp.w;
        float mid = ui_x(c, vx + vw / 2.0f), half = vw * c->sy / 2.0f, hp = 0.5f * c->sy;
        if (c->ox > 0.5f && proj_widened(st))
            half *= ws_aspect / (4.0f / 3.0f);
        s->pl[0] = 2.0f * half / PW;
        s->pl[1] = 2.0f * (mid + hp) / PW - 1.0f;
        s->pl[2] = -(float)st->vp.h / H;
        s->pl[3] = (2.0f * (float)st->vp.y + 1.0f + (float)st->vp.h) / H - 1.0f;
        s->pl[4] = st->vp.minz;
        s->pl[5] = st->vp.maxz - st->vp.minz;
        if (in_scene) {
            s->pl[1] += scn.jndc[0];
            s->pl[3] += scn.jndc[1];
        }
        s->sc[0] = (int)lroundf(mid - half);
        s->sc[1] = (int)lroundf((float)st->vp.y * c->sy);
        s->sc[2] = (int)lroundf(mid + half);
        s->sc[3] = (int)lroundf((float)(st->vp.y + st->vp.h) * c->sy);
        memcpy(s->P, st->xf[XF_PROJ].m, sizeof s->P);
        memcpy(s->V, st->xf[XF_VIEW].m, sizeof s->V);
        /* its fog, as draw.frag (table fog) or tnl.vert (vertex fog) has it */
        uint32_t fmode = 0, frange = 0;
        if (r[RS_FOGENABLE] && r[RS_FOGTABLEMODE] >= 1 && r[RS_FOGTABLEMODE] <= 3)
            fmode = r[RS_FOGTABLEMODE];
        else if (r[RS_FOGENABLE] && r[RS_FOGVERTEXMODE] >= 1 && r[RS_FOGVERTEXMODE] <= 3) {
            fmode = r[RS_FOGVERTEXMODE];
            frange = r[RS_RANGEFOGENABLE] != 0;
        }
        s->fog_col = r[RS_FOGCOLOR];
        memcpy(&s->fogp[0], &r[RS_FOGSTART], 4);
        memcpy(&s->fogp[1], &r[RS_FOGEND], 4);
        memcpy(&s->fogp[2], &r[RS_FOGDENSITY], 4);
        int edge = fmode && vk_fog_end > 0;
        s->fogp[3] = (float)(fmode | frange << 2 | (uint32_t)edge << 3);
        s->fog_edge[0] = vk_fog_end;
        s->fog_edge[1] = vk_fog_start;
        s->ambient = r[RS_AMBIENT];
    } else if (s->c != c || s->z != z)
        return;
    st_sm_taken++;
    st_sm_kind[vk_puff->kind & 3]++;
    if (s->n >= SMOKE_TAKE)
        return;
    /* its corners in view space (model -> world -> view, D3D row vectors) */
    const Mat4 *W = &st->xf[XF_WORLD], *V = &st->xf[XF_VIEW];
    float q[4][3];
    for (int i = 0; i < 4; i++) {
        float w[3];
        for (int j = 0; j < 3; j++)
            w[j] = pos[i][0] * W->m[0][j] + pos[i][1] * W->m[1][j] + pos[i][2] * W->m[2][j] + W->m[3][j];
        for (int j = 0; j < 3; j++)
            q[i][j] = w[0] * V->m[0][j] + w[1] * V->m[1][j] + w[2] * V->m[2][j] + V->m[3][j];
    }
    SmPuff *p = &s->p[s->n];
    float e1 = 0, e2 = 0;
    for (int j = 0; j < 3; j++) {
        p->c[j] = 0.25f * (q[0][j] + q[1][j] + q[2][j] + q[3][j]);
        e1 += (q[1][j] - q[0][j]) * (q[1][j] - q[0][j]);
        e2 += (q[3][j] - q[0][j]) * (q[3][j] - q[0][j]);
    }
    float hw = 0.5f * fmaxf(sqrtf(e1), sqrtf(e2));
    if (!(hw > 0.0f) || p->c[2] < -2.0f * hw)
        return;
    float u4[4] = {fminf(fminf(uv[0][0], uv[1][0]), fminf(uv[2][0], uv[3][0])),
                   fminf(fminf(uv[0][1], uv[1][1]), fminf(uv[2][1], uv[3][1])),
                   fmaxf(fmaxf(uv[0][0], uv[1][0]), fmaxf(uv[2][0], uv[3][0])),
                   fmaxf(fmaxf(uv[0][1], uv[1][1]), fmaxf(uv[2][1], uv[3][1]))};
    const SmCell *cell = sm_cell(st->tex[0], u4);
    /* (the ball a third wider than the blob, as faint in all: the sprite's soft edge spreads it) */
    float R = hw * cell->fill * sm_widen, a = cell->alpha * (float)(diffuse >> 24) / 255.0f / (sm_widen * sm_widen);
    int fresh;
    SmHist *h = sm_hist_get(vk_puff->serial, &fresh);
    if (fresh) {
        h->w = hw;
        h->r0 = h->r1 = R;
        h->a0 = h->a1 = a;
    } else if (fabsf(hw - h->w) > 1e-3f * hw) {     /* (a new phase) */
        h->w = hw;
        h->r0 = h->r1;
        h->a0 = h->a1;
        h->r1 = R;
        h->a1 = a;
    }
    h->frame = nframes;
    float g = fminf(fmaxf(vk_puff->grow, 0.0f), 1.0f);
    R = h->r0 + g * (h->r1 - h->r0);
    a = h->a0 + g * (h->a1 - h->a0);
    static const float gains[4] = {0.75f, 1.0f, 1.2f, 1.0f};    /* (exhausts, the ground's smoke, dust, splashes' mist) */
    float gain = gains[vk_puff->kind & 3];
    /* (0.8: the noise's share of the density on average) */
    float tau = sm_tau(fminf(a * gain, 0.97f)) * 2.2f * sm_density;   /* (2.2: about as dark as the sprites in all) */
    p->r = R;
    p->sigma = R > 0.0f ? tau / (R * 16.0f / 15.0f) : 0.0f;
    for (int j = 0; j < 3; j++)
        p->col[j] = (float)((diffuse >> (16 - 8 * j)) & 255) / 255.0f;
    p->dist = sqrtf(p->c[0] * p->c[0] + p->c[1] * p->c[1] + p->c[2] * p->c[2]);
    p->m = *vk_puff;
    s->n++;
}

void vkr_smoke_view_begin(const VkSmokeView *v)
{
    if (smv_depth < 4) {
        if (!smv[smv_depth] && !(smv[smv_depth] = malloc(sizeof(SmView))))
            fatal("out of memory");
        SmView *s = smv[smv_depth];
        s->has = 0;
        s->n = 0;
        s->nglass = 0;
        s->stage = 0;
        s->ts_open = 0;
        s->v = *v;
    }
    smv_depth++;
    sm_rt.ok = 0;                 /* (only this view's own structures) */
}

/* the tiled noise: 32 x 32 x 32 points (each axis repeating), four kinds (smoke.frag noise3) */
static uint32_t sm_hash(int x, int y, int z, int seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2246822519u + (uint32_t)seed * 3266489917u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* Worley (1 - the nearest feature's distance in cells) at point x, y, z of 32, f cells a period */
static float sm_worley(int x, int y, int z, int f, int seed)
{
    float cs = 32.0f / (float)f, px = ((float)x + 0.5f) / cs, py = ((float)y + 0.5f) / cs, pz = ((float)z + 0.5f) / cs;
    int cx = (int)floorf(px), cy = (int)floorf(py), cz = (int)floorf(pz);
    float best = 9.0f;
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int gx = cx + dx, gy = cy + dy, gz = cz + dz;
                uint32_t h = sm_hash(((gx % f) + f) % f, ((gy % f) + f) % f, ((gz % f) + f) % f, seed);
                float fx = (float)gx + (float)(h & 1023) / 1024.0f, fy = (float)gy + (float)((h >> 10) & 1023) / 1024.0f,
                      fz = (float)gz + (float)((h >> 20) & 1023) / 1024.0f;
                float d = (fx - px) * (fx - px) + (fy - py) * (fy - py) + (fz - pz) * (fz - pz);
                best = d < best ? d : best;
            }
    return 1.0f - fminf(sqrtf(best), 1.0f);
}

static float sm_fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

/* gradient noise at point x, y, z of 32, f lattice cells a period (about -1..1) */
static float sm_perlin(int x, int y, int z, int f, int seed)
{
    float cs = 32.0f / (float)f, p[3] = {((float)x + 0.5f) / cs, ((float)y + 0.5f) / cs, ((float)z + 0.5f) / cs};
    int c[3];
    float t[3];
    for (int i = 0; i < 3; i++) {
        c[i] = (int)floorf(p[i]);
        t[i] = p[i] - (float)c[i];
    }
    float v[8];
    for (int k = 0; k < 8; k++) {
        int o[3] = {k & 1, (k >> 1) & 1, (k >> 2) & 1};
        uint32_t h = sm_hash((c[0] + o[0]) % f, (c[1] + o[1]) % f, (c[2] + o[2]) % f, seed);
        float g[3] = {(float)(h & 255) / 127.5f - 1.0f, (float)((h >> 8) & 255) / 127.5f - 1.0f,
                      (float)((h >> 16) & 255) / 127.5f - 1.0f};
        v[k] = g[0] * (t[0] - (float)o[0]) + g[1] * (t[1] - (float)o[1]) + g[2] * (t[2] - (float)o[2]);
    }
    float u = sm_fade(t[0]), w = sm_fade(t[1]), s = sm_fade(t[2]);
    float a = v[0] + u * (v[1] - v[0]), b = v[2] + u * (v[3] - v[2]), cc = v[4] + u * (v[5] - v[4]),
          d = v[6] + u * (v[7] - v[6]);
    float e = a + w * (b - a), g = cc + w * (d - cc);
    return e + s * (g - e);
}

/* its texels (272 x 136: 32 slices of 34 x 34, a texel of border each side), about 30 ms of the CPU: worked
   out on a thread of their own while the game starts (sm_noise_start), not in the frame that first needs them */
enum { SM_N = 32, SM_TW = 272, SM_TH = 136 };
static void *sm_noise_pixels(void *unused)
{
    (void)unused;
    enum { N = SM_N, TW = SM_TW, TH = SM_TH };
    float *ch = malloc(sizeof(float) * 4 * N * N * N);
    uint8_t *img = calloc(TW * TH, 4);
    if (!ch || !img)
        fatal("out of memory");
    float lo[4] = {1e9f, 1e9f, 1e9f, 1e9f}, hi[4] = {-1e9f, -1e9f, -1e9f, -1e9f};
    for (int z = 0; z < N; z++)
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                float pf = sm_perlin(x, y, z, 4, 1) + 0.5f * sm_perlin(x, y, z, 8, 2) + 0.25f * sm_perlin(x, y, z, 16, 3);
                float w4 = sm_worley(x, y, z, 4, 4), w8 = sm_worley(x, y, z, 8, 5), w16 = sm_worley(x, y, z, 16, 6);
                float v[4] = {0.5f * (pf * 0.5f + 0.5f) + 0.5f * (0.65f * w4 + 0.35f * w8), w8, w16, pf};
                float *o = ch + 4 * ((z * N + y) * N + x);
                for (int k = 0; k < 4; k++) {
                    o[k] = v[k];
                    lo[k] = fminf(lo[k], v[k]);
                    hi[k] = fmaxf(hi[k], v[k]);
                }
            }
    for (int sl = 0; sl < N; sl++)
        for (int ty = 0; ty < 34; ty++)
            for (int tx = 0; tx < 34; tx++) {
                int gx = (tx - 1) & 31, gy = (ty - 1) & 31;
                const float *v = ch + 4 * ((sl * N + gy) * N + gx);
                uint8_t *o = img + 4 * ((size_t)((sl >> 3) * 34 + ty) * TW + (size_t)((sl & 7) * 34 + tx));
                for (int k = 0; k < 4; k++)
                    o[k] = (uint8_t)lroundf(255.0f * (v[k] - lo[k]) / fmaxf(hi[k] - lo[k], 1e-6f));
            }
    free(ch);
    return img;
}

static pthread_t sm_noise_thr;
static int sm_noise_started;

static void sm_noise_start(void)
{
    sm_noise_started = !pthread_create(&sm_noise_thr, NULL, sm_noise_pixels, NULL);
}

static void sm_noise_make(void)
{
    enum { TW = SM_TW, TH = SM_TH };
    void *r = NULL;
    uint8_t *img = sm_noise_started && !pthread_join(sm_noise_thr, &r) ? r : sm_noise_pixels(NULL);
    sm_noise_started = 0;
    sm_noise = img_new(K_TEX, TW, TH, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    end_rendering();
    barrier(sm_noise, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBuffer buf;
    uint8_t *p;
    VkDeviceSize off = ring(TW * TH * 4, 16, &buf, &p);
    memcpy(p, img, TW * TH * 4);
    VkBufferImageCopy rg = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {TW, TH, 1}};
    vkCmdCopyBufferToImage(cb, buf, sm_noise->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    barrier(sm_noise, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    free(img);
}


/* A see-through surface of the view (draw_impl: blended by alpha, depth tested, no depth writes,
   in 0x419280's draws, not a puff's): kept to be drawn again at the view's end, its depth and alpha
   being what the puffs behind it are seen through */
static void sm_glass_add(GpuImg *c, const PushDraw *pc, int topo, int tnl, VkBuffer vbuf, VkBuffer ibuf, VkDeviceSize voff,
                         VkDeviceSize ioff, uint32_t nidx, uint32_t first, int32_t vbase, const int *sc,
                         VkCullModeFlags cull, VkFormat zfmt, const int *bb, float zmin)
{
    SmView *s = smv_depth > 0 && smv_depth <= 4 ? smv[smv_depth - 1] : NULL;
    if (!s || s->nglass >= SMOKE_GLASS)
        return;
    struct SmGlass *g = &s->glass[s->nglass++];
    g->pc = *pc;
    g->c = c;
    g->vbuf = vbuf;
    g->ibuf = ibuf;
    g->voff = voff;
    g->ioff = ioff;
    g->nidx = nidx;
    g->first = first;
    g->vbase = vbase;
    memcpy(g->sc, sc, sizeof g->sc);
    memcpy(g->bb, bb, sizeof g->bb);
    g->zmin = zmin;
    g->topo = topo;
    g->tnl = tnl;
    g->cull = cull;
    g->zfmt = zfmt;
}

/* the depth image in holder *a, made at w x h in format f if it is not (aux_img's for depth) */
static GpuImg *sm_depth_img(GpuImg **a, GpuImg **spare, uint32_t w, uint32_t h, VkFormat f)
{
    if (*a && (*a)->w == w && (*a)->h == h && (*a)->format == f)
        return *a;
    if (*spare && (*spare)->w == w && (*spare)->h == h && (*spare)->format == f) {
        GpuImg *t = *a;
        *a = *spare;
        *spare = t;
        return *a;
    }
    if (*spare)
        img_discard(*spare);
    *spare = *a;
    *a = img_new(K_DEPTH, w, h, 1, f, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    return *a;
}

/* shaders/smoke.frag's pass (0 the smaller depth, 1 the march, 2 applying, with mot the scene's
   masks too, 4 the puffs' shadow) and smokevis.frag (3, the sun grids) */
/* (color: the target's format: ZH_FORMAT for passes 0 and 4, AO_FORMAT 1, the view's colour 2, AUX_FORMAT 3) */
static VkPipeline smoke_pipeline(int pass, int mot, VkFormat color)
{
    uint32_t key = 0x08000000u | (uint32_t)pass | (uint32_t)!!mot << 4;
    for (int i = 0; i < npipes; i++)
        if (pipes[i].key == key && pipes[i].color == color)
            return pipes[i].p;
    if (npipes == COUNT(pipes))
        fatal("Vulkan: too many pipelines");
    Pipe *p = &pipes[npipes++];
    p->key = key;
    p->depth = VK_FORMAT_UNDEFINED;
    p->color = color;
    spec_pass = pass != 3 ? pass : -1;
    pipe_mot = mot ? 1u | 8u : 0;       /* (the masks; no motion vectors: the surfaces' stay) */
    p->p = pipeline_create(sm_smv, pass == 3 ? sm_smvis : mot ? sm_smfm : sm_smf, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0,
                           VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, p->color, VK_FORMAT_UNDEFINED, 0, 0, 0,
                           pass == 1 ? 3 : pass == 2 ? 2 : 0);
    spec_pass = -1;
    pipe_mot = 0;
    return p->p;
}

/* a pass over image g's rectangle r (one instance: the whole image, scissored) */
static void sm_pass(GpuImg *g, int pass, PushSmoke *pc, const int *r, const float *clear)
{
    begin_rendering(g, NULL);
    g->gpu_newer = 1;
    if (clear) {                        /* (all of it first) */
        VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {clear[0], clear[1], clear[2], clear[3]}}}};
        VkClearRect cr = {{{0, 0}, {g->w, g->h}}, 0, 1};
        vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    }
    bind_pipeline(smoke_pipeline(pass, 0, g->format));
    VkViewport vp = {0, 0, (float)g->w, (float)g->h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sr = {{r[0], r[1]}, {(uint32_t)(r[2] - r[0]), (uint32_t)(r[3] - r[1])}};
    vkCmdSetScissor(cb, 0, 1, &sr);
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *pc, pc);
    vkCmdDraw(cb, 6, 1, 0, 0);
    end_rendering();
    barrier(g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static int sm_far_first(const void *a, const void *b)
{
    float x = ((const SmPuff *)a)->dist, y = ((const SmPuff *)b)->dist;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* view-space point v -> render-target pixels (0: behind the camera) */
static int sm_project(const SmView *s, const float *v, float *px)
{
    float w = v[0] * s->P[0][3] + v[1] * s->P[1][3] + v[2] * s->P[2][3] + s->P[3][3];
    if (!(w > 1e-3f))
        return 0;
    float x = (v[0] * s->P[0][0] + v[1] * s->P[1][0] + v[2] * s->P[2][0] + s->P[3][0]) / w;
    float y = (v[0] * s->P[0][1] + v[1] * s->P[1][1] + v[2] * s->P[2][1] + s->P[3][1]) / w;
    px[0] = (s->pl[0] * x + s->pl[1] + 1.0f) * 0.5f * (float)s->c->w;
    px[1] = (s->pl[2] * y + s->pl[3] + 1.0f) * 0.5f * (float)s->c->h;
    return 1;
}

/* the view's puffs as volumes (shaders/smoke.frag, smokevis.frag) */
/* The view's puffs, first: the nearest SMOKE_MAX back to front, their rectangles, steps and block;
   the noise; the see-through surfaces drawn again; pass 0 (the smaller image's depths); with the sun
   shadows traced (slot >= 0: the view's acceleration structures) each puff's sun grid and, if
   shadows, the puffs' shadow image (pass 4: how much sun gets through them to the surface, a pixel
   per 2S x 2S), which vk_shadow_end's resolve takes. Returns that image for resolve.frag
   (slot * 16 + its scale), or 0. */
static float sm_first(SmView *s, int slot, int shadows)
{
    double t0 = wall_ms();
    s->stage = 2;
    if (sm_slots_used >= SMOKE_SLOTS || !s->has || !s->n)
        return 0.0f;
    GpuImg *c = s->c, *z = s->z;
    Frame *f = &fr[fi];
    int n = s->n;
    qsort(s->p, (size_t)n, sizeof *s->p, sm_far_first);
    SmPuff *p0 = s->p;
    if (n > SMOKE_MAX) {
        p0 += n - SMOKE_MAX;
        n = SMOKE_MAX;
    }
    int S = sm_scale > 0 ? sm_scale : res_aa > 1.5f && !(s->in_scene && scn.fsr) ? 4 : 2;
    uint32_t w2 = (c->w + (uint32_t)S - 1) / (uint32_t)S, h2 = (c->h + (uint32_t)S - 1) / (uint32_t)S;
    int v0 = s->sc[0] > 0 ? s->sc[0] : 0, v1 = s->sc[1] > 0 ? s->sc[1] : 0;
    int v2 = s->sc[2] < (int)c->w ? s->sc[2] : (int)c->w, v3 = s->sc[3] < (int)c->h ? s->sc[3] : (int)c->h;
    if (v2 <= v0 || v3 <= v1)
        return 0.0f;
    float P23 = s->P[2][3] != 0.0f ? s->P[2][3] : 1.0f;
    float focal = 0.5f * fabsf(s->P[1][1] * s->pl[2]) / fabsf(P23) * (float)c->h;    /* (pixels at depth 1) */
    /* the sun in view space (the view matrix turns world into view) */
    float Lv[3];
    for (int j = 0; j < 3; j++)
        Lv[j] = s->v.sun[0] * s->V[0][j] + s->v.sun[1] * s->V[1][j] + s->v.sun[2] * s->V[2][j];
    int u[4] = {1 << 30, 1 << 30, -(1 << 30), -(1 << 30)}, m = 0;
    double work = 0;
    for (int i = 0; i < n; i++) {
        SmPuff *p = &p0[i];
        float R = p->r;
        if (!(R > 0.25f) || !(p->sigma > 0.0f) || p->c[2] < -R)
            continue;
        float rc[4] = {(float)v0, (float)v1, (float)v2, (float)v3};
        if (p->c[2] - R > 1.0f) {       /* (else the camera is in it or next to it: the whole view) */
            rc[0] = rc[1] = 1e9f;
            rc[2] = rc[3] = -1e9f;
            for (int k = 0; k < 8; k++) {
                float v[3] = {p->c[0] + ((k & 1) ? R : -R), p->c[1] + ((k & 2) ? R : -R), p->c[2] + ((k & 4) ? R : -R)}, px[2];
                if (!sm_project(s, v, px)) {
                    rc[0] = (float)v0, rc[1] = (float)v1, rc[2] = (float)v2, rc[3] = (float)v3;
                    break;
                }
                rc[0] = fminf(rc[0], px[0]);
                rc[1] = fminf(rc[1], px[1]);
                rc[2] = fmaxf(rc[2], px[0]);
                rc[3] = fmaxf(rc[3], px[1]);
            }
        }
        rc[0] = fmaxf(floorf(rc[0] / (float)S), (float)(v0 / S));
        rc[1] = fmaxf(floorf(rc[1] / (float)S), (float)(v1 / S));
        rc[2] = fminf(ceilf(rc[2] / (float)S), (float)((v2 + S - 1) / S));
        rc[3] = fminf(ceilf(rc[3] / (float)S), (float)((v3 + S - 1) / S));
        if (rc[2] <= rc[0] || rc[3] <= rc[1])
            continue;
        memcpy(p->rect, rc, sizeof rc);
        /* where its shadow can fall: the ball swept away from the sun down to the ground below it */
        float D = (fmaxf(-p->m.ground_dy, 0.0f) + R) / fmaxf(s->v.sun[1], 0.15f);
        D = D < R ? R : D > 4000.0f ? 4000.0f : D;
        float sr[4] = {1e9f, 1e9f, -1e9f, -1e9f};
        int whole = 0;
        for (int k = 0; k < 16 && !whole; k++) {
            float e = (k & 8) ? D : 0.0f, v[3] = {p->c[0] + ((k & 1) ? R : -R) - Lv[0] * e,
                                                  p->c[1] + ((k & 2) ? R : -R) - Lv[1] * e,
                                                  p->c[2] + ((k & 4) ? R : -R) - Lv[2] * e}, px[2];
            if (!sm_project(s, v, px)) {
                whole = 1;
                break;
            }
            sr[0] = fminf(sr[0], px[0]);
            sr[1] = fminf(sr[1], px[1]);
            sr[2] = fmaxf(sr[2], px[0]);
            sr[3] = fmaxf(sr[3], px[1]);
        }
        if (whole) {
            sr[0] = (float)v0, sr[1] = (float)v1, sr[2] = (float)v2, sr[3] = (float)v3;
        }
        p->srect[0] = fmaxf(floorf(sr[0]), (float)v0);
        p->srect[1] = fmaxf(floorf(sr[1]), (float)v1);
        p->srect[2] = fminf(ceilf(sr[2]), (float)v2);
        p->srect[3] = fminf(ceilf(sr[3]), (float)v3);
        float rpx = R * focal / fmaxf(p->c[2], 1.0f) / (float)S;
        int steps = sm_steps > 0 ? sm_steps : (int)(rpx / 3.0f) + 3;
        p->steps = steps < 3 ? 3 : steps > 10 ? 10 : steps;
        work += (double)(rc[2] - rc[0]) * (rc[3] - rc[1]) * p->steps;
        u[0] = (int)rc[0] < u[0] ? (int)rc[0] : u[0];
        u[1] = (int)rc[1] < u[1] ? (int)rc[1] : u[1];
        u[2] = (int)rc[2] > u[2] ? (int)rc[2] : u[2];
        u[3] = (int)rc[3] > u[3] ? (int)rc[3] : u[3];
        p0[m++] = *p;
    }
    n = m;
    if (!n)
        return 0.0f;
    double area = (double)w2 * h2;
    if (!sm_steps && work > sm_budget * area) {     /* (coarser steps for all) */
        double k = sm_budget * area / work;
        work = 0;
        for (int i = 0; i < n; i++) {
            int st = (int)lround(p0[i].steps * k);
            p0[i].steps = st < 2 ? 2 : st;
            work += (double)(p0[i].rect[2] - p0[i].rect[0]) * (p0[i].rect[3] - p0[i].rect[1]) * p0[i].steps;
        }
        /* still too much at 2 steps (the camera in a cloud of them): the farthest go, behind the
           nearer ones */
        while (n > 1 && work > 2.0 * sm_budget * area) {
            work -= (double)(p0[0].rect[2] - p0[0].rect[0]) * (p0[0].rect[3] - p0[0].rect[1]) * p0[0].steps;
            p0++;
            n--;
        }
    }
    /* the sun grids: with this view's sun shadows traced */
    int grid = slot >= 0 ? slot < SH_SLOTS && sm_smvis
                         : sm_rt.ok && sm_rt.c == c && sm_rt.frame == nframes && sm_rt.slot < SH_SLOTS && sm_smvis;
    int rslot = slot >= 0 ? slot : sm_rt.slot;
    /* the shadows' casters: the nearest SMOKE_SHADOWS, and the area they darken */
    int ns = shadows && sm_shadows ? (n < SMOKE_SHADOWS ? n : SMOKE_SHADOWS) : 0;
    int us[4] = {1 << 30, 1 << 30, -(1 << 30), -(1 << 30)};
    for (int i = n - ns; i < n; i++) {
        const float *r = p0[i].srect;
        us[0] = (int)r[0] < us[0] ? (int)r[0] : us[0];
        us[1] = (int)r[1] < us[1] ? (int)r[1] : us[1];
        us[2] = (int)r[2] > us[2] ? (int)r[2] : us[2];
        us[3] = (int)r[3] > us[3] ? (int)r[3] : us[3];
    }
    if (ns && (us[2] <= us[0] || us[3] <= us[1]))
        ns = 0;
    /* the view's block */
    uint32_t base = (uint32_t)(fi * SMOKE_SLOTS + sm_slots_used++) * SMOKE_BLOCK;
    float *B = (float *)sm_buf.map + (size_t)base * 4;
    memset(B, 0, SMOKE_HDR * 16);
    B[0] = s->P[0][0];
    B[1] = s->P[1][1];
    B[2] = s->P[2][0];
    B[3] = s->P[2][1];
    memcpy(&B[4], s->pl, 4 * sizeof(float));
    B[8] = s->P[2][2];
    B[9] = s->P[3][2];
    B[10] = P23;
    B[11] = (float)ns;
    B[12] = s->pl[4];
    B[13] = s->pl[5];
    B[14] = 1.0f / (float)c->w;
    B[15] = 1.0f / (float)c->h;
    for (int i = 0; i < 3; i++)         /* view -> world: the view matrix's rows (it turns world into view) */
        for (int j = 0; j < 3; j++)
            B[16 + 4 * i + j] = s->V[i][j];
    for (int j = 0; j < 3; j++) {
        B[28 + j] = s->v.sun[j];
        B[32 + j] = s->v.sun_color[j];
        B[36 + j] = s->ambient & 0xffffff ? (float)((s->ambient >> (16 - 8 * j)) & 255) / 255.0f : 0.35f;
        B[44 + j] = (float)((s->fog_col >> (16 - 8 * j)) & 255) / 255.0f;
    }
    B[31] = (float)grid;
    B[39] = s->fogp[3];
    memcpy(&B[40], s->fogp, 3 * sizeof(float));
    B[48] = s->fog_edge[0];
    B[49] = s->fog_edge[1];
    /* headlights VOLUMETRIC: the ray-traced view whose beams light the puffs (-1 none) */
    int bslot = slot >= 0 ? slot : sm_rt.ok && sm_rt.c == c && sm_rt.frame == nframes ? sm_rt.slot : -1;
    const ShadowGlobals *BG = bslot >= 0 && bslot < SH_SLOTS ? (const ShadowGlobals *)sh_buf.map + bslot : NULL;
    B[52] = BG && BG->beamn[0] > 0 && BG->hl3[3] > 0 ? (float)bslot : -1.0f;
    for (int i = 0; i < n; i++) {
        const SmPuff *p = &p0[i];
        float *o = B + 4 * (SMOKE_HDR + SMOKE_PV * i);
        memcpy(o, p->c, 3 * sizeof(float));
        o[3] = p->r;
        memcpy(o + 4, p->col, 3 * sizeof(float));
        o[7] = p->sigma;
        o[8] = (float)p->m.kind;
        o[9] = (float)((p->m.serial * 2654435761u) >> 8) / 16777216.0f;
        o[10] = p->m.age;
        o[11] = p->m.floor_dy;
        memcpy(o + 12, p->rect, 4 * sizeof(float));
        o[16] = p->m.light;
        o[17] = (float)p->steps;
        o[18] = grid ? (float)i : -1.0f;
        o[19] = 0.0f;
        memcpy(o + 20, p->srect, 4 * sizeof(float));
        st_sm_steps += p->steps;
    }
    if (sm_log && nframes % 25 == 0) {             /* TD6_SMOKE_LOG=1: the view and its nearest puffs */
        logmsg(LOG_ALWAYS, "smoke: frame %lu view %ux%u S %d puffs %d (taken %d) grid %d shadows %d sun %.2f %.2f %.2f colour "
               "%.2f %.2f %.2f ambient %08x fog %.0f %.0f how %.0f work %.2f", nframes, c->w, c->h, S, n, s->n, grid, ns,
               s->v.sun[0], s->v.sun[1], s->v.sun[2], s->v.sun_color[0], s->v.sun_color[1], s->v.sun_color[2], s->ambient,
               s->fogp[0], s->fogp[1], s->fogp[3], work / area);
        for (int i = n - 1; i >= 0 && i >= n - 6; i--)
            logmsg(LOG_ALWAYS, "smoke:   kind %d serial %u r %.1f sigma %.4f dist %.0f c %.0f %.0f %.0f col %.3f %.3f %.3f light %.2f "
                   "steps %d grow %.2f age %.0f floor %.0f ground %.0f", p0[i].m.kind, p0[i].m.serial, p0[i].r, p0[i].sigma,
                   p0[i].dist, p0[i].c[0], p0[i].c[1], p0[i].c[2], p0[i].col[0], p0[i].col[1], p0[i].col[2], p0[i].m.light,
                   p0[i].steps, p0[i].m.grow, p0[i].m.age, p0[i].m.floor_dy, p0[i].m.ground_dy);
    }
    st_sm_views++;
    st_sm_drawn += (unsigned long)n;
    st_sm_grid += grid ? (unsigned long)n : 0;
    st_sm_casters += (unsigned long)ns;
    st_sm_area += work / area;
    /* the passes */
    end_rendering();
    if (!sm_noise)
        sm_noise_make();
    if (z->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)  /* (vk_shadow_end's already is) */
        barrier(z, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    static GpuImg *spare[4];
    aux_img(&att_smz, &spare[0], w2, h2, ZH_FORMAT);
    int timed = st_queries && !(f->sm_timed & 1);
    uint32_t tsb = TS * (uint32_t)fi;
    if (timed)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 11);
    int q[4] = {u[0], u[1], u[2] < (int)w2 ? u[2] : (int)w2, u[3] < (int)h2 ? u[3] : (int)h2};
    /* the see-through surfaces over the puffs' area drawn again into images the smaller image's size
       (as drawn: alpha test, the distance fade's dither), the nearest's depth and alpha kept: puffs
       behind glass are seen through it */
    int ng = 0;
    for (int i = 0; i < s->nglass; i++) {
        struct SmGlass *g = &s->glass[i];
        g->tnl = g->c == c && g->zfmt == z->format ? g->tnl : -1;     /* (-1: not this view's) */
        int over = 0;           /* over a puff on screen and nearer than its far side */
        for (int j = 0; j < n && g->tnl >= 0 && !over; j++)
            over = g->bb[0] < (int)p0[j].rect[2] * S && g->bb[2] > (int)p0[j].rect[0] * S &&
                   g->bb[1] < (int)p0[j].rect[3] * S && g->bb[3] > (int)p0[j].rect[1] * S &&
                   g->zmin / fabsf(P23) < p0[j].c[2] + p0[j].r;
        if (!over)
            g->tnl = -1;
        ng += g->tnl >= 0;
    }
    if (ng && sm_glass && q[2] > q[0] && q[3] > q[1]) {
        static GpuImg *gspare[2];
        aux_img(&att_sms, &gspare[0], w2, h2, RT_FORMAT);
        sm_depth_img(&att_smsz, &gspare[1], w2, h2, z->format);
        begin_rendering(att_sms, att_smsz);
        att_sms->gpu_newer = 1;
        VkClearAttachment gc[2] = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {0, 0, 0, 0}}}},
                                   {VK_IMAGE_ASPECT_DEPTH_BIT, 0, {.depthStencil = {1.0f, 0}}}};
        VkClearRect gr = {{{q[0], q[1]}, {(uint32_t)(q[2] - q[0]), (uint32_t)(q[3] - q[1])}}, 0, 1};
        vkCmdClearAttachments(cb, 2, gc, 1, &gr);
        for (int i = 0; i < s->nglass; i++) {
            struct SmGlass *g = &s->glass[i];
            if (g->tnl < 0)
                continue;
            int sc[4] = {(g->bb[0] > g->sc[0] ? g->bb[0] : g->sc[0]) / S, (g->bb[1] > g->sc[1] ? g->bb[1] : g->sc[1]) / S,
                         ((g->bb[2] < g->sc[2] ? g->bb[2] : g->sc[2]) + S - 1) / S,
                         ((g->bb[3] < g->sc[3] ? g->bb[3] : g->sc[3]) + S - 1) / S};
            sc[0] = sc[0] > q[0] ? sc[0] : q[0];
            sc[1] = sc[1] > q[1] ? sc[1] : q[1];
            sc[2] = sc[2] < q[2] ? sc[2] : q[2];
            sc[3] = sc[3] < q[3] ? sc[3] : q[3];
            if (sc[2] <= sc[0] || sc[3] <= sc[1])
                continue;
            bind_pipeline(draw_pipeline(g->topo, 0, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, g->zfmt, g->tnl, 5, 0));
            set_state(att_sms, sc, g->cull, 1, 1, VK_COMPARE_OP_LESS, 0);
            vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof g->pc, &g->pc);
            vkCmdBindVertexBuffers(cb, 0, 1, &g->vbuf, &g->voff);
            vkCmdBindIndexBuffer(cb, g->ibuf, g->ioff, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, g->nidx, 1, g->first, g->vbase, 0);
            st_draws++;
        }
        end_rendering();
        barrier(att_sms, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(att_smsz, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        st_sm_glass += (unsigned long)ng;
    } else
        ng = 0;
    PushSmoke pc = {{base, (uint32_t)n, (uint32_t)z->slot, 0},
                    {(uint32_t)att_smz->slot, (uint32_t)sm_noise->slot, (uint32_t)smp_linear, (uint32_t)smp_nearest},
                    {(float)w2, (float)h2, (float)S, (float)sm_debug},
                    {0.6f * taa_react, 0.8f, (float)rslot, 1.0f},
                    {ng ? (uint32_t)att_smsz->slot : 0, ng ? (uint32_t)att_sms->slot : 0, ng ? 1u : 0u, 0}};
    sm_pass(att_smz, 0, &pc, q, NULL);
    if (grid) {
        aux_img(&att_smg, &spare[2], 9, 3 * SMOKE_MAX, AUX_FORMAT);
        PushSmoke pg = pc;
        pg.c[0] = 9.0f;
        pg.c[1] = (float)(3 * SMOKE_MAX);
        int gr[4] = {0, 0, 9, 3 * n};
        sm_pass(att_smg, 3, &pg, gr, NULL);
    }
    float out = 0.0f;
    if (ns) {                           /* the puffs' shadow: pass 4 over where they fall, a pixel per 2S x 2S */
        int S4 = 2 * S;
        uint32_t w4 = (c->w + (uint32_t)S4 - 1) / (uint32_t)S4, h4 = (c->h + (uint32_t)S4 - 1) / (uint32_t)S4;
        aux_img(&att_smsh, &spare[3], w4, h4, ZH_FORMAT);
        PushSmoke ps = pc;
        ps.c[0] = (float)w4;
        ps.c[1] = (float)h4;
        ps.c[2] = (float)S4;
        int sr[4] = {us[0] / S4, us[1] / S4, (us[2] + S4 - 1) / S4, (us[3] + S4 - 1) / S4};
        sr[2] = sr[2] < (int)w4 ? sr[2] : (int)w4;
        sr[3] = sr[3] < (int)h4 ? sr[3] : (int)h4;
        sm_pass(att_smsh, 4, &ps, sr, (const float[4]){1, 0, 0, 0});
        out = (float)(att_smsh->slot * 16 + S4);
    }
    if (timed && slot >= 0) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 12);
        f->sm_timed |= 1;
    }
    s->ts_open = timed && slot < 0;     /* (at the view's end: one window for both, sm_last ends it) */
    s->stage = 1;
    s->p0 = p0;
    s->np = n;
    s->S = S;
    s->w2 = w2;
    s->h2 = h2;
    s->grid = grid;
    memcpy(s->vr, (int[4]){v0, v1, v2, v3}, sizeof s->vr);
    memcpy(s->q, (int[4]){u[0], u[1], u[2] < (int)w2 ? u[2] : (int)w2, u[3] < (int)h2 ? u[3] : (int)h2}, sizeof s->q);
    s->pc = pc;
    st_sm_cpu_ms += wall_ms() - t0;
    return out;
}

/* ... then the march (pass 1: into the smaller image, back to front) and onto the view (pass 2) */
static void sm_last(SmView *s)
{
    double t0 = wall_ms();
    if (s->stage != 1)
        return;
    GpuImg *c = s->c;
    Frame *f = &fr[fi];
    int S = s->S, n = s->np, *q = s->q, *v = s->vr;
    uint32_t w2 = s->w2, h2 = s->h2;
    PushSmoke pc = s->pc;
    static GpuImg *spare;
    aux_img(&att_sma, &spare, w2, h2, AO_FORMAT);
    int timed = st_queries && !(f->sm_timed & 2);
    uint32_t tsb = TS * (uint32_t)fi;
    if (timed && !s->ts_open)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 13);
    begin_rendering(att_sma, NULL);
    att_sma->gpu_newer = 1;
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {0, 0, 0, 1}}}};
    VkClearRect cr = {{{q[0], q[1]}, {(uint32_t)(q[2] - q[0]), (uint32_t)(q[3] - q[1])}}, 0, 1};
    vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    bind_pipeline(smoke_pipeline(1, 0, att_sma->format));
    VkViewport vp = {0, 0, (float)w2, (float)h2, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &cr.rect);
    pc.a[3] = s->grid ? (uint32_t)att_smg->slot : 0;
    pc.d[3] = 0.0f;
    pc.e[3] = (uint32_t)sm_clip;
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cb, 6, (uint32_t)n, 0, 0);
    end_rendering();
    barrier(att_sma, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (s->z->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)   /* (drawn into since sm_first) */
        barrier(s->z, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    /* onto the view */
    want_mot = s->in_scene;             /* (in temporal anti-aliasing's scene: its masks too) */
    begin_rendering(c, NULL);
    want_mot = 0;
    c->gpu_newer = 1;
    bind_pipeline(smoke_pipeline(2, r_mot, c->format));
    VkViewport vf = {0, 0, (float)c->w, (float)c->h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vf);
    int x0 = q[0] * S > v[0] ? q[0] * S : v[0], y0 = q[1] * S > v[1] ? q[1] * S : v[1];
    int x1 = q[2] * S < v[2] ? q[2] * S : v[2], y1 = q[3] * S < v[3] ? q[3] * S : v[3];
    VkRect2D fr2 = {{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}};
    vkCmdSetScissor(cb, 0, 1, &fr2);
    pc.a[3] = (uint32_t)att_sma->slot;
    pc.c[0] = (float)c->w;
    pc.c[1] = (float)c->h;
    pc.d[3] = 1.0f;
    pc.e[3] = (uint32_t)sm_clip;
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cb, 6, 1, 0, 0);
    st_draws += 4;
    if (timed && s->ts_open) {          /* (the window sm_first opened: 12 and 13 together) */
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 12);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 13);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 14);
        f->sm_timed |= 1 | 2 | 4;
    } else if (timed) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 14);
        f->sm_timed |= 2;
    }
    s->ts_open = 0;
    st_sm_cpu_ms += wall_ms() - t0;
}

/* vk_shadow_end, its acceleration structures built: with the sun shadows traced and puffs in the
   view, their first passes now and their shadow image for the resolve (resolve.frag: slot * 16 +
   its scale, 0 none) */
static float vk_smoke_early(GpuImg *c, GpuImg *z, int slot)
{
    SmView *s = smv_depth > 0 && smv_depth <= 4 ? smv[smv_depth - 1] : NULL;
    if (!sm_shadows || !s || !s->has || !s->n || s->stage || s->c != c || s->z != z || sm_debug == 7)
        return 0.0f;
    return sm_first(s, slot, 1);
}

void vkr_smoke_view_end(void)
{
    if (smv_depth <= 0)
        return;
    smv_depth--;
    if (smv_depth >= 4)
        return;
    SmView *s = smv[smv_depth];
    if (s->has && s->n && vk_on() && sm_debug != 7) {   /* (TD6_SMOKE_DEBUG=7: the puffs taken, none drawn) */
        if (!s->stage)
            sm_first(s, -1, 0);
        sm_last(s);
    }
    s->has = 0;
    s->n = 0;
    s->nglass = 0;
    s->stage = 0;
}

/* ------------------------------------------------------------------ colour precision */
/* (DESIGN.md decision 4, "Colour precision") With "colour_precision = high" the race's main view is drawn into
   the scene's colour at 16 bits a channel (hp_format, R16G16B16A16_UNORM): with FSR 2 its input, else an image
   of the back buffer's own size (vk_scene_begin .. vk_scene_end without the jitter, vectors and masks). The
   values are the game's own, gamma space as Direct3D 7 blended them, clamped to 0..1 as the 8-bit target
   clamps them; only their rounding goes: every blend, the fog, the ambient occlusion's darkening, the shadows'
   taking away (the sun's part in 10 bits: aux_format), the reflections and the smoke's layers keep their
   fractions. At the scene's end the copy into the 8-bit back buffer (taa.frag; after FSR 2 where it runs, at
   the screen's size) adds a dither of about one level: a fixed 64 x 64 blue-noise pattern (noise_sync) in
   screen space, its ranks turned into a triangular spread of -1..1 level, less towards black and white (none
   at 0 and 1), so the last steps of a gradient become a fine grain that does not crawl or flicker (the same
   pattern every frame, and after FSR 2's history). What is drawn after the scene (the mirrors, name labels,
   HUD, menus) draws into the back buffer as before, not dithered. The output step (vk_present, present.frag)
   stays 8-bit: an HDR swapchain later reads a 16-bit back buffer instead (DESIGN.md has what it needs).
   To compare: TD6_HP_DITHER=0 rounds without the dither; TD6_HP_FORMAT=float takes half floats, =10
   A2B10G10R10 for the colour too; TD6_HP_SUN=8 or 16 the sun's part in 8 or 16 bits; TD6_HP_FLIP=n turns
   HIGH on and off every n frames (with TD6_VK_STATS=2's per-frame GPU times: the two timed side by side). */

/* a 64 x 64 blue-noise pattern (void and cluster, Ulichney 1993; sigma 1.5): each pixel's rank 0..4095 */
#define BN 64
static void bn_splat(float *e, const float *lut, int p, float s)
{
    int px = p % BN, py = p / BN;
    for (int y = 0; y < BN; y++) {
        const float *l = lut + ((y - py) & (BN - 1)) * BN;
        float *r = e + y * BN;
        for (int x = 0; x < BN; x++)
            r[x] += s * l[(x - px) & (BN - 1)];
    }
}

static int bn_pick(const float *e, const uint8_t *bit, int want, int most)
{
    int best = -1;
    for (int i = 0; i < BN * BN; i++)
        if (bit[i] == want && (best < 0 || (most ? e[i] > e[best] : e[i] < e[best])))
            best = i;
    return best;
}

static void blue_noise(uint16_t *rank)
{
    enum { N = BN * BN };
    float *lut = malloc(N * sizeof *lut), *e = calloc(N, sizeof *e), *e0 = malloc(N * sizeof *e0);
    uint8_t *bit = calloc(N, 1), *bit0 = malloc(N);
    if (!lut || !e || !e0 || !bit || !bit0)
        fatal("out of memory");
    for (int y = 0; y < BN; y++)
        for (int x = 0; x < BN; x++) {
            int dx = x < BN - x ? x : BN - x, dy = y < BN - y ? y : BN - y;
            lut[y * BN + x] = expf(-(float)(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f));
        }
    /* a tenth of the pixels at random (a fixed seed), then moved from the tightest cluster into the largest
       void until that is where it came from */
    uint32_t seed = 0x2545f491u;
    int ones = 0;
    while (ones < N / 10) {
        seed = seed * 1664525u + 1013904223u;
        int p = (int)(seed >> 20) & (N - 1);
        if (!bit[p]) {
            bit[p] = 1;
            bn_splat(e, lut, p, 1.0f);
            ones++;
        }
    }
    for (int it = 0; it < N; it++) {
        int c = bn_pick(e, bit, 1, 1);
        bit[c] = 0;
        bn_splat(e, lut, c, -1.0f);
        int v = bn_pick(e, bit, 0, 0);
        bit[v] = 1;
        bn_splat(e, lut, v, 1.0f);
        if (v == c)
            break;
    }
    memcpy(bit0, bit, N);
    memcpy(e0, e, N * sizeof *e);
    for (int r = ones - 1; r >= 0; r--) {   /* the pattern's own pixels: the tightest cluster ranks last */
        int c = bn_pick(e, bit, 1, 1);
        bit[c] = 0;
        bn_splat(e, lut, c, -1.0f);
        rank[c] = (uint16_t)r;
    }
    memcpy(bit, bit0, N);
    memcpy(e, e0, N * sizeof *e);
    int r = ones;
    for (; r < N / 2; r++) {                /* up to half: the largest void next */
        int v = bn_pick(e, bit, 0, 0);
        bit[v] = 1;
        bn_splat(e, lut, v, 1.0f);
        rank[v] = (uint16_t)r;
    }
    memset(e, 0, N * sizeof *e);            /* the rest: the pixels still empty, their tightest cluster next */
    for (int i = 0; i < N; i++)
        if (!bit[i])
            bn_splat(e, lut, i, 1.0f);
    for (; r < N; r++) {
        int c = bn_pick(e, bit, 0, 1);
        bit[c] = 1;
        bn_splat(e, lut, c, -1.0f);
        rank[c] = (uint16_t)r;
    }
    free(lut);
    free(e);
    free(e0);
    free(bit);
    free(bit0);
}

/* the dither's pattern on the GPU (made the first time it is needed: about 40 ms); 0 if it cannot be */
static int noise_sync(void)
{
    if (noise_img)
        return 1;
    if (!hp_format)
        return 0;
    uint16_t rank[BN * BN];
    uint32_t px[BN * BN];
    double t0 = wall_ms();
    blue_noise(rank);
    for (int i = 0; i < BN * BN; i++)   /* r: (rank + 1/2) / 4096 */
        px[i] = (uint32_t)lround(((double)rank[i] + 0.5) / (BN * BN) * 65535.0);
    noise_img = img_new(K_TEX, BN, BN, 1, NOISE_FORMAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    end_rendering();
    barrier(noise_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    upload_bytes(noise_img, 0, (const uint8_t *)px, BN, BN);
    barrier(noise_img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    LOG(LOG_D3D, "vk: colour precision: dither pattern made in %.1f ms", wall_ms() - t0);
    return 1;
}

/* ------------------------------------------------------------------ temporal anti-aliasing */
/* (DESIGN.md decision 4, "Temporal anti-aliasing") With "antialiasing = fsr" the race's main view
   (taa.c: vk_scene_begin .. vk_scene_end, 0x465950 from the race frame) is drawn into images of
   its own at 1 / render scale of the back buffer's size: colour, depth, motion vectors and masks
   (attachments 3 and 4 of its draws: draw.frag MOTION), moved by a sub-pixel jitter each frame
   (FSR 2's Halton 2, 3 sequence) in the draws' clip-space placement, so the ray-traced passes,
   which work from that placement and the depth, see the same jitter. Each draw gets a Motion
   block (motion.glsl): the matrix to where its vertices were in the previous frame. At the view's
   end FSR 2 resolves the images into an image of the back buffer's size (fsr2.cpp), drawn into the
   back buffer over the view's area (shaders/taa.frag); the frame's other draws (mirrors, HUD, name
   labels, menus) follow there at the screen's size, without jitter, as before.

   Where things were. The game's vertices are camera-relative world space: the terrain and the
   sky dome written so each frame (their world matrix the identity), objects and cars with world
   matrices whose translation is camera-relative. With d the camera's move since the previous
   frame (its 16.16 positions' difference, so the map's wrap does not matter), a static thing was
   at p W T(d) V' P' (V', P' the previous frame's view and projection); the sky dome moves with
   the camera: p V' P'. Moving things (cars, wheels, debris, flags, animated objects) are matched
   with the previous frame's draws: by their mesh (vertex and index counts, texture, first indices
   and texture coordinates: not the positions, which damage may change) and their place (from
   where it was to three of its last moves on, or moved with the camera: "matching draws" below),
   and then take p W' V' P' with that draw's world matrix W'. Unmatched draws are taken as static
   (new things in view; static ones also match exactly).

   Masks. Depth-writing draws write the motion vector; the sky dome too (the background, no depth
   writes); solid see-through parts (cut-out lattices, fences: placed models, not the CPU's
   sprites and bits) where they are solid; the water's colour pass (its own surface). Blended
   draws without depth writes mark the reactive mask (FSR 2
   then keeps less history there: smoke, dust, sparks, glows, glass) or the transparency and
   composition mask (reflections, the water: what they show changes with the view) by their alpha
   or their added brightness. TD6_TAA_DEBUG=1 shows the motion vectors, 2 the masks, 3 the depth,
   4 the scene image scaled up without FSR 2; TD6_TAA_JITTER=0 no jitter, TD6_TAA_MATCH=0 no
   matching (everything static), TD6_TAA_SHARPEN (0..1, below 0 none) RCAS's sharpness,
   TD6_TAA_REACTIVE scales the reactive mask, TD6_TAA_LOG=1 logs camera cuts. */

/* float -> half (for the mask strengths) */
static uint16_t f2h(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000u, e = (u >> 23) & 255u, m = u & 0x7fffffu;
    if (e < 113)
        return (uint16_t)s;
    if (e > 142)
        return (uint16_t)(s | 0x7c00u);
    return (uint16_t)(s | (e - 112) << 10 | m >> 13);
}

static float halton(uint32_t i, uint32_t b)
{
    float f = 1.0f, r = 0.0f;
    for (; i; i /= b) {
        f /= (float)b;
        r += f * (float)(i % b);
    }
    return r;
}

/* 4 x 4 row-major (D3D row vectors: p M) */
static void m4_mul(double *r, const double *a, const double *b)
{
    double t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
    memcpy(r, t, sizeof t);
}

static void m4_of(double *r, const Mat4 *m)
{
    for (int i = 0; i < 16; i++)
        r[i] = m->m[i / 4][i % 4];
}

static void m4_ident(double *r)
{
    for (int i = 0; i < 16; i++)
        r[i] = i % 5 == 0;
}

/* TnlConst.world (3 rows of a column-vector 3 x 4) -> a D3D world matrix */
static void m4_world(double *r, const float (*w)[4])
{
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++)
            r[4 * j + i] = w[i][j];
        r[12 + i] = w[i][3];
        r[4 * i + 3] = 0.0;
    }
    r[15] = 1.0;
}

/* D3D clip space -> Vulkan clip space (vk.c's placement ax bx ay by mnz dz, jittered by jx, jy) */
static void m4_place(double *r, const float *pl, float jx, float jy)
{
    memset(r, 0, 16 * sizeof *r);
    r[0] = pl[0];
    r[5] = pl[2];
    r[10] = pl[5];
    r[12] = (double)pl[1] + jx;
    r[13] = (double)pl[3] + jy;
    r[14] = pl[4];
    r[15] = 1.0;
}

/* ---- matching draws with the previous frame's (moving things)
   The game moves its world in steps of 10 ms (100 a second) and draws a frame after however many
   steps were due: none, one, two or more (a frame rate above 100 has frames where nothing moved;
   at 60 one step and two take turns). So a thing is looked for anywhere from where it was to three
   of its last moves on (a move of 0, a frame without a step, keeps the one before), or where it
   was moved as the camera was (the player's car, and things alongside it, found again at once). */
#define MREC_MAX 8192
#define MENT_MAX (MREC_MAX * 5)
#define MBUCKETS 16384
#define MCELL 128.0f              /* units */
typedef struct {
    uint64_t key;                 /* the mesh (draw_key) */
    float t[3];                   /* its world translation (camera-relative in its frame) */
    float v[3];                   /* its last move in a frame (0: still, or not known) */
    float w[3][4];                /* its world matrix (TnlConst.world) */
    int known, used;              /* known: matched in its frame (v is its own) */
} MRec;
typedef struct {
    int rec, next;
} MEnt;
static MRec mrec[2][MREC_MAX];
static MEnt ment[2][MENT_MAX];    /* a record is in the cells of the places it may be found at */
static int mrec_n[2], ment_n[2], mrec_head[2][MBUCKETS], mcur;

static uint64_t draw_key(const void *raw, int nv, const uint16_t *idx, int ni, int tex)
{
    uint64_t h = 1469598103934665603ull;
#define MIX(x) h = (h ^ (uint64_t)(x)) * 1099511628211ull
    MIX(nv);
    MIX(ni);
    MIX(tex + 1);
    for (int i = 0; i < ni && i < 12; i++)
        MIX(idx[i]);
    for (int i = 0; i < 3 && raw; i++) {       /* texture coordinates of the first, middle and last vertices */
        uint32_t u[2];
        memcpy(u, (const uint8_t *)raw + 40 * (size_t)(i * (nv - 1) / 2) + 32, 8);
        MIX(u[0]);
        MIX(u[1]);
    }
#undef MIX
    return h;
}

static uint32_t mbucket(uint64_t key, int x, int y, int z)
{
    uint64_t h = key ^ (uint64_t)(uint32_t)x * 0x9e3779b97f4a7c15ull ^ (uint64_t)(uint32_t)y * 0xc2b2ae3d27d4eb4full ^
                 (uint64_t)(uint32_t)z * 0x165667b19e3779f9ull;
    return (uint32_t)(h ^ h >> 29) & (MBUCKETS - 1);
}

static float len3(const float *a)
{
    return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

/* how far q (a place in the previous frame's camera space) is from where record r could be now,
   as a share of how far it may be (under 1: it may be r), with the camera's move d */
static float match_score(const MRec *r, const float *q, const float *d)
{
    float e[3] = {q[0] - r->t[0], q[1] - r->t[1], q[2] - r->t[2]};
    float vv = r->v[0] * r->v[0] + r->v[1] * r->v[1] + r->v[2] * r->v[2], s = 0.0f;
    if (vv > 1e-6f) {             /* along its own move: from no step to three moves on */
        s = (e[0] * r->v[0] + e[1] * r->v[1] + e[2] * r->v[2]) / vv;
        s = s < 0.0f ? 0.0f : s > 3.0f ? 3.0f : s;
    }
    float o[3] = {e[0] - s * r->v[0], e[1] - s * r->v[1], e[2] - s * r->v[2]};
    /* a known thing within 8 units plus 1.5 times its move; one not seen moving yet within 64 */
    float a = len3(o) / (r->known ? 8.0f + 1.5f * sqrtf(vv) : 64.0f);
    float c[3] = {e[0] - d[0], e[1] - d[1], e[2] - d[2]};           /* moved as the camera was */
    float b = len3(c) / (8.0f + 0.5f * len3(d));
    return a < b ? a : b;
}

/* the previous frame's draw of this mesh that q (its place in the previous frame's camera space, if
   it stood still) is, or NULL; d the camera's move since */
static MRec *match_find(uint64_t key, const float *q, const float *d)
{
    int p = mcur ^ 1;
    if (!mrec_n[p])
        return NULL;
    int cx = (int)floorf(q[0] / MCELL), cy = (int)floorf(q[1] / MCELL), cz = (int)floorf(q[2] / MCELL);
    MRec *best = NULL;
    float bs = 1.0f;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++)
                for (int i = mrec_head[p][mbucket(key, cx + dx, cy + dy, cz + dz)]; i >= 0; i = ment[p][i].next) {
                    MRec *r = &mrec[p][ment[p][i].rec];
                    if (r->key != key || r->used)
                        continue;
                    float s = match_score(r, q, d);
                    if (s < bs) {
                        bs = s;
                        best = r;
                    }
                }
    return best;
}

/* this frame's draw of a mesh (key): its world matrix w, its last move v (known: its own), the
   camera's move d; filed under the places it may be found at next frame */
static void match_add(uint64_t key, const float (*w)[4], const float *v, int known, const float *d)
{
    int c = mcur;
    if (mrec_n[c] >= MREC_MAX || ment_n[c] + 5 > MENT_MAX)
        return;
    int n = mrec_n[c]++;
    MRec *r = &mrec[c][n];
    r->key = key;
    memcpy(r->w, w, sizeof r->w);
    for (int i = 0; i < 3; i++) {
        r->t[i] = w[i][3];
        r->v[i] = v[i];
    }
    r->known = known;
    r->used = 0;
    /* where it was, 1.5 and 3 moves on, 1 and 2 of the camera's on: each cell once */
    static const float kv[5] = {0.0f, 1.5f, 3.0f, 0.0f, 0.0f}, kd[5] = {0.0f, 0.0f, 0.0f, 1.0f, 2.0f};
    int cells[5][3], nc = 0;
    for (int j = 0; j < 5; j++) {
        int cl[3];
        for (int i = 0; i < 3; i++)
            cl[i] = (int)floorf((r->t[i] + kv[j] * v[i] + kd[j] * d[i]) / MCELL);
        int dup = 0;
        for (int k = 0; k < nc && !dup; k++)
            dup = cells[k][0] == cl[0] && cells[k][1] == cl[1] && cells[k][2] == cl[2];
        if (dup)
            continue;
        memcpy(cells[nc++], cl, sizeof cl);
        uint32_t b = mbucket(key, cl[0], cl[1], cl[2]);
        MEnt *m = &ment[c][ment_n[c]];
        m->rec = n;
        m->next = mrec_head[c][b];
        mrec_head[c][b] = ment_n[c]++;
    }
}

static void match_clear(int t)
{
    mrec_n[t] = 0;
    ment_n[t] = 0;
    memset(mrec_head[t], 0xff, sizeof mrec_head[t]);
}

/* ---- the scene's images */
static void tc_view_free(void)   /* (the views are their images' view2: freed with them) */
{
    tc_view = VK_NULL_HANDLE;
    scn.zview = VK_NULL_HANDLE;
}

/* the scene's images for back buffer c (and its depth buffer z's format); 0 if FSR 2 (or DLSS) cannot run.
   Without them ("colour_precision = high" alone) only its colour and depth, at the back buffer's size.
   RENDER SCALE AUTO: FSR 2's or DLSS's context is made for the back buffer's size (the largest the
   scene may be) and kept; a new scene size only makes the scene's images again (the old ones freed once
   the frames using them are done), with no wait and no new history. */
static int scene_images(GpuImg *c, GpuImg *z)
{
    float ratio = scn.fsr ? rs_ratio(c) : 1.0f;
    uint32_t w = (uint32_t)lroundf((float)c->w / ratio), h = (uint32_t)lroundf((float)c->h / ratio);
    if (w < 16 || h < 16)
        return 0;
    VkFormat zf = z ? z->format : depth24, cf = hp_want && hp_format ? hp_format : RT_FORMAT;
    GpuImg *o = aa_own ? scn.hist[0] : scn.out;
    int ctx = scn.fsr && (!o || o->w != c->w || o->h != c->h || scn.backend != aa_backend() || scn.dyn != rs_auto);
    int remake = ctx || !scn.c || scn.c->w != w || scn.c->h != h || scn.c->format != cf || !scn.z ||
                 scn.z->format != zf;
    int resize = remake && !ctx && rs_auto && scn.fsr && scn.c && scn.c->format == cf && scn.z->format == zf;
    if (remake) {
        if (!resize)
            vkDeviceWaitIdle(dev);    /* (FSR 2's or DLSS's context is made again) */
        tc_view_free();
        GpuImg **old[] = {&scn.c, &scn.z, &scn.vel, &scn.mask, &scn.out, &scn.hist[0], &scn.hist[1]};
        for (int i = 0; i < (resize ? 4 : 7); i++)      /* (resized: the outputs, the back buffer's size, kept) */
            if (*old[i]) {
                img_discard(*old[i]);
                *old[i] = NULL;
            }
        VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        scn.c = img_new(K_RT, w, h, 1, cf, u | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        scn.z = img_new(K_DEPTH, w, h, 1, zf, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        scn.zview = scn.z->view;
        if (scn.fsr) {
            scn.vel = img_new(K_AUX, w, h, 1, VEL_FORMAT, u);
            scn.mask = img_new(K_AUX, w, h, 1, MASK_FORMAT, u);
            if (!resize && aa_own)
                for (int i = 0; i < 2; i++)
                    scn.hist[i] = img_new(K_AUX, c->w, c->h, 1, UPS_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                                            VK_IMAGE_USAGE_SAMPLED_BIT);
            else if (!resize)
                scn.out = img_new(K_AUX, c->w, c->h, 1, UPS_FORMAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);   /* (DLSS clears it) */
            VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = scn.mask->image,
                                        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = MASK_FORMAT,
                                        .components = {VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO,
                                                       VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE},
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VKC(vkCreateImageView(dev, &vi, NULL, &scn.mask->view2));
            tc_view = scn.mask->view2;
            if (scn.z->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {     /* (FSR 2 samples the depth alone) */
                vi.image = scn.z->image;
                vi.format = zf;
                vi.components = (VkComponentMapping){0};
                vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                VKC(vkCreateImageView(dev, &vi, NULL, &scn.z->view2));
                scn.zview = scn.z->view2;
            }
        }
        /* nothing in it yet */
        barrier(scn.c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue black = {.float32 = {0, 0, 0, 1}};
        VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, scn.c->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
        /* the context: for this size, or with AUTO for up to the largest it may have */
        uint32_t mw = rs_auto ? (uint32_t)lroundf((float)c->w / rs_lo) : w;
        uint32_t mh = rs_auto ? (uint32_t)lroundf((float)c->h / rs_lo) : h;
#ifdef HAVE_DLSS
        if (!scn.fsr || !aa_dlss)
            dlss_destroy();
        else if (!dlss_context(cb, mw, mh, c->w, c->h, rs_auto ? (rs_dlss_mode > 0 ? rs_dlss_mode : SCALE_DLSS_MODE) : 0.0f)) {
            LOG(LOG_ALWAYS, "vk: temporal anti-aliasing off: DLSS cannot run here");
            dlss_ok = 0;
            return 0;
        }
#endif
#ifdef HAVE_FSR2
        if (scn.fsr && !aa_dlss && !aa_own && !fsr2_context(phys, dev, vkGetDeviceProcAddr, mw, mh, c->w, c->h, rs_auto)) {
            LOG(LOG_ALWAYS, "vk: temporal anti-aliasing off: FSR 2 cannot run here");
            taa_ok = 0;
            return 0;
        }
#endif
        scn.backend = aa_backend();
        scn.dyn = rs_auto;
        scn.fw = w;
        scn.fh = h;
        scn.fdw = c->w;
        scn.fdh = c->h;
        if (resize) {                 /* (the history goes on: FSR 2 and DLSS take a new input size) */
            if (rs_log)
                LOG(LOG_ALWAYS, "vk: render scale auto: scene %ux%u (1 / %.2f) at frame %lu, GPU %.2f ms a frame against %.2f",
                    w, h, ratio, nframes, rs_gpu, rs_target_ms);
        } else {
            scn.reset = 1;
            if (scn.fsr)
                LOG(LOG_ALWAYS, "vk: temporal anti-aliasing: scene %ux%u -> %ux%u%s%s", w, h, c->w, c->h,
                    cf != RT_FORMAT ? ", 16-bit colour" : "", rs_auto ? ", render scale auto" : "");
            else
                LOG(LOG_ALWAYS, "vk: colour precision high: scene %ux%u", w, h);
        }
    }
    /* the same picture as the back buffer's, at the scene's size */
    scn.c->lw = c->lw;
    scn.c->lh = c->lh;
    scn.c->sx = (float)w / (float)c->lw;
    scn.c->sy = (float)h / (float)c->lh;
    scn.c->ox = ((float)w - (float)c->lw * scn.c->sy) / 2.0f;
    return 1;
}

/* A draw (or clear) of the scene into back buffer *c: the scene's images instead (the first one
   sets them up: cleared, the frame's jitter); 0 if it goes where it was going */
static int scene_target(GpuImg **c, GpuImg **z, const D3DState *st)
{
    (void)st;
    if (!scn.drawn) {
        if (!scene_images(*c, *z)) {
            scn.active = 0;
            return 0;
        }
        scn.dst = *c;
        scn.dz = *z;
        end_rendering();
        /* its depth as the game cleared it before the view, nothing moving, no masks; its colour as the
           back buffer's if the frame cleared that (else what the view leaves undrawn keeps the last) */
        VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue zero = {.float32 = {0, 0, 0, 0}};
        if (scn.fsr) {
            barrier(scn.vel, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cb, scn.vel->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rr);
            barrier(scn.mask, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cb, scn.mask->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rr);
        }
        if (scn.clear_set && scn.clear_frame == nframes) {
            barrier(scn.c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cb, scn.c->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &scn.clear_col, 1, &rr);
        }
        barrier(scn.z, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue one = {1.0f, 0};
        VkImageSubresourceRange zr = {scn.z->aspect, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cb, scn.z->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &zr);
        /* the frame's jitter (FSR 2's sequence: Halton 2, 3 over its phase count, 8 x the scale squared,
           which DLSS also advises; none without FSR 2 or DLSS) */
        uint32_t w = scn.c->w, h = scn.c->h;
        float sr = (float)scn.dst->w / (float)w;
        uint32_t n = (uint32_t)ceilf(8.0f * sr * sr);
#ifdef HAVE_FSR2
        if (scn.fsr)
            n = (uint32_t)fsr2_phase_count((int32_t)w, (int32_t)scn.dst->w);
#endif
        uint32_t i = scn.fsr ? scn.phase++ % (n ? n : 8) : 0;
        scn.jx = taa_jitter && scn.fsr ? halton(i + 1, 2) - 0.5f : 0.0f;
        scn.jy = taa_jitter && scn.fsr ? halton(i + 1, 3) - 0.5f : 0.0f;
        scn.jndc[0] = 2.0f * scn.jx / (float)w;
        scn.jndc[1] = 2.0f * scn.jy / (float)h;
        taa_mip = log2f((float)w / (float)scn.dst->w) - 1.0f + taa_mip_add;
        /* the camera's move since the previous scene */
        for (int k = 0; k < 3; k++)
            scn.d[k] = scn.prev_ok ? (double)(int32_t)((uint32_t)scn.cam[k] - (uint32_t)scn.pcam[k]) / 65536.0 : 0.0;
        scn.sc[0] = scn.sc[1] = 1 << 30;
        scn.sc[2] = scn.sc[3] = -(1 << 30);
        scn.drawn = 1;
    }
    if (*c != scn.dst)
        return 0;
    *c = scn.c;
    *z = scn.z;
    return 1;
}

/* The scene's camera from its first perspective draw (and whether it jumped since the previous) */
static void scene_camera(const D3DState *st)
{
    const Mat4 *P = &st->xf[XF_PROJ];
    if (scn.have_vp || P->m[2][3] == 0.0f)
        return;
    m4_of(scn.V, &st->xf[XF_VIEW]);
    m4_of(scn.P, P);
    scn.have_vp = 1;
    if (scn.prev_ok) {
        /* a camera cut: the view turned by more than about 37 degrees, moved more than 15 m or zoomed
           by a quarter in a frame (replay cameras, restarts, the race start) */
        double f = scn.V[2] * scn.pV[2] + scn.V[6] * scn.pV[6] + scn.V[10] * scn.pV[10];
        double m = sqrt(scn.d[0] * scn.d[0] + scn.d[1] * scn.d[1] + scn.d[2] * scn.d[2]);
        double z = scn.pP[5] != 0 ? scn.P[5] / scn.pP[5] : 0;
        if (f < 0.8 || m > 1000.0 || z > 1.25 || z < 0.8) {
            if (taa_log)
                logmsg(LOG_ALWAYS, "vk: temporal anti-aliasing: camera cut at frame %lu (turn %.2f, move %.0f, zoom %.2f)",
                       nframes, f, m, z);
            scn.prev_ok = 0;
            scn.reset = 1;
            scn.d[0] = scn.d[1] = scn.d[2] = 0;
            match_clear(mcur ^ 1);
        }
    }
    if (!scn.prev_ok) {
        memcpy(scn.pV, scn.V, sizeof scn.V);
        memcpy(scn.pP, scn.P, sizeof scn.P);
    }
}

/* A draw of the scene: its Motion block (where its vertices were in the previous frame) and what it
   writes into attachments 3 and 4 (draw_mot) */
static void scene_motion(const D3DState *st, PushDraw *pc, const TnlConst *k, const void *raw, int nv, const uint16_t *idx,
                         int ni, int pretransformed, const float *pl0, int blend, uint32_t sbf, uint32_t dbf, int zwrite,
                         Surface *t0, const int *sc)
{
    double t_in = st_print ? wall_ms() : 0;
    Frame *f = &fr[fi];
    if (!k) {                     /* (the CPU's draws: a TnlConst index of their own for the block) */
        if (f->ntnl >= TNL_MAX) {
            draw_mot = 1;
            return;
        }
        pc->tnl[0] = (uint32_t)TNL_MAX * (uint32_t)fi + f->ntnl++;
    }
    scene_camera(st);
    int cls = scn.cls, sky = cls == VK_SC_SKY;
    /* the previous frame's view and this draw's projection then (its x, y scale moved as the scene's) */
    double V0[16], P0[16], VP0[16], D[16], Pl[16], M[16], W[16], T[16];
    m4_of(P0, &st->xf[XF_PROJ]);
    if (scn.have_vp) {
        memcpy(V0, scn.pV, sizeof V0);
        if (scn.P[0] != 0 && scn.P[5] != 0) {
            P0[0] *= scn.pP[0] / scn.P[0];
            P0[5] *= scn.pP[5] / scn.P[5];
            P0[8] += scn.pP[8] - scn.P[8];
            P0[9] += scn.pP[9] - scn.P[9];
        }
    } else
        m4_of(V0, &st->xf[XF_VIEW]);
    m4_mul(VP0, V0, P0);
    m4_ident(D);                  /* the camera's move: a static thing's place in the previous frame's camera space */
    D[12] = scn.d[0];
    D[13] = scn.d[1];
    D[14] = scn.d[2];
    m4_place(Pl, pl0, 0, 0);
    int placed = 0;               /* a model put in the world by a matrix (not built each frame by the CPU) */
    if (pretransformed) {
        /* screen-space vertices (none in the race's view so far): no motion, only the jitter taken off */
        m4_ident(M);
        M[12] = -scn.jndc[0];
        M[13] = -scn.jndc[1];
    } else if (k) {
        m4_world(W, k->world);
        const float (*w)[4] = k->world;
        int ident = w[0][0] == 1 && w[0][1] == 0 && w[0][2] == 0 && w[0][3] == 0 && w[1][0] == 0 && w[1][1] == 1 &&
                    w[1][2] == 0 && w[1][3] == 0 && w[2][0] == 0 && w[2][1] == 0 && w[2][2] == 1 && w[2][3] == 0;
        placed = !ident;
        MRec *m = NULL;
        float mv[3] = {0, 0, 0}, last[3] = {0, 0, 0};
        if (!sky && !ident && !far_bind && cls != VK_SC_TERRAIN && cls != VK_SC_WATER && raw) {
            /* a thing with a world matrix: matched with the previous frame's draw of its mesh */
            uint64_t key = draw_key(raw, nv, idx, ni, pc->texsmp[0]);
            float dc[3] = {(float)scn.d[0], (float)scn.d[1], (float)scn.d[2]};
            float q[3] = {w[0][3] + dc[0], w[1][3] + dc[1], w[2][3] + dc[2]};
            if (taa_match && scn.prev_ok && (m = match_find(key, q, dc)) != NULL) {
                m->used = 1;
                for (int i = 0; i < 3; i++)
                    mv[i] = q[i] - m->t[i];
                int moved = mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2] > 0.25f;
                for (int i = 0; i < 3; i++)       /* (a frame without a step keeps the move before) */
                    last[i] = moved ? mv[i] : 0.7f * m->v[i];
                st_taa_matched++;
                st_taa_moving += mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2] > 0.01f;
            }
            match_add(key, w, last, m != NULL, dc);
        }
        if (m)
            m4_world(T, m->w);
        else if (sky)
            memcpy(T, W, sizeof T);       /* (the sky dome moves with the camera) */
        else
            m4_mul(T, W, D);
        m4_mul(M, T, VP0);
        m4_mul(M, M, Pl);
    } else {
        /* the CPU's transform: this frame's clip position -> the world -> the previous frame's */
        double WVP[16], inv[16], PlJ[16], invPl[16];
        m4_of(W, &st->xf[XF_WORLD]);
        double V1[16];
        m4_of(V1, &st->xf[XF_VIEW]);
        m4_mul(WVP, W, V1);
        double P1[16];
        m4_of(P1, &st->xf[XF_PROJ]);
        m4_mul(WVP, WVP, P1);
        m4_place(PlJ, pl0, scn.jndc[0], scn.jndc[1]);
        if (inverse4(WVP, inv) && inverse4(PlJ, invPl)) {
            if (sky)
                memcpy(T, W, sizeof T);
            else
                m4_mul(T, W, D);
            m4_mul(M, invPl, inv);
            m4_mul(M, M, T);
            m4_mul(M, M, VP0);
            m4_mul(M, M, Pl);
        } else {
            m4_ident(M);
            M[12] = -scn.jndc[0];
            M[13] = -scn.jndc[1];
        }
    }
    /* what it marks: the motion vector (velw: 1 replaced, 2 where it is solid), the reactive and the
       transparency and composition masks (kinds: 1 by alpha, 2 by the brightness it adds) */
    int velw = 0, rk = 0, tk = 0, solid = 0;
    float rs = 0, ts = 0;
    int add = blend && dbf == 2, bias = st->rs[RS_ZBIAS] > 0;
    if (zwrite || sky)
        velw = 1;
    else if (blend) {
        switch (cls) {
        case VK_SC_TERRAIN:       /* its other passes lie on it */
        case VK_SC_DECAL:         /* skid marks, objects' shadows: as still as the ground */
            break;
        case VK_SC_CAR_SHADOW:    /* it moves with the car over the ground */
            rk = 1;
            rs = 0.9f;
            break;
        case VK_SC_WATER:         /* its colour pass is its surface; its waves are animated */
            if (!add) {
                velw = 1;
                tk = 1;
                ts = 0.6f;
            } else {
                rk = 2;
                rs = 0.5f;
            }
            break;
        case VK_SC_PARTS:
            if (!add && sbf == 5 && t0 && t0->gpu) {
                GpuImg *g = t0->gpu;
                if (!g->cut_known) {
                    st_scans++;
                    g->cutout = cut_tiles(t0, &g->cut_frac);
                    g->cut_known = 1;
                }
                /* lattices, fences: their bars are surfaces. Not sprites and bits (smoke, dust, mud
                   thrown up: the CPU's quads, single triangles), with cut-out textures as some are:
                   where they would mark their own motion the ground seen round them would take
                   it, a heat-haze shimmer about the cars */
                solid = g->cut_frac >= 0.1f && placed && nv > 4;
            }
            if (solid)
                velw = 2;
            else if (add && bias) {               /* glass's reflection */
                tk = 2;
                ts = 1.0f;
            } else if (add) {                     /* glows, lights */
                rk = 2;
                rs = 0.8f;
            } else {                              /* glass, smoke */
                rk = 1;
                rs = 0.5f;
                tk = 1;
                ts = 0.8f;
            }
            break;
        default:
            if (add && bias) {                    /* the reflection pass over cars and objects */
                tk = 2;
                ts = 1.0f;
            } else if (add) {
                rk = 2;
                rs = 0.8f;
            } else {
                rk = 1;
                rs = 0.6f;
                tk = 1;
                ts = 0.5f;
            }
            break;
        }
    }
    rs *= taa_react;
    MotionConst *mc = &motmap[pc->tnl[0]];
    for (int i = 0; i < 16; i++)
        mc->prev[i] = (float)M[i];
    mc->jit[0] = scn.jndc[0];
    mc->jit[1] = scn.jndc[1];
    uint32_t fl = (uint32_t)rk | (uint32_t)tk << 2 | (solid ? 16u : 0u), s = (uint32_t)f2h(fminf(rs, 1.0f)) |
                  (uint32_t)f2h(ts) << 16;
    memcpy(&mc->jit[2], &fl, 4);
    memcpy(&mc->jit[3], &s, 4);
    draw_mot = 1u | (uint32_t)velw << 1 | (rk || tk ? 8u : 0u);
    st_taa_draws++;
    if (st_print)
        st_taa_cpu_ms += wall_ms() - t_in;
}

int vk_taa_available(void)
{
    return vk_on() && taa_ok;
}

int vk_dlss_available(void)
{
    return vk_on() && dlss_ok;
}

int vk_own_taa_available(void)
{
    return vk_on() && own_ok;
}

int vk_taa_on(void)
{
    return vk_on() && taa_can() && aa_temporal;
}

/* the main view goes into the scene's images: for FSR 2 or DLSS, or for "colour_precision = high" alone */
static int scene_wanted(void)
{
    return (taa_can() && aa_temporal) || (hp_want && hp_format);
}

int vk_scene_on(void)
{
    return vk_on() && scene_wanted();
}

int vk_hp_available(void)
{
    return vk_on() && hp_format;
}

void vkr_scene_begin(const VkScene *s)
{
    if (!vk_scene_on() || scn.active)
        return;
    frame_begin();
    scn.active = 1;
    scn.fsr = taa_can() && aa_temporal;     /* (FSR 2 or DLSS: the upscaled scene) */
    scn.drawn = 0;
    scn.have_vp = 0;
    scn.cls = VK_SC_OBJECT;
    memcpy(scn.cam, s->cam, sizeof scn.cam);
    if (scn.last + 1 != nframes)  /* (no scene in the previous frame: nothing to reproject) */
        scn.prev_ok = 0;
    if (!scn.prev_ok)
        scn.reset = 1;
    if (scn.fsr) {
        mcur ^= 1;
        match_clear(mcur);
        if (!scn.prev_ok)
            match_clear(mcur ^ 1);
    }
}

void vkr_scene_camera(const VkScene *s)
{
    if (scn.active && !scn.drawn)
        memcpy(scn.cam, s->cam, sizeof scn.cam);
}

void vkr_scene_class(int cls)
{
    scn.cls = cls;
}

static VkPipeline taa_pipeline(VkFormat depth)
{
    uint32_t key = 0x10000000u;
    for (int i = 0; i < npipes; i++)
        if (pipes[i].key == key && pipes[i].depth == depth)
            return pipes[i].p;
    if (npipes == COUNT(pipes))
        fatal("Vulkan: too many pipelines");
    Pipe *p = &pipes[npipes++];
    p->key = key;
    p->depth = depth;
    p->color = RT_FORMAT;
    p->p = pipeline_create(sm_pv, sm_taa, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, VK_BLEND_FACTOR_ONE,
                           VK_BLEND_FACTOR_ZERO, RT_FORMAT, depth, 0, 0, 0, 0);
    return p->p;
}

typedef struct {
    float uv[4];
    int32_t idx[4];
    float k[4];
    int32_t q[4];                 /* x: the dither's noise texture (-1: none), y: 1 the source's alpha (else 1) */
} PushTaa;
_Static_assert(sizeof(PushTaa) <= sizeof(PushDraw), "push constant range");

/* the upscaled scene (src; or a debug view of the scene's images: mode, taa.frag) into back buffer c over the
   view's area, where its depth buffer z is as the game cleared it: what the game drew before the view
   and kept from it by depth (the in-car view's mirrors: drawn first, then their glass as near depth,
   which the view's draws fail against) stays. A 16-bit scene going into the 8-bit back buffer takes the
   dither (taa.frag dither()); without FSR 2 its alpha goes with it (alpha) as the back buffer had it drawn
   there. */
static void taa_copy(GpuImg *c, GpuImg *z, GpuImg *src, int mode, int alpha, float sharp)
{
    int same = src->w == c->w && src->h == c->h;
    int dither = mode == 0 && hp_dither && scn.c->format != RT_FORMAT && c->format == RT_FORMAT && noise_sync();
    begin_rendering(c, z);
    c->gpu_newer = 1;
    bind_pipeline(taa_pipeline(z ? z->format : VK_FORMAT_UNDEFINED));
    VkViewport vp = {0, 0, (float)c->w, (float)c->h, 1, 1};      /* (the triangle at depth 1: LESS_OR_EQUAL) */
    vkCmdSetViewport(cb, 0, 1, &vp);
    double kx = (double)c->w / scn.c->w, ky = (double)c->h / scn.c->h;
    int x0 = (int)floor(scn.sc[0] * kx), y0 = (int)floor(scn.sc[1] * ky);
    int x1 = (int)ceil(scn.sc[2] * kx), y1 = (int)ceil(scn.sc[3] * ky);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > (int)c->w ? (int)c->w : x1;
    y1 = y1 > (int)c->h ? (int)c->h : y1;
    VkRect2D r = {{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}};
    vkCmdSetScissor(cb, 0, 1, &r);
    PushTaa p = {{1.0f / (float)c->w, 1.0f / (float)c->h, 0, 0}, {src->slot, same ? smp_nearest : smp_linear, mode,
                 scn.c->slot}, {40.0f, same ? sharp : 0.0f, 0, 0}, {dither ? noise_img->slot : -1, alpha, 0, 0}};
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(cb, 3, 1, 0, 0);
    end_rendering();
}

/* ANTI-ALIASING TAA: the port's own temporal upscaler (taau.frag), one pass into this frame's history from
   the last one's; the history then goes into the back buffer as FSR 2's output does, sharpened as FSR 2
   sharpens (RCAS, TD6_TAA_SHARPEN). Lighter than FSR 2 (for the integrated GPUs), softer. */
typedef struct {
    float size[4];                /* the scene's size, the history's */
    float jit[4];                 /* the jitter (scene pixels), no history */
    int32_t idx[4];               /* colour, depth, motion vectors, masks */
    int32_t idx2[4];              /* the last history, the nearest sampler, the linear one */
} PushTaau;
_Static_assert(sizeof(PushTaau) <= sizeof(PushDraw), "push constant range");

static VkPipeline taau_pipeline(void)
{
    uint32_t key = 0x10000001u;
    for (int i = 0; i < npipes; i++)
        if (pipes[i].key == key)
            return pipes[i].p;
    if (npipes == COUNT(pipes))
        fatal("Vulkan: too many pipelines");
    Pipe *p = &pipes[npipes++];
    p->key = key;
    p->depth = VK_FORMAT_UNDEFINED;
    p->color = UPS_FORMAT;
    p->p = pipeline_create(sm_pv, sm_taau, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, VK_BLEND_FACTOR_ONE,
                           VK_BLEND_FACTOR_ZERO, UPS_FORMAT, VK_FORMAT_UNDEFINED, 0, 0, 0, 0);
    return p->p;
}

static float taau_move = 0.1f;    /* TD6_TAAU_MOVE: how much more of the frame where the picture moves fast */
static float taau_sharp = -1.0f;  /* TD6_TAAU_SHARPEN: its sharpening, 0..1 (below 0: TD6_TAA_SHARPEN's) */
static float taau_spread = 1.25f; /* TD6_TAAU_SPREAD: how far the history may be from the frame's samples (spreads) */

static GpuImg *taau_dispatch(void)
{
    static int env;
    if (!env) {
        env = 1;
        const char *e = getenv("TD6_TAAU_MOVE"), *s = getenv("TD6_TAAU_SHARPEN"), *g = getenv("TD6_TAAU_SPREAD");
        if (e) taau_move = (float)atof(e);
        if (s) taau_sharp = fminf((float)atof(s), 1.0f);
        if (g) taau_spread = (float)atof(g);
    }
    scn.hcur ^= 1;
    GpuImg *h = scn.hist[scn.hcur], *last = scn.hist[scn.hcur ^ 1];
    barrier(last, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    begin_rendering(h, NULL);
    bind_pipeline(taau_pipeline());
    VkViewport vp = {0, 0, (float)h->w, (float)h->h, 0, 1};
    VkRect2D r = {{0, 0}, {h->w, h->h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &r);
    PushTaau p = {{(float)scn.c->w, (float)scn.c->h, (float)h->w, (float)h->h},
                  {scn.jx, scn.jy, scn.reset || !scn.prev_ok ? 1.0f : 0.0f, taau_move},
                  {scn.c->slot, scn.z->slot, scn.vel->slot, scn.mask->slot},
                  {last->slot, smp_nearest, smp_linear, (int32_t)lroundf(taau_spread * 100.0f)}};
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(cb, 3, 1, 0, 0);
    end_rendering();
    barrier(h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return h;
}

/* The scene is drawn: FSR 2 (or DLSS) resolves it into the back buffer (without them, the 16-bit scene is
   copied there) */
void vkr_scene_end(void)
{
    if (!scn.active)
        return;
    scn.active = 0;
    if (!scn.drawn)
        return;
    double t_in = wall_ms();
    end_rendering();
    GpuImg *c = scn.dst;
    Frame *f = &fr[fi];
    uint32_t tsb = TS * (uint32_t)fi;
    int timed = st_queries && !f->taa_timed;
    if (timed)
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 9);
    barrier(scn.c, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!scn.fsr) {               /* "colour_precision = high" alone: the same size, pixel for pixel, with its alpha */
        taa_copy(c, scn.dz, scn.c, 0, 1, 0.0f);
        if (timed) {
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 10);
            f->taa_timed = 1;
        }
        scn.prev_ok = 0;          /* (FSR 2 chosen next: no history) */
        scn.last = nframes;
        scn.reset = 1;
        st_taa_scenes++;
        st_taa_cpu_ms += wall_ms() - t_in;
        return;
    }
    barrier(scn.z, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(scn.vel, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(scn.mask, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    int ran = 0;
    GpuImg *res = scn.out;
    float sharp = 0.0f;
    if (aa_own && !taa_debug && scn.hist[0]) {
        res = taau_dispatch();
        float s = taau_sharp >= 0 ? taau_sharp : taa_sharp;
        sharp = s >= 0 ? exp2f(-(2.0f - 2.0f * s)) : 0.0f;   /* (as FSR 2 takes its sharpness) */
        ran = 1;
    }
#if defined(HAVE_FSR2) || defined(HAVE_DLSS)
    if (!taa_debug && !aa_own) {
        barrier(scn.out, VK_IMAGE_LAYOUT_GENERAL);
        double now = wall_ms(), ms = deterministic || scn.t_last <= 0 ? 1000.0 / 60.0 : now - scn.t_last;
        scn.t_last = now;
        /* the camera: near and far planes, vertical angle (from its projection) */
        double P22 = scn.P[10], P32 = scn.P[14], n = P22 != 0 ? -P32 / P22 : 1.0, fa = P22 > 1.0 ? n * P22 / (P22 - 1.0) : 1e5;
        Fsr2Dispatch d = {
            .cb = cb,
            .color = {scn.c->image, scn.c->view, scn.c->format == RT_FORMAT ? VK_FORMAT_R8G8B8A8_UNORM : scn.c->format,
                      scn.c->w, scn.c->h},
            .depth = {scn.z->image, scn.zview, VK_FORMAT_D32_SFLOAT, scn.z->w, scn.z->h},
            .motion = {scn.vel->image, scn.vel->view, VEL_FORMAT, scn.vel->w, scn.vel->h},
            .reactive = {scn.mask->image, scn.mask->view, VK_FORMAT_R8_UNORM, scn.mask->w, scn.mask->h},
            .tc = {scn.mask->image, tc_view, VK_FORMAT_R8_UNORM, scn.mask->w, scn.mask->h},
            .output = {scn.out->image, scn.out->view, UPS_FORMAT, scn.out->w, scn.out->h},
            .render_w = scn.c->w, .render_h = scn.c->h, .jitter_x = scn.jx, .jitter_y = scn.jy,
            .mv_scale_x = (float)scn.c->w, .mv_scale_y = (float)scn.c->h,
            .frame_ms = (float)(ms < 1.0 ? 1.0 : ms > 100.0 ? 100.0 : ms), .sharpness = taa_sharp,
            .cam_near = (float)n, .cam_far = (float)fa,
            .fov_y = scn.P[5] > 0 ? (float)(2.0 * atan(1.0 / scn.P[5])) : 1.0f, .metres = 66.67f,
            .reset = scn.reset || !scn.prev_ok};
#ifdef HAVE_DLSS
        if (aa_dlss) {                            /* (NGX checks the views' own formats) */
            d.depth.format = scn.z->format;
            d.reactive.format = MASK_FORMAT;
            ran = dlss_dispatch(&d);
        } else
#endif
#ifdef HAVE_FSR2
            ran = fsr2_dispatch(&d);
#else
            ran = 0;
#endif
        scn.out->layout = VK_IMAGE_LAYOUT_GENERAL;
        bound_pipe = VK_NULL_HANDLE;              /* (FSR 2 or DLSS bound its own pipelines and descriptors) */
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &dset, 0, NULL);
        barrier(scn.out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
#endif
    if (ran)
        taa_copy(c, scn.dz, res, 0, 0, sharp);
    else                          /* debug views; the scene scaled up (FSR 2 failing) */
        taa_copy(c, scn.dz, taa_debug == 1 ? scn.vel : taa_debug == 2 ? scn.mask : taa_debug == 3 ? scn.z : scn.c,
                 taa_debug >= 1 && taa_debug <= 3 ? taa_debug : 0, 0, 0.0f);
    if (timed) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, st_queries, tsb + 10);
        f->taa_timed = 1;
    }
    /* this scene is the next one's previous */
    memcpy(scn.pV, scn.V, sizeof scn.V);
    memcpy(scn.pP, scn.P, sizeof scn.P);
    memcpy(scn.pcam, scn.cam, sizeof scn.cam);
    scn.prev_ok = scn.have_vp;
    scn.last = nframes;
    scn.reset = 0;
    st_taa_scenes++;
    st_taa_cpu_ms += wall_ms() - t_in;
}

static int clear_widened;        /* the rects being cleared belong to a widened camera's viewport */

static VkClearColorValue clear_color(uint32_t argb)
{
    VkClearColorValue v;
    v.float32[0] = (float)((argb >> 16) & 0xff) / 255.0f;
    v.float32[1] = (float)((argb >> 8) & 0xff) / 255.0f;
    v.float32[2] = (float)(argb & 0xff) / 255.0f;
    v.float32[3] = (float)(argb >> 24) / 255.0f;
    return v;
}

static void clear_rects(GpuImg *c, int n, const int *rects, const VkClearAttachment *att, uint32_t natt)
{
    VkClearRect cr[64];
    int k = 0;
    float wide = ws_aspect / (4.0f / 3.0f);
    for (int i = 0; i < n && k < 64; i++) {
        const int *q = rects + 4 * i;
        float fx0, fx1;
        if (q[0] <= 0 && q[2] >= (int)c->lw) {
            fx0 = 0;
            fx1 = (float)c->w;
        } else if (c->ox > 0 && clear_widened) {
            float mid = ui_x(c, (float)(q[0] + q[2]) / 2.0f), half = (float)(q[2] - q[0]) * c->sy * wide / 2.0f;
            fx0 = mid - half;
            fx1 = mid + half;
        } else {
            fx0 = ui_x(c, (float)q[0]);
            fx1 = ui_x(c, (float)q[2]);
        }
        int x0 = (int)lroundf(fx0), y0 = (int)lroundf((float)q[1] * c->sy);
        int x1 = (int)lroundf(fx1), y1 = (int)lroundf((float)q[3] * c->sy);
        x0 = x0 > 0 ? x0 : 0;
        y0 = y0 > 0 ? y0 : 0;
        x1 = x1 < (int)c->w ? x1 : (int)c->w;
        y1 = y1 < (int)c->h ? y1 : (int)c->h;
        if (x0 >= x1 || y0 >= y1)
            continue;
        cr[k++] = (VkClearRect){{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, 0, 1};
    }
    if (k && natt)
        vkCmdClearAttachments(cb, natt, att, (uint32_t)k, cr);
}

void vkr_clear(const D3DState *st, int n, const int *rects, uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
    Surface *rs = st->rt;
    if (!vk_on() || !rs || rs->dead || !rs->mem)
        return;
    frame_begin();
    GpuImg *c = rt_sync(rs);
    GpuImg *d = depth_sync(st->zb, c);
    int in_scene = scn.active && scene_target(&c, &d, NULL);      /* temporal anti-aliasing: the scene's images */
    if (!in_scene && (flags & 1) && scn.active == 0 && scene_wanted()) {
        /* the back buffer's colour before the scene: the scene starts from it (scene_target) */
        scn.clear_set = 1;
        scn.clear_frame = nframes;
        scn.clear_col = clear_color(pix_decode(rs, pix_encode(rs, color)));
    }
    want_mot = in_scene;
    begin_rendering(c, d);
    want_mot = 0;
    clear_widened = n == 1 && rects[0] == (int)st->vp.x && rects[1] == (int)st->vp.y &&
                    rects[2] == (int)(st->vp.x + st->vp.w) && rects[3] == (int)(st->vp.y + st->vp.h);
    VkClearAttachment att[2];
    uint32_t na = 0;
    if (flags & 1) {
        /* what the program's surface format keeps of the colour */
        att[na++] = (VkClearAttachment){VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                        {.color = clear_color(pix_decode(rs, pix_encode(rs, color)))}};
        c->gpu_newer = 1;
    }
    if ((flags & 2) && d)
        att[na++] = (VkClearAttachment){VK_IMAGE_ASPECT_DEPTH_BIT, 0,
                                        {.depthStencil = {z < 0 ? 0 : z > 1 ? 1 : z, stencil}}};
    clear_rects(c, n, rects, att, na);
    clear_widened = 0;
}

/* ------------------------------------------------------------------ DirectDraw */
static GpuImg *zfor(Surface *s, GpuImg *c)
{
    Surface *z = surface_zbuffer(s);
    return z && z->gpu && z->gpu->kind == K_DEPTH ? depth_sync(z, c) : NULL;
}

void vkr_fill(Surface *s, const int *rect, uint32_t raw)
{
    if (!vk_on())
        return;
    GpuImg *g = (s->caps & DDSCAPS_ZBUFFER) && s->gpu && s->gpu->kind == K_DEPTH ? s->gpu : NULL;
    if ((s->caps & DDSCAPS_ZBUFFER) && !g)
        return;                         /* (a depth buffer without a GPU image) */
    frame_begin();
    int r[4] = {0, 0, (int)s->width, (int)s->height};
    if (rect)
        memcpy(r, rect, sizeof r);
    if (s->caps & DDSCAPS_ZBUFFER) {
        uint32_t sh = s->zmask ? (uint32_t)__builtin_ctz(s->zmask) : 0;
        double max = (double)(s->zmask >> sh);
        VkClearDepthStencilValue v = {max > 0 ? (float)((double)((raw & s->zmask) >> sh) / max) : 1.0f, 0};
        barrier(g, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange rr = {g->aspect, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cb, g->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rr);
        if (rect)
            LOG(LOG_D3D, "vk: depth fill of a rectangle done on the whole surface");
        return;
    }
    GpuImg *c = rt_sync(s);
    begin_rendering(c, zfor(s, c));
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = clear_color(pix_decode(s, raw))}};
    clear_rects(c, 1, r, &a, 1);
    c->gpu_newer = 1;
}

void vkr_blt(Surface *dst, const int *drect, Surface *src, const int *srect, int key, uint32_t lo, uint32_t hi)
{
    static int warned_range, warned_self, warned_key;
    if (!vk_on())
        return;
    frame_begin();
    int d[4] = {0, 0, (int)dst->width, (int)dst->height}, s[4] = {0, 0, (int)src->width, (int)src->height};
    if (drect)
        memcpy(d, drect, sizeof d);
    if (srect)
        memcpy(s, srect, sizeof s);
    GpuImg *c = rt_sync(dst);
    if (is_rt(src->gpu) || vk_target(src)) {
        /* render target to render target: an image blit */
        if (src == dst) {
            if (!warned_self++)
                LOG(LOG_ALWAYS, "vk: Blt within one render target is not supported");
            return;
        }
        if (key && !warned_key++)
            LOG(LOG_ALWAYS, "vk: colour key on a render-target-to-render-target Blt ignored");
        GpuImg *sg = rt_sync(src);
        end_rendering();
        barrier(sg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (int i = 0; i < 4; i++) {
            s[i] = (int)lroundf((float)s[i] * (i & 1 ? sg->sy : sg->sx));
            d[i] = (int)lroundf((float)d[i] * (i & 1 ? c->sy : c->sx));
        }
        VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .srcOffsets = {{s[0], s[1], 0}, {s[2], s[3], 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .dstOffsets = {{d[0], d[1], 0}, {d[2], d[3], 1}}};
        for (int i = 0; i < 2; i++) {
            b.srcOffsets[i].x = b.srcOffsets[i].x < 0 ? 0 : b.srcOffsets[i].x > (int)sg->w ? (int)sg->w : b.srcOffsets[i].x;
            b.srcOffsets[i].y = b.srcOffsets[i].y < 0 ? 0 : b.srcOffsets[i].y > (int)sg->h ? (int)sg->h : b.srcOffsets[i].y;
            b.dstOffsets[i].x = b.dstOffsets[i].x < 0 ? 0 : b.dstOffsets[i].x > (int)c->w ? (int)c->w : b.dstOffsets[i].x;
            b.dstOffsets[i].y = b.dstOffsets[i].y < 0 ? 0 : b.dstOffsets[i].y > (int)c->h ? (int)c->h : b.dstOffsets[i].y;
        }
        vkCmdBlitImage(cb, sg->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                       sg->sx == c->sx && sg->sy == c->sy ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        c->gpu_newer = 1;
        return;
    }
    /* a surface in memory: drawn as a textured quad */
    GpuImg *t = tex_sync(src);
    if (!t)
        return;
    if (key && hi != lo && !warned_range++)
        LOG(LOG_ALWAYS, "vk: colour key ranges are treated as their low value");
    VkBuffer vbuf, ibuf;
    uint8_t *vp, *ip;
    VkDeviceSize voff = ring(4 * sizeof(GV), 16, &vbuf, &vp);
    VkDeviceSize ioff = ring(6 * 4, 16, &ibuf, &ip);
    GV *gv = (GV *)vp;
    float PW = (float)c->w, H = (float)c->lh, tw = (float)t->w, th = (float)t->h;
    for (int i = 0; i < 4; i++) {
        int x = i & 1 ? d[2] : d[0], y = i & 2 ? d[3] : d[1];
        int u = i & 1 ? s[2] : s[0], vv = i & 2 ? s[3] : s[1];
        /* 2D pictures keep their shape (centred with widescreen) */
        gv[i] = (GV){{2.0f * ui_x(c, (float)x) / PW - 1.0f, 2.0f * (float)y / H - 1.0f, 0, 1}, 0xffffffffu, 0xff000000u,
                     {(float)u / tw, (float)vv / th}, {0, 0}};
    }
    static const uint32_t quad[6] = {0, 1, 2, 1, 3, 2};
    memcpy(ip, quad, sizeof quad);
    PushDraw pc;
    memset(&pc, 0, sizeof pc);
    pc.stage[0] = stage_word(2, 2, 1);        /* SELECTARG1(TEXTURE) */
    pc.stage[1] = stage_word(2, 2, 1);
    pc.stage[2] = pc.stage[3] = 1;
    pc.texsmp[0] = t->slot;
    pc.texsmp[1] = -1;
    pc.texsmp[2] = c->sx == 1.0f && c->sy == 1.0f ? smp_nearest : smp_linear;   /* scaled up: smooth */
    pc.misc[0] = key ? 0x1000000u | (pix_decode(src, lo) & 0xffffff) : 0;
    GpuImg *z = zfor(dst, c);
    begin_rendering(c, z);
    c->gpu_newer = 1;
    bind_pipeline(draw_pipeline(0, 0, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, z ? z->format : VK_FORMAT_UNDEFINED, 0,
                                r_mrt, 0));
    int sc[4] = {0, 0, (int)c->w, (int)c->h};
    set_state(c, sc, VK_CULL_MODE_NONE, 0, 0, VK_COMPARE_OP_ALWAYS, 0);
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &voff);
    vkCmdBindIndexBuffer(cb, ibuf, ioff, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);
}

/* ------------------------------------------------------------------ CPU access, destruction */
static void readback_into(Surface *s, GpuImg *g)
{
    readback_reserve((VkDeviceSize)g->w * g->h * 4);
    barrier(g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {g->w, g->h, 1}};
    vkCmdCopyImageToBuffer(cb, g->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 1, &r);
    flush_wait();
    uint32_t w = s->width < g->lw ? s->width : g->lw, h = s->height < g->lh ? s->height : g->lh;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t py = (uint32_t)(((float)y + 0.5f) * g->sy);
        const uint32_t *p = (const uint32_t *)(rb_map + (size_t)(py < g->h ? py : g->h - 1) * g->w * 4);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t px = (uint32_t)ui_x(g, (float)x + 0.5f);
            surf_write(s, x, y, pix_encode(s, p[px < g->w ? px : g->w - 1]));
        }
    }
}

void vkr_cpu_access(Surface *s, int write)
{
    GpuImg *g = s->gpu;
    if (!is_rt(g) || state <= 0)
        return;
    if (g->gpu_newer) {
        readbacks++;
        if (readbacks <= 5 || readbacks % 100 == 0) {
            char nm[96];
            LOG(LOG_ALWAYS, "vk: reading back %s for the CPU (%lu so far)", surface_name(s, nm, sizeof nm), readbacks);
        }
        frame_begin();
        readback_into(s, g);
        g->gpu_newer = 0;
    }
    if (write)
        g->cpu_newer = 1;
}

/* Flip: the chain's colour images move as ddraw.c moved its memory (with t, s and t trade; else each
   surface takes the next one's and the last the first's), in call order with the draws */
void vkr_flip(Surface *s, Surface *t)
{
    if (t) {
        struct GpuImg *g = s->gpu;
        s->gpu = t->gpu;
        t->gpu = g;
        return;
    }
    struct GpuImg *g = s->gpu;
    Surface *p = s;
    for (; p->next != s; p = p->next)
        p->gpu = p->next->gpu;
    p->gpu = g;
}

void vkr_surface_destroyed(Surface *s)
{
    sm_cells_drop(s);
    if (s->gpu && state > 0)
        img_discard(s->gpu);
    s->gpu = NULL;
}

/* ------------------------------------------------------------------ presenting */
static void gamma_sync(void)
{
    if (gamma_valid && !memcmp(gamma_seen, gfx_gamma, sizeof gamma_seen))
        return;
    memcpy(gamma_seen, gfx_gamma, sizeof gamma_seen);
    gamma_valid = 1;
    VkBuffer buf;
    uint8_t *p;
    VkDeviceSize off = ring(256 * 8, 16, &buf, &p);
    uint16_t *q = (uint16_t *)p;
    for (int i = 0; i < 256; i++) {
        q[4 * i] = gfx_gamma[0][i];
        q[4 * i + 1] = gfx_gamma[1][i];
        q[4 * i + 2] = gfx_gamma[2][i];
        q[4 * i + 3] = 0xffff;
    }
    barrier(gamma_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {256, 1, 1}};
    vkCmdCopyBufferToImage(cb, buf, gamma_img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier(gamma_img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static void write_ppm(const char *name, unsigned long n, const uint8_t *px, uint32_t stride, uint32_t w, uint32_t h)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s_%05lu.ppm", dumpdir, name, n);
    FILE *f = fopen(path, "wb");
    if (!f) {
        logmsg(LOG_ALWAYS, "frame dumps: cannot write %s: %s", path, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    uint8_t *row = malloc((size_t)w * 3);
    if (!row)
        fatal("out of memory");
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *p = px + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            row[3 * x] = p[4 * x + 2];
            row[3 * x + 1] = p[4 * x + 1];
            row[3 * x + 2] = p[4 * x];
        }
        fwrite(row, 3, w, f);
    }
    free(row);
    fclose(f);
    LOG(LOG_D3D, "frame %lu dumped to %s", n, path);
}

/* Draws image g (rect r) into the target, letterboxed; target layout ends as `final`. */
static void present_pass(GpuImg *g, const int *r, VkImage timg, VkImageView tview, VkFormat tfmt, uint32_t tw,
                         uint32_t th, VkImageLayout final)
{
    raw_barrier(timg, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tview,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.color = {{0, 0, 0, 1}}}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {tw, th}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    if (g) {
        float sw = (float)(r[2] - r[0]), sh = (float)(r[3] - r[1]);
        float aspect = gfx_mode.set ? 4.0f / 3.0f : sw / sh;
        float uv[4] = {(float)r[0] / (float)g->lw, (float)r[1] / (float)g->lh, (float)r[2] / (float)g->lw,
                       (float)r[3] / (float)g->lh};
        if (g->kind == K_RT && g->ox > 0.5f) {       /* widescreen: the whole picture */
            aspect = (float)g->w / (float)g->h;
            uv[0] = uv[1] = 0.0f;
            uv[2] = uv[3] = 1.0f;
        }
        float vw = (float)tw, vh = vw / aspect;
        if (vh > (float)th) {
            vh = (float)th;
            vw = vh * aspect;
        }
        VkViewport vp = {((float)tw - vw) / 2, ((float)th - vh) / 2, vw, vh, 0, 1};
        VkRect2D sc = {{0, 0}, {tw, th}};
        bind_pipeline(present_pipeline(tfmt));
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        PushPresent pp = {{uv[0], uv[1], uv[2], uv[3]},
                          {g->slot, gamma_img->slot, smp_linear, smp_nearest}};
        vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pp, &pp);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cb);
    raw_barrier(timg, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, final);
    bound_pipe = VK_NULL_HANDLE;
}

void vkr_present(Surface *src, const int *rect)
{
    if (!vk_on())
        return;
    double t_in = wall_ms();
    frame_begin();
    Frame *f = &fr[fi];
    nframes++;
    int dump = dumping && nframes >= dump_from && nframes % dump_every == 0;
    GpuImg *g = NULL;
    if (src && !src->dead && src->mem)
        g = vk_target(src) || is_rt(src->gpu) ? rt_sync(src) : tex_sync(src);
    int r[4] = {0, 0, g ? (int)g->lw : 1, g ? (int)g->lh : 1};
    if (rect && g) {
        r[0] = rect[0] > 0 ? rect[0] : 0;
        r[1] = rect[1] > 0 ? rect[1] : 0;
        r[2] = rect[2] < (int)g->lw ? rect[2] : (int)g->lw;
        r[3] = rect[3] < (int)g->lh ? rect[3] : (int)g->lh;
        if (r[2] <= r[0] || r[3] <= r[1])
            g = NULL;
    }
    gamma_sync();
    end_rendering();
    if (g && dump) {
        readback_reserve((VkDeviceSize)g->w * g->h * 4 + (out_img ? (VkDeviceSize)out_img->w * out_img->h * 4 : 0));
        barrier(g, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {g->w, g->h, 1}};
        vkCmdCopyImageToBuffer(cb, g->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 1, &c);
    }
    if (g)
        barrier(g, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
    uint32_t img = 0;
    int acquired = 0;
    if (offscreen) {
        present_pass(g, r, out_img->image, out_img->view, out_img->format, out_img->w, out_img->h,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        out_img->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (g && dump && dump_present) {
            VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)g->w * g->h * 4,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {out_img->w, out_img->h, 1}};
            vkCmdCopyImageToBuffer(cb, out_img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 1, &c);
        }
    } else if (swapchain_ok()) {
        VkResult ar = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &img);
        if (ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR) {
            acquired = 1;
            if (ar == VK_SUBOPTIMAL_KHR)
                sc_stale = 1;
            present_pass(g, r, sc_images[img], sc_views[img], sc_format, sc_w, sc_h, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            wait = f->acquired;
            signal = sc_done[img];
        } else if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            sc_stale = 1;
        } else {
            fatal("Vulkan: vkAcquireNextImageKHR failed (%d)", ar);
        }
    }
    submit(wait, signal);
    if (acquired) {
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &signal, .swapchainCount = 1, .pSwapchains = &swapchain,
                               .pImageIndices = &img};
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR)
            sc_stale = 1;
        else if (pr != VK_SUCCESS)
            fatal("Vulkan: vkQueuePresentKHR failed (%d)", pr);
    }
    if (g && dump) {
        VKC(vkWaitForFences(dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
        int p0 = (int)lroundf((float)r[0] * g->sx), p1 = (int)lroundf((float)r[1] * g->sy);
        int p2 = (int)lroundf((float)r[2] * g->sx), p3 = (int)lroundf((float)r[3] * g->sy);
        p2 = p2 > (int)g->w ? (int)g->w : p2;
        p3 = p3 > (int)g->h ? (int)g->h : p3;
        write_ppm("frame", nframes, rb_map + (size_t)p1 * g->w * 4 + (size_t)p0 * 4, g->w * 4,
                  (uint32_t)(p2 - p0), (uint32_t)(p3 - p1));
        if (offscreen && dump_present)
            write_ppm("present", nframes, rb_map + (size_t)g->w * g->h * 4, out_img->w * 4, out_img->w, out_img->h);
    }
    fi = (fi + 1) % FRAMES;
    sh_views = 0;
    sm_slots_used = 0;
    bg_phase = 1;
    if (hp_flip && nframes % (unsigned long)hp_flip == 0)
        hp_want ^= 1;
    res_update();                 /* the next frame's render size follows the window */
    st_present_ms += wall_ms() - t_in;
    {
        double now = wall_ms(), ft = st_last > 0 ? now - st_last : 0;
        st_last = now;
        if (ft > st_max) st_max = ft;
        if (ft > 1000.0 / 60.0) st_over++;
    }
    if (nframes % 100 == 0) {
        double t = wall_ms();
        if (st_print || LOGON(LOG_D3D))
            logmsg(LOG_D3D, "vk: per frame over the last 100: %.1f draws (%.1f pipeline binds, %.2f cut-out scans), "
                   "%.1f texture uploads (%.0f KB), %.1f render "
                   "passes; %.2f ms in all, of which %.2f vertex processing, %.2f draw recording, %.2f present "
                   "(waits included); game thread CPU %.2f ms; GPU %.2f ms; slowest %.1f ms, %.0f over 16.7 ms; "
                   "%lu readbacks so far; %d pipelines; GPU idle between frames %.2f ms; %.1f image barriers",
                   st_draws / 100.0, st_binds / 100.0, st_scans / 100.0, st_uploads / 100.0, st_upload_bytes / 102400.0, st_passes / 100.0,
                   (t - st_t0) / 100.0, vk_tnl_ms / 100.0, st_draw_ms / 100.0, st_present_ms / 100.0,
                   (thread_cpu_ms() - st_cpu0) / 100.0, st_gpu_n ? st_gpu_ms / st_gpu_n : 0.0, st_max, st_over,
                   readbacks, npipes, st_gpu_gap_n ? st_gpu_gap_ms / st_gpu_gap_n : 0.0, st_barriers / 100.0);
        if ((st_print || LOGON(LOG_D3D)) && st_rt_views)
            logmsg(LOG_D3D, "vk: shadows per frame over the last 100: %.2f views, %.1f geometries, %.0f triangles "
                   "(not opaque %.1f, %.0f); "
                   "GPU %.2f ms (structures %.2f, ambient occlusion %.2f: rays %.2f, blur %.2f, applying %.2f; "
                   "reflections %.2f (%.1f draws); shadow rays the rest); CPU: view end %.3f ms (sizes %.3f), draws %.3f ms",
                   st_rt_views / 100.0, st_rt_geo / 100.0, st_rt_tris / 100.0, st_rt_cut / 100.0, st_rt_cut_tris / 100.0,
                   st_gpu_n ? st_gpu_sh_ms / st_gpu_n : 0.0,
                   st_gpu_n ? st_gpu_build_ms / st_gpu_n : 0.0,
                   st_gpu_n ? (st_gpu_ao_ms[0] + st_gpu_ao_ms[1] + st_gpu_ao_ms[2]) / st_gpu_n : 0.0,
                   st_gpu_n ? st_gpu_ao_ms[0] / st_gpu_n : 0.0, st_gpu_n ? st_gpu_ao_ms[1] / st_gpu_n : 0.0,
                   st_gpu_n ? st_gpu_ao_ms[2] / st_gpu_n : 0.0, st_gpu_n ? st_gpu_refl_ms / st_gpu_n : 0.0,
                   st_refl_draws / 100.0, st_rt_ms / 100.0, st_rt_sizes_ms / 100.0, st_rt_draw_ms / 100.0);
        if ((st_print || LOGON(LOG_D3D)) && st_taa_scenes && scn.c)
            logmsg(LOG_D3D, "vk: temporal anti-aliasing per frame over the last 100: %.2f scenes (%ux%u -> %ux%u), %.0f draws "
                   "(%.0f matched with the previous frame's, %.0f of them moving); GPU %.2f ms (FSR 2 and its copy); CPU %.3f ms",
                   st_taa_scenes / 100.0, scn.c->w, scn.c->h, scn.out ? scn.out->w : scn.hist[0] ? scn.hist[0]->w : 0,
                   scn.out ? scn.out->h : scn.hist[0] ? scn.hist[0]->h : 0,
                   st_taa_draws / 100.0, st_taa_matched / 100.0, st_taa_moving / 100.0,
                   st_gpu_n ? st_gpu_taa_ms / st_gpu_n : 0.0, st_taa_cpu_ms / 100.0);
        if ((st_print || LOGON(LOG_D3D)) && st_sm_views)
            logmsg(LOG_D3D, "vk: volumetric smoke per frame over the last 100: %.2f views, %.1f puffs taken (exhaust %.1f, "
                   "smoke %.1f, dust %.1f, splashes %.1f), %.1f drawn (%.1f with sun grids, %.1f see-through surfaces "
                   "over them, %.1f casting shadows), %.2f steps a puff, %.0f%% of the "
                   "smaller image's area; GPU %.2f ms (the first view's, %.2f of it its first passes; at most %.2f in a "
                   "frame); CPU %.3f ms",
                   st_sm_views / 100.0, st_sm_taken / 100.0, st_sm_kind[0] / 100.0, st_sm_kind[1] / 100.0,
                   st_sm_kind[2] / 100.0, st_sm_kind[3] / 100.0, st_sm_drawn / 100.0, st_sm_grid / 100.0, st_sm_glass / 100.0,
                   st_sm_casters / 100.0,
                   st_sm_drawn ? st_sm_steps / st_sm_drawn : 0.0, st_sm_views ? 100.0 * st_sm_area / st_sm_views : 0.0,
                   st_gpu_n ? st_gpu_sm_ms / st_gpu_n : 0.0, st_gpu_n ? st_gpu_sm1_ms / st_gpu_n : 0.0, st_gpu_sm_max,
                   st_sm_cpu_ms / 100.0);
        st_sm_views = 0;
        memset(st_sm_kind, 0, sizeof st_sm_kind);
        st_sm_glass = st_sm_casters = 0;
        st_sm_taken = st_sm_drawn = st_sm_grid = st_sm_steps = st_sm_area = st_sm_cpu_ms = st_gpu_sm_ms = st_gpu_sm_max = st_gpu_sm1_ms = 0;
        st_taa_scenes = st_taa_draws = st_taa_matched = st_taa_moving = 0;
        st_taa_cpu_ms = st_gpu_taa_ms = 0;
        st_rt_sizes_ms = st_rt_draw_ms = 0;
        st_rt_views = st_rt_geo = st_rt_tris = st_rt_cut = st_rt_cut_tris = 0;
        st_rt_ms = st_gpu_sh_ms = st_gpu_build_ms = st_gpu_ao_ms[0] = st_gpu_ao_ms[1] = st_gpu_ao_ms[2] = 0;
        st_gpu_refl_ms = 0;
        st_refl_draws = 0;
        st_gpu_ms = 0;
        st_gpu_n = 0;
        st_gpu_gap_ms = st_gpu_gap_n = st_barriers = 0;
        st_cpu0 = thread_cpu_ms();
        st_max = st_over = 0;
        st_draws = st_uploads = st_upload_bytes = st_passes = st_binds = st_scans = 0;
        st_draw_ms = st_present_ms = vk_tnl_ms = 0;
        st_t0 = t;
    }
}
