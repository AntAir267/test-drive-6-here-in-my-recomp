/* Ray-traced reflections (Enhanced REFLECTIONS RAY TRACED; vk.c, draw.frag's RT_REFLECT variant):
   what a reflective surface's ray meets in the view's acceleration structures (rt.glsl), shaded as
   the game draws it: an object or a car by its texture and vertex colours, lit by its own draw's
   Direct3D lights (tnl.vert's constants); the terrain by the map's colour map (its sun, slopes and
   the hills' shadows baked in, as the game draws it); then fogged over the ray's length. A ray
   that meets nothing shows the sky dome (the view's sky geometries round the camera, in a top
   level of their own: tlas[8 + slot]) with the sun's glint, else the fog colour the frame is
   cleared to. The including shader declares textures[], samplers[] and tlas[16] and includes
   shadow.glsl and rt.glsl first. */

/* tnl.vert's per-draw constants (std430; TnlConst in gfx.h) */
struct Light {
    vec4 type_range;            /* type (1 point, 2 spot, 3 directional), range, falloff */
    vec4 pos, dir;              /* camera space; dir points from the light */
    vec4 att;                   /* att0, att1, att2 */
    vec4 spot;                  /* cos(theta / 2), cos(phi / 2) */
    vec4 diffuse, specular, ambient;
};
struct Draw {
    mat4 wvp, wv;               /* D3D row-vector matrices, so M * v here */
    vec4 nm[3];                 /* normal matrix rows */
    vec4 place, place2;
    vec4 mat[4];                /* material diffuse, ambient, specular, emissive */
    vec4 power_fog;
    vec4 ambient;
    uvec4 flags;                /* x: 1 light 2 colorvertex 16 normalize; z lights; w material sources */
    Light lights[8];
    vec4 world[3];
};
layout(std430, set = 0, binding = 2) readonly buffer Tnl { Draw d[]; } tnl;

#define INST_TERRAIN 0x800000   /* vk.c: the terrain's instances' custom index (the objects' is 0) */

vec4 refl_bgra(uint c)
{
    return vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) / 255.0;
}

/* the fog over e game units, as the view's draws have it (draw.frag, tnl.vert) */
float refl_fog(Globals G, float e)
{
    uint mode = uint(G.fogp.w) & 3u;
    if (mode == 0u)
        return 1.0;
    float f = mode == 3u ? (G.fogp.y != G.fogp.x ? (G.fogp.y - e) / (G.fogp.y - G.fogp.x) : 1.0)
                         : mode == 1u ? exp(-G.fogp.z * e) : exp(-(G.fogp.z * e) * (G.fogp.z * e));
    return clamp(f, 0.0, 1.0);
}

/* D3DMCS_*: 0 material, 1 vertex diffuse, 2 vertex specular (only with COLORVERTEX) */
vec4 refl_source(bool cv, uint src, vec4 vd, vec4 vs, vec4 mat)
{
    return cv && src == 1u ? vd : cv && src == 2u ? vs : mat;
}

/* Direct3D 7 lighting of a hit on draw k's mesh (tnl.vert's, without the specular): object-space
   place p and normal n, vertex colours vd, vs */
vec4 refl_light(uint k, vec3 p, vec3 n, vec4 vd, vec4 vs)
{
    uint fl = tnl.d[k].flags.x;
    if ((fl & 1u) == 0u)
        return vd;
    bool cv = (fl & 2u) != 0u;
    uint src = tnl.d[k].flags.w;
    vec4 md = refl_source(cv, src & 15u, vd, vs, tnl.d[k].mat[0]);
    vec4 ma = refl_source(cv, (src >> 8) & 15u, vd, vs, tnl.d[k].mat[1]);
    vec4 me = refl_source(cv, (src >> 12) & 15u, vd, vs, tnl.d[k].mat[3]);
    vec3 nc = n.x * tnl.d[k].nm[0].xyz + n.y * tnl.d[k].nm[1].xyz + n.z * tnl.d[k].nm[2].xyz;
    float ln = length(nc);
    nc = ln > 0.0 ? nc / ln : nc;
    vec3 e = (tnl.d[k].wv * vec4(p, 1.0)).xyz;
    vec3 amb = tnl.d[k].ambient.rgb, dif = vec3(0.0);
    uint nl = min(tnl.d[k].flags.z, 8u);
    for (uint i = 0u; i < nl; i++) {
        float type = tnl.d[k].lights[i].type_range.x;
        vec3 L;
        float att = 1.0, spot = 1.0;
        if (type == 3.0) {
            L = -tnl.d[k].lights[i].dir.xyz;
        } else {
            L = tnl.d[k].lights[i].pos.xyz - e;
            float d = length(L);
            float range = tnl.d[k].lights[i].type_range.y;
            if (range > 0.0 && d > range)
                continue;
            vec4 a = tnl.d[k].lights[i].att;
            float den = a.x + a.y * d + a.z * d * d;
            att = den > 0.0 ? 1.0 / den : 1.0;
            L = d > 0.0 ? L / d : L;
            if (type == 2.0) {
                float rho = -dot(L, tnl.d[k].lights[i].dir.xyz);
                float ct = tnl.d[k].lights[i].spot.x, cp = tnl.d[k].lights[i].spot.y;
                float falloff = tnl.d[k].lights[i].type_range.z;
                if (rho <= cp)
                    spot = 0.0;
                else if (rho < ct && ct > cp)
                    spot = pow((rho - cp) / (ct - cp), falloff > 0.0 ? falloff : 1.0);
            }
        }
        amb += att * spot * tnl.d[k].lights[i].ambient.rgb;
        dif += att * spot * max(dot(nc, L), 0.0) * tnl.d[k].lights[i].diffuse.rgb;
    }
    return clamp(vec4(me.rgb + ma.rgb * amb + md.rgb * dif, md.a), 0.0, 1.0);
}

/* texels per game unit on triangle p0 p1 p2 (object space; draw k's world matrix turns it) with
   texture coordinates t0 t1 t2 of a texture of size ts, for a ray along d: its level of detail for a
   footprint w game units wide is log2(w * this). The footprint lies stretched on a surface the ray
   meets at a grazing angle (taken whole, so a fine pattern blurs rather than sparkles). */
float refl_density(vec3 p0, vec3 p1, vec3 p2, vec2 t0, vec2 t1, vec2 t2, vec2 ts, uint k, vec3 d)
{
    vec2 u1 = (t1 - t0) * ts, u2 = (t2 - t0) * ts;
    vec3 c = cross(p1 - p0, p2 - p0);
    float a = length(c);
    vec3 nw = vec3(dot(tnl.d[k].world[0].xyz, c), dot(tnl.d[k].world[1].xyz, c), dot(tnl.d[k].world[2].xyz, c));
    float cs = abs(dot(nw, d)) / max(length(nw), 1.0e-6);
    return sqrt(abs(u1.x * u2.y - u1.y * u2.x) / max(a, 1.0e-6)) / max(cs, 0.1);
}

/* How much of a reflection ray a candidate hit on a geometry that is not opaque stops (rt.glsl
   blocks(), as coverage): 0 where the distance fade has the object not drawn at the ray's dither
   threshold thr; a cut-out's texture alpha at the level of detail of the ray's footprint there (w0
   + t * cone game units), for see-through parts times their colours' alpha. So lattices, fences
   and leaves finer than the footprint thin out to a share of it, as the game's mipmaps and
   blending draw them, instead of sparkling. haze (LIGHT: 1): where the footprint is wider than the
   texture's smallest level (most of the game's textures have one level), towards the share of the
   texture that is solid (Geom.misc, vk.c), so a lattice seen by few rays is an even veil, not
   blocks. */
float refl_alpha(rayQueryEXT rq, Globals G, uint slot, float thr, float w0, float cone, vec3 d, float haze)
{
    uint g = rayQueryGetIntersectionGeometryIndexEXT(rq, false);
    if (int(g) >= G.slots2.y)
        return 1.0;
    Geom m = sh.geo[slot * uint(G.slots2.w) + g];
    if (thr >= 0.0 && (m.flags & 4u) != 0u &&
        (thr < float((m.flags >> 8) & 511u) / 256.0 || thr >= float((m.flags >> 20) & 511u) / 256.0))
        return 0.0;
    if ((m.flags & 3u) == 0u)
        return 1.0;
    uint p = rayQueryGetIntersectionPrimitiveIndexEXT(rq, false);
    vec2 b = rayQueryGetIntersectionBarycentricsEXT(rq, false);
    vec3 bw = vec3(1.0 - b.x - b.y, b.x, b.y);
    Indices ix = Indices(m.iaddr);
    Vertices vx = Vertices(m.vaddr);
    uint i0 = ix.i[3u * p] * 10u, i1 = ix.i[3u * p + 1u] * 10u, i2 = ix.i[3u * p + 2u] * 10u;
    float a = 1.0;
    if ((m.flags & 2u) != 0u) {
        vec2 t0 = vec2(vx.f[i0 + 8u], vx.f[i0 + 9u]), t1 = vec2(vx.f[i1 + 8u], vx.f[i1 + 9u]),
             t2 = vec2(vx.f[i2 + 8u], vx.f[i2 + 9u]);
        vec2 ts = vec2(textureSize(sampler2D(textures[nonuniformEXT(m.tex)], samplers[nonuniformEXT(m.smp)]), 0));
        float k = refl_density(vec3(vx.f[i0], vx.f[i0 + 1u], vx.f[i0 + 2u]), vec3(vx.f[i1], vx.f[i1 + 1u], vx.f[i1 + 2u]),
                               vec3(vx.f[i2], vx.f[i2 + 1u], vx.f[i2 + 2u]), t0, t1, t2, ts, m.tnl, d);
        float lod = log2(max((w0 + rayQueryGetIntersectionTEXT(rq, false) * cone) * k, 1.0e-6));
        a = textureLod(sampler2D(textures[nonuniformEXT(m.tex)], samplers[nonuniformEXT(m.smp)]),
                       t0 * bw.x + t1 * bw.y + t2 * bw.z, max(lod, 0.0)).a;
        /* (where the footprint holds less than a texel the game's alpha test at aref decides) */
        a = lod <= 0.0 ? (a >= m.aref ? 1.0 : 0.0) : smoothstep(0.0, 2.0 * m.aref, a);
        if (haze > 0.0 && m.misc != 0u) {
            float ex = lod - float(textureQueryLevels(sampler2D(textures[nonuniformEXT(m.tex)], samplers[nonuniformEXT(m.smp)])) - 1);
            a = mix(a, uintBitsToFloat(m.misc), smoothstep(0.0, 2.0, ex) * haze);
        }
    }
    if ((m.flags & 1u) != 0u)           /* see-through parts: their colours' alpha too (A8R8G8B8 at float 6) */
        a *= dot(bw, vec3(float(floatBitsToUint(vx.f[i0 + 6u]) >> 24), float(floatBitsToUint(vx.f[i1 + 6u]) >> 24),
                          float(floatBitsToUint(vx.f[i2 + 6u]) >> 24))) / 255.0;
    return a;
}

/* a hit on an object or a car (geometry g, triangle p, barycentrics b): its texture (at a level of
   detail for the ray's footprint, w game units wide there) times its lit vertex colours */
vec3 refl_object(Globals G, uint slot, uint g, uint p, vec2 b, float w, vec3 d)
{
    Geom m = sh.geo[slot * uint(G.slots2.w) + g];
    vec3 bw = vec3(1.0 - b.x - b.y, b.x, b.y);
    Indices ix = Indices(m.iaddr);
    Vertices vx = Vertices(m.vaddr);
    uint i0 = ix.i[3u * p] * 10u, i1 = ix.i[3u * p + 1u] * 10u, i2 = ix.i[3u * p + 2u] * 10u;
    vec3 p0 = vec3(vx.f[i0], vx.f[i0 + 1u], vx.f[i0 + 2u]), p1 = vec3(vx.f[i1], vx.f[i1 + 1u], vx.f[i1 + 2u]),
         p2 = vec3(vx.f[i2], vx.f[i2 + 1u], vx.f[i2 + 2u]);
    vec3 n = bw.x * vec3(vx.f[i0 + 3u], vx.f[i0 + 4u], vx.f[i0 + 5u]) +
             bw.y * vec3(vx.f[i1 + 3u], vx.f[i1 + 4u], vx.f[i1 + 5u]) +
             bw.z * vec3(vx.f[i2 + 3u], vx.f[i2 + 4u], vx.f[i2 + 5u]);
    vec4 vd = bw.x * refl_bgra(floatBitsToUint(vx.f[i0 + 6u])) + bw.y * refl_bgra(floatBitsToUint(vx.f[i1 + 6u])) +
              bw.z * refl_bgra(floatBitsToUint(vx.f[i2 + 6u]));
    vec4 vs = bw.x * refl_bgra(floatBitsToUint(vx.f[i0 + 7u])) + bw.y * refl_bgra(floatBitsToUint(vx.f[i1 + 7u])) +
              bw.z * refl_bgra(floatBitsToUint(vx.f[i2 + 7u]));
    vec2 t0 = vec2(vx.f[i0 + 8u], vx.f[i0 + 9u]), t1 = vec2(vx.f[i1 + 8u], vx.f[i1 + 9u]),
         t2 = vec2(vx.f[i2 + 8u], vx.f[i2 + 9u]);
    vec3 tex = vec3(1.0);
    if (m.tex >= 0) {                   /* (the footprint picks the level: no shimmer) */
        vec2 ts = vec2(textureSize(sampler2D(textures[nonuniformEXT(m.tex)], samplers[nonuniformEXT(m.smp)]), 0));
        float lod = log2(max(w * refl_density(p0, p1, p2, t0, t1, t2, ts, m.tnl, d), 1.0e-6));
        tex = textureLod(sampler2D(textures[nonuniformEXT(m.tex)], samplers[nonuniformEXT(m.smp)]),
                         bw.x * t0 + bw.y * t1 + bw.z * t2, max(lod, 0.0)).rgb;
    }
    return tex * refl_light(m.tnl, bw.x * p0 + bw.y * p1 + bw.z * p2, n, vd, vs).rgb;
}

/* a hit on the terrain at camera-relative place p: the map's colour map (16 tiles of 64 x 64 cells,
   as the terrain's colour pass draws them), less what its detail pass darkens on average */
vec3 refl_terrain(Globals G, vec3 p, float w)
{
    vec2 c = mod((p.xz + G.cam.xz) / 256.0, 256.0);         /* cells */
    ivec2 t = ivec2(floor(c / 64.0)) & 3;
    vec2 uv = ((c - vec2(t) * 64.0) * 3.984375 + 0.5) / 256.0;
    int s = G.tiles[t.y][t.x];
    vec3 col = G.tavg.rgb;
    if (s >= 0)                         /* (a texel is 64 game units) */
        col = textureLod(sampler2D(textures[nonuniformEXT(s)], samplers[G.rslots.x]), uv,
                         max(log2(max(w / 64.0, 1.0e-3)), 0.0)).rgb;
    return col * G.terr.x * G.terr.y;
}

/* the sky dome along d from the camera (the view's sky top level, tlas[8 + slot]: the dome round
   the camera is far, so from near it a reflection sees it as the camera would): its texture and
   colours blended over the fog colour the frame is cleared to, or that colour under its rim */
vec3 refl_sky(Globals G, uint slot, vec3 d)
{
    if (G.rslots.y < 0)
        return G.fogcol.rgb;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, tlas[8u + slot], gl_RayFlagsOpaqueEXT, 0xffu, vec3(0.0), 0.0, d, 1.0e7);
    while (rayQueryProceedEXT(q)) {
    }
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return G.fogcol.rgb;
    uint g = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true)) + rayQueryGetIntersectionGeometryIndexEXT(q, true);
    Geom m = sh.geo[slot * uint(G.slots2.w) + g];
    uint p = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
    vec2 b = rayQueryGetIntersectionBarycentricsEXT(q, true);
    vec3 bw = vec3(1.0 - b.x - b.y, b.x, b.y);
    Indices ix = Indices(m.iaddr);
    Vertices vx = Vertices(m.vaddr);
    uint i0 = ix.i[3u * p] * 10u, i1 = ix.i[3u * p + 1u] * 10u, i2 = ix.i[3u * p + 2u] * 10u;
    vec4 vd = bw.x * refl_bgra(floatBitsToUint(vx.f[i0 + 6u])) + bw.y * refl_bgra(floatBitsToUint(vx.f[i1 + 6u])) +
              bw.z * refl_bgra(floatBitsToUint(vx.f[i2 + 6u]));
    vec2 uv = bw.x * vec2(vx.f[i0 + 8u], vx.f[i0 + 9u]) + bw.y * vec2(vx.f[i1 + 8u], vx.f[i1 + 9u]) +
              bw.z * vec2(vx.f[i2 + 8u], vx.f[i2 + 9u]);
    vec4 s = vd;
    if (m.tex >= 0)
        s *= textureLod(sampler2D(textures[nonuniformEXT(m.tex)], samplers[nonuniformEXT(m.smp)]), uv, 0.0);
    return mix(G.fogcol.rgb, s.rgb, clamp(s.a, 0.0, 1.0));
}

/* the sun as the sky shows it (its sprite after the dome: a bright disc in a glow), for a ray along
   d: what a reflection that meets the sky adds (a glint, as the game's env map has its sun) */
vec3 refl_sun(Globals G, vec3 d)
{
    float c = max(dot(d, G.sun.xyz), 0.0);
    return G.sunc.rgb * (2.0 * pow(c, 3000.0) + 0.35 * pow(c, 200.0) + 0.08 * pow(c, 16.0));
}

/* the fog over a reflection ray's t game units from o, e0 from the eye */
vec3 refl_fogged(Globals G, vec3 c, float t, float e0)
{
    float f = refl_fog(G, t);
    if (G.fogedge.x > 0.0)              /* drawdist.c: full fog at the view's edge */
        f *= clamp((G.fogedge.x - (e0 + t)) / (G.fogedge.x - G.fogedge.y), 0.0, 1.0);
    return mix(G.fogcol.rgb, c, f);
}

/* What a ray from o (camera-relative, e0 game units from the eye) along d meets, shaded and fogged
   over its length. thr: the pixel's dither threshold, so an object fading in or out by the distance
   fade is seen as much as it is drawn there. A ray cone for the textures' levels of detail: w0 game
   units wide at o, widening by cone per unit (the pixel's angle and the mirror's curvature). What
   see-through layers (leaves, lattices, fences, glass) let through before the solid hit is mixed
   with the nearest of them by their coverage (refl_alpha). */
vec3 reflection(Globals G, uint slot, vec3 o, vec3 d, float thr, float e0, float w0, float cone, float haze)
{
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas[slot], 0u, 0x03u, o, 0.0, d, G.refl.z);
    float through = 1.0, lt = 3.0e38;   /* what the layers let through; the nearest one's t */
    uint lg = 0u, lp = 0u;
    vec2 lb = vec2(0.0);
    while (rayQueryProceedEXT(rq))
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            float a = refl_alpha(rq, G, slot, thr, w0, cone, d, haze), t = rayQueryGetIntersectionTEXT(rq, false);
            if (a >= 0.98) {
                rayQueryConfirmIntersectionEXT(rq);
            } else if (a > 0.01 && (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ||
                                     t < rayQueryGetIntersectionTEXT(rq, true))) {
                through *= 1.0 - a;
                if (t < lt) {
                    lt = t;
                    lg = rayQueryGetIntersectionGeometryIndexEXT(rq, false);
                    lp = rayQueryGetIntersectionPrimitiveIndexEXT(rq, false);
                    lb = rayQueryGetIntersectionBarycentricsEXT(rq, false);
                }
            }
        }
    vec3 c;
    float th = 3.0e38;
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        c = refl_sky(G, slot, d) + refl_sun(G, d);
    } else {
        int ci = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
        th = rayQueryGetIntersectionTEXT(rq, true);
        if (ci == INST_TERRAIN)
            c = refl_fogged(G, refl_terrain(G, o + d * th, w0 + th * cone), th, e0);
        else
            c = refl_fogged(G, refl_object(G, slot, rayQueryGetIntersectionGeometryIndexEXT(rq, true),
                                           rayQueryGetIntersectionPrimitiveIndexEXT(rq, true),
                                           rayQueryGetIntersectionBarycentricsEXT(rq, true), w0 + th * cone, d), th, e0);
    }
    if (lt < th && through < 1.0)
        c = mix(refl_fogged(G, refl_object(G, slot, lg, lp, lb, w0 + lt * cone, d), lt, e0), c, through);
    return c;
}
