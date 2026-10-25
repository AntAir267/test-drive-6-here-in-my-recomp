/* Direct3D draws: positions arrive in Vulkan clip space (the CPU did the D3D 7 transform,
   lighting and viewport mapping, including the half-pixel offset), so this only passes through.
   Colours go out twice: smooth for Gouraud and flat for D3DSHADE_FLAT (first vertex).
   Compiled again with MOTION (temporal anti-aliasing's scene, vk.c): the position without the
   frame's jitter and where the vertex was in the previous frame (motion.glsl). */
#version 450
#ifdef MOTION
#extension GL_GOOGLE_include_directive : require
#define MOTION_VERTEX
#include "motion.glsl"
layout(push_constant) uniform PC {
    uvec4 stage;
    ivec4 texsmp;
    uvec4 misc;
    vec4 fog;
    uvec4 flags;
    uvec4 tnl;                  /* x: the draw's Motion block */
    uvec4 shadow;
} pc;
#endif
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_diffuse;
layout(location = 2) in vec4 a_specular;
layout(location = 3) in vec2 a_tc0;
layout(location = 4) in vec2 a_tc1;
layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec4 v_specular;
layout(location = 2) flat out vec4 f_diffuse;
layout(location = 3) flat out vec4 f_specular;
layout(location = 4) out vec4 v_tc;
layout(location = 5) out vec4 v_dsh;          /* (ray-traced shadows: as tnl.vert; nothing to split here) */
layout(location = 6) out vec4 v_ssh;
layout(location = 7) flat out vec4 f_dsh;
layout(location = 8) flat out vec4 f_ssh;
layout(location = 9) out vec3 v_wpos;
layout(location = 13) flat out uint v_fade;   /* (tnl.vert's instanced copies' fade: none here) */

void main()
{
    gl_Position = a_pos;
    gl_PointSize = 1.0;
    v_fade = 0u;
    v_diffuse = a_diffuse;
    f_diffuse = a_diffuse;
    v_specular = a_specular;
    f_specular = a_specular;
    v_tc = vec4(a_tc0, a_tc1);
    v_dsh = a_diffuse;
    f_dsh = a_diffuse;
    v_ssh = a_specular;
    f_ssh = a_specular;
    v_wpos = vec3(0.0);
#ifdef MOTION
    motion_out(a_pos, pc.tnl.x);    /* (the previous frame's matrix takes this frame's clip space) */
#endif
}
