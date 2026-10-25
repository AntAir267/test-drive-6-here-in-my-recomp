/* Temporal anti-aliasing (vk.c "temporal anti-aliasing"): the race's main view is drawn into a
   scene image of its own with a jittered projection, and every draw there has a Motion block
   (binding 5, at the draw's TnlConst index pc.tnl.x): the matrix taking its vertex (tnl.vert: the
   model's position; draw.vert: this frame's Vulkan clip position) to where it was in the previous
   frame (Vulkan clip space, no jitter), this frame's jitter (Vulkan clip space, per w) and how it
   marks the reactive and transparency masks. draw.frag writes the motion vector, previous minus
   current in UV units, and the masks. Included by tnl.vert and draw.vert with MOTION_VERTEX, by
   draw.frag with MOTION. */
struct Motion {
    mat4 prev;                  /* D3D row-vector matrix, so M * v */
    vec4 jit;                   /* x, y: the jitter; z: flags (bits: 0-1 reactive by 1 alpha, 2 alpha x
                                   brightness, 3 in full; 2-3 the same for the transparency and
                                   composition mask; 4 the motion vector only where alpha >= 1/2); w:
                                   the two masks' strengths (half floats) */
};
layout(std430, set = 0, binding = 5) readonly buffer Mot { Motion m[]; } mot;
#ifdef MOTION_VERTEX
layout(location = 11) out vec3 v_cur;          /* this frame's Vulkan clip x, y, w without the jitter */
layout(location = 12) out vec3 v_prev;         /* ... the previous frame's */

/* after gl_Position is set; p: the vertex as the draw's Motion matrix takes it */
void motion_out(vec4 p, uint i)
{
    vec4 q = mot.m[i].prev * p;
    v_cur = vec3(gl_Position.xy - mot.m[i].jit.xy * gl_Position.w, gl_Position.w);
    v_prev = q.xyw;
}
#else
layout(location = 11) in vec3 v_cur;
layout(location = 12) in vec3 v_prev;
#endif
