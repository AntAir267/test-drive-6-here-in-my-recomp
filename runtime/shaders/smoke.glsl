/* Volumetric smoke and dust (vk.c sm_first, sm_last): the view's block of puffs (binding 6, std430, vec4s:
   SMOKE_HDR of the view's own, then SMOKE_PV a puff, back to front) and the passes' push constants.
   Included by smoke.vert, smoke.frag and smokevis.frag.

   The view (from its block's start):
     0  P00 P11 P20 P21    the projection (D3D, row vectors)
     1  ax bx ay by        D3D clip -> Vulkan clip (as TnlConst.place: the jitter, widescreen in it)
     2  P22 P32 P23, the puffs casting shadows (the last so many)
     3  mnz dz 1/W 1/H     depth range; the render target's size (GPU pixels)
     4..6                  view -> world rotation (rows: world = (dot(r0, v), dot(r1, v), dot(r2, v)))
     7  sun (view space), w: 1 the sun grid from rays
     8  the sun's colour
     9  the ambient light (the game's), w: the fog: mode | range << 2 | edge << 3
    10  the fog: start, end, density
    11  the fog colour
    12  full fog at the view's edge: x there, y from
    13  x: headlights VOLUMETRIC, the ray-traced view whose beams light the puffs (shadow.glsl; -1 none)
   A puff:
     0  centre (view space), radius (game units)
     1  colour as the game drew it, extinction at the densest (per game unit)
     2  kind (VK_PUFF_*), seed 0..1, age (ticks), the water's surface (world y from the centre)
     3  its rectangle in the smaller image's pixels
     4  the light map's sun share there, steps, its sun grid's row (-1 none), -
     5  where its shadow can fall (GPU pixels: the ball swept away from the sun to the ground below) */
#define SMOKE_HDR 16u
#define CABIN 160.0                     /* units behind a car's glass that are inside the car */
#define SMOKE_PV 6u
#define K_EXHAUST 0
#define K_SMOKE 1
#define K_DUST 2
#define K_SPRAY 3
layout(std430, set = 0, binding = 6) readonly buffer Smoke { vec4 v[]; } sm;
layout(push_constant) uniform PC {
    uvec4 a;                    /* x: the view's block (vec4 index), y: puffs, z: the depth's texture,
                                   w: the input image (pass 1: the sun grid; 2: the smaller image) */
    uvec4 b;                    /* x: the smaller depth image, y: the noise, z: a linear sampler, w: nearest */
    vec4 c;                     /* xy: the size of the image drawn into (pass 1: the smaller image's); z: S,
                                   the render target's pixels a smaller image's pixel each way; w: debug */
    vec4 d;                     /* x, y: the reactive and transparency masks' strength; z: the ray-traced
                                   view's slot (sun grid); w: 1 the whole image drawn into (smoke.vert),
                                   0 each puff's rectangle */
    uvec4 e;                    /* pass 0: x, y the see-through surfaces drawn again (their depth, their
                                   colour: its alpha), z: 1 there are some; w: pass 1 1 the cabins behind
                                   car glass kept clear, pass 2 the march's front (0: neither) */
} pc;

uint puff_at(uint i)
{
    return pc.a.x + SMOKE_HDR + SMOKE_PV * i;
}

vec4 hdr(uint k)
{
    return sm.v[pc.a.x + k];
}
