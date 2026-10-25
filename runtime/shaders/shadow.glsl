/* Ray-traced shadows (vk.c): the per-frame block and the geometries of the frame's acceleration
   structure, std430 (ShadowGlobals and ShadowGeom in vk.c). Included by draw.frag, resolve.frag
   and ao.frag. */
struct Beam {                   /* a headlight's beam (VkBeam): game units, camera-relative world */
    vec4 pos;                   /* the lamp; w its reach */
    vec4 dir;                   /* where it points (unit); w its half-width across (tangent) */
    vec4 up;                    /* the car's up (unit); w its half-width above its axis (tangent; below: 3 x) */
    vec4 col;                   /* its light near it on a surface facing it; w where that is halved */
};
struct Globals {
    mat4 inv_vp;                /* D3D clip space -> camera-relative world (row vectors, so M * v) */
    vec4 place;                 /* ax bx ay by: D3D clip -> Vulkan clip (as TnlConst.place) */
    vec4 place2;                /* mnz dz; 1 / GPU width, 1 / GPU height */
    vec4 sun;                   /* towards the sun (world), ray length (game units) */
    vec4 cam;                   /* the camera in game units (the light map's place); w: how much of
                                   the water's sun part (its own and the ground's seen through it) a
                                   shadow on it takes */
    vec4 fade;                  /* shadows fade out from x to y (eye distance); z: ray offset per unit
                                   of distance; w: how much of the terrain's colour a shadow takes
                                   per unit of the object light map */
    ivec4 slots;                /* object light map texture, a sampler, attachment 1, attachment 2 */
    ivec4 slots2;               /* depth, geometries this frame, nearest sampler, geometries per frame */
    vec4 ray0, rayx, rayy;      /* the camera's rays (camera-relative world, not unit length): view pixel
                                   x, y's (its centre's) is ray0 + x * rayx + y * rayy */
    /* ray-traced reflections (refl.glsl) */
    ivec4 tiles[4];             /* the terrain's colour map: its 16 tiles' textures (256 x 256 texels for
                                   64 x 64 cells; tile z / 64 * 4 + x / 64; -1 not drawn yet) */
    ivec4 rslots;               /* x: the tiles' sampler, y: the sky's first geometry (-1 none), z: a
                                   trilinear repeating sampler (LIGHT's stand-in), w: - */
    vec4 terr;                  /* the terrain: x the colour pass's vertex grey, y what its detail pass
                                   leaves of it, z, w: - */
    vec4 tavg;                  /* the colour map's mean colour (for tiles not drawn yet) */
    vec4 refl;                  /* x: how far the strength rises towards grazing (Fresnel), y: how much
                                   of the colour below that rise takes away, z: the rays' length, w: a
                                   view pixel's angle (radians) */
    vec4 fogp, fogcol;          /* the view's fog: start, end, density, how (mode 1 EXP 2 EXP2 3 LINEAR);
                                   its colour (rgb) */
    vec4 fogedge;               /* drawdist.c's full fog at the view's edge: x there, from y (0: none) */
    vec4 refl2;                 /* x: how bright what rays meet is shown (the game's env map, its
                                   stand-in, is darker than the world), y: a debug view; z, w:
                                   LIGHT's game look takes over from 0 at z to most at w, the
                                   spread of what the 4 nearest smaller-image pixels meet */
    vec4 sunc;                  /* the sun's colour (rgb; its glint in reflections), w: - */
    ivec4 beamn;                /* headlights (headlights.c, resolve.frag pass 6): x the beams, y
                                   INSANE_HL_DEBUG, z 1 the terrain casts their shadows, w the view's
                                   colour copied (its texture) */
    vec4 hl;                    /* rgb the ambient light lit objects take; w the summed light's ceiling */
    vec4 hl2;                   /* rgb the terrain's colour pass's vertex colour (the light it is drawn with) */
    vec4 hl3;                   /* rgb the beams' colour (green 1); w how much of it the air scatters per game
                                   unit (VOLUMETRIC; 0 none) */
    Beam beams[16];
    ivec4 flaren;               /* the lamps' flares (vk.c flares): x how many */
    vec4 flare[24];             /* two each: x y its place (GPU pixels), its eye distance, its strength; its
                                   core's and halo's sizes (pixels), how far it reaches, - */
};
struct Geom {
    uvec2 vaddr, iaddr;         /* the draw's 40-byte vertices and 32-bit indices (device addresses) */
    int tex, smp;               /* its texture (alpha cut-outs), sampler */
    float aref;                 /* alpha below this lets light through */
    uint flags;
    uint tnl;                   /* its transform and lighting constants (tnl.vert's Draw) */
    uint misc;                  /* the share of its texture that is solid (float bits; 0 unknown) */
};
layout(std430, set = 0, binding = 3) readonly buffer Shadow {
    Globals g[8];               /* a view slot each: frame * 4 + view (vk.c SH_SLOTS) */
    Geom geo[];
} sh;

/* A beam's light on a surface at p (camera-relative world) with normal n (unit; 0: facing it):
   across its width a soft edge, above its axis a sharp cut-off (a low beam's), falling off as
   1 / (1 + (d / half)^2) and smoothly to nothing at its reach. The slant counts as n.l ^ 0.35: the
   road, seen by the lamp at a few degrees, still takes it (as the eye, adapted to the dark, sees
   a lit road) */
vec3 beam_light(Beam b, vec3 p, vec3 n)
{
    vec3 v = p - b.pos.xyz;
    float d2 = dot(v, v);
    if (d2 >= b.pos.w * b.pos.w)
        return vec3(0.0);
    float a = dot(v, b.dir.xyz);
    if (a <= 0.0)
        return vec3(0.0);
    vec3 side = cross(b.dir.xyz, b.up.xyz);
    float x = dot(v, side) / (a * b.dir.w), y = dot(v, b.up.xyz) / a;
    y /= y > 0.0 ? b.up.w : 3.0 * b.up.w;
    float r = x * x + y * y;
    if (r >= 1.0)
        return vec3(0.0);
    float d = sqrt(d2), cone = 1.0 - smoothstep(0.25, 1.0, r);
    float nl = dot(n, n) > 0.0 ? pow(max(-dot(n, v) / d, 0.0), 0.35) : 1.0;
    float q = d2 / (b.pos.w * b.pos.w), win = (1.0 - q * q) * (1.0 - q * q);
    return b.col.rgb * (cone * nl * win / (1.0 + d2 / (b.col.w * b.col.w)));
}
