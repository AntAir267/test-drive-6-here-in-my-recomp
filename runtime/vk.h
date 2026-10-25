/* The Vulkan renderer (vk.c) as DirectDraw/Direct3D see it. Everything is called with the
   graphics lock held. It is on when the program runs in a window (--window) or with
   TD6_GPU_OFFSCREEN=1; otherwise (or if Vulkan cannot start) vk_on() is 0 and the CPU paths
   (swrast for frame dumps) are used as before. */
#pragma once
#include "gfx.h"

int vk_on(void);                  /* starts Vulkan on first use when wanted */
/* Is s drawn on the GPU (render targets: 3D-capable surfaces that are not textures)? */
int vk_target(const Surface *s);

/* Direct3D */
void vk_draw(const D3DState *st, int prim, const SwVertex *v, int nv, const uint16_t *idx, int ni,
             int pretransformed);
/* rects: n x (x0, y0, x1, y1), already clipped to the viewport; flags: D3DCLEAR_* */
void vk_clear(const D3DState *st, int n, const int *rects, uint32_t flags, uint32_t color, float z,
              uint32_t stencil);

/* DirectDraw on a GPU surface (vk_target): colour/depth fills and Blts, in call order */
void vk_fill(Surface *s, const int *rect, uint32_t raw);
void vk_blt(Surface *dst, const int *drect, Surface *src, const int *srect, int key, uint32_t lo, uint32_t hi);
/* A frame is presented from src (rect or NULL = all): draws it to the window, dumps it */
void vk_present(Surface *src, const int *rect);
/* Flip moved a flip chain's memory (t: s and t traded; NULL: each takes the next one's): the GPU
   images move the same way, in order with the other calls */
void vk_flip(Surface *s, Surface *t);
/* The renderer has done every call made so far (the render thread idle; at once without one) */
void vk_sync(void);
/* At exit, before anything is torn down: the render thread finishes every call (the last frame
   presented and dumped) and ends; later calls run at once */
void vk_stop(void);

/* The CPU is about to read (and with write, change) the memory of s: brings a GPU-drawn
   surface's pixels back first, and has the GPU copy refreshed before its next use. */
void vk_cpu_access(Surface *s, int write);
extern double vk_tnl_ms;          /* statistics: CPU vertex processing time (d3d.c adds to it) */
void vk_surface_destroyed(Surface *s);
/* Distance fade and far terrain (drawdist.c). vk_fade: 0, or VK_FADE(lo, hi) (0..256): the next
   draws fill only the pixels whose 8 x 8 ordered-dither threshold is in [lo, hi) / 256, so draws
   with ranges that meet fill each pixel once (a cross-fade). vk_fog_end: > 0, fogged draws reach
   full fog at that eye depth, growing to it from vk_fog_start. */
#define VK_FADE(lo, hi) (0x80000000u | (uint32_t)(lo) << 9 | (uint32_t)(hi))
extern uint32_t vk_fade;
extern int vk_nocast;               /* the next draws take shadows but cast none and are in no reflection
                                       (the far objects, drawdist.c: ray-tracing structures cost the most) */
extern float vk_fog_end, vk_fog_start;
extern int vk_tnl_min;              /* the fewest vertices a draw has for GPU transform (16) */
int vk_far_mesh(const void *verts, uint32_t nv, const uint32_t *idx, uint32_t ni);
void vk_far_draw(const D3DState *st, const TnlConst *k, uint32_t first, uint32_t count, int32_t vbase);
/* Ray-traced sun shadows, ambient occlusion and reflections (shadows.c). vk_rt_available: the
   device can trace rays. A view drawn between vk_shadow_begin and vk_shadow_end gets them;
   vk_shadow_class says what its next draws are (VK_SH_*). The terrain's mesh (map units, 12-byte
   positions) and the game's object light map (256 x 256 bytes, a cell's sun 0..255) are given once
   per map. Headlights (headlights.c): the view's beams, each lighting what is in front of it per
   pixel (draw.frag), its shadows traced from the lamp (resolve.frag). */
#define VK_BEAMS 16
typedef struct {
    float pos[3];                 /* the lamp, game units from the camera */
    float range;                  /* how far it reaches, game units */
    float dir[3], up[3];          /* where it points, and the car's up (unit, world) */
    float spread[2];              /* the beam's half-width across and up, as tangents */
    float color[3];               /* its light near it on a surface facing it (1: the surface's own colour) */
    float half;                   /* where that has fallen to half, game units */
    float lamp_dx;                /* its car's two lamps (their flares) this far either side of pos */
    float fade;                   /* how much of it is left by its distance from the camera, 0..1 */
} VkBeam;
typedef struct {
    double cam[3];                /* the camera, game units (16.16 positions / 65536) */
    float sun[3];                 /* towards the sun, world */
    float sun_color[3];           /* its colour (the game's light, before the object light map) */
    float ray_len;                /* how far a shadow ray looks, game units */
    float fade_start, fade_end;   /* shadows fade out between these eye distances */
    float offset;                 /* ray start off the surface, per unit of distance */
    float terrain_dark;           /* how much of the terrain's colour a shadow takes, per unit of
                                     the light map (0..1) */
    int terrain;                  /* the terrain mesh is the map's: cast its shadows on objects */
    int shadows;                  /* trace the sun shadows */
    int ao;                       /* ambient occlusion (shaders/ao.frag): 0 off, 1 LIGHT, 2 FULL */
    int ao_rays;                  /* its rays per small-image pixel */
    float ao_rows;                /* its small image's rows (about; the view's height over a whole number) */
    float ao_radius;              /* its rays' length, game units */
    float ao_strength, ao_terrain;   /* how much a fully closed-in pixel's ambient light is darkened
                                     (objects and cars; the terrain) */
    float ao_falloff;             /* a hit at t counts (1 - t / ao_radius) ^ ao_falloff */
    float ao_fade_start, ao_fade_end;   /* ambient occlusion fades out between these eye distances */
    float ao_foliage;             /* how much of it foliage (mostly cut-out textures) takes, 0..1 */
    float ao_parts;               /* how much of it solid see-through parts (lattices, fences) take, 0..1 */
    float ao_max;                 /* the most of a pixel's ambient light it takes, 0..1 */
    float water_dark;             /* how much of the water's colour a shadow takes, per unit of the light map */
    float water_ao;               /* how much ambient occlusion the water takes, 0..1 */
    float water_shadow;           /* how much of the water's sun part (its own and the ground's seen
                                     through it) a shadow on it takes, 0..1 */
    int refl;                     /* ray-traced reflections (shaders/refl.glsl): 0 off, 1 LIGHT, 2 FULL */
    float refl_fresnel;           /* how far their strength rises from the game's towards 1 at grazing
                                     angles (Schlick), 0..1 */
    float refl_darken;            /* how much of the colour below that rise takes away, 0..1 */
    float refl_len;               /* their rays' length, game units */
    float refl_gain;              /* how bright what they meet is shown (the game's env map is darker) */
    float terrain_detail;         /* what the terrain's detail pass leaves of its colour (in them) */
    int nbeams;                   /* headlights: the beams lighting the view (0 none), the nearest first */
    float beam_cap;               /* ... their light summed on a surface stays under this (softly) */
    float beam_haze;              /* ... how much of it the air scatters, per metre (0: none; VOLUMETRIC) */
    float beam_tint[3];           /* ... their colour (every beam's the same; green 1) */
    VkBeam beams[VK_BEAMS];
} VkShadowView;
/* PARTS: see-through model parts; SKY: the sky dome (what reflections meeting nothing show) */
enum { VK_SH_OBJECT, VK_SH_TERRAIN, VK_SH_PARTS, VK_SH_NONE, VK_SH_WATER, VK_SH_SKY };
int vk_rt_available(void);
void vk_shadow_begin(const VkShadowView *v);
void vk_shadow_class(int cls);
void vk_shadow_end(void);
int vk_shadow_terrain(const float *xyz, uint32_t nv, const uint32_t *idx, uint32_t ni);
int vk_shadow_lightmap(const uint8_t *lm);
/* Volumetric smoke and dust (smoke.c; DESIGN.md decision 4, "Volumetric smoke and dust"). In a view
   of the world (vk_smoke_view_begin .. _end) a draw made while vk_puff is set is a puff's sprite
   (d3d.c gives it to vk_smoke_take: its quad's corners in model space, colour and texture
   coordinates) and is not drawn; at the view's end its puffs are drawn as volumes. */
enum { VK_PUFF_EXHAUST, VK_PUFF_SMOKE, VK_PUFF_DUST, VK_PUFF_SPRAY };
typedef struct {
    int kind;                     /* VK_PUFF_*: an exhaust's, the ground's (SMOKE, DUST), a splash (WATER) */
    uint32_t serial;              /* the object's number: its noise, its size kept from phase to phase */
    float grow;                   /* 0..1: how far it is into its phase (its size grows on smoothly) */
    float age;                    /* game ticks, rising (the noise's slow churn) */
    float light;                  /* the object light map at its place, 0..1: the sun's share there */
    float floor_dy;               /* the water's surface relative to its centre (world units; -1e9 none) */
    float ground_dy;              /* the ground relative to its centre (world units): where its shadow ends */
} VkPuff;
typedef struct {
    float sun[3];                 /* towards the sun (world) */
    float sun_color[3];           /* its colour (the game's light) */
    int level;                    /* 2 VOLUMETRIC */
} VkSmokeView;
extern const VkPuff *vk_puff;
extern int vk_smoke_depth;        /* depth buffers can be read (the volumes do) */
extern int vk_smoke_parts;        /* the see-through parts are being drawn: glass, lattices (not puffs) are kept,
                                     to be drawn again at the view's end (the puffs behind them seen through them) */
void vk_smoke_view_begin(const VkSmokeView *v);
void vk_smoke_view_end(void);
void vk_smoke_take(const D3DState *st, const float (*pos)[3], int n, uint32_t diffuse, const float (*uv)[2]);
/* Temporal anti-aliasing and upscaling (taa.c; DESIGN.md decision 4, "Temporal anti-aliasing"):
   with "antialiasing = fsr" the race's main view (vk_scene_begin .. vk_scene_end) is drawn into
   a scene image of its own at the render scale, its projection jittered, with a motion vector
   per pixel and reactive and transparency masks; at its end FSR 2 resolves it into the back
   buffer at the screen's size, and the rest of the frame (mirrors, HUD, name labels, menus) is
   drawn over it there as before. vk_scene_class says what the next draws are (VK_SC_*). */
typedef struct {
    int32_t cam[3];               /* the camera, 16.16 game units as the game keeps it */
} VkScene;
/* DECAL: skid marks and objects' shadows (still on the ground); CAR_SHADOW: a car's shadow (it moves with the car) */
enum { VK_SC_OBJECT, VK_SC_SKY, VK_SC_TERRAIN, VK_SC_WATER, VK_SC_PARTS, VK_SC_DECAL, VK_SC_CAR_SHADOW };
int vk_taa_on(void);                    /* "antialiasing = fsr" in force (and FSR 2 can run) */
int vk_taa_available(void);             /* FSR 2 is built in and the device can run it */
int vk_dlss_available(void);            /* DLSS is built in and the device can run it (NVIDIA RTX) */
int vk_own_taa_available(void);         /* ANTI-ALIASING TAA (the port's own temporal upscaler) can run here */
/* "colour_precision = high" (DESIGN.md decision 4, "Colour precision"): the scene's colour in 16 bits a channel,
   dithered into the back buffer at its end; without FSR 2 the scene is the main view at the back buffer's size */
int vk_scene_on(void);                  /* the main view is drawn as the scene (FSR 2 or high colour precision) */
int vk_hp_available(void);              /* the device can draw the 16-bit scene */
void vk_scene_begin(const VkScene *s);
void vk_scene_camera(const VkScene *s); /* the camera as the view draws (its first draws) */
void vk_scene_class(int cls);
void vk_scene_end(void);
void vk_options(void);                  /* re-read the Enhanced options (resolution, antialiasing, vsync, textures) */
void vk_native_size(int *w, int *h);    /* the picture area in pixels */
void vk_render_info(char *out, int n);  /* what Vulkan draws at, for the menu */
