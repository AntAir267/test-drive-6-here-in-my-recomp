/* Direct3D 7 pixel pipeline for two texture stages: the stage colour/alpha operations and
   arguments, colour key, specular add, vertex or table fog, alpha test. Blending is fixed
   function (pipeline state). Push constants (see PushDraw in vk.c):
     stage  = s0 colour, s0 alpha, s1 colour, s1 alpha: op | arg1 << 8 | arg2 << 16
     texsmp = texture index s0, s1 (-1: none), sampler index s0, s1
     misc   = colour key s0, s1 (0x1RRGGBB when keyed), TFACTOR, fog colour (ARGB)
     fog    = fog start, end, density, alpha reference (0..255)
     flags.x: 0-3 alpha func (0 = no test), 4 specular, 5 flat, 6-7 fog (1 vertex, 2 table),
              8-9 table fog mode (1 EXP, 2 EXP2, 3 LINEAR), 10 full fog at the eye depth flags.z,
              growing to it from flags.w (floats)
     flags.y: distance fade (drawdist.c): 0, or bit 31 | hi in bits 0-8 | lo in bits 9-17: only
              the pixels whose 8 x 8 ordered-dither threshold is in [lo, hi) / 256 are drawn
     shadow = ray-traced shadows (vk.c, the race's main view): x: 0x100 | how the sun's part of the
              colour is found: 1 vertex-lit (v_dsh / v_ssh: the colours without the sun), 2 the
              terrain (its colour map has the sun baked in: the game's light map says how much),
              3 all of it (the terrain's shine pass, the water's waves), 4 the water (bits 16-23:
              how much of it per unit of the light map); 0 none. y: what att2 gets (1 terrain, 2
              an object, 3 the water; 0 nothing). z: the frame (sh.g[z]). w: ambient occlusion:
              bits 0-7 1 for a see-through part (it lowers att2's alpha where it adds light), 2 a
              solid one (its distance where solid), bits 8-15 att2's alpha for the depth-writing
              draws: how much ambient occlusion the surface takes.
              bits 16-22, ray-traced reflections (vk.c): 16 a reflective draw in the view (the car
              and object reflection pass, the water's colour pass): its texture's colour (the
              game's stand-in for what it reflects) left out, added after the view by 17 the same
              draw again with what a ray meets instead (RT_REFLECT: the difference only; 18 the
              draw's source factor is its alpha, 19-20 the kind: 1 the reflection pass, 2 the
              water; 21 no ray: the texture itself, as the game drew it; 22 the smaller image's pass;
              23-24 the smaller image's scale, log2 (0 a ray a pixel; its texture in tnl.y); 25
              LIGHT).
   With shadows the draw also writes attachment 1, the part of its colour the sun gives (a shadow
   takes it away again: shaders/resolve.frag), blended like the colour, and attachment 2, the face
   normal and the class (and for ambient occlusion how much of it), for the depth-writing draws.
   Compiled three times: as it is, with SHADOW_OUTPUTS (vk.c's ray-traced views), with RT_REFLECT
   (the reflective draws again, after the view); and with MOTION (with SHADOW_OUTPUTS, RT_REFLECT or
   neither:
   temporal anti-aliasing's scene, motion.glsl), attachment 3 the motion vector (previous minus
   current position, UV units; alpha 1, or for solid see-through parts 0 where they are not solid:
   blended), attachment 4 the reactive (r) and transparency and composition (g) masks (MAX
   blending). */
#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#ifdef RT_REFLECT
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
/* the depth test before the shader (it writes no depth; hidden pixels of the water's big mesh cost
   nothing) */
layout(early_fragment_tests) in;
#endif
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler samplers[];
layout(push_constant) uniform PC {
    uvec4 stage;
    ivec4 texsmp;
    uvec4 misc;
    vec4 fog;
    uvec4 flags;
    uvec4 tnl;
    uvec4 shadow;
} pc;
layout(location = 13) flat in uint v_fade;  /* an instanced copy's distance fade (tnl.vert), as flags.y */
#ifdef SHADOW_OUTPUTS               /* (compiled twice: vk.c's main view with shadows has these) */
#include "shadow.glsl"
layout(location = 5) in vec4 v_dsh;
layout(location = 6) in vec4 v_ssh;
layout(location = 7) flat in vec4 f_dsh;
layout(location = 8) flat in vec4 f_ssh;
layout(location = 9) in vec3 v_wpos;
layout(location = 1) out vec4 o_sun;
layout(location = 2) out vec4 o_geo;
#endif
#ifdef RT_REFLECT
layout(set = 0, binding = 4) uniform accelerationStructureEXT tlas[16];  /* a view slot each, then their skies */
#include "shadow.glsl"
#include "rt.glsl"
#include "refl.glsl"
layout(location = 9) in vec3 v_wpos;
layout(location = 10) in vec3 v_wnrm;
/* 1: the full-size pass of draws that trace nothing there (LIGHT's): the ray code left out of the
   pipeline, so it needs far fewer registers and runs at full occupancy (vk.c draw_pipeline mrt 4) */
layout(constant_id = 0) const int REFL_NO_RAYS = 0;
#endif
#ifdef MOTION
#include "motion.glsl"
layout(location = 3) out vec4 o_vel;
layout(location = 4) out vec4 o_mask;
#endif
layout(location = 0) in vec4 v_diffuse;
layout(location = 1) in vec4 v_specular;
layout(location = 2) flat in vec4 f_diffuse;
layout(location = 3) flat in vec4 f_specular;
layout(location = 4) in vec4 v_tc;
layout(location = 0) out vec4 o_color;

const uint BAYER[64] = uint[64](0u, 32u, 8u, 40u, 2u, 34u, 10u, 42u, 48u, 16u, 56u, 24u, 50u, 18u, 58u, 26u,
                                12u, 44u, 4u, 36u, 14u, 46u, 6u, 38u, 60u, 28u, 52u, 20u, 62u, 30u, 54u, 22u,
                                3u, 35u, 11u, 43u, 1u, 33u, 9u, 41u, 51u, 19u, 59u, 27u, 49u, 17u, 57u, 25u,
                                15u, 47u, 7u, 39u, 13u, 45u, 5u, 37u, 63u, 31u, 55u, 23u, 61u, 29u, 53u, 21u);

vec4 argb(uint c)
{
    return vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) / 255.0;
}

vec4 arg(uint a, vec4 dif, vec4 cur, vec4 tex, vec4 tf, vec4 spec)
{
    uint s = a & 15u;
    vec4 v = s == 0u ? dif : s == 2u ? tex : s == 3u ? tf : s == 4u ? spec : cur;
    if ((a & 32u) != 0u)
        v = vec4(v.a);
    if ((a & 16u) != 0u)
        v = vec4(1.0) - v;
    return v;
}

vec4 top(uint o, vec4 x, vec4 y, vec4 dif, vec4 cur, vec4 tex, vec4 tf)
{
    vec4 r;
    switch (o) {
    case 2u: r = x; break;
    case 3u: r = y; break;
    case 4u: r = x * y; break;
    case 5u: r = 2.0 * x * y; break;
    case 6u: r = 4.0 * x * y; break;
    case 7u: r = x + y; break;
    case 8u: r = x + y - 0.5; break;
    case 9u: r = 2.0 * (x + y - 0.5); break;
    case 10u: r = x - y; break;
    case 11u: r = x + y - x * y; break;
    case 12u: r = mix(y, x, dif.a); break;
    case 13u: r = mix(y, x, tex.a); break;
    case 14u: r = mix(y, x, tf.a); break;
    case 15u: r = x + y * (1.0 - tex.a); break;
    case 16u: r = mix(y, x, cur.a); break;
    case 18u: r = vec4(x.rgb + x.a * y.rgb, x.a * y.a); break;
    case 19u: r = vec4(x.rgb * y.rgb + x.a, x.a * y.a); break;
    case 20u: r = vec4((1.0 - x.a) * y.rgb + x.rgb, x.a * y.a); break;
    case 21u: r = vec4((1.0 - x.rgb) * y.rgb + x.a, x.a * y.a); break;
    case 24u: r = vec4(4.0 * dot(x.rgb - 0.5, y.rgb - 0.5)); break;
    default: r = x; break;      /* PREMODULATE, bump mapping: first argument */
    }
    return clamp(r, 0.0, 1.0);
}

bool keyed(vec4 t, uint key)
{
    return (key & 0x1000000u) != 0u &&
           all(equal(uvec3(t.rgb * 255.0 + 0.5), uvec3((key >> 16) & 255u, (key >> 8) & 255u, key & 255u)));
}

bool cmp(uint f, float a, float b)
{
    switch (f) {
    case 1u: return false;
    case 2u: return a < b;
    case 3u: return a == b;
    case 4u: return a <= b;
    case 5u: return a > b;
    case 6u: return a != b;
    case 7u: return a >= b;
    default: return true;
    }
}

vec4 stage(uint c, uint a, vec4 dif, vec4 cur, vec4 tex, vec4 tf, vec4 spec)
{
    vec4 r;
    r.rgb = top(c & 255u, arg((c >> 8) & 255u, dif, cur, tex, tf, spec), arg(c >> 16, dif, cur, tex, tf, spec),
                dif, cur, tex, tf).rgb;
    r.a = (a & 255u) > 1u ? top(a & 255u, arg((a >> 8) & 255u, dif, cur, tex, tf, spec),
                                arg(a >> 16, dif, cur, tex, tf, spec), dif, cur, tex, tf).a
                          : cur.a;
    return r;
}

/* the two texture stages and the specular add, for one pair of vertex colours */
vec4 combine(vec4 dif, vec4 spec, vec4 t0, vec4 t1, vec4 tf)
{
    vec4 cur = dif;
    if ((pc.stage.x & 255u) > 1u) {
        cur = stage(pc.stage.x, pc.stage.y, dif, cur, t0, tf, spec);
        if ((pc.stage.z & 255u) > 1u)
            cur = stage(pc.stage.z, pc.stage.w, dif, cur, t1, tf, spec);
    }
    if ((pc.flags.x & 16u) != 0u)
        cur.rgb = min(cur.rgb + spec.rgb, vec3(1.0));
    return cur;
}

#ifdef SHADOW_OUTPUTS
/* a unit vector as two numbers -1..1 (octahedral) */
vec2 oct_encode(vec3 n)
{
    vec2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? p : (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
}

/* the object light map at wpos (0 in the hills' shadows, 1 in full sun), read as the game does
   (0x401270: each cell's two triangles split along (0,0)-(1,1)). The terrain: its colour map has
   the sun baked in, so a shadow takes as much as the game's own shadow shapes take there
   (shadows.c): 3/4 of it (and nothing in the hills' shadows, which the colour map has already) */
float light_map(vec3 wpos)
{
    Globals G = sh.g[pc.shadow.z];
    vec2 p = (G.cam.xz + wpos.xz) / 256.0, f = fract(p);       /* cells */
    ivec2 a = ivec2(floor(p)) & 255, b = (a + 1) & 255;
    float h00 = texelFetch(sampler2D(textures[G.slots.x], samplers[G.slots.y]), a, 0).r;
    float h10 = texelFetch(sampler2D(textures[G.slots.x], samplers[G.slots.y]), ivec2(b.x, a.y), 0).r;
    float h01 = texelFetch(sampler2D(textures[G.slots.x], samplers[G.slots.y]), ivec2(a.x, b.y), 0).r;
    float h11 = texelFetch(sampler2D(textures[G.slots.x], samplers[G.slots.y]), b, 0).r;
    return f.x < f.y ? (1.0 - f.y) * (h00 - h01) + f.x * (h11 - h01) + h01
                     : (1.0 - f.x) * (h00 - h10) + f.y * (h11 - h10) + h10;
}
#endif

#ifdef RT_REFLECT
/* combine() for the reflective draws, whose one stage is always texture x diffuse (MODULATE, colour
   and alpha; the game's reflection and water passes) done straight: the same arithmetic without
   combine's general switch, which cost 0.45 ms a call over a screen of water */
vec4 rcombine(vec4 dif, vec4 spec, vec4 t0, vec4 t1, vec4 tf)
{
    if (pc.stage.x == 0x204u && pc.stage.y == 0x204u && (pc.stage.z & 255u) <= 1u) {
        vec4 cur = clamp(t0 * dif, 0.0, 1.0);
        if ((pc.flags.x & 16u) != 0u)
            cur.rgb = min(cur.rgb + spec.rgb, vec3(1.0));
        return cur;
    }
    return combine(dif, spec, t0, t1, tf);
}

#ifdef MOTION
/* temporal anti-aliasing's scene (RT_REFLECT with MOTION): the reflection drawn again marks the masks
   as its draw's Motion block says (taa: the reflection passes by the light they add, the water's
   colour pass by its alpha), from the light it adds (the draw in the view, without its texture's
   colour, added almost none); no motion vector (the surface's is there) */
void refl_masks(vec3 add, float a)
{
    Motion M = mot.m[pc.tnl.x];
    uint mf = floatBitsToUint(M.jit.z), rk = mf & 3u, tk = (mf >> 2) & 3u;
    vec2 s = unpackHalf2x16(floatBitsToUint(M.jit.w));
    float lum = clamp(max(add.r, max(add.g, add.b)), 0.0, 1.0);
    a = clamp(a, 0.0, 1.0);
    o_mask = vec4((rk == 1u ? a : rk == 2u ? lum : rk == 3u ? 1.0 : 0.0) * s.x,
                  (tk == 1u ? a : tk == 2u ? lum : tk == 3u ? 1.0 : 0.0) * s.y, 0.0, 0.0);
}
#endif

/* A reflective draw again after its view (vk.c): what its texture's colour multiplies, with what a
   ray from the surface meets in place of the texture, added to the colour (the draw itself, with
   that colour left out, was drawn in the view). The strength rises from the game's (the draw's
   source factor: its alpha) towards 1 at grazing angles by Schlick's Fresnel term times
   Globals.refl.x, and that rise takes refl.y of it from the colour below (blending: ONE,
   ONE_MINUS_SRC_ALPHA). A draw takes its rays at full size (a ray a pixel) or from an image 2, 4 or
   8 times smaller each way (vk.c picks by the draw's kind and strength and the option; shadow.w
   bits 23-24, the image's texture in tnl.y): the same draws first drawn into it (shadow.w bit 22;
   the depth test by hand against the depth buffer's 2 x 2 pixels under the pixel's middle), what
   each pixel's ray meets and its eye distance; then each full-size pixel takes the nearest of those
   on its own surface (about its eye distance), or its own ray where none is. LIGHT (bit 25) traces
   none at full size: where little round it is on its surface, or what they meet disagrees (what is
   too fine for the smaller image's rate), it shows the game's own stand-in (the env map or the sky
   texture as the game draws it, softened) instead, so fine things blend into the game's look
   rather than into blocks or noise. */
void reflective(vec4 dif, vec4 spec, vec4 t0, vec4 t1, vec4 tf, vec3 fn, float curve, vec3 soft, float de)
{
    uint fl = pc.flags.x;
#ifdef MOTION
    o_vel = vec4(0.0);                  /* (attachment 3 not written: vk.c) */
    o_mask = vec4(0.0);
#endif
    vec4 c0 = rcombine(dif, spec, vec4(vec3(0.0), t0.a), t1, tf);
    uint af = fl & 15u;
    if (af != 0u && !cmp(af, floor(c0.a * 255.0 + 0.5), pc.fog.w))
        discard;
    uint how = pc.shadow.w >> 16;
    bool small = (how & 64u) != 0u, water = ((how >> 3) & 3u) == 2u, light = (how & 512u) != 0u;
    uint ls = (how >> 7) & 3u;                          /* the smaller image's scale (log2; 0 none) */
    /* (the smaller image's pass: the full-size pixel at this one's top left) */
    uvec2 q = (uvec2(gl_FragCoord.xy) << (small ? ls : 0u)) & 7u;
    float thr = (float(BAYER[q.y * 8u + q.x]) + 0.5) / 64.0;
    uint fd = pc.flags.y != 0u ? pc.flags.y : v_fade;
    if (fd != 0u && (thr < float((fd >> 9) & 511u) / 256.0 || thr >= float(fd & 511u) / 256.0))
        discard;
    /* the fog factor, as main's */
    uint fs = (fl >> 6) & 3u;
    float f = 1.0;
    if (fs != 0u) {
        if (fs == 1u) {
            f = spec.a;
        } else {
            float d = 1.0 / gl_FragCoord.w;
            uint m = (fl >> 8) & 3u;
            if (m == 3u)
                f = pc.fog.y != pc.fog.x ? (pc.fog.y - d) / (pc.fog.y - pc.fog.x) : 1.0;
            else if (m == 1u)
                f = exp(-pc.fog.z * d);
            else
                f = exp(-(pc.fog.z * d) * (pc.fog.z * d));
        }
        if ((fl & 1024u) != 0u) {
            float e = uintBitsToFloat(pc.flags.z), s = uintBitsToFloat(pc.flags.w);
            f *= clamp((e - 1.0 / gl_FragCoord.w) / (e - s), 0.0, 1.0);
        }
        f = clamp(f, 0.0, 1.0);
    }
    float sa = (how & 4u) != 0u ? c0.a : 1.0;          /* the draw's source factor */
    vec3 w = (rcombine(dif, spec, vec4(vec3(1.0), t0.a), t1, tf).rgb - c0.rgb) * (f * sa);
    if (max(w.r, max(w.g, w.b)) < 0.25 / 255.0)
        discard;                                        /* (far off: the game's fade took it) */
    if ((how & 32u) != 0u) {                            /* no ray: the texture, as the game drew it */
        o_color = vec4((rcombine(dif, spec, t0, t1, tf).rgb - c0.rgb) * (f * sa), 0.0);
#ifdef MOTION
        refl_masks(o_color.rgb, sa);
#endif
        return;
    }
    Globals G = sh.g[pc.shadow.z];
    float e0 = length(v_wpos);
    vec3 v = -v_wpos / max(e0, 1.0e-6);                 /* towards the eye */
    vec3 n = water ? vec3(0.0, 1.0, 0.0) : v_wnrm;
    float ln = length(n);
    float lf = length(fn);
    n = ln > 1.0e-6 ? n / ln : lf > 0.0 ? (dot(fn, v) < 0.0 ? -fn : fn) / lf : v;
    float nv = dot(n, v);
    if (nv < 0.05) {                                    /* (a smooth normal turned away at an outline) */
        n = normalize(n + v * (0.05 - nv));
        nv = dot(n, v);
    }
    vec3 d = reflect(-v, n), o = v_wpos + n * (0.5 + e0 * 4.0e-4);
    float cone = G.refl.w + (water ? 0.0 : min(curve, 0.5));
    float k = float(1u << ls);
    if (small) {                        /* (main did the depth test) */
        /* (LIGHT: the textures' and cut-outs' detail lower still, so fine patterns blur, not alias) */
        float kc = light ? k * 2.0 : k;
        vec3 r = gl_HelperInvocation ? vec3(0.0)     /* (helper pixels, there for derivatives, need none) */
                                     : reflection(G, pc.shadow.z, o, d, thr, e0, e0 * G.refl.w * kc, cone * kc,
                                                  light ? 1.0 : 0.0);
        o_color = vec4(r, e0);
        return;
    }
    vec3 r = vec3(0.0);
    bool own = ls == 0u;
    float u = 0.0;                      /* LIGHT: how much of the game's own stand-in */
    if (!own) {
        /* the nearest pixels of the smaller image with this surface's ray (about this eye distance):
           FULL the 4 round it, weighed by place; LIGHT the 3 x 3 round the nearest under a tent 1.5
           of them wide each way (the water the 4 round it), which also says how much what they meet
           spreads (smoothly, pixel to pixel). (Fixed taps, read with the nearest sampler at their
           centres.) */
        int img = int(pc.tnl.y);
        vec2 hp = gl_FragCoord.xy / k - 0.5;
        vec2 isz = 1.0 / vec2(textureSize(sampler2D(textures[img], samplers[G.slots2.z]), 0));
        float ws = 0.0, wt = 0.0, l1 = 0.0, l2 = 0.0;
        float tol = e0 * 0.03 + 4.0 + de * k * 1.5;
        if (light && !water) {
            ivec2 hc = ivec2(floor(hp + 0.5));
            for (int i = 0; i < 9; i++) {
                ivec2 t = hc + ivec2(i % 3 - 1, i / 3 - 1);
                vec2 dd = abs(vec2(t) - hp);
                float wb = max(1.0 - dd.x / 1.5, 0.0) * max(1.0 - dd.y / 1.5, 0.0);
                vec4 s = textureLod(sampler2D(textures[img], samplers[G.slots2.z]), (vec2(t) + 0.5) * isz, 0.0);
                wt += wb;
                float m = s.a > 0.0 && abs(s.a - e0) < tol ? wb : 0.0;
                float l = dot(s.rgb, vec3(0.299, 0.587, 0.114));
                r += m * s.rgb;
                ws += m;
                l1 += m * l;
                l2 += m * l * l;
            }
        } else if (light) {             /* (the water's reflections are broad: the 4 nearest will do) */
            ivec2 hb = ivec2(floor(hp));
            vec2 fr = hp - vec2(hb);
            for (int i = 0; i < 4; i++) {
                ivec2 o4 = ivec2(i & 1, i >> 1);
                float wb = (o4.x != 0 ? fr.x : 1.0 - fr.x) * (o4.y != 0 ? fr.y : 1.0 - fr.y);
                vec4 s = textureLod(sampler2D(textures[img], samplers[G.slots2.z]), (vec2(hb + o4) + 0.5) * isz, 0.0);
                wt += wb;
                float m = s.a > 0.0 && abs(s.a - e0) < tol ? wb : 0.0;
                float l = dot(s.rgb, vec3(0.299, 0.587, 0.114));
                r += m * s.rgb;
                ws += m;
                l1 += m * l;
                l2 += m * l * l;
            }
        } else {
            ivec2 hb = ivec2(floor(hp));
            vec2 fr = hp - vec2(hb);
            for (int i = 0; i < 4; i++) {
                ivec2 o4 = ivec2(i & 1, i >> 1);
                vec4 s = textureLod(sampler2D(textures[img], samplers[G.slots2.z]), (vec2(hb + o4) + 0.5) * isz, 0.0);
                float wb = (o4.x != 0 ? fr.x : 1.0 - fr.x) * (o4.y != 0 ? fr.y : 1.0 - fr.y) + 1.0e-3;
                if (s.a > 0.0 && abs(s.a - e0) < e0 * 0.03 + 4.0) {
                    r += wb * s.rgb;
                    ws += wb;
                }
            }
        }
        if (ws > 0.02) {
            r /= ws;
        } else if (!light) {
            own = true;
        }
        if (light) {                    /* unsure: little of the tent on this surface, or a wide spread */
            float sd = ws > 0.0 ? sqrt(max(l2 / ws - (l1 / ws) * (l1 / ws), 0.0)) : 0.0;
            u = max(1.0 - smoothstep(0.2, 0.7, ws / wt), smoothstep(G.refl2.z, G.refl2.w, sd));
        }
    }
    if (G.refl2.y == 1.0) {             /* INSANE_REFL_DEBUG=1: green from the smaller image, red their own
                                           (none near on their surface), blue full size; LIGHT: red the
                                           game's stand-in */
        o_color = vec4(own && ls != 0u ? vec3(1.0, 0.0, 0.0) : ls != 0u ? mix(vec3(0.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0), u)
                                                                  : vec3(0.0, 0.0, 1.0), 1.0);
        return;
    }
    if (REFL_NO_RAYS == 0 && own && !gl_HelperInvocation)   /* (helper pixels, there for derivatives, need none) */
        r = reflection(G, pc.shadow.z, o, d, thr, e0, e0 * G.refl.w, cone, 0.0);
    /* (the water's stand-in is the sky's own texture: as bright as the world) */
    r *= water ? 1.0 : G.refl2.x;
    if (u > 0.0 && any(greaterThan(r, vec3(0.0)))) {   /* (the stand-in at about what was met here: no bright or dark patches) */
        float ls0 = max(dot(soft, vec3(0.299, 0.587, 0.114)), 0.02), lr = dot(r, vec3(0.299, 0.587, 0.114));
        soft *= clamp(mix(1.0, lr / ls0, 0.7), 0.4, 2.0);
    }
    r = mix(r, soft, u);
    vec3 add = (rcombine(dif, spec, vec4(r, t0.a), t1, tf).rgb - c0.rgb) * (f * sa);
    float s0 = clamp(sa, 1.0 / 255.0, 1.0), fr = G.refl.x * pow(clamp(1.0 - nv, 0.0, 1.0), 5.0);
    float s1 = s0 + (1.0 - s0) * fr;
    o_color = vec4(add * min(s1 / s0, 4.0), (s1 - s0) * G.refl.y * f);
#ifdef MOTION
    refl_masks(o_color.rgb, sa);
#endif
}
#endif

void main()
{
    uint fl = pc.flags.x;
    bool flat_shade = (fl & 32u) != 0u;
    vec4 dif = flat_shade ? f_diffuse : v_diffuse;
    vec4 spec = flat_shade ? vec4(f_specular.rgb, v_specular.a) : v_specular;
    vec4 tf = argb(pc.misc.z);
    /* sample first, in uniform control flow (mipmap selection needs derivatives) */
    vec4 t0 = vec4(1.0), t1 = vec4(1.0);
#ifdef RT_REFLECT
    /* (LIGHT's full-size pass: a plain trilinear sampler, not the draw's anisotropic one) */
    int s0 = REFL_NO_RAYS == 1 ? sh.g[pc.shadow.z].rslots.z : pc.texsmp.z;
    if (pc.texsmp.x >= 0)
        t0 = texture(sampler2D(textures[pc.texsmp.x], samplers[s0]), v_tc.xy);
#else
    if (pc.texsmp.x >= 0)
        t0 = texture(sampler2D(textures[pc.texsmp.x], samplers[pc.texsmp.z]), v_tc.xy);
#endif
    if (pc.texsmp.y >= 0)
        t1 = texture(sampler2D(textures[pc.texsmp.y], samplers[pc.texsmp.w]), v_tc.zw);
#ifdef RT_REFLECT
    /* the face normal (derivatives: before any discard), where the mesh has no normals */
    vec3 rfn = cross(dFdx(v_wpos), dFdy(v_wpos));
    /* LIGHT's stand-in where its rays are unsure: the game's own texture, softened (a 256 texel
       map at about 22 x 22: a plain trilinear sampler at a fixed level) */
    vec3 rsoft = pc.texsmp.x >= 0 && ((pc.shadow.w >> 25) & 1u) != 0u   /* (LIGHT only) */
                     ? textureLod(sampler2D(textures[pc.texsmp.x], samplers[sh.g[pc.shadow.z].rslots.z]), v_tc.xy, 3.5).rgb
                     : vec3(1.0);
    /* the ray cone's widening per unit of length: the pixel's angle and twice the normal's turn
       across it (a curved mirror spreads its rays) */
    vec3 rnn = normalize(v_wnrm + vec3(0.0, 1.0e-7, 0.0));
    float rcone = 2.0 * max(length(dFdx(rnn)), length(dFdy(rnn)));
    float rde = fwidth(length(v_wpos));      /* (how fast the eye distance changes across a pixel) */
    if (((pc.shadow.w >> 22) & 1u) != 0u) {
        /* the smaller image's pass (no depth attachment): the depth test by hand, first, against the
           depth buffer's 2 x 2 pixels under this pixel's middle (drawn where they have this surface
           or one behind: the reflective draws' own test has z bias) */
        Globals G = sh.g[pc.shadow.z];
        float k = float(1u << ((pc.shadow.w >> 23) & 3u));
        vec4 zq = textureGather(sampler2D(textures[G.slots2.x], samplers[G.slots2.z]), gl_FragCoord.xy * k * G.place2.zw, 0);
        if (gl_FragCoord.z > max(max(zq.x, zq.y), max(zq.z, zq.w)) * 1.00001 + 1.0e-7)
            discard;
    }
#endif
#ifdef SHADOW_OUTPUTS
    if (((pc.shadow.w >> 16) & 1u) != 0u)      /* a reflective draw: its texture's colour comes later */
        t0.rgb = vec3(0.0);
    /* the shadow outputs' face normal (derivatives: before any discard) */
    vec3 fn = vec3(0.0, 1.0, 0.0);
    bool shadows = (pc.shadow.x & 0x100u) != 0u;
    if (shadows) {
        vec3 c = cross(dFdx(v_wpos), dFdy(v_wpos));
        float l = length(c);
        if (l > 0.0) {
            fn = c / l;
            if (dot(fn, v_wpos) > 0.0)      /* towards the camera (the origin) */
                fn = -fn;
        }
    }
#endif
    if ((pc.stage.x & 255u) > 1u) {
        if (keyed(t0, pc.misc.x))
            discard;
        if ((pc.stage.z & 255u) > 1u && keyed(t1, pc.misc.y))
            discard;
    }
#ifdef RT_REFLECT
    reflective(dif, spec, t0, t1, tf, rfn, rcone, rsoft, rde);
    return;
#endif
    vec4 cur = combine(dif, spec, t0, t1, tf);
    uint fs = (fl >> 6) & 3u;
    float f = 1.0;
    if (fs != 0u) {
        if (fs == 1u) {
            f = spec.a;
        } else {
            float d = 1.0 / gl_FragCoord.w;
            uint m = (fl >> 8) & 3u;
            if (m == 3u)
                f = pc.fog.y != pc.fog.x ? (pc.fog.y - d) / (pc.fog.y - pc.fog.x) : 1.0;
            else if (m == 1u)
                f = exp(-pc.fog.z * d);
            else
                f = exp(-(pc.fog.z * d) * (pc.fog.z * d));
        }
        if ((fl & 1024u) != 0u) {
            float e = uintBitsToFloat(pc.flags.z), s = uintBitsToFloat(pc.flags.w);
            f *= clamp((e - 1.0 / gl_FragCoord.w) / (e - s), 0.0, 1.0);
        }
        f = clamp(f, 0.0, 1.0);
    }
    vec3 lit = cur.rgb;
    if (fs != 0u)
        cur.rgb = mix(argb(pc.misc.w).rgb, cur.rgb, f);
    uint af = fl & 15u;
    if (af != 0u && !cmp(af, floor(cur.a * 255.0 + 0.5), pc.fog.w))
        discard;
    uint fd = pc.flags.y != 0u ? pc.flags.y : v_fade;
    if (fd != 0u) {
        uvec2 q = uvec2(gl_FragCoord.xy) & 7u;
        float t = (float(BAYER[q.y * 8u + q.x]) + 0.5) / 64.0;
        if (t < float((fd >> 9) & 511u) / 256.0 || t >= float(fd & 511u) / 256.0)
            discard;
    }
    o_color = cur;
#ifdef MOTION
    {
        Motion M = mot.m[pc.tnl.x];
        vec2 now = v_cur.xy / v_cur.z, was = v_prev.z > 1.0e-6 ? v_prev.xy / v_prev.z : now;
        uint mf = floatBitsToUint(M.jit.z);
        float a = clamp(cur.a, 0.0, 1.0), lum = a * max(cur.r, max(cur.g, cur.b));
        o_vel = vec4((was - now) * 0.5, 0.0, (mf & 16u) != 0u ? (a >= 0.5 ? 1.0 : 0.0) : 1.0);
        uint rk = mf & 3u, tk = (mf >> 2) & 3u;
        vec2 s = unpackHalf2x16(floatBitsToUint(M.jit.w));
        o_mask = vec4((rk == 1u ? a : rk == 2u ? lum : rk == 3u ? 1.0 : 0.0) * s.x,
                      (tk == 1u ? a : tk == 2u ? lum : tk == 3u ? 1.0 : 0.0) * s.y, 0.0, 0.0);
    }
#endif
#ifdef SHADOW_OUTPUTS
    o_sun = vec4(0.0, 0.0, 0.0, cur.a);
    /* attachment 2: r 0 nothing, 1..127 terrain, 129..255 an object, the rest n.l * 126, 128 a
       surface not in the depth buffer (the water, solid see-through parts); g, b the face normal
       (octahedral), for those their eye distance in game units (15 bits; bit 15 set for the parts);
       a how much ambient occlusion the surface takes */
    uint cls = pc.shadow.y;
    float nl = clamp(dot(fn, sh.g[pc.shadow.z].sun.xyz), 0.0, 1.0);
    o_geo = vec4(cls == 0u ? 0.0 : float((cls - 1u) * 128u + 1u + uint(nl * 126.0 + 0.5)) / 255.0,
                 oct_encode(fn) * 0.5 + 0.5, float((pc.shadow.w >> 8) & 255u) / 255.0);
    if (cls == 3u) {                    /* the water: class 128, its distance (15 bits) */
        uint q = uint(clamp(length(v_wpos), 0.0, 32767.0));
        o_geo.rgb = vec3(128.0, float(q >> 8), float(q & 255u)) / 255.0;
    }
    if ((pc.shadow.w & 255u) == 2u) {   /* solid see-through parts (lattices, fences): class 128, the
                                           distance with bit 15 set, where solid (blended by alpha) */
        uint q = uint(clamp(length(v_wpos), 0.0, 32767.0)) | 32768u;
        o_geo = vec4(vec3(128.0, float(q >> 8), float(q & 255u)) / 255.0, cur.a >= 0.5 ? 1.0 : 0.0);
    }
    if ((pc.shadow.w & 255u) == 1u)     /* see-through parts: ambient occlusion leaves alone what they
                                           visibly cover (car glass and its reflection: the AO is the
                                           surface's below, it would darken the glass with it):
                                           blended, alpha * (1 - this); faint or dark ones (smoke) not */
        o_geo = vec4(0.0, 0.0, 0.0, smoothstep(0.1, 0.3, cur.a) * smoothstep(0.02, 0.12, max(lit.r, max(lit.g, lit.b))));
    if (shadows) {
        uint mode = pc.shadow.x & 255u;
        vec3 sun = vec3(0.0);
        if (mode == 1u) {
            vec4 d = flat_shade ? f_dsh : v_dsh;
            vec4 s = flat_shade ? vec4(f_ssh.rgb, v_ssh.a) : v_ssh;
            sun = max(lit - combine(d, s, t0, t1, tf).rgb, vec3(0.0));
        } else if (mode == 2u) {
            sun = lit * sh.g[pc.shadow.z].fade.w * light_map(v_wpos);
        } else if (mode == 3u) {
            sun = lit;
        } else if (mode == 4u) {
            sun = lit * float((pc.shadow.x >> 16) & 255u) / 255.0 * light_map(v_wpos);
        }
        o_sun.rgb = sun * f;
    }
#endif
}
