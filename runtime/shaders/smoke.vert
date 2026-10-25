/* Volumetric smoke and dust (vk.c sm_first, sm_last): a puff's rectangle of the smaller image it is
   marched into (shaders/smoke.frag pass 1), one instance a puff, two triangles each (the puff's
   block has the rectangle in the smaller image's pixels); with pc.d.w set, the whole image drawn
   into (one instance: the other passes, the scissor keeping them to the puffs' area). */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "smoke.glsl"
layout(location = 0) flat out uint f_puff;

void main()
{
    const int corner[6] = int[6](0, 1, 2, 2, 1, 3);
    int k = corner[gl_VertexIndex % 6];
    uint i = uint(gl_InstanceIndex);
    vec4 r = pc.d.w > 0.5 ? vec4(0.0, 0.0, pc.c.xy) : sm.v[puff_at(i) + 3u];
    vec2 p = vec2((k & 1) != 0 ? r.z : r.x, (k & 2) != 0 ? r.w : r.y);
    gl_Position = vec4(p / pc.c.xy * 2.0 - 1.0, 0.0, 1.0);
    f_puff = i;
}
