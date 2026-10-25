/* Ray-traced ambient occlusion (vk.c ao_passes), after a race view is drawn, on the same
   acceleration structures as the sun shadows (rt.glsl). Drawn as one triangle over the view
   (present.vert); push constants a.z is the pass:
   0  trace, into a small image (one pixel for each S x S pixels of the view, S = m.w): from the
      surface the depth buffer holds at the block's first pixel, N short rays (k.x game units)
      over the hemisphere round its normal (attachment 2: draw.frag's face normal), cosine
      weighted, each 4 x 4 pixels of the small image sharing out one stratified set of 16 * N
      directions between them (each ray also has its own threshold for objects in the distance
      fade's dither, rt.glsl blocks). A hit counts (1 - t / k.x) ^ k.w:
      most at the surface, nothing at k.x. The terrain's pixels test only the objects and cars
      (its colour map has the hills' own shading already), the rest everything. Out: how much
      the pixel's ambient light is darkened (strength k.y on objects, k.z on the terrain, and the
      distance fade in it) and the eye distance.
   1, 2  blur across, then down: 5 taps weighed 1 2 2 2 1, so each of the 4 x 4 sets of
      directions counts the same and the pattern cancels on a flat surface, and by how near each
      tap lies to the plane of the pixel's surface (another surface counts nothing).
   3  over the view at full size: the 4 nearest pixels of the small image, weighed by place and
      by how near they lie to the plane of this pixel's surface, times how much of it the surface
      takes (attachment 2's alpha: 1, less for foliage, less where see-through parts add light
      over it), applied to the colour by blending, colour * (1 - d) + output: what is darkened is
      the colour less the sun's part (attachment 1) less the fog's part (the fog colour m.rgb, the
      fog factor worked out as the view's draws do from g, v and f.zw), so direct sun and fog are
      left as they are.
   a.y: bits 0-7 a debug view (INSANE_AO_DEBUG=1: the darkening alone, grey; 2 the fog factor, 3
   the sun's part, 4 how much the surface takes), bits 8-15 N, 16-18 ray flags for timing
   (INSANE_AO_FLAGS: 1 first hit, 2 all opaque, 4 no terrain).
   4  LIGHT's applying, instead of 3 (apply_light). */
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_GOOGLE_include_directive : require
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler samplers[];
layout(set = 0, binding = 4) uniform accelerationStructureEXT tlas[8];   /* a view slot each */
layout(push_constant) uniform PC {
    uvec4 a;                    /* x: the view's slot, y: debug view | rays << 8, z: pass, w: the input image's texture */
    ivec4 r;                    /* the view's rectangle in small-image pixels: x0 | y0 << 16, x1 | y1 << 16; z:
                                   how much ambient occlusion solid see-through parts take, w: the most it
                                   darkens (float bits) */
    vec4 k;                     /* ray length (game units), strength on objects, on the terrain, falloff power */
    vec4 m;                     /* the fog colour (rgb); w: S, view pixels per small-image pixel each way */
    vec4 f;                     /* fades out from x to y (eye distance); full fog at the view's edge: z end, w start */
    vec4 g;                     /* the view's fog: start, end, density, how (mode 1 EXP 2 EXP2 3 LINEAR | range << 2 | edge << 3) */
    vec4 v;                     /* the camera's depth axis (view z = p . v.xyz + v.w) */
} pc;
/* the pass: a pipeline each, the others' code left out of it (fewer registers) */
layout(constant_id = 0) const int SPEC_PASS = -1;
#define PASS (SPEC_PASS >= 0 ? uint(SPEC_PASS) : pc.a.z)
#include "shadow.glsl"
#include "rt.glsl"
layout(location = 0) out vec4 o_color;

vec3 oct_decode(vec2 e)
{
    vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
        v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return normalize(v);
}

/* the face normal attachment 2 has (g) at p: up for the water, towards the camera for a solid
   see-through part (a bar of a lattice, a wire: its side the camera sees) */
vec3 normal_of(vec4 g, vec3 p)
{
    if (uint(g.r * 255.0 + 0.5) == CLS_WATER)
        return coded_part(g) ? -normalize(p) : vec3(0.0, 1.0, 0.0);
    return oct_decode(g.gb * 2.0 - 1.0);
}

/* the rays test only objects and cars (the terrain and the water: the terrain's colour map has the
   hills' own shading) */
bool objects_only(vec4 g)
{
    uint c = uint(g.r * 255.0 + 0.5);
    return c < CLS_WATER || (c == CLS_WATER && !coded_part(g));
}

/* the small image at q, its taps kept inside the view */
ivec2 inside(ivec2 q)
{
    ivec2 lo = ivec2(pc.r.x & 65535, pc.r.x >> 16), hi = ivec2(pc.r.y & 65535, pc.r.y >> 16);
    return clamp(q, lo, hi - 1);
}

vec4 small(Globals G, ivec2 q)
{
    return FETCH(int(pc.a.w), inside(q));
}

/* the view pixel small-image pixel q stands for: a place in its S x S block that moves from block
   to block (along a column of blocks through all 4 columns of a block, along a row through all 4
   rows), so thin things (posts, poles, wires, lattice bars) narrower than a block still have some
   blocks of their own */
ivec2 small_px(ivec2 q)
{
    int S = int(pc.m.w);
    ivec2 o = ivec2((q.x + q.y) & 3, (3 * q.x + 2 * q.y) & 3);
    if (S >= 4)
        return q * S + ((o * S) >> 2);
    return q * S + o % S;
}

/* where the surface of small-image pixel q (eye distance dist) is: along its view pixel's ray */
vec3 small_pos(Globals G, ivec2 q, float dist)
{
    return normalize(position(G, vec2(small_px(inside(q))) + 0.5, 0.5)) * dist;
}

/* 1 on the plane through p0 with normal n0, falling to 0 at tol times p0's eye distance from it */
float plane_w(vec3 p0, vec3 n0, float d0, vec3 p, float tol)
{
    return max(1.0 - abs(dot(n0, p - p0)) / (d0 * tol), 0.0);
}

/* a unit vector as two numbers -1..1 (octahedral), and back */
vec2 oct_encode(vec3 n)
{
    vec2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? p : (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
}

/* how alike two normals are: 1 within about 35 degrees, 0 from about 70 (a post's side against
   the ground beside it: another surface, though on its plane) */
float normal_w(vec3 n0, vec3 n)
{
    return smoothstep(0.35, 0.8, dot(n0, n));
}

/* the small image's normal (b, a: octahedral) */
vec3 small_n(vec4 t)
{
    return oct_decode(t.zw);
}

/* How much the ambient light at p (eye distance dist, normal n) is darkened: N rays of the
   hemisphere. Small-image pixels (full = false): ray i in azimuth sector i of N (so the rays of
   neighbouring pixels go much the same way: faster), the 16 pixels of a 4 x 4 block on a 4 x 4
   grid of elevation (sin^2) and azimuth in it, as a Latin square: every row and every column of
   the block has all 4 elevations (and the rows all 4 azimuths), so the blur's two passes each mix
   them. A view pixel on its own (full = true, no blur after it): its N rays in 4 elevation bands
   and N / 4 azimuth sectors, offset by its place in a 4 x 4 block. Each ray's dither threshold
   from a low-discrepancy sequence. */
float occlusion(Globals G, vec3 p, vec3 n, float dist, bool terrain, uint N, ivec2 q, bool full)
{
    float sg = n.z >= 0.0 ? 1.0 : -1.0, aa = -1.0 / (sg + n.z), bb = n.x * n.y * aa;
    vec3 t1 = vec3(1.0 + sg * n.x * n.x * aa, sg * bb, -sg * n.x), t2 = vec3(bb, sg + n.y * n.y * aa, -n.y);
    /* off the surface along its normal (more with the distance: the depth buffer's precision), and
       towards the camera */
    vec3 o = p + n * (0.5 + dist * 4.0e-4) - p * (dist * 1.5e-7);
    float R = pc.k.x, occ = 0.0;
    uint bx = uint(q.x & 3), by = uint(q.y & 3), set = bx + 4u * by;
    for (uint i = 0u; i < N; i++) {
        vec2 u = full ? vec2((float(i & 3u) + (float(bx) + 0.5) / 4.0) / 4.0,
                             (float(i >> 2) + (float(by) + 0.5) / 4.0) / float(max(N / 4u, 1u)))
                      : vec2((float((bx + by) & 3u) + 0.5) / 4.0,
                             (float(i) + (float((bx + 2u * by) & 3u) + 0.5) / 4.0) / float(N));
        float thr = fract(0.5 + float(set * N + i) * 0.5497004779);
        float rr = sqrt(u.x), phi = 6.2831853 * u.y;
        vec3 d = t1 * (rr * cos(phi)) + t2 * (rr * sin(phi)) + n * sqrt(max(1.0 - u.x, 0.0));
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, tlas[pc.a.x], ((pc.a.y >> 16) & 1u) * gl_RayFlagsTerminateOnFirstHitEXT |
                              ((pc.a.y >> 17) & 1u) * gl_RayFlagsOpaqueEXT, terrain || ((pc.a.y >> 18) & 1u) != 0u ? 0x02u : 0x03u, o, 0.0, d, R);
        while (rayQueryProceedEXT(rq))
            if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
                blocks(rq, G, pc.a.x, thr))
                rayQueryConfirmIntersectionEXT(rq);
        if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT)
            occ += pow(max(1.0 - rayQueryGetIntersectionTEXT(rq, true) / R, 0.0), pc.k.w);
    }
    /* at most r.w of the ambient light (the light bounced off what closes it in stays) */
    return clamp(occ / float(max(N, 1u)) * (terrain ? pc.k.z : pc.k.y), 0.0, intBitsToFloat(pc.r.w));
}

vec4 trace(Globals G, ivec2 q)
{
    ivec2 px = small_px(q);
    vec4 g = FETCH(G.slots.w, px);
    uint cls = uint(g.r * 255.0 + 0.5);
    float depth = FETCH(G.slots2.x, px).r;
    if (cls == 0u || (depth >= 1.0 && cls != CLS_WATER))
        return vec4(0.0);                       /* nothing (the sky, the HUD, ...) */
    vec3 p = cls == CLS_WATER ? water_position(G, vec2(px) + 0.5, g) : position(G, vec2(px) + 0.5, depth);
    vec3 n = normal_of(g, p);
    float dist = length(p);
    float fade = 1.0 - smoothstep(pc.f.x, pc.f.y, dist);
    if (fade <= 0.0)
        return vec4(0.0, dist, oct_encode(n));  /* too far */
    float d = occlusion(G, p, n, dist, objects_only(g), (pc.a.y >> 8) & 255u, q, false) * fade;
    return vec4(d, dist, oct_encode(n));
}

vec4 blur(Globals G, ivec2 q, ivec2 dir)
{
    vec4 c = small(G, q);
    if (c.y <= 0.0)
        return c;
    vec3 n0 = small_n(c), p0 = small_pos(G, q, c.y);
    const float W[5] = float[5](1.0, 2.0, 2.0, 2.0, 1.0);
    float s = 2.0 * c.x, ws = 2.0;
    for (int j = -2; j <= 2; j++) {
        if (j == 0)
            continue;
        vec4 t = small(G, q + dir * j);
        float w = t.y > 0.0 ? W[j + 2] * plane_w(p0, n0, c.y, small_pos(G, q + dir * j, t.y), 0.015) *
                                  normal_w(n0, small_n(t)) : 0.0;
        s += w * t.x;
        ws += w;
    }
    return vec4(s / ws, c.yzw);
}

/* the fog factor at p0 (eye distance d0; draw.frag, tnl.vert) */
float fog_at(vec3 p0, float d0)
{
    uint how = uint(pc.g.w), mode = how & 3u;
    float fog = 1.0, zv = dot(p0, pc.v.xyz) + pc.v.w;
    if (mode != 0u) {
        float e = (how & 4u) != 0u ? d0 : abs(zv);
        fog = mode == 3u ? (pc.g.y != pc.g.x ? (pc.g.y - e) / (pc.g.y - pc.g.x) : 1.0)
                         : mode == 1u ? exp(-pc.g.z * e) : exp(-(pc.g.z * e) * (pc.g.z * e));
        if ((how & 8u) != 0u)
            fog *= clamp((pc.f.z - zv) / (pc.f.z - pc.f.w), 0.0, 1.0);
        fog = clamp(fog, 0.0, 1.0);
    }
    return fog;
}

/* LIGHT's applying (pass 4): as pass 3, cheaper. The 4 nearest pixels of the small image in two
   gathers (their darkening, their eye distances), weighed by place and by how near they lie to the
   plane of this pixel's surface (their view pixels' rays: Globals ray0, rayx, rayy); no normals, no
   ring */
void apply_light(Globals G, ivec2 q)
{
    vec2 f = vec2(q) / pc.m.w;
    ivec2 b = ivec2(floor(f));
    f -= vec2(b);
    vec2 tsz = vec2(textureSize(sampler2D(textures[int(pc.a.w)], samplers[G.slots2.z]), 0));
    vec2 uv = (vec2(b) + 1.0) / tsz;
    vec4 gx = textureGather(sampler2D(textures[int(pc.a.w)], samplers[G.slots2.z]), uv, 0);
    if (max(max(gx.x, gx.y), max(gx.z, gx.w)) < 2.5 / 255.0)
        discard;
    vec4 g = FETCH(G.slots.w, q);
    uint cls = uint(g.r * 255.0 + 0.5);
    bool water = cls == CLS_WATER;
    float depth = FETCH(G.slots2.x, q).r;
    if ((depth >= 1.0 && !water) || cls == 0u || g.a < 0.5 / 255.0)
        discard;
    vec3 p0 = water ? water_position(G, vec2(q) + 0.5, g) : position(G, vec2(q) + 0.5, depth);
    float d0 = length(p0);
    vec3 n0 = normal_of(g, p0);
    vec4 gy = textureGather(sampler2D(textures[int(pc.a.w)], samplers[G.slots2.z]), uv, 1);
    /* the gathers' order: (0, 1), (1, 1), (1, 0), (0, 0); a column or row past the view's edge counts nothing */
    ivec2 hi = ivec2(pc.r.y & 65535, pc.r.y >> 16);
    vec2 in1 = vec2(b.x + 1 < hi.x ? 1.0 : 0.0, b.y + 1 < hi.y ? 1.0 : 0.0);
    f *= in1;
    vec4 bw = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y)) +
              1.0e-3 * vec4(in1.y, in1.x * in1.y, in1.x, 1.0);
    float h0 = dot(n0, p0), tol = 1.0 / (d0 * 0.015);
    const ivec2 o4[4] = ivec2[4](ivec2(0, 1), ivec2(1, 1), ivec2(1, 0), ivec2(0, 0));
    float s = 0.0, ws = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 px = vec2(small_px(b + o4[i]));
        vec3 r = G.ray0.xyz + px.x * G.rayx.xyz + px.y * G.rayy.xyz;
        float w = gy[i] > 0.0 ? bw[i] * max(1.0 - abs(dot(n0, r) * inversesqrt(dot(r, r)) * gy[i] - h0) * tol, 0.0) : 0.0;
        s += w * gx[i];
        ws += w;
    }
    float d = ws > 1.0e-4 ? s / ws : 0.0;
    d *= water && coded_part(g) ? intBitsToFloat(pc.r.z) : g.a;
    if (d < 2.0 / 255.0)
        discard;
    float fog = fog_at(p0, d0);
    vec3 sun = FETCH(G.slots.z, q).rgb;
    o_color = vec4(d * min(sun + pc.m.rgb * (1.0 - fog), vec3(1.0)), 1.0 - d);
}

void main()
{
    Globals G = sh.g[pc.a.x];
    ivec2 q = ivec2(gl_FragCoord.xy);
    uint dbg = pc.a.y & 255u;
    if (PASS == 0u) {
        o_color = trace(G, q);
        return;
    }
    if (PASS <= 2u) {
        o_color = blur(G, q, PASS == 1u ? ivec2(1, 0) : ivec2(0, 1));
        return;
    }
    if (PASS == 4u) {
        apply_light(G, q);
        return;
    }
    /* apply: the 4 nearest pixels of the small image (its pixel q stands for view pixel q * S) */
    vec2 f = vec2(q) / pc.m.w;
    ivec2 b = ivec2(floor(f));
    f -= vec2(b);
    /* first their darkening alone, in one gather (most pixels have none round them) */
    vec2 tsz = vec2(textureSize(sampler2D(textures[int(pc.a.w)], samplers[G.slots2.z]), 0));
    vec4 gx = textureGather(sampler2D(textures[int(pc.a.w)], samplers[G.slots2.z]), (vec2(b) + 1.0) / tsz, 0);
    if (max(max(gx.x, gx.y), max(gx.z, gx.w)) < 2.5 / 255.0 && dbg == 0u) {
        /* nothing darkened by the 4 nearest: unless the ring round them darkens something (a thin
           thing with blocks of its own a little further off), nothing here */
        float m = 0.0;
        for (int y = -1; y <= 2; y++)
            for (int x = -1; x <= 2; x++)
                if (x < 0 || x > 1 || y < 0 || y > 1)
                    m = max(m, small(G, b + ivec2(x, y)).x);
        if (m < 2.5 / 255.0)
            discard;
    }
    vec4 t[4] = vec4[4](small(G, b), small(G, b + ivec2(1, 0)), small(G, b + ivec2(0, 1)), small(G, b + ivec2(1, 1)));
    vec4 g = FETCH(G.slots.w, q);
    bool water = uint(g.r * 255.0 + 0.5) == CLS_WATER;
    float depth = FETCH(G.slots2.x, q).r;
    if ((depth >= 1.0 && !water) || uint(g.r * 255.0 + 0.5) == 0u)
        discard;                                /* nothing (the sky, ...) */
    if (g.a < 0.5 / 255.0 && dbg == 0u)
        discard;                                /* takes none (under glass) */
    vec3 p0 = water ? water_position(G, vec2(q) + 0.5, g) : position(G, vec2(q) + 0.5, depth);
    float d0 = length(p0);
    vec3 n0 = normal_of(g, p0);
    if (max(max(t[0].x, t[1].x), max(t[2].x, t[3].x)) < 2.5 / 255.0 && dbg == 0u) {
        /* nothing darkened round here, unless this pixel's surface is one they miss (a thin thing:
           none of them at about its distance and facing about its way; the ring round them may
           have its own) */
        bool near = false;
        for (int i = 0; i < 4; i++)
            near = near || (abs(t[i].y - d0) < d0 * 0.03 && dot(small_n(t[i]), n0) > 0.6);
        if (near)
            discard;
    }
    float bw[4] = float[4]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    const ivec2 o4[4] = ivec2[4](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1), ivec2(1, 1));
    float s = 0.0, ws = 0.0;
    for (int i = 0; i < 4; i++)
        if (t[i].y > 0.0) {
            float w = (bw[i] + 1.0e-3) * plane_w(p0, n0, d0, small_pos(G, b + o4[i], t[i].y), 0.015) *
                      normal_w(n0, small_n(t[i]));
            s += w * t[i].x;
            ws += w;
        }
    if (ws < 1.0e-4)                            /* none on this surface: the ring round them */
        for (int y = -1; y <= 2; y++)
            for (int x = -1; x <= 2; x++)
                if (x < 0 || x > 1 || y < 0 || y > 1) {
                    vec4 u = small(G, b + ivec2(x, y));
                    if (u.y > 0.0) {
                        float w = plane_w(p0, n0, d0, small_pos(G, b + ivec2(x, y), u.y), 0.015) *
                                  normal_w(n0, small_n(u));
                        s += w * u.x;
                        ws += w;
                    }
                }
    /* times how much of it the surface takes */
    float d = ws > 1.0e-4 ? s / ws : 0.0;     /* (none on this surface within 2 blocks: none) */
    if (ws <= 1.0e-4 && dbg == 6u) {           /* INSANE_AO_DEBUG=6: these pixels in red */
        o_color = vec4(1.0, 0.0, 0.0, 0.0);
        return;
    }
    d *= uint(g.r * 255.0 + 0.5) == CLS_WATER && coded_part(g) ? intBitsToFloat(pc.r.z) : g.a;
    /* the fog factor (draw.frag, tnl.vert), so the fog's part of the colour is left alone */
    uint how = uint(pc.g.w), mode = how & 3u;
    float fog = 1.0, zv = dot(p0, pc.v.xyz) + pc.v.w;
    if (mode != 0u) {
        float e = (how & 4u) != 0u ? d0 : abs(zv);
        fog = mode == 3u ? (pc.g.y != pc.g.x ? (pc.g.y - e) / (pc.g.y - pc.g.x) : 1.0)
                         : mode == 1u ? exp(-pc.g.z * e) : exp(-(pc.g.z * e) * (pc.g.z * e));
        if ((how & 8u) != 0u)
            fog *= clamp((pc.f.z - zv) / (pc.f.z - pc.f.w), 0.0, 1.0);
        fog = clamp(fog, 0.0, 1.0);
    }
    if (dbg != 0u) {                            /* INSANE_AO_DEBUG: 1 the darkening alone, 2 the fog factor, 3 the sun's part */
        o_color = vec4(dbg == 2u ? vec3(fog) : dbg == 3u ? FETCH(G.slots.z, q).rgb : dbg == 4u ? vec3(g.a) : dbg == 5u ? vec3(fract(d0 / 300.0), water ? 1.0 : 0.0, 0.0)
                                                                                     : vec3(1.0 - d), 0.0);
        return;
    }
    if (d < 2.0 / 255.0)
        discard;
    vec3 sun = FETCH(G.slots.z, q).rgb;
    o_color = vec4(d * min(sun + pc.m.rgb * (1.0 - fog), vec3(1.0)), 1.0 - d);
}
