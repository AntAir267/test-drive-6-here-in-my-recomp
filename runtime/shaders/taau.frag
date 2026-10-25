/* ANTI-ALIASING TAA (vk.c "temporal anti-aliasing", taau_dispatch): the port's own temporal upscaler, one
   pass, lighter than FSR 2 (for the integrated GPUs). Each pixel of the history (the back buffer's size,
   drawn over a triangle covering it, present.vert) takes this frame's scene samples round it, weighted by
   how near their jittered places are, and the history where the motion vector says the pixel was last
   frame (a Catmull-Rom filter), held to what the scene's samples round it allow (their spread in YCoCg),
   then the two blended: mostly history, more of the frame where a sample sits on the pixel, where the
   reactive mask asks for it, and all of it with no history. The same inputs as FSR 2's: colour, depth
   (not inverted), motion vectors (previous minus current, UV), the masks (r reactive, g transparency).
   size: xy the scene's size, zw the history's; jit: xy this frame's jitter (scene pixels: the picture is
   moved by it), z 1: no history, w how much more of the frame where it moves 8 scene pixels a frame or
   more; idx: colour, depth, motion, masks; idx2: x the previous history, y the nearest sampler, z the
   linear one, w how far the history may be from the samples' mean, in their spreads, x 100. */
#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler samplers[];
layout(push_constant) uniform PC {
    vec4 size;
    vec4 jit;
    ivec4 idx;
    ivec4 idx2;
} pc;
layout(location = 0) out vec4 o_color;

vec3 ycocg(vec3 c) { return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b); }

vec3 hist_at(vec2 uv)
{
    return textureLod(sampler2D(textures[pc.idx2.x], samplers[pc.idx2.z]), uv, 0.0).rgb;
}

/* the history at uv through a Catmull-Rom filter in five bilinear fetches (its corners left out) */
vec3 history(vec2 uv)
{
    vec2 sz = pc.size.zw, pos = uv * sz, t1 = floor(pos - 0.5) + 0.5, f = pos - t1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2, t0 = (t1 - 1.0) / sz, t3 = (t1 + 2.0) / sz, t12 = (t1 + w2 / w12) / sz;
    vec3 c = hist_at(vec2(t12.x, t0.y)) * (w12.x * w0.y) + hist_at(vec2(t0.x, t12.y)) * (w0.x * w12.y) +
             hist_at(t12) * (w12.x * w12.y) + hist_at(vec2(t3.x, t12.y)) * (w3.x * w12.y) +
             hist_at(vec2(t12.x, t3.y)) * (w12.x * w3.y);
    float w = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(c / w, 0.0);
}

/* h moved towards the box's middle onto the box (YCoCg), if outside it */
vec3 clip_box(vec3 lo, vec3 hi, vec3 h)
{
    vec3 c = 0.5 * (hi + lo), e = 0.5 * (hi - lo) + 1e-4, v = h - c, a = abs(v / e);
    float m = max(a.x, max(a.y, a.z));
    return m > 1.0 ? c + v / m : h;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / pc.size.zw;
    vec2 rp = uv * pc.size.xy;                          /* the pixel's place in scene pixels (unjittered) */
    ivec2 ip = ivec2(floor(rp + pc.jit.xy)), rmax = ivec2(pc.size.xy) - 1;
    /* the frame's 3 x 3 samples round the pixel, weighed by their distance (a 4 x 4 Lanczos filter, as FSR 2
       enlarges, cost twice as much and looked no sharper here); they also give the spread the history is
       held to, and the nearest surface's motion */
    vec3 sum = vec3(0.0), m1 = vec3(0.0), m2 = vec3(0.0), lo = vec3(1e9), hi = vec3(-1e9);
    float wsum = 0.0, wmax = 0.0, zmin = 2.0;
    ivec2 near = clamp(ip, ivec2(0), rmax);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            ivec2 p = clamp(ip + ivec2(dx, dy), ivec2(0), rmax);
            vec3 c = texelFetch(sampler2D(textures[pc.idx.x], samplers[pc.idx2.y]), p, 0).rgb;
            vec2 d = vec2(p) + 0.5 - pc.jit.xy - rp;    /* the sample's place (moved by the jitter) to the pixel */
            float w = exp(-2.29 * dot(d, d));
            sum += c * w;
            wsum += w;
            wmax = max(wmax, w);
            vec3 y = ycocg(c);
            m1 += y;
            m2 += y * y;
            lo = min(lo, y);
            hi = max(hi, y);
            float z = texelFetch(sampler2D(textures[pc.idx.y], samplers[pc.idx2.y]), p, 0).r;
            if (z < zmin) {                             /* the motion of the nearest surface round it */
                zmin = z;
                near = p;
            }
        }
    vec3 cur = sum / wsum;
    vec2 mv = texelFetch(sampler2D(textures[pc.idx.z], samplers[pc.idx2.y]), near, 0).xy;
    vec2 masks = texelFetch(sampler2D(textures[pc.idx.w], samplers[pc.idx2.y]), clamp(ip, ivec2(0), rmax), 0).rg;
    vec2 puv = uv + mv;
    if (pc.jit.z > 0.5 || any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) {
        o_color = vec4(cur, 1.0);
        return;
    }
    /* the history, held within the samples' spread round the pixel */
    vec3 mean = m1 / 9.0, sd = sqrt(abs(m2 / 9.0 - mean * mean));
    float g = 0.01 * float(pc.idx2.w);
    vec3 hy = clip_box(max(lo, mean - sd * g), min(hi, mean + sd * g), ycocg(history(puv)));
    vec3 h = vec3(hy.x + hy.y - hy.z, hy.x + hy.z, hy.x - hy.y - hy.z);
    /* how much of this frame: a sample on the pixel 1/10, one half a pixel off 3/100; more when moving
       (the history is filtered again each frame: it blurs) and as the masks ask */
    float speed = length(mv * pc.size.xy);
    float a = 0.1 * wmax + pc.jit.w * clamp(speed / 8.0, 0.0, 1.0);
    a = max(a, max(masks.r * 0.9, masks.g * 0.3));
    /* weighted by 1 / (1 + brightness) so a lone bright sample does not flicker */
    float wc = a / (1.0 + ycocg(cur).x), wh = (1.0 - a) / (1.0 + hy.x);
    o_color = vec4((cur * wc + h * wh) / (wc + wh), 1.0);
}
