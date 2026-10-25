/* Volumetric smoke and dust (vk.c sm_first, sm_last), for a view of the world's puffs (smoke.glsl:
   back to front), passes 0 and 4 (and smokevis.frag's sun grids) before its sun shadows' resolve
   when they are ray traced, 1 and 2 after the view is drawn. The constant SPEC_PASS is the pass:
   0  the smaller image's depth: for each S x S block of the render target, the eye depth (view z)
      of the nearest or (alternate pixels, as a chequerboard) the farthest of 4 of its pixels, so
      both sides of an edge have pixels of their own for pass 2; and the nearest see-through
      surface's (glass, a lattice: vk.c draws them again at this size) where it is in front of
      that, its alpha in the fraction.
   1  the march (smoke.vert: each puff's rectangle of the smaller image, back to front, blended
      colour + colour below * T, T below * T): along the pixel's ray through the puff's sphere,
      as far as the depth (so it fades into what it meets: no hard edges), N steps (the puff's
      own, by its size on screen) of its density, a soft ball (flattened and lowered for the
      wheels' dust) of tiled 3D noise turned with the world (so it does not crawl as the camera
      turns), its own place in the noise from its number, slowly churning with its age, cut at
      the water's surface; its light: the sun (the light map's share at its place, or the sun grid
      pass 3 of smokevis.frag traced, through which the scene's shadows fall on it) through the
      puff itself towards the sun (Beer-Lambert over the chord to its edge, a tap of the density
      halfway), forward scattering (Henyey-Greenstein), and the ambient light, brighter at the
      top; the colour as the game drew it (its own light divided out, ours in), whiter for
      splashes; the fog by each step's distance, as the view's draws have it. The steps behind
      a see-through surface are seen through it (its 1 - alpha; it is in the colour already, its
      reflection too), those in front over it; behind a surface that much hides what is behind
      it (a car's glass) the first CABIN units are the car's inside, where no smoke is. Out: the
      light the puff adds (rgb) and the share of what is behind that it lets through (a).
   2  over the render target, at the puffs' rectangle: the smaller image brought up to full size,
      the 4 nearest pixels weighed by place and by how near their depth is to this pixel's (so the
      volumes keep to the edges of what is in front of them; where this pixel is well nearer than
      a smaller pixel's surface, only that pixel's smoke in front of this one's, smoke_front),
      blended colour * T + light; with MOTION (temporal anti-aliasing's scene) the reactive and
      transparency masks by the opacity (FSR 2 keeps less history there: no trails), no motion
      vectors (the surfaces' stay).
   4  the puffs' shadow (a pixel per c.z x c.z of the render target): from the surface at the
      block's middle pixel towards the sun through the nearest casters whose shadow can fall there,
      Beer-Lambert over 3 points of the chord through each; out the sun let through (never below
      0.2: light the puffs scatter on and the sky's) and the eye distance, for resolve.frag. */
#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler samplers[];
#include "smoke.glsl"
#include "shadow.glsl"
layout(constant_id = 0) const int SPEC_PASS = 1;
layout(location = 0) flat in uint f_puff;
layout(location = 0) out vec4 o_color;
#ifdef MOTION
layout(location = 3) out vec4 o_vel;
layout(location = 4) out vec4 o_mask;
#endif

#define TEX(slot) sampler2D(textures[nonuniformEXT(slot)], samplers[pc.b.w])
#define TEXL(slot) sampler2D(textures[nonuniformEXT(slot)], samplers[pc.b.z])

/* the eye depth (view z) of depth-buffer value d (far: 1e9) */
float view_z(float d)
{
    if (d >= 1.0)
        return 1.0e9;
    vec4 h2 = hdr(2u), h3 = hdr(3u);
    float zd = (d - h3.x) / h3.y;
    float den = zd * h2.z - h2.x;
    return abs(den) > 1.0e-12 ? h2.y / den : 1.0e9;
}

/* the view-space direction through render-target pixel px (z = 1) */
vec3 view_dir(vec2 px)
{
    vec4 h0 = hdr(0u), h1 = hdr(1u), h2 = hdr(2u), h3 = hdr(3u);
    vec2 ndc = px * h3.zw * 2.0 - 1.0;
    vec2 cl = vec2((ndc.x - h1.y) / h1.x, (ndc.y - h1.w) / h1.z);
    return vec3((cl.x * h2.z - h0.z) / h0.x, (cl.y * h2.z - h0.w) / h0.y, 1.0);
}

/* the tiled noise (32 x 32 x 32, period 1 in u): 32 slices of 34 x 34 (a texel of border round
   each) in an 8 x 4 grid; r: billows (Perlin-Worley, 4 cells a period), g: Worley 8, b: Worley
   16, a: Perlin */
vec4 noise3(vec3 u)
{
    vec3 p = fract(u) * 32.0;
    float z0 = floor(p.z), t = p.z - z0;
    int s0 = int(z0) & 31, s1 = (s0 + 1) & 31;
    vec2 xy = p.xy + 1.5, inv = vec2(1.0 / 272.0, 1.0 / 136.0);
    vec4 a = textureLod(TEXL(pc.b.y), (vec2(float(s0 & 7), float(s0 >> 3)) * 34.0 + xy) * inv, 0.0);
    vec4 b = textureLod(TEXL(pc.b.y), (vec2(float(s1 & 7), float(s1 >> 3)) * 34.0 + xy) * inv, 0.0);
    return mix(a, b, t);
}

struct Puff {
    vec3 c;
    float r, sigma, seed, age, floor_dy, light, erode;
    vec3 col;
    int kind;
};

/* puff i of the view's block */
Puff load_puff(uint i)
{
    uint b = puff_at(i);
    vec4 p0 = sm.v[b], p1 = sm.v[b + 1u], p2 = sm.v[b + 2u], p4 = sm.v[b + 4u];
    Puff P;
    P.c = p0.xyz;
    P.r = p0.w;
    P.col = p1.rgb;
    P.sigma = p1.a;
    P.kind = int(p2.x + 0.5);
    P.seed = p2.y;
    P.age = p2.z;
    P.floor_dy = p2.w;
    P.light = p4.x;
    P.erode = P.kind == K_SPRAY ? 0.45 : P.kind == K_EXHAUST ? 0.35 : 0.28;
    return P;
}

/* the puff's density at pw (world axes, from its centre), 0..about 1 (times sigma) */
float density(Puff P, vec3 pw)
{
    vec3 q = pw / P.r;
    if (P.kind == K_DUST || P.kind == K_SMOKE)
        q.y = (q.y + 0.12) * 1.3;       /* (the wheels' dust and smoke: flatter, lying low) */
    float r2 = dot(q, q);
    if (r2 >= 1.0)
        return 0.0;
    float prof = (1.0 - r2) * (1.0 - r2);
    vec3 u = pw / P.r * 0.45 + vec3(P.seed, P.seed * 2.31, P.seed * 3.17) + vec3(0.0013, -0.004, 0.0021) * P.age;
    vec4 n = noise3(u);
    float det = n.g * 0.65 + n.b * 0.35;
    float d = prof * (0.3 + 1.4 * n.r) - (1.0 - det) * P.erode * (1.0 - 0.5 * prof);
    if (P.floor_dy > -1.0e8)            /* (no further than the water's surface) */
        d *= smoothstep(P.floor_dy, P.floor_dy + 0.12 * P.r, pw.y);
    return max(d, 0.0);
}

/* the sun's share at pw: the puff's sun grid (3 x 3 x 3 points 0.6 r apart, smokevis.frag), or
   the light map's at its place */
float sun_vis(Puff P, vec3 pw, float row)
{
    if (row < 0.0)
        return P.light;
    vec3 g = clamp(pw / (0.6 * P.r) + 1.0, 0.0, 2.0);
    float z0 = min(floor(g.z), 1.0), fz = g.z - z0;
    vec2 base = vec2(0.5, row * 3.0 + 0.5) + g.xy;
    vec2 inv = 1.0 / vec2(textureSize(TEXL(pc.a.w), 0));
    float a = textureLod(TEXL(pc.a.w), (base + vec2(z0 * 3.0, 0.0)) * inv, 0.0).r;
    float b = textureLod(TEXL(pc.a.w), (base + vec2(z0 * 3.0 + 3.0, 0.0)) * inv, 0.0).r;
    return mix(a, b, fz);
}

/* the fog factor at eye distance e (view z zv), as the view's draws work it out (1: none) */
float fog_at(float e, float zv)
{
    vec4 h9 = hdr(9u), h10 = hdr(10u), h12 = hdr(12u);
    uint how = uint(h9.w), mode = how & 3u;
    if (mode == 0u)
        return 1.0;
    float x = (how & 4u) != 0u ? e : zv;
    float f = mode == 3u ? (h10.y != h10.x ? (h10.y - x) / (h10.y - h10.x) : 1.0)
                         : mode == 1u ? exp(-h10.z * x) : exp(-(h10.z * x) * (h10.z * x));
    if ((how & 8u) != 0u)
        f *= clamp((h12.x - zv) / (h12.x - h12.y), 0.0, 1.0);
    return clamp(f, 0.0, 1.0);
}

float luma(vec3 c)
{
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void march()
{
    Puff P = load_puff(f_puff);
    vec4 p4 = sm.v[puff_at(f_puff) + 4u];
    ivec2 q = ivec2(gl_FragCoord.xy);
    if (pc.c.w > 2.5 && pc.c.w < 3.5) { /* INSANE_SMOKE_DEBUG=3: the puffs' rectangles */
        o_color = vec4(0.1, 0.0, 0.0, 0.8);
        return;
    }
    vec3 dir = view_dir((vec2(q) + 0.5) * pc.c.z);
    float dl = length(dir);
    vec3 d = dir / dl;
    vec4 hz = texelFetch(TEX(pc.b.x), q, 0);
    float tsc = hz.r * dl;
    /* a see-through surface (glass, a lattice) in front of the scene there: what is behind it is seen
       through it (its alpha ag), what is in front over it */
    float zg = floor(hz.g), ag = clamp(fract(hz.g) / 0.99, 0.0, 1.0);
    float tg = zg < hz.r ? zg * dl : 1.0e30;
    /* glass that much hides what is behind it is a car's (lattices and fences: cut-outs, clear between
       their bars): the first CABIN units behind it are inside the car, where no smoke shows */
    float cab = pc.e.w != 0u ? smoothstep(0.15, 0.35, ag) : 0.0;
    float bb = dot(d, P.c), disc = bb * bb - (dot(P.c, P.c) - P.r * P.r);
    if (disc <= 0.0)
        discard;
    float hh = sqrt(disc), t0 = max(bb - hh, 1.0), t1 = min(bb + hh, tsc);
    if (t1 <= t0)
        discard;
    int N = int(p4.y);
    float dt = (t1 - t0) / float(N);
    mat3 W = transpose(mat3(hdr(4u).xyz, hdr(5u).xyz, hdr(6u).xyz));
    vec3 dw = W * d, pw0 = W * (d * t0 - P.c);
    vec4 h7 = hdr(7u);
    vec3 Lw = h7.xyz, sunc = hdr(8u).rgb, amb = hdr(9u).rgb, fogc = hdr(11u).rgb;
    float row = p4.z;
    /* forward scattering: Henyey-Greenstein (times 4 pi: 1 on average), partly */
    float g = P.kind == K_SPRAY ? 0.55 : P.kind == K_DUST ? 0.45 : P.kind == K_SMOKE ? 0.35 : 0.25;
    float ct = dot(dw, Lw), hgd = 1.0 + g * g - 2.0 * g * ct;
    float ph = mix(1.0, (1.0 - g * g) / (hgd * sqrt(hgd)), 0.75);
    /* the colour as drawn has the game's light (the ambient and the sun by the light map) in it: ours
       goes round it, the same on average (about half the sun getting into a puff), 0.65 of it */
    float norm = max(luma(sunc) * P.light * 0.55 * 0.55 + luma(amb) * 0.95, 0.05);
    vec3 col = P.col;
    if (P.kind == K_SPRAY)              /* (splashes whiter, the dust brighter: their light is scattered more) */
        col = mix(col, vec3(luma(col)), 0.4) * 1.15;
    else if (P.kind == K_DUST)
        col *= 1.1;
    float msf = P.kind == K_DUST ? 0.5 : P.kind == K_SPRAY ? 0.6 : 0.35;   /* (light scattered on inside) */
    /* headlights VOLUMETRIC (the view's block 13.x: the ray-traced view with the beams, -1 none): their
       light scattered as the sun's, on the puff's own colour (the game's light divided out, at most 1) */
    int bs = int(hdr(13u).x);
    vec3 albb = bs >= 0 ? min(col / max(norm, 0.3), vec3(1.0)) * sh.g[max(bs, 0)].hl3.rgb : vec3(0.0);
    float T = 1.0, Tb = 1.0;
    vec3 L = vec3(0.0), Lb = vec3(0.0);
    for (int k = 0; k < N; k++) {
        float s = (float(k) + 0.5) * dt;
        vec3 pw = pw0 + dw * s;
        float dn = density(P, pw);
        if (t0 + s >= tg)
            dn *= 1.0 - cab * (1.0 - smoothstep(tg + CABIN - 20.0, tg + CABIN, t0 + s));
        if (dn <= 0.0)
            continue;
        /* towards the sun: the chord to the ball's edge, the density halfway along it */
        float pl = dot(pw, Lw), c2 = dot(pw, pw) - P.r * P.r;
        float tex = -pl + sqrt(max(pl * pl - c2, 0.0));
        float dm = density(P, pw + Lw * (tex * 0.5));
        float tau = P.sigma * (dm * 0.75 + dn * 0.25) * tex;
        float ts = max(exp(-tau), msf * exp(-0.25 * tau));
        float vis = sun_vis(P, pw, row);
        float hgt = clamp(pw.y / P.r * 0.5 + 0.5, 0.0, 1.0);
        vec3 lit = (sunc * (vis * ts * ph * 0.55) + amb * mix(0.75, 1.15, hgt)) / norm;
        vec3 S = col * mix(vec3(1.0), lit, 0.65);
        if (bs >= 0) {
            vec3 qc = dw * (t0 + s);    /* (camera-relative world) */
            float fb = 0.0;
            for (int i = 0; i < sh.g[bs].beamn.x; i++) {
                float f = beam_light(sh.g[bs].beams[i], qc, vec3(0.0)).g;
                if (f > 0.0) {
                    float cb = dot(normalize(qc - sh.g[bs].beams[i].pos.xyz), -dw), hb = 1.0 + g * g - 2.0 * g * cb;
                    fb += f * (1.0 - g * g) / (hb * sqrt(hb));
                }
            }
            S += albb * (fb * 0.5);
        }
        if (pc.c.w > 7.5 && pc.c.w < 8.5) /* INSANE_SMOKE_DEBUG=8: the sun's share (white) and the self-shadow (red) */
            S = vec3(vis, vis * ts, vis * ts);
        float t = t0 + s;
        S = mix(fogc, S, fog_at(t, t * d.z));
        if (pc.c.w > 8.5)               /* INSANE_SMOKE_DEBUG=9: in front of a see-through surface red, behind it blue */
            S = t < tg ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0);
        float st = exp(-P.sigma * dn * dt);
        if (t < tg) {
            L += T * (1.0 - st) * S;
            T *= st;
        } else {
            Lb += Tb * (1.0 - st) * S;
            Tb *= st;
        }
        if (T < 0.01 || T * Tb < 0.01)
            break;
    }
    /* the glass over what is behind it, what is in front over both (the glass already in the colour:
       its share 1 - ag lets the light behind it through) */
    o_color = vec4(L + T * (1.0 - ag) * Lb, T * (1.0 - (1.0 - ag) * (1.0 - Tb)));
}

/* the eye depth where render-target pixel p's ray first enters a puff's ball (1e30 none) */
float smoke_front(ivec2 p)
{
    vec3 dir = view_dir(vec2(p) + 0.5);
    float dl = length(dir);
    vec3 d = dir / dl;
    vec2 hp = (vec2(p) + 0.5) / pc.c.z;
    float t = 1.0e30;
    for (uint i = 0u; i < pc.a.y; i++) {
        uint b = puff_at(i);
        vec4 r = sm.v[b + 3u];
        if (hp.x < r.x || hp.x >= r.z || hp.y < r.y || hp.y >= r.w)
            continue;
        vec4 c = sm.v[b];
        float bb = dot(d, c.xyz), disc = bb * bb - (dot(c.xyz, c.xyz) - c.w * c.w);
        if (disc > 0.0)
            t = min(t, max(bb - sqrt(disc), 1.0));
    }
    return t < 1.0e29 ? t / dl : 1.0e30;
}

void main()
{
    if (SPEC_PASS == 0) {
        ivec2 q = ivec2(gl_FragCoord.xy);
        int S = int(pc.c.z);
        ivec2 lim = textureSize(TEX(pc.a.z), 0) - 1, o = q * S + (S >= 4 ? ivec2(1) : ivec2(0));
        float z0 = view_z(texelFetch(TEX(pc.a.z), min(o, lim), 0).r);
        float z1 = view_z(texelFetch(TEX(pc.a.z), min(o + ivec2(1, 0), lim), 0).r);
        float z2 = view_z(texelFetch(TEX(pc.a.z), min(o + ivec2(0, 1), lim), 0).r);
        float z3 = view_z(texelFetch(TEX(pc.a.z), min(o + ivec2(1, 1), lim), 0).r);
        bool far = ((q.x + q.y) & 1) != 0;
        float zs = far ? max(max(z0, z1), max(z2, z3)) : min(min(z0, z1), min(z2, z3));
        /* the see-through surfaces drawn again at this size (vk.c): the nearest, if it is in front of the
           scene, its eye depth with its alpha in the fraction */
        float zg = 1.0e9;
        if (pc.e.z != 0u) {
            float g = view_z(texelFetch(TEX(pc.e.x), q, 0).r);
            if (g < zs)
                zg = floor(g) + min(texelFetch(TEX(pc.e.y), q, 0).a, 1.0) * 0.99;
        }
        o_color = vec4(zs, zg, 0.0, 0.0);
        return;
    }
    if (SPEC_PASS == 1) {
        march();
        return;
    }
    if (SPEC_PASS == 4) {
        /* the puffs' shadow (a pixel per c.z x c.z of the render target, its middle one's surface):
           from the surface towards the sun, through the
           nearest casters whose shadow can fall here (their rectangles), their density at 3 points of
           the chord through each (Beer-Lambert); out the sun let through and the eye distance */
        ivec2 q = ivec2(gl_FragCoord.xy), px = q * int(pc.c.z) + int(pc.c.z) / 2;
        px = min(px, textureSize(TEX(pc.a.z), 0) - 1);
        float zs = view_z(texelFetch(TEX(pc.a.z), px, 0).r);
        vec3 dir = view_dir(vec2(px) + 0.5);
        float dl = length(dir);
        if (zs >= 1.0e8) {
            o_color = vec4(1.0, 0.0, 0.0, 0.0);
            return;
        }
        vec3 pv = dir * zs, Lw = hdr(7u).xyz;
        mat3 W = transpose(mat3(hdr(4u).xyz, hdr(5u).xyz, hdr(6u).xyz));
        uint n = pc.a.y, ns = uint(hdr(2u).w);
        float tau = 0.0;
        for (uint i = n - ns; i < n && tau < 6.0; i++) {
            uint b = puff_at(i);
            vec4 sr = sm.v[b + 5u];
            if (float(px.x) < sr.x || float(px.x) >= sr.z || float(px.y) < sr.y || float(px.y) >= sr.w)
                continue;
            vec4 c = sm.v[b];
            vec3 o = W * (pv - c.xyz);
            float bb = dot(o, Lw), disc = bb * bb - (dot(o, o) - c.w * c.w);
            if (disc <= 0.0)
                continue;
            float h = sqrt(disc), ta = max(-bb - h, 0.0), tb = -bb + h;
            if (tb <= ta)
                continue;
            Puff P = load_puff(i);
            float dt = (tb - ta) / 3.0, sum = 0.0;
            for (int k = 0; k < 3; k++)
                sum += density(P, o + Lw * (ta + (float(k) + 0.5) * dt));
            tau += P.sigma * sum * dt;
        }
        /* (light scattered on by the puffs and from the sky reaches into their shadows: never black) */
        o_color = vec4(0.2 + 0.8 * exp(-0.8 * tau), zs * dl, 0.0, 0.0);
        return;
    }
    /* pass 2: up to full size, by place and depth */
    ivec2 p = ivec2(gl_FragCoord.xy);
    float zf = view_z(texelFetch(TEX(pc.a.z), p, 0).r);
    vec2 hq = (vec2(p) + 0.5) / pc.c.z - 0.5;
    ivec2 h0 = ivec2(floor(hq)), lim = textureSize(TEX(pc.a.w), 0) - 1;
    vec2 f = hq - vec2(h0);
    vec4 acc = vec4(0.0), bestv = vec4(0.0, 0.0, 0.0, 1.0), v4[4];
    float ws = 0.0, best = 1.0e30, zh4[4];
    bool near = false, smoky = false;   /* one of the 4 has this pixel's surface (or a nearer one); some smoke */
    for (int k = 0; k < 4; k++) {
        ivec2 h = clamp(h0 + ivec2(k & 1, k >> 1), ivec2(0), lim);
        zh4[k] = texelFetch(TEX(pc.b.x), h, 0).r;
        v4[k] = texelFetch(TEX(pc.a.w), h, 0);
        near = near || zf >= zh4[k] * 0.95;
        smoky = smoky || 1.0 - v4[k].a >= 1.0 / 1024.0 || max(v4[k].r, max(v4[k].g, v4[k].b)) >= 1.0 / 1024.0;
    }
    if (!smoky && pc.c.w < 1.5)         /* (none here: as below, sooner) */
        discard;
    float zfr = near || !smoky || pc.e.w == 0u ? 0.0 : smoke_front(p);
    for (int k = 0; k < 4; k++) {
        float wb = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);
        float zh = zh4[k];
        float dz = abs(zh - zf) / max(min(zh, zf), 1.0);
        vec4 v = v4[k];
        /* nothing of a smaller pixel's smoke behind this pixel's surface: where that pixel's surface is
           well behind this one's (a car's edge, a wheel, a bar narrower than a smaller pixel), none of
           it if another of the 4 has this surface, else only the share from where this pixel's ray
           enters the nearest puff to its depth (none if it enters none before it) */
        if (zf < zh * 0.95 && pc.e.w != 0u) {
            if (near)
                continue;
            float fr = clamp((zf - zfr) / max(zh - zfr, 1.0), 0.0, 1.0);
            v = vec4(v.rgb * fr, 1.0 - (1.0 - v.a) * fr);
        }
        float w = wb * exp(-dz * 40.0) + 1.0e-6 * wb;
        acc += v * w;
        ws += w;
        if (dz < best) {
            best = dz;
            bestv = v;
        }
    }
    vec4 r = ws > 1.0e-3 ? acc / ws : bestv;
    if (1.0 - r.a < 1.0 / 1024.0 && max(r.r, max(r.g, r.b)) < 1.0 / 1024.0 && pc.c.w < 1.5)
        discard;
    if (pc.c.w > 0.5)                   /* INSANE_SMOKE_DEBUG=1: the volumes alone, over grey (2, 3: all of
                                           their area grey) */
        r = vec4(r.rgb + 0.5 * r.a, 0.0);
    o_color = r;
#ifdef MOTION
    o_vel = vec4(0.0);
    float op = 1.0 - r.a;
    o_mask = vec4(op * pc.d.x, op * pc.d.y, 0.0, 0.0);
#endif
}
