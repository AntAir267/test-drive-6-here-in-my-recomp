/* The renderer's own side (vk.c) as vkthread.c calls it. vk.h's calls are vkthread.c's: in a test
   run, or with "render_thread = off", each runs its vk.c function (vkr_*) at once; otherwise they
   are recorded for the render thread, which runs the same functions in the same order (DESIGN.md
   decision 4, "The render thread"). */
#pragma once
#include "vk.h"

/* What the game side had set when a call was made, as vk.c reads it (vk.c's vk_fade, vk_nocast,
   vk_fog_end, vk_fog_start, vk_smoke_parts, vk_puff, ws_*_depth and gfx_gamma are these on its side) */
typedef struct {
    uint32_t fade;
    int nocast;
    float fog_end, fog_start;
    int smoke_parts;
    const VkPuff *puff;
    int ws_race_depth, ws_hud_depth, ws_menu_depth;
    uint16_t gamma[3][256];
} VkrCtx;
extern VkrCtx vkr;

#define VKR_TNL_MAX 4096                /* GPU-lit draws per frame (vk.c TNL_MAX) */

void vkr_draw(const D3DState *st, int prim, const SwVertex *v, int nv, const uint16_t *idx, int ni,
              int pretransformed);
void vkr_draw_tnl(const D3DState *st, int prim, const void *raw, int nv, const uint16_t *idx, int ni,
                  const TnlConst *k);
void vkr_draw_tnl_inst(const D3DState *st, int prim, const void *raw, int nv, const uint16_t *idx, int ni,
                       const TnlConst *k, const float *recs, int n);
void vkr_clear(const D3DState *st, int n, const int *rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil);
void vkr_fill(Surface *s, const int *rect, uint32_t raw);
void vkr_blt(Surface *dst, const int *drect, Surface *src, const int *srect, int key, uint32_t lo, uint32_t hi);
void vkr_present(Surface *src, const int *rect);
void vkr_cpu_access(Surface *s, int write);
void vkr_surface_destroyed(Surface *s);
int vkr_far_mesh(const void *verts, uint32_t nv, const uint32_t *idx, uint32_t ni);
void vkr_far_draw(const D3DState *st, const TnlConst *k, uint32_t first, uint32_t count, int32_t vbase);
void vkr_shadow_begin(const VkShadowView *v);
void vkr_shadow_class(int cls);
void vkr_shadow_end(void);
int vkr_shadow_terrain(const float *xyz, uint32_t nv, const uint32_t *idx, uint32_t ni);
int vkr_shadow_lightmap(const uint8_t *lm);
void vkr_smoke_view_begin(const VkSmokeView *v);
void vkr_smoke_view_end(void);
void vkr_smoke_take(const D3DState *st, const float (*pos)[3], int n, uint32_t diffuse, const float (*uv)[2]);
void vkr_scene_begin(const VkScene *s);
void vkr_scene_camera(const VkScene *s);
void vkr_scene_class(int cls);
void vkr_scene_end(void);
void vkr_options(void);
/* Flip on the GPU side: the colour images of a flip chain move as its memory did (ddraw.c) */
void vkr_flip(Surface *s, Surface *t);

/* A texture's pixels as they were when the call being run was made, if the game has written them
   since (vkthread.c keeps a copy at the write's Lock instead of waiting for the render thread), with
   its dirty counter then; NULL: the program's memory is still as it was. The render thread only. */
const uint8_t *vk_snap_pixels(const Surface *s, unsigned long *dirty);
/* the time spent making pipelines so far (ms) */
double vkr_pipe_ms(void);
