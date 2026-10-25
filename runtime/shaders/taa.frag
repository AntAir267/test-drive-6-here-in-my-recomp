/* Temporal anti-aliasing (vk.c "temporal anti-aliasing"): the upscaled scene into the back buffer
   (the view's area), drawn over a triangle covering the target (present.vert). idx: x the source
   image, y its sampler, z what to show (0 the source; 1 the motion vectors, 2 the masks, 3 the
   depth: debug views of the scene image's inputs), w the scene's colour image (debug views). uv:
   the target's pixel -> the source's UV (scale, offset). q (vk.c "colour precision"): x the
   dither's blue-noise pattern for a 16-bit scene going into the 8-bit back buffer (-1: none), y 1:
   the source's alpha (the 16-bit scene without FSR 2), else 1. */
#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler samplers[];
layout(push_constant) uniform PC {
    vec4 uv;                    /* source UV = gl_FragCoord.xy * uv.xy + uv.zw */
    ivec4 idx;
    vec4 k;                     /* x: the debug views' motion scale (UV -> shown), y: sharpening 0..1 (the source
                                   the target's size: ANTI-ALIASING TAA's history; FSR 2 sharpens its own) */
    ivec4 q;
} pc;
layout(location = 0) out vec4 o_color;

/* About one level of triangular dither before the 8-bit target rounds: the pattern's rank (uniform 0..1,
   blue noise, the same every frame) turned into a triangular spread of -1..1 level with its order kept, so
   it stays blue; less of it towards 0 and 1 (none at black and white, which it could only push one way) */
vec3 dither(vec3 c)
{
    float n = texelFetch(sampler2D(textures[pc.q.x], samplers[pc.idx.y]), ivec2(gl_FragCoord.xy) & 63, 0).r;
    float t = n < 0.5 ? sqrt(2.0 * n) - 1.0 : 1.0 - sqrt(2.0 - 2.0 * n);
    c = clamp(c, 0.0, 1.0);
    vec3 a = clamp(255.0 * min(c, 1.0 - c) + 0.5, 0.5, 1.0);
    return c + t * a * (1.0 / 255.0);
}

/* robust contrast-adaptive sharpening as FSR's RCAS does it: the four neighbours' lobe, as much as stays
   within what they and the pixel span */
vec3 sharpen(vec3 e)
{
    ivec2 p = ivec2(gl_FragCoord.xy), m = textureSize(sampler2D(textures[pc.idx.x], samplers[pc.idx.y]), 0) - 1;
    vec3 b = texelFetch(sampler2D(textures[pc.idx.x], samplers[pc.idx.y]), clamp(p + ivec2(0, -1), ivec2(0), m), 0).rgb;
    vec3 d = texelFetch(sampler2D(textures[pc.idx.x], samplers[pc.idx.y]), clamp(p + ivec2(-1, 0), ivec2(0), m), 0).rgb;
    vec3 f = texelFetch(sampler2D(textures[pc.idx.x], samplers[pc.idx.y]), clamp(p + ivec2(1, 0), ivec2(0), m), 0).rgb;
    vec3 h = texelFetch(sampler2D(textures[pc.idx.x], samplers[pc.idx.y]), clamp(p + ivec2(0, 1), ivec2(0), m), 0).rgb;
    vec3 mn = min(min(b, d), min(min(f, h), e)), mx = max(max(b, d), max(max(f, h), e));
    vec3 lo = -mn / (4.0 * mx + 1e-4), hi = (1.0 - mx) / (4.0 * mn - 4.0 - 1e-4);
    vec3 l = max(lo, hi);
    float lobe = max(-0.1875, min(max(l.r, max(l.g, l.b)), 0.0)) * pc.k.y;
    return clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 0.0, 1.0);
}

void main()
{
    vec2 uv = gl_FragCoord.xy * pc.uv.xy + pc.uv.zw;
    vec4 s = textureLod(sampler2D(textures[pc.idx.x], samplers[pc.idx.y]), uv, 0.0);
    if (pc.idx.z == 0 && pc.k.y > 0.0)
        s.rgb = sharpen(s.rgb);
    if (pc.idx.z == 0) {
        o_color = vec4(pc.q.x >= 0 ? dither(s.rgb) : s.rgb, pc.q.y != 0 ? s.a : 1.0);
        return;
    }
    vec3 base = textureLod(sampler2D(textures[pc.idx.w], samplers[pc.idx.y]), uv, 0.0).rgb;
    float grey = dot(base, vec3(0.3, 0.59, 0.11)) * 0.5;
    if (pc.idx.z == 1)          /* motion: red right, green down, blue up-left (towards the previous place) */
        o_color = vec4(grey + max(s.x * pc.k.x, 0.0), grey + max(s.y * pc.k.x, 0.0),
                       grey + max(-(s.x + s.y) * 0.5 * pc.k.x, 0.0), 1.0);
    else if (pc.idx.z == 2)     /* reactive red, transparency and composition green */
        o_color = vec4(grey + s.r, grey + s.g, grey, 1.0);
    else                        /* depth (near bright) */
        o_color = vec4(vec3(pow(clamp(1.0 - s.r, 0.0, 1.0), 0.25)), 1.0);
}
