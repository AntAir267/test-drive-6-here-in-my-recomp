/* Volumetric smoke and dust with ray-traced shadows (vk.c sm_first): each puff's sun grid, 3 x 3
   x 3 points 0.6 of its radius apart along the world's axes round its centre, a ray from each
   towards the sun through the view's acceleration structures (rt.glsl: objects and cars as drawn,
   cut-outs by their textures, and the terrain), so the scene's shadows fall on the puffs (smoke.frag
   samples the grid trilinearly). Out (an image 9 wide, 3 rows a puff: x = gx + 3 gz, y = gy + 3 i):
   the sun's share, 1 lit or 0, going over to the light map's share at the place as the shadows
   fade out in the distance. */
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_GOOGLE_include_directive : require
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler samplers[];
layout(set = 0, binding = 4) uniform accelerationStructureEXT tlas[8];   /* a view slot each */
#include "smoke.glsl"
#include "shadow.glsl"
#include "rt.glsl"
layout(location = 0) flat in uint f_puff;
layout(location = 0) out vec4 o_color;

void main()
{
    ivec2 q = ivec2(gl_FragCoord.xy);
    uint i = uint(q.y) / 3u, slot = uint(pc.d.z);
    vec3 gp = vec3(float(q.x % 3), float(q.y % 3), float(q.x / 3));
    uint b = puff_at(i);
    vec4 p0 = sm.v[b], p4 = sm.v[b + 4u];
    mat3 W = transpose(mat3(hdr(4u).xyz, hdr(5u).xyz, hdr(6u).xyz));
    Globals G = sh.g[slot];
    vec3 o = W * p0.xyz + (gp - 1.0) * (0.6 * p0.w), L = G.sun.xyz;
    float dist = length(o), fade = smoothstep(G.fade.x, G.fade.y, dist);
    float vis = 1.0;
    /* objects and cars from the point; the terrain from no lower than the centre (the lower points
       of a puff on the ground are in it) */
    for (int k = 0; k < 2 && fade < 1.0 && vis > 0.0; k++) {
        vec3 s = k == 0 ? o : vec3(o.x, max(o.y, (W * p0.xyz).y), o.z);
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, tlas[slot], gl_RayFlagsTerminateOnFirstHitEXT, k == 0 ? 0x02u : 0x01u, s, 0.5, L,
                              G.sun.w);
        while (rayQueryProceedEXT(rq))
            if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
                blocks(rq, G, slot, -1.0))
                rayQueryConfirmIntersectionEXT(rq);
        if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT)
            vis = 0.0;
    }
    o_color = vec4(mix(vis, p4.x, fade), 0.0, 0.0, 1.0);
}
