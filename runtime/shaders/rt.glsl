/* Rays against a view's acceleration structures (vk.c vk_shadow_end), shared by resolve.frag (the
   sun shadows) and ao.frag (ambient occlusion). The including shader declares textures[],
   samplers[] and includes shadow.glsl first. */
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Indices { uint i[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Vertices { float f[]; };

#define FETCH(slot, p) texelFetch(sampler2D(textures[slot], samplers[G.slots2.z]), p, 0)

/* Does a candidate hit on a geometry that is not opaque block the ray? (Geom.flags: 1 see-through
   part, its colours' alpha counts; 2 its texture's alpha counts (cut-outs); 4 fading in or out by
   the distance fade, drawn in the pixels whose dither threshold is in [lo, hi) / 256: lo in bits
   8-16, hi in bits 20-28.) Leaves, lattices and fences: the texture's alpha at the hit (u, v at
   floats 8 and 9 of the 40-byte vertex), for see-through parts times their colours' alpha. thr:
   the ray's own dither threshold (0..1), so that a fading object blocks that share of the rays
   and an object's two models in a cross-fade block one ray between them; below 0 (the shadows)
   a fading object blocks as drawn in full. */
bool blocks(rayQueryEXT rq, Globals G, uint slot, float thr)
{
    uint g = rayQueryGetIntersectionGeometryIndexEXT(rq, false);
    if (int(g) >= G.slots2.y)
        return true;
    Geom m = sh.geo[slot * uint(G.slots2.w) + g];
    if (thr >= 0.0 && (m.flags & 4u) != 0u &&
        (thr < float((m.flags >> 8) & 511u) / 256.0 || thr >= float((m.flags >> 20) & 511u) / 256.0))
        return false;
    if ((m.flags & 3u) == 0u)
        return true;
    uint p = rayQueryGetIntersectionPrimitiveIndexEXT(rq, false);
    vec2 b = rayQueryGetIntersectionBarycentricsEXT(rq, false);
    vec3 bw = vec3(1.0 - b.x - b.y, b.x, b.y);
    Indices ix = Indices(m.iaddr);
    Vertices vx = Vertices(m.vaddr);
    uint i0 = ix.i[3u * p] * 10u, i1 = ix.i[3u * p + 1u] * 10u, i2 = ix.i[3u * p + 2u] * 10u;
    float a = 1.0;
    if ((m.flags & 2u) != 0u) {
        vec2 uv = vec2(vx.f[i0 + 8u], vx.f[i0 + 9u]) * bw.x + vec2(vx.f[i1 + 8u], vx.f[i1 + 9u]) * bw.y +
                  vec2(vx.f[i2 + 8u], vx.f[i2 + 9u]) * bw.z;
        a = textureLod(sampler2D(textures[nonuniformEXT(m.tex)], samplers[nonuniformEXT(m.smp)]), uv, 0.0).a;
    }
    if ((m.flags & 1u) != 0u)           /* see-through parts: their colours' alpha too (A8R8G8B8 at float 6) */
        a *= dot(bw, vec3(float(floatBitsToUint(vx.f[i0 + 6u]) >> 24), float(floatBitsToUint(vx.f[i1 + 6u]) >> 24),
                          float(floatBitsToUint(vx.f[i2 + 6u]) >> 24))) / 255.0;
    return a >= m.aref;
}

/* the camera-relative position of pixel px at depth d: the pixel -> Vulkan -> D3D clip space ->
   world */
vec3 position(Globals G, vec2 px, float d)
{
    vec2 ndc = px * G.place2.zw * 2.0 - 1.0;
    vec4 w = G.inv_vp * vec4((ndc.x - G.place.y) / G.place.x, (ndc.y - G.place.w) / G.place.z,
                             (d - G.place2.x) / G.place2.y, 1.0);
    return w.xyz / w.w;
}

/* attachment 2's class byte (draw.frag): 0 nothing, 1..127 terrain, 128 a surface not in the
   depth buffer (the water; solid see-through parts: bit 7 of g), 129..255 objects */
#define CLS_WATER 128u

/* such a surface is a see-through part (else the water) */
bool coded_part(vec4 g)
{
    return g.g * 255.0 >= 127.5;
}

/* its place at pixel px: attachment 2 has its eye distance (the depth buffer has what is behind) */
vec3 water_position(Globals G, vec2 px, vec4 g)
{
    float dist = mod(floor(g.g * 255.0 + 0.5), 128.0) * 256.0 + g.b * 255.0;
    return normalize(position(G, px, 0.5)) * dist;
}
