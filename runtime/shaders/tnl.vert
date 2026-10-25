/* Direct3D 7 transform and lighting on the GPU, for the game's untransformed vertex buffers
   (XYZ|NORMAL|DIFFUSE|SPECULAR|TEX1, 40 bytes): the same steps as tnl_vertex in d3d.c (which still
   does it on the CPU where positions are needed there). Per-draw constants (TnlConst in gfx.h)
   come from a storage buffer, picked by the push constants' tnl.x; tnl.z/w are the two stages'
   texture coordinate sets. Outputs as draw.vert.
   Ray-traced shadows (pc.shadow.x & 0x100, the race's main view): the colours also come out as
   they would be with the sun blocked (v_dsh, v_ssh: the directional light's diffuse and specular
   left out, ambient and the other lights kept), and the camera-relative world position and
   normal. Compiled again with MOTION (temporal anti-aliasing's scene, vk.c): the position without
   the frame's jitter and where the vertex was in the previous frame (motion.glsl). */
#version 450
#ifdef MOTION
#extension GL_GOOGLE_include_directive : require
#define MOTION_VERTEX
#include "motion.glsl"
#endif
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec4 a_diffuse;
layout(location = 3) in vec4 a_specular;
layout(location = 4) in vec2 a_tc;
layout(location = 0) out vec4 v_diffuse;
layout(location = 1) out vec4 v_specular;
layout(location = 2) flat out vec4 f_diffuse;
layout(location = 3) flat out vec4 f_specular;
layout(location = 4) out vec4 v_tc;
layout(location = 5) out vec4 v_dsh;
layout(location = 6) out vec4 v_ssh;
layout(location = 7) flat out vec4 f_dsh;
layout(location = 8) flat out vec4 f_ssh;
layout(location = 9) out vec3 v_wpos;
layout(location = 10) out vec3 v_wnrm;      /* (ray-traced reflections: the world normal) */
layout(location = 13) flat out uint v_fade; /* an instanced copy's distance fade (draw.frag), else 0 */

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
    vec4 place, place2;         /* ax bx ay by; mnz dz: D3D clip space -> Vulkan (vk.c) */
    vec4 mat[4];                /* material diffuse, ambient, specular, emissive */
    vec4 power_fog;             /* specular power, fog start, end, density */
    vec4 ambient;
    uvec4 flags;                /* x: 1 light 2 colorvertex 4 specular 8 localviewer 16 normalize
                                   32 rangefog; y fog mode; z lights; w material sources */
    Light lights[8];
    vec4 world[3];              /* world matrix rows (column-vector 3x4) */
};
layout(std430, set = 0, binding = 2) readonly buffer Tnl { Draw d[]; } tnl;
/* Instanced draws (vk.c vk_draw_tnl_inst: drawdist.c's map copies): each copy's record of 4 vec4s,
   its world matrix's rows (column-vector 3x4), then x its sun's scale (the directional lights'
   diffuse), y its fade (VK_FADE's bits); the draw's own world matrix is the identity */
layout(std430, set = 0, binding = 7) readonly buffer Inst { vec4 v[]; } inst;
layout(push_constant) uniform PC {
    uvec4 stage;
    ivec4 texsmp;
    uvec4 misc;
    vec4 fog;
    uvec4 flags;
    uvec4 tnl;
    uvec4 shadow;               /* x: mode | 0x100 shadow outputs on (draw.frag) */
    uvec4 inst;                 /* x: the first copy's record, y: 1 instanced */
} pc;

#define K tnl.d[pc.tnl.x]

vec3 normalize3(vec3 v)
{
    float l = length(v);
    return l > 0.0 ? v / l : v;
}

/* D3DMCS_*: 0 material, 1 vertex diffuse, 2 vertex specular (only with COLORVERTEX) */
vec4 source(bool cv, uint src, vec4 vd, vec4 vs, vec4 mat)
{
    if (cv) {
        if (src == 1u) return vd;
        if (src == 2u) return vs;
    }
    return mat;
}

void main()
{
    vec4 p = vec4(a_pos, 1.0);
    vec3 a_n = a_normal;
    float sunk = 1.0;
    v_fade = 0u;
    if (pc.inst.y != 0u) {          /* a copy: into the world by its own matrix */
        uint b = (pc.inst.x + uint(gl_InstanceIndex)) * 4u;
        vec4 r0 = inst.v[b], r1 = inst.v[b + 1u], r2 = inst.v[b + 2u], e = inst.v[b + 3u];
        p = vec4(dot(r0, p), dot(r1, p), dot(r2, p), 1.0);
        a_n = vec3(dot(r0.xyz, a_normal), dot(r1.xyz, a_normal), dot(r2.xyz, a_normal));
        sunk = e.x;
        v_fade = floatBitsToUint(e.y);
    }
    vec4 c = K.wvp * p;
    uint fl = K.flags.x;
    uint fogmode = K.flags.y;
    vec3 e = vec3(0.0);
    if ((fl & 1u) != 0u || fogmode != 0u)
        e = (K.wv * p).xyz;
    vec4 vd = a_diffuse, vs = a_specular, od, os, dsh, ssh;
    bool split = (pc.shadow.x & 0x1ffu) == 0x101u;
    if ((fl & 1u) == 0u) {
        od = vd;
        os = vs;
        dsh = od;
        ssh = os;
    } else {
        bool cv = (fl & 2u) != 0u;
        uint src = K.flags.w;
        vec4 md = source(cv, src & 15u, vd, vs, K.mat[0]);
        vec4 ms = source(cv, (src >> 4) & 15u, vd, vs, K.mat[2]);
        vec4 ma = source(cv, (src >> 8) & 15u, vd, vs, K.mat[1]);
        vec4 me = source(cv, (src >> 12) & 15u, vd, vs, K.mat[3]);
        vec3 n = a_n.x * K.nm[0].xyz + a_n.y * K.nm[1].xyz + a_n.z * K.nm[2].xyz;
        if ((fl & 16u) != 0u)
            n = normalize3(n);
        vec3 dif = vec3(0.0), amb = K.ambient.rgb, spe = vec3(0.0), difsun = vec3(0.0), spesun = vec3(0.0);
        vec3 view = vec3(0.0, 0.0, -1.0);
        if ((fl & 8u) != 0u)
            view = normalize3(-e);
        float power = K.power_fog.x;
        bool spec = (fl & 4u) != 0u && power > 0.0;
        uint nl = K.flags.z;
        for (uint i = 0u; i < nl; i++) {
            float type = K.lights[i].type_range.x;
            vec3 L;
            float att = 1.0, spot = 1.0;
            if (type == 3.0) {
                L = -K.lights[i].dir.xyz;
            } else {
                L = K.lights[i].pos.xyz - e;
                float d = length(L);
                float range = K.lights[i].type_range.y;
                if (range > 0.0 && d > range)
                    continue;
                vec4 a = K.lights[i].att;
                float den = a.x + a.y * d + a.z * d * d;
                att = den > 0.0 ? 1.0 / den : 1.0;
                L = normalize3(L);
                if (type == 2.0) {
                    float rho = -dot(L, K.lights[i].dir.xyz);
                    float ct = K.lights[i].spot.x, cp = K.lights[i].spot.y;
                    float falloff = K.lights[i].type_range.z;
                    if (rho <= cp)
                        spot = 0.0;
                    else if (rho < ct && ct > cp)
                        spot = pow((rho - cp) / (ct - cp), falloff > 0.0 ? falloff : 1.0);
                }
            }
            float k = att * spot;
            amb += k * K.lights[i].ambient.rgb;
            float ndl = dot(n, L);
            if (ndl <= 0.0)
                continue;
            vec3 d = k * ndl * K.lights[i].diffuse.rgb * (type == 3.0 ? sunk : 1.0), s = vec3(0.0);
            dif += d;
            if (spec) {
                vec3 h = normalize3(L + view);
                float nh = dot(n, h);
                if (nh > 0.0)
                    s = k * pow(nh, power) * K.lights[i].specular.rgb;
                spe += s;
            }
            if (type == 3.0) {          /* the sun: what a shadow takes away */
                difsun += d;
                spesun += s;
            }
        }
        od = vec4(me.rgb + ma.rgb * amb + md.rgb * dif, md.a);
        os = vec4((fl & 4u) != 0u ? ms.rgb * spe : vec3(0.0), ms.a);
        od = clamp(od, 0.0, 1.0);
        os = clamp(os, 0.0, 1.0);
        dsh = od;
        ssh = os;
        if (split) {
            dsh = clamp(vec4(me.rgb + ma.rgb * amb + md.rgb * (dif - difsun), md.a), 0.0, 1.0);
            ssh = clamp(vec4((fl & 4u) != 0u ? ms.rgb * (spe - spesun) : vec3(0.0), ms.a), 0.0, 1.0);
        }
    }
    if (fogmode != 0u) {
        float d = (fl & 32u) != 0u ? length(e) : abs(e.z), f;
        float fs = K.power_fog.y, fe = K.power_fog.z, dens = K.power_fog.w;
        if (fogmode == 3u)
            f = fe != fs ? (fe - d) / (fe - fs) : 1.0;
        else if (fogmode == 1u)
            f = exp(-dens * d);
        else
            f = exp(-(dens * d) * (dens * d));
        os.a = clamp(f, 0.0, 1.0);
        ssh.a = os.a;
    }
    vec4 pl = K.place;
    vec2 pz = K.place2.xy;
    gl_Position = vec4(pl.x * c.x + pl.y * c.w, pl.z * c.y + pl.w * c.w, pz.x * c.w + pz.y * c.z, c.w);
    gl_PointSize = 1.0;
    v_diffuse = od;
    f_diffuse = od;
    v_specular = os;
    f_specular = os;
    v_tc = vec4(pc.tnl.z == 0u ? a_tc : vec2(0.0), pc.tnl.w == 0u ? a_tc : vec2(0.0));
    v_dsh = dsh;
    f_dsh = dsh;
    v_ssh = ssh;
    f_ssh = ssh;
    v_wpos = (pc.shadow.x & 0x100u) != 0u ? vec3(dot(K.world[0], p), dot(K.world[1], p), dot(K.world[2], p))
                                          : vec3(0.0);
    v_wnrm = (pc.shadow.x & 0x100u) != 0u ? vec3(dot(K.world[0].xyz, a_n), dot(K.world[1].xyz, a_n),
                                                 dot(K.world[2].xyz, a_n))
                                          : vec3(0.0);
#ifdef MOTION
    motion_out(p, pc.tnl.x);
#endif
}
