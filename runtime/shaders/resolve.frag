/* Ray-traced sun shadows, after a race view is drawn (vk.c vk_shadow_end): one ray per pixel from
   the surface the depth buffer holds towards the sun. Where something is in the way, the part of
   the pixel's colour the sun gave (attachment 1, written by draw.frag and blended like the colour)
   is taken away: the pipeline blends with REVERSE_SUBTRACT, colour - output. Attachment 2 has the
   surface's class and n.l (draw.frag: the terrain, where only the frame's objects and cars cast,
   the game's colour map having the hills' shadows already; an object, where everything casts, the
   terrain too; the water and solid see-through parts by their distance). Leaves, lattices and
   fences: candidate hits on geometry with cut-outs read the texture's alpha at the hit (the draw's
   own vertices, by device address); an object fading in or out by the distance fade blocks the
   pixels its dither would draw (the pixel's own threshold), so its shadow cross-fades with it.
   Drawn as one triangle over the view (present.vert). Push constants a: x the view's slot, y a
   debug view (INSANE_RT_DEBUG), z the pass, w the input image's texture.
   Hard edges (INSANE_SH_SOFT=0): pass 0 traces and takes away at once.
   Soft edges, as the sun's disc gives them (b.w: the tangent of its angular size): sharp where a
   blocker touches the surface, softer the farther it is (contact hardening):
   1  trace into an image T (a pixel per b.x x b.x view pixels: 2 with 4X anti-aliasing, the
      block's first pixel that takes one): how much shadow, the half-width of the penumbra there in
      T's pixels (from the blocker's distance, the receiver's and its n.l), the eye distance
   5  U, a quarter of T's size each way: the mean of each 4 x 4 of T on one surface, the widest
      penumbra (T and U RGBA8: sh_enc)
   3, 4  filter U across, then down: a box of the penumbra's half-width (for a pixel without a hit
      the mean of its neighbours' with one), at most b.y pixels, over the taps on the pixel's own
      surface (its plane: the eye distance, the normal)
   2  take away at full size: T's hard shadow, where the penumbra is wider than U's pixel U's soft
      one (its 4 nearest pixels on this surface), at least b.z of the hard one (thin casters stay).
      Without soft edges (c.z 0) T alone (4X anti-aliasing's quarter of the rays).
   6  headlights (vk.c, before the sun's): their light into an image half the size (beam_half),
   7  added over the view (beam_apply).
   8  the lamps' flares, added after the sun's shadows (flare_pass).
   Both 0 and 2 take the volumetric smoke's shadow too (c.w: its image * 16 + its scale, vk.c
   sm_first; 0 none): of the sun the surface still has, what the puffs let through (smoke_sun). */
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
    uvec4 a;                    /* x: the view's slot, y: debug view, z: pass, w: the input image's texture */
    vec4 b;                     /* x: T's view pixels per pixel each way, y: the widest half-width (T pixels),
                                   z: how much of the hard shadow stays, w: tan of the sun's angular size */
    vec4 c;                     /* x: the focal length in T's pixels; y: 1 the nearest hit (else the first found);
                                   z: U's texture (soft edges; 0 none); w: the puffs' shadow (texture * 16 + scale) */
    ivec4 r;                    /* the view in T's pixels: x0 y0 x1 y1 (U's: halved) */
} pc;
/* the pass: a pipeline each, the others' code left out of it (fewer registers) */
layout(constant_id = 0) const int SPEC_PASS = -1;
#define PASS (SPEC_PASS >= 0 ? uint(SPEC_PASS) : pc.a.z)
#include "shadow.glsl"
layout(location = 0) out vec4 o_color;

#include "rt.glsl"

const uint BAYER[64] = uint[64](0u, 32u, 8u, 40u, 2u, 34u, 10u, 42u, 48u, 16u, 56u, 24u, 50u, 18u, 58u, 26u,
                                12u, 44u, 4u, 36u, 14u, 46u, 6u, 38u, 60u, 28u, 52u, 20u, 62u, 30u, 54u, 22u,
                                3u, 35u, 11u, 43u, 1u, 33u, 9u, 41u, 51u, 19u, 59u, 27u, 49u, 17u, 57u, 25u,
                                15u, 47u, 7u, 39u, 13u, 45u, 5u, 37u, 63u, 31u, 55u, 23u, 61u, 29u, 53u, 21u);

/* the surface's normal at a view pixel (attachment 2 g; p its place): the water's up, a solid
   see-through part's towards the camera */
vec3 surface_normal(vec4 g, vec3 p)
{
    if (uint(g.r * 255.0 + 0.5) == CLS_WATER)
        return coded_part(g) ? -normalize(p) : vec3(0.0, 1.0, 0.0);
    vec2 e = g.gb * 2.0 - 1.0;
    vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
        v.xy = (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    return normalize(v);
}

/* Pixel px's shadow: x how much of its sun part a shadow takes (the distance fade in it; -1: not a
   surface that takes one), y the blocker's distance (0: none), z the eye distance, w its n.l */
vec4 shadow(Globals G, ivec2 px)
{
    /* the surface's class and n.l (draw.frag): 0 nothing; 1..127 the terrain, 128 the water or a
       solid see-through part, 129..255 an object */
    vec4 g = FETCH(G.slots.w, px);
    uint geo = uint(g.r * 255.0 + 0.5);
    bool coded = geo == CLS_WATER, part = coded && coded_part(g), water = coded && !part;
    float nl = part ? 1.0 : water ? clamp(G.sun.y, 0.0, 1.0) : float((geo - 1u) & 127u) / 126.0;
    if (geo == 0u || nl < 0.5 / 126.0)
        return vec4(-1.0);              /* nothing, or facing away: the lighting has it dark */
    float depth = FETCH(G.slots2.x, px).r;
    if (depth >= 1.0 && !coded)
        return vec4(-1.0);
    vec3 sun = FETCH(G.slots.z, px).rgb;
    if (max(sun.r, max(sun.g, sun.b)) < 0.5 / 255.0 && pc.a.y == 0u)
        return vec4(-1.0);              /* nothing for a shadow to take */
    /* (the water: from its surface, which takes shadows from objects and cars as the terrain does;
       solid see-through parts from theirs) */
    vec3 p = coded ? water_position(G, vec2(px) + 0.5, g) : position(G, vec2(px) + 0.5, depth), L = G.sun.xyz;
    float dist = length(p);
    if (part) {                         /* (its side the camera sees: facing away, the lighting has it dark) */
        nl = dot(-p / max(dist, 1.0e-3), L);
        if (nl < 0.5 / 126.0)
            return vec4(-1.0);
    }
    float fade = 1.0 - smoothstep(G.fade.x, G.fade.y, dist);
    if (fade <= 0.0)
        return vec4(-1.0);
    /* off the surface: towards the sun, as far as takes it the offset above the surface (it grows
       with the distance), and for the depth buffer's precision (it falls with the square of the
       distance) towards the camera */
    vec3 o = p + L * ((0.25 + G.fade.z * dist) / max(nl, 0.15)) - p * (dist * 1.5e-7);
    if (pc.a.y == 3u)                   /* INSANE_RT_DEBUG=3: all but the ray (timing) */
        return vec4(fade * 0.001 * clamp(o.x, 0.0, 1.0), 0.0, dist, nl);
    /* an object in the distance fade blocks where its dither draws it: this pixel's threshold */
    uvec2 q8 = uvec2(px) & 7u;
    float thr = (float(BAYER[q8.y * 8u + q8.x]) + 0.5) / 64.0;
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas[pc.a.x], pc.c.y > 0.5 ? 0u : gl_RayFlagsTerminateOnFirstHitEXT,
                          geo < CLS_WATER || water ? 0x02u : 0x03u, o, 0.0, L, G.sun.w);
    while (rayQueryProceedEXT(rq))
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            blocks(rq, G, pc.a.x, thr))
            rayQueryConfirmIntersectionEXT(rq);
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return vec4(0.0, 0.0, dist, nl);
    return vec4(water ? fade * G.cam.w : fade, rayQueryGetIntersectionTEXT(rq, true), dist, nl);
}

/* T and U (RGBA8): r how much shadow, g the penumbra's half-width (in the image's own pixels, / 16),
   b a the eye distance (log2 / 16 in 16 bits; 0: not a surface that takes one) */
vec4 sh_enc(float s, float r, float z)
{
    if (z <= 0.0)
        return vec4(0.0);
    uint l = clamp(uint(log2(max(z, 1.0)) / 16.0 * 65535.0 + 0.5), 1u, 65535u);
    return vec4(s, clamp(r / 16.0, 0.0, 1.0), float(l >> 8) / 255.0, float(l & 255u) / 255.0);
}

vec4 sh_dec(vec4 t)
{
    uint l = uint(t.b * 255.0 + 0.5) << 8 | uint(t.a * 255.0 + 0.5);
    return vec4(t.r, t.g * 16.0, l == 0u ? 0.0 : exp2(float(l) / 65535.0 * 16.0), 0.0);
}

/* U: a pixel per UF x UF of T */
#define UF 4

/* U's pixel q stands for view pixel q * UF * b.x */
ivec2 u_inside(ivec2 q)
{
    return clamp(q, pc.r.xy / UF, (pc.r.zw + UF - 1) / UF - 1);
}

/* U at q (pass 5): the UF x UF pixels of T it covers, those on the first's surface (by eye
   distance): their mean shadow, their widest penumbra (in U's pixels), the first's eye distance */
vec4 down(Globals G, ivec2 q)
{
    float s = 0.0, n = 0.0, r = 0.0, z = -1.0;
    for (int k = 0; k < UF * UF; k++) {
        vec4 t = sh_dec(FETCH(int(pc.a.w), min(q * UF + ivec2(k % UF, k / UF), pc.r.zw - 1)));
        if (t.z <= 0.0)
            continue;
        if (z < 0.0)
            z = t.z;
        if (abs(t.z - z) > z * 0.03)
            continue;
        s += t.x;
        n += 1.0;
        r = max(r, t.y / float(UF));
    }
    return n > 0.0 ? sh_enc(s / n, r, z) : vec4(0.0);
}

/* the filter along dir over U (pc.a.w): a box of the penumbra's half-width (for a pixel without
   one, the mean of its neighbours' with one), over the taps on the pixel's own surface (its plane:
   normal from attachment 2, the eye distance). The taps' rays step from the pixel's in clip space:
   h0 + j hd. */
float plane_ok(vec4 h, vec3 n0, float ndd, float z0, float z)
{
    if (z <= 0.0)
        return 0.0;
    vec3 v = h.xyz / h.w;
    float dn = dot(n0, v) * inversesqrt(dot(v, v));     /* the plane's distance along the tap's ray */
    float want = abs(dn) > 1.0e-4 ? ndd / dn : -1.0;
    return want > 0.0 ? max(1.0 - abs(z - want) / (z0 * 0.02), 0.0) : 0.0;
}

vec4 filter_pass(Globals G, ivec2 q, ivec2 dir)
{
    vec4 c = sh_dec(FETCH(int(pc.a.w), q));
    if (c.z <= 0.0)
        return vec4(0.0);               /* not a surface that takes one */
    int R = int(pc.b.y);
    /* the ray of view pixel q * UF S in homogeneous clip space -> world, and its step per U pixel */
    float step = float(UF * int(pc.b.x));
    vec2 ndc = (vec2(q) * step + 0.5) * G.place2.zw * 2.0 - 1.0;
    vec4 h0 = G.inv_vp * vec4((ndc.x - G.place.y) / G.place.x, (ndc.y - G.place.w) / G.place.z,
                              (0.5 - G.place2.x) / G.place2.y, 1.0);
    vec2 dn = vec2(dir) * step * G.place2.zw * 2.0;
    vec4 hd = G.inv_vp * vec4(dn.x / G.place.x, dn.y / G.place.z, 0.0, 0.0);
    vec3 p0 = normalize(h0.xyz / h0.w) * c.z;
    vec3 n0 = surface_normal(FETCH(G.slots.w, q * UF * int(pc.b.x)), p0);
    float ndd = dot(n0, p0);
    /* the half-width: its own, or its neighbours' on its surface */
    float k = c.y;
    if (k <= 0.0) {
        float rsum = 0.0, rw = 0.0;
        for (int j = -4; j <= 4; j++) {
            if (j == 0 || abs(j) > R)
                continue;
            vec4 u = sh_dec(FETCH(int(pc.a.w), u_inside(q + dir * j)));
            if (u.y > 0.0) {
                float ok = plane_ok(h0 + float(j) * hd, n0, ndd, c.z, u.z);
                rsum += ok * u.y;
                rw += ok;
            }
        }
        k = rw > 0.0 ? rsum / rw : 0.0;
    }
    k = min(k, float(R));
    if (k < 0.25)
        return sh_enc(c.x, k, c.z);     /* a hard edge here (or none) */
    float s = c.x, ws = 1.0;
    for (int j = -4; j <= 4; j++) {
        float box = clamp(k + 0.5 - abs(float(j)), 0.0, 1.0);
        if (j == 0 || box <= 0.0)
            continue;
        vec4 u = sh_dec(FETCH(int(pc.a.w), u_inside(q + dir * j)));
        float wt = box * plane_ok(h0 + float(j) * hd, n0, ndd, c.z, u.z);
        s += wt * u.x;
        ws += wt;
    }
    return sh_enc(s / ws, k, c.z);
}

/* the volumetric smoke's shadow at view pixel px (eye distance eye): the sun its puffs let through
   (smoke.frag pass 4: an image S times smaller, its eye distance with it), its 4 nearest pixels
   (gathered: none in shadow, the rest not needed) weighed by place and by how near their eye
   distance is to this one's */
float smoke_sun(Globals G, ivec2 px, float eye)
{
    if (pc.c.w < 1.0 || eye <= 0.0)
        return 1.0;
    int v = int(pc.c.w + 0.5), slot = v >> 4, S = v & 15;
#define SMT sampler2D(textures[slot], samplers[G.slots2.z])     /* (nearest, clamped to the edge) */
    vec2 f = (vec2(px) + 0.5) / float(S) - 0.5, b = floor(f);
    f -= b;
    vec2 uv = (b + 1.0) / vec2(textureSize(SMT, 0));      /* (between the 4: x (b.x, b.y + 1), y (+1, +1), z (+1, b.y), w b) */
    vec4 r = textureGather(SMT, uv, 0);
    if (min(min(r.x, r.y), min(r.z, r.w)) > 0.999)
        return 1.0;
    vec4 d = abs(textureGather(SMT, uv, 1) - eye) / eye;
#undef SMT
    vec4 w = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y)) * max(1.0 - d / 0.05, 0.0);
    float ws = w.x + w.y + w.z + w.w;
    if (ws > 1.0e-3)
        return dot(w, r) / ws;
    float bd = d.w, bv = r.w;           /* (none near this surface: the nearest) */
    if (d.x < bd) {
        bd = d.x;
        bv = r.x;
    }
    if (d.y < bd) {
        bd = d.y;
        bv = r.y;
    }
    return d.z < bd ? r.z : bv;
}

/* Headlights (passes 6 and 7, after the ambient occlusion, before the sun's shadows): the beams'
   light on a surface, the one lighting it most taken away where a ray towards its lamp meets
   something (cars and objects, cut-outs as the sun's; the terrain with beamn.z), the sum softly kept
   under hl.w, worked out at half the size each way (the light is smooth; a quarter of the rays), is
   added times the surface's own colour at full size. No work in the draws. */
/* 1 where a ray from p (normal n) towards beam b's lamp meets something, else 0 */
float beam_blocked(Globals G, ivec2 px, vec3 p, vec3 n, int b)
{
    vec3 to = sh.g[pc.a.x].beams[b].pos.xyz - p;
    float dist = length(to), eye = length(p);
    to /= max(dist, 1.0e-3);
    /* off the surface towards the lamp (more where it is seen at a slant), and towards the camera
       for the depth buffer's precision */
    float nl = dot(n, n) > 0.0 ? max(dot(n, to), 0.15) : 1.0;
    vec3 o = p + to * ((0.25 + G.fade.z * eye) / nl) - p * (eye * 1.5e-7);
    uvec2 q8 = uvec2(px) & 7u;
    float thr = (float(BAYER[q8.y * 8u + q8.x]) + 0.5) / 64.0;
    if (G.beamn.y == 3 && o.x < 1.0e30)         /* (INSANE_HL_DEBUG=3: all but the ray, timing) */
        return 0.0;
    rayQueryEXT rq;
    /* (to 60 cm short of the lamp: its own car's nose, which may stand out past it, casts nothing) */
    rayQueryInitializeEXT(rq, tlas[pc.a.x], gl_RayFlagsTerminateOnFirstHitEXT | (G.beamn.y == 4 ? gl_RayFlagsOpaqueEXT : 0u),
                          G.beamn.z != 0 ? 0x03u : 0x02u, o, 0.0, to, max(dist - 40.0, 0.0));
    while (rayQueryProceedEXT(rq))
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            blocks(rq, G, pc.a.x, thr))
            rayQueryConfirmIntersectionEXT(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 0.0 : 1.0;
}

/* VOLUMETRIC: the light the air scatters towards the eye along the view ray d (unit, from the camera)
   out to tmax: for each beam, over the stretch of the ray inside its reach and in front of its lamp,
   4 points (dithered by thr: the temporal anti-aliasing averages them) of its light, forward
   scattering (Henyey-Greenstein 0.5: strongest looking into a lamp), times hl3.w; the beam giving
   the most loses it where a ray from the middle of its stretch to its lamp meets something (a car in
   front of the lamp shades the air beyond it). Softly kept under 0.35. */
/* the air's share of beam b's light at q (camera-relative world): not the beam's crisp cone and
   low-beam cut-off, which in the air read as a solid tube, but a soft round one (Gaussian across,
   twice as tall), strongest by the lamp and fading within about AIR_NEAR, through air that is not
   even (the smoke's tiled noise in the world: wisps rather than a fog bank) */
#define AIR_NEAR 600.0                  /* game units (9 m): where the air's glow has halved */
/* the volumetric smoke's tiled noise (vk.c sm_noise_make; smoke.frag noise3: 32 slices of 34 x 34 in an
   8 x 4 grid, period 1 in u), its billows (r) and Worley 8 (g): two fetches */
vec2 air_noise(vec3 u)
{
    int slot = sh.g[pc.a.x].flaren.y, smp = sh.g[pc.a.x].flaren.z;
    vec3 p = fract(u) * 32.0;
    float z0 = floor(p.z), t = p.z - z0;
    int s0 = int(z0) & 31, s1 = (s0 + 1) & 31;
    vec2 xy = p.xy + 1.5, inv = vec2(1.0 / 272.0, 1.0 / 136.0);
    vec4 a = textureLod(sampler2D(textures[slot], samplers[smp]), (vec2(float(s0 & 7), float(s0 >> 3)) * 34.0 + xy) * inv, 0.0);
    vec4 b = textureLod(sampler2D(textures[slot], samplers[smp]), (vec2(float(s1 & 7), float(s1 >> 3)) * 34.0 + xy) * inv, 0.0);
    return mix(a.rg, b.rg, t);
}

/* (how far from its lamp the air is marched: its glow goes smoothly to nothing there, as it was cut off
   while still bright: a hard edge across the haze, Ant: "a harsh line at the top") */
float air_reach(Beam b) { return min(b.pos.w, 2.8 * AIR_NEAR); }

float air_light(Beam b, vec3 q, vec3 cam)
{
    vec3 v = q - b.pos.xyz;
    float R = air_reach(b), d2 = dot(v, v), a = dot(v, b.dir.xyz);
    if (d2 >= R * R || a <= 0.0)
        return 0.0;
    vec3 side = cross(b.dir.xyz, b.up.xyz);
    float x = dot(v, side) / (a * b.dir.w), y = dot(v, b.up.xyz) / (a * b.up.w * 4.0);
    float qq = d2 / (R * R), win = (1.0 - qq) * (1.0 - qq);
    float l = b.col.g * exp(-2.2 * (x * x + y * y)) * win / (1.0 + d2 / (AIR_NEAR * AIR_NEAR));
    if (l < 0.02)                       /* (little to show: no noise worked out, its average instead) */
        return l * 0.6;
    vec2 nn = air_noise((q + cam) / 900.0);   /* (a period 13.5 m, its billows about 3 m) */
    float n = nn.x * 0.7 + nn.y * 0.3;
    return l * (0.25 + 1.5 * n * n);
}

float beam_air(Globals G, ivec2 px, vec3 d, float tmax, float thr)
{
    float air = 0.0, top = 0.0, b0 = 0.0, b1 = 0.0;
    int best = -1;
    for (int i = 0; i < G.beamn.x; i++) {
        vec4 bp = sh.g[pc.a.x].beams[i].pos, bd = sh.g[pc.a.x].beams[i].dir;
        float R = air_reach(sh.g[pc.a.x].beams[i]), tca = dot(bp.xyz, d), d2 = dot(bp.xyz, bp.xyz) - tca * tca;
        if (d2 >= R * R)
            continue;
        float thc = sqrt(R * R - d2), t0 = max(tca - thc, 0.0), t1 = min(tca + thc, tmax);
        float dd = dot(d, bd.xyz), tp = dot(bp.xyz, bd.xyz) / (abs(dd) > 1.0e-6 ? dd : 1.0e-6);
        if (dd > 0.0)                   /* (in front of the lamp only) */
            t0 = max(t0, tp);
        else
            t1 = min(t1, tp);
        if (t1 <= t0)
            continue;
        float dt = (t1 - t0) / 4.0, s = 0.0;
        for (int k = 0; k < 4; k++) {
            vec3 q = d * (t0 + (float(k) + thr) * dt);
            float f = air_light(sh.g[pc.a.x].beams[i], q, G.cam.xyz);
            if (f <= 0.0)
                continue;
            float ct = dot(normalize(q - bp.xyz), -d), h = 1.25 - ct;
            s += f * 0.75 / (h * sqrt(h));
        }
        s *= dt * G.hl3.w;
        air += s;
        if (s > top) {
            top = s;
            best = i;
            b0 = t0;
            b1 = t1;
        }
    }
    if (best >= 0 && top > 0.004 && G.beamn.y != 2)
        air -= top * beam_blocked(G, px, d * mix(b0, b1, 0.25 + 0.5 * thr), vec3(0.0), best);
    return 0.35 * (1.0 - exp(-max(air, 0.0) / 0.35));
}

/* pass 6, into an image half the view's size each way, at the first view pixel of each 2 x 2: r the
   beams' light on the surface (the beams' colour times it), g the air's (VOLUMETRIC), a the eye
   distance (0: no surface that takes light there) */
vec4 beam_half(Globals G, ivec2 q)
{
    ivec2 px = min(q * 2, pc.r.zw - 1);
    if (G.beamn.y == 6)                 /* (6: no light, timing) */
        return vec4(0.0);
    vec4 g = FETCH(G.slots.w, px);
    uint geo = uint(g.r * 255.0 + 0.5);
    float depth = FETCH(G.slots2.x, px).r;
    uvec2 q8 = uvec2(px) & 7u;
    float thr = (float(BAYER[q8.y * 8u + q8.x]) + 0.5) / 64.0;
    if (geo == 0u || depth >= 1.0) {    /* (the sky, or nothing that takes light: the air in front) */
        if (G.hl3.w <= 0.0)
            return vec4(0.0);
        vec3 d = normalize(position(G, vec2(px) + 0.5, depth >= 1.0 ? 0.5 : depth));
        return vec4(0.0, beam_air(G, px, d, depth >= 1.0 ? 1.0e6 : length(position(G, vec2(px) + 0.5, depth)), thr),
                    0.0, 0.0);
    }
    vec3 p = position(G, vec2(px) + 0.5, depth);
    float eye = length(p);
    float air = G.hl3.w > 0.0 ? beam_air(G, px, p / eye, eye, thr) : 0.0;
    vec3 n = geo == CLS_WATER ? vec3(0.0) : surface_normal(g, p);
    float e = 0.0, top = 0.0;
    int best = -1;
    for (int i = 0; i < G.beamn.x; i++) {
        float l = beam_light(sh.g[pc.a.x].beams[i], p, n).g;   /* (from the buffer: not G's copy, indexed) */
        e += l;
        if (l > top) {
            top = l;
            best = i;
        }
    }
    if (top < 0.005)
        return vec4(0.0, air, 0.0, eye);
    float blocked = G.beamn.y == 2 ? 0.0 : beam_blocked(G, px, p, n, best);
    e -= top * blocked;
    e = G.hl.w * (1.0 - exp(-e / G.hl.w));
    if (G.beamn.y == 1 && blocked > 0.0)        /* (INSANE_HL_DEBUG=1: where the ray met something, red) */
        e = -1.0;
    return vec4(e, air, 0.0, eye);
}

/* pass 7, over the view, added: the light of pass 6's 4 nearest pixels on this pixel's surface (by
   eye distance; else the nearest of them in it) times the surface's own colour: its colour as drawn
   (a copy, beamn.w) over L, the light the game drew it with (the terrain: its colour pass's vertex
   colour; an object: the ambient light and the sun at its n.l, at least 0.35: at night that light is
   small and the quotient would blow up), scaled down as a whole where over 1 (its hue kept) */
vec4 beam_apply(Globals G, ivec2 px)
{
    /* (the 4 first, and nothing more read where none has light: most of the view) */
    vec2 hp = (vec2(px) + 0.5) * 0.5 - 0.5, f = fract(hp);
    ivec2 b = ivec2(floor(hp)), lim = (pc.r.zw + 1) / 2 - 1;
    vec4 s4[4];
    float lit = 0.0;
    for (int i = 0; i < 4; i++) {
        s4[i] = FETCH(int(pc.a.w), clamp(b + ivec2(i & 1, i >> 1), ivec2(0), lim));
        lit = max(lit, max(abs(s4[i].r), max(abs(s4[i].g), abs(s4[i].b))));
    }
    if (lit < 1.0 / 512.0)
        discard;
    vec4 g = FETCH(G.slots.w, px);
    uint geo = uint(g.r * 255.0 + 0.5);
    float depth = FETCH(G.slots2.x, px).r;
    if (geo == 0u || depth >= 1.0) {    /* (the sky: the air's light alone, the 4 by place) */
        float a = 0.0;
        for (int i = 0; i < 4; i++)
            a += (((i & 1) != 0) ? f.x : 1.0 - f.x) * (((i >> 1) != 0) ? f.y : 1.0 - f.y) * s4[i].g;
        if (a < 1.0 / 512.0)
            discard;
        return vec4(G.hl3.rgb * a, 0.0);
    }
    float eye = length(position(G, vec2(px) + 0.5, depth));
    vec2 e = vec2(0.0), near = vec2(0.0);
    float ws = 0.0, nd = 1.0e9;
    for (int i = 0; i < 4; i++) {
        ivec2 o = ivec2(i & 1, i >> 1);
        vec4 s = s4[i];
        if (s.a <= 0.0)
            continue;
        float d = abs(s.a - eye), w = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y) *
                                       max(1.0 - d / (eye * 0.04 + 2.0), 0.0);
        e += w * s.rg;
        ws += w;
        if (d < nd) {
            nd = d;
            near = s.rg;
        }
    }
    e = ws > 1.0e-3 ? e / ws : near;
    if (G.beamn.y == 1 && e.x < 0.0)
        return vec4(0.5, 0.0, 0.0, 0.0);
    if (max(e.x, e.y) < 1.0 / 512.0)
        discard;
    bool obj = geo > CLS_WATER;
    vec3 L = obj ? G.hl.rgb + G.sunc.rgb * (float((geo - 1u) & 127u) / 126.0) : G.hl2.rgb;
    vec3 own = FETCH(G.beamn.w, px).rgb / max(L, vec3(obj ? 0.35 : 0.15));
    own /= max(max(own.r, max(own.g, own.b)), 1.0);
    return vec4(G.hl3.rgb * (own * max(e.x, 0.0) + max(e.y, 0.0)), 0.0);
}

/* pass 8, added: the lamps' flares (vk.c flares). Each: hidden by the share of 5 depth-buffer pixels
   at and round its place (a core apart) that have something nearer than the lamp; a white core
   (Gaussian, the lamp's size), a halo in the beams' colour, a thin sideways streak */
vec4 flare_pass(Globals G, ivec2 px)
{
    vec2 p = vec2(px) + 0.5;
    vec3 add = vec3(0.0);
    for (int i = 0; i < G.flaren.x; i++) {
        vec4 f = sh.g[pc.a.x].flare[2 * i], r = sh.g[pc.a.x].flare[2 * i + 1];
        vec2 dv = p - f.xy;
        if (abs(dv.x) > r.z || abs(dv.y) > r.y * 2.0)
            continue;
        float vis = 0.0;
        for (int k = 0; k < 5; k++) {
            ivec2 o = k == 0 ? ivec2(0) : ivec2(k == 1 ? 1 : k == 2 ? -1 : 0, k == 3 ? 1 : k == 4 ? -1 : 0);
            ivec2 s = clamp(ivec2(f.xy + vec2(o) * r.x), pc.r.xy, pc.r.zw - 1);
            float dz = FETCH(G.slots2.x, s).r;
            float e = dz >= 1.0 ? 1.0e9 : length(position(G, vec2(s) + 0.5, dz));
            vis += e > f.z - 30.0 ? 0.2 : 0.0;
        }
        if (vis <= 0.0)
            continue;
        float d2 = dot(dv, dv), d = sqrt(d2);
        float core = exp(-d2 / (r.x * r.x)), halo = exp(-d / (r.y * 0.6));
        float streak = exp(-abs(dv.y) / max(r.x * 0.35, 0.75)) * exp(-abs(dv.x) / (r.y * 1.6));
        add += f.w * vis * (vec3(core * 1.6) + G.hl3.rgb * (halo * 0.45 + streak * 0.25));
    }
    if (max(add.r, max(add.g, add.b)) < 1.0 / 512.0)
        discard;
    return vec4(add, 0.0);
}

void main()
{
    Globals G = sh.g[pc.a.x];
    ivec2 px = ivec2(gl_FragCoord.xy);
    if (PASS == 8u) {
        o_color = flare_pass(G, px);
        return;
    }
    if (PASS == 6u) {
        o_color = beam_half(G, px);
        return;
    }
    if (PASS == 7u) {
        o_color = beam_apply(G, px);
        return;
    }
    if (pc.a.y == 2u) {                 /* INSANE_RT_DEBUG=2: what the camera sees of the structures */
        vec3 dir = normalize(position(G, gl_FragCoord.xy, 0.5));
        rayQueryEXT pq;
        rayQueryInitializeEXT(pq, tlas[pc.a.x], gl_RayFlagsOpaqueEXT, 0x03u, vec3(0.0), 0.0, dir, 1.0e6);
        while (rayQueryProceedEXT(pq)) {
        }
        if (rayQueryGetIntersectionTypeEXT(pq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
            discard;
        uint h = rayQueryGetIntersectionGeometryIndexEXT(pq, true) * 2654435761u;
        o_color = rayQueryGetIntersectionInstanceIdEXT(pq, true) != 0 ? vec4(0.5, 0.0, 0.5, 0.0)
                  : vec4(float(h & 255u) / 255.0, float((h >> 8) & 255u) / 255.0, float((h >> 16) & 255u) / 255.0, 0.0);
        return;
    }
    if (PASS == 1u) {                   /* into T: this block's first pixel that takes one */
        int S = int(pc.b.x);
        vec4 sh4 = vec4(-1.0);
        for (int k = 0; k < S * S && sh4.x < 0.0; k++)
            sh4 = shadow(G, px * S + ivec2(k % S, k / S));
        if (sh4.x < 0.0) {
            o_color = vec4(0.0);
            return;
        }
        /* the penumbra's half-width in T's pixels: the blocker's distance times the sun's size, on
           the surface (stretched by 1 / n.l), seen from the eye distance */
        float r = 0.5 * sh4.y * pc.b.w / max(sh4.w, 0.25) * pc.c.x / max(sh4.z, 1.0);
        o_color = sh_enc(sh4.x, sh4.y > 0.0 ? max(r, 1.0 / 16.0) : 0.0, sh4.z);
        return;
    }
    if (PASS == 5u) {
        o_color = down(G, px);
        return;
    }
    if (PASS == 3u || PASS == 4u) {
        o_color = filter_pass(G, px, PASS == 3u ? ivec2(1, 0) : ivec2(0, 1));
        return;
    }
    float s;
    if (PASS == 2u) {
        /* the hard shadow from T; with soft edges (c.z: U's texture) where U's penumbra is wider
           than half its pixel, U's filtered shadow from its 4 nearest pixels on this pixel's
           surface (eye distance), at least b.z of the hard one (thin casters stay) */
        int S = int(pc.b.x);
        vec4 t = sh_dec(FETCH(int(pc.a.w), px / S));
        s = t.x;
        if (pc.c.z > 0.5 && t.z > 0.0) {
            vec2 f = (vec2(px) + 0.5) / float(UF * S) - 0.5;
            ivec2 b = ivec2(floor(f));
            f -= vec2(b);
            float bw[4] = float[4]((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
            float ss = 0.0, ws = 0.0, k = 0.0;
            for (int i = 0; i < 4; i++) {
                vec4 u = sh_dec(FETCH(int(pc.c.z), u_inside(b + ivec2(i & 1, i >> 1))));
                float w = bw[i] * (u.z > 0.0 ? max(1.0 - abs(u.z - t.z) / (t.z * 0.03), 0.0) : 0.0);
                ss += w * u.x;
                k += w * u.y;
                ws += w;
            }
            if (ws > 1.0e-3) {
                k /= ws;
                s = max(mix(t.x, ss / ws, smoothstep(0.25, 0.6, k)), pc.b.z * t.x);
            }
        }
        s = 1.0 - (1.0 - s) * smoke_sun(G, px, t.z);
    } else {
        vec4 h = shadow(G, px);
        s = h.x < 0.0 ? h.x : 1.0 - (1.0 - h.x) * smoke_sun(G, px, h.z);
    }
    if (s <= 0.0)
        discard;
    o_color = pc.a.y != 0u ? vec4(0.0, 0.6, 0.6, 0.0) * s : vec4(FETCH(G.slots.z, px).rgb * s, 0.0);
}
