/* The presented frame: the source rectangle of the render target, scaled, through the
   program's gamma ramp (a 256x1 lookup texture), then encoded for the swapchain. idx = render
   target, ramp, linear sampler, nearest sampler.

   The output step (DESIGN.md decision 4, "Colour precision"): ramp() gives the frame as the game
   means it, gamma-space values 0..1 after its own ramp; encode() turns them into what the
   swapchain holds. Today that is an 8-bit UNORM swapchain in sRGB_NONLINEAR and the values go as
   they are (the 3D scene's 16 bits were dithered into the back buffer at its end). An HDR
   swapchain plugs in at encode(): the values decoded to linear light (the display's 2.2 power),
   a tone-mapping step (the identity for the game's 0..1 colours, unless highlights are to rise
   above paper white), then paper white / 80 nits for scRGB (R16G16B16A16_SFLOAT,
   EXTENDED_SRGB_LINEAR), or BT.709 -> BT.2020 and the ST 2084 curve on nits / 10000 for HDR10
   (A2B10G10R10, HDR10_ST2084, a dither of 1/1023); with a 16-bit back buffer then, ramp() reads
   between the table's entries. */
#version 450
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler samplers[];
layout(push_constant) uniform PC {
    vec4 uvrect;
    ivec4 idx;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

/* the program's gamma ramp (8-bit index, as Direct3D 7's) */
vec3 ramp(vec3 c)
{
    ivec3 i = ivec3(clamp(c, 0.0, 1.0) * 255.0 + 0.5);
    return vec3(texelFetch(sampler2D(textures[pc.idx.y], samplers[pc.idx.w]), ivec2(i.r, 0), 0).r,
                texelFetch(sampler2D(textures[pc.idx.y], samplers[pc.idx.w]), ivec2(i.g, 0), 0).g,
                texelFetch(sampler2D(textures[pc.idx.y], samplers[pc.idx.w]), ivec2(i.b, 0), 0).b);
}

/* SDR: the values as they are */
vec4 encode(vec3 c)
{
    return vec4(c, 1.0);
}

void main()
{
    vec2 uv = mix(pc.uvrect.xy, pc.uvrect.zw, v_uv);
    o_color = encode(ramp(texture(sampler2D(textures[pc.idx.x], samplers[pc.idx.z]), uv).rgb));
}
