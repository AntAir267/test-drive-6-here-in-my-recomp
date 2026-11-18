/* Widescreen (option "widescreen = off | on | <w>:<h>"; on = the screen's shape): Hor+.

   TD6 keeps up to five viewports (0x4aab64 + n * 0xbc: the rectangle in 640 x 480 units, x and y
   scales, and two projections): a perspective one for the 3D world built by 0x4032a0(near, far,
   fov; ECX the matrix, EDX the viewport) from one field of view and the rectangle's shape, and a
   flat one (0x403380) for the HUD and the menus, drawn as lit vertices through it. A full-width
   viewport's perspective gets a wider horizontal view with the same vertical one (its x scale
   divided by the widening), and the renderer spreads such a camera's viewport across the screen
   (vk.c: proj_widened, the projection's y/x scale being the wide aspect). The flat projection is
   left alone: menus and the HUD keep their 4:3 shape in the middle, except that inside the race
   HUD's routine (0x453cb0) each piece moves to the screen edge it sits nearest, and in the front
   end's frame (0x41d100) its top banner and bottom bar reach the screen's sides (vk.c). The game's
   view cone for what it draws (widen_cone) is widened with the camera. Off by default
   and in test runs: the game's memory then never changes. */
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"
#include "image.h"
#include "options.h"
#include "widescreen.h"
#include "vk.h"
#include "funcs.h"

static int ws_on, ws_log, allowed;
static float ws_want;            /* requested aspect, 0 = follow the screen */
float ws_aspect = 4.0f / 3.0f;   /* current target aspect (width / height) */
float ws_world_aspect;           /* y/x scale the widened camera's projection has */
int ws_race_depth, ws_hud_depth, ws_menu_depth;

#define VIEWPORT(n) (0x4aab64u + (uint32_t)(n) * 0xbcu)

static float rdf(uint32_t a) { uint32_t u = R32(a); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); W32(a, u); }

/* The full-width perspectives built, by viewport (the builder's callers, 0x4018c0 and 0x402d80,
   copy its result to the viewport's record, + 0x28): the x scale as built and as we left it. One
   built before the screen's shape was known (the front end's, at start-up) or kept across a
   window resize is widened again at the next frame (refresh), unless the game rewrote it. */
static struct { int used; float orig, set; } built[5];
#define VP_PERSPECTIVE(n) (VIEWPORT(n) + 0x28u)

static float widened(float orig)
{
    return ws_aspect > 4.0f / 3.0f ? orig / (ws_aspect / (4.0f / 3.0f)) : orig;
}

/* The game's view cone, for its visibility tests of the level's models (0x432b90) and the other
   objects (0x432d00): built by 0x433b80 from the screen's size at a race's start and by 0x433c60
   when a camera changes its focal length (0x5855a8). Its sides' plane normal (0x585518 x,
   0x585520 z) comes from half the width (0x585590) over the focal length, a little wider than the
   camera sees. Widened as the camera is, or what the wider view shows at its sides is dropped
   until it is well inside (pop-in at the screen's edges). */
static void widen_cone(void)
{
    float w = rdf(0x585590u), d = rdf(0x5855a8u);
    if (!ws_on || ws_aspect <= 4.0f / 3.0f || !(w > 0.0f) || !(d > 0.0f))
        return;
    double hw = 0.5 * w * (ws_aspect / (4.0 / 3.0)), s = sqrt((double)d * d + hw * hw);
    wrf(0x585518u, (float)(d / s));
    wrf(0x585520u, (float)(-hw / s));
}

static int view_cone_hook(CPU *c) { F_00433b80_orig(c); widen_cone(); return 1; }
static int view_cone_again_hook(CPU *c) { F_00433c60_orig(c); widen_cone(); return 1; }

static void refresh(void)
{
    if (!ws_on)
        return;
    widen_cone();                           /* (for a window resized since the cone was built) */
    for (int n = 0; n < 5; n++) {
        uint32_t m = VP_PERSPECTIVE(n);
        if (!built[n].used || rdf(m) != built[n].set)
            continue;
        float want = widened(built[n].orig);
        if (want != built[n].set) {
            if (ws_log)
                LOG(LOG_ALWAYS, "viewport %d: perspective widened again for aspect %g", n, ws_aspect);
            wrf(m, want);
            built[n].set = want;
        }
        if (want != built[n].orig && want != 0.0f)
            ws_world_aspect = rdf(m + 20) / want;
    }
}

/* 0x4032a0 at entry: ECX the matrix out, EDX the viewport, [esp+4] near, +8 far, +12 fov */
static int perspective_hook(CPU *c)
{
    uint32_t m = c->ecx, vp = c->edx;
    F_004032a0_orig(c);
    int32_t w = (int32_t)(R32(VIEWPORT(vp) + 8) - R32(VIEWPORT(vp)));
    if (ws_log)
        LOG(LOG_ALWAYS, "perspective(viewport %u, %d wide): P %g %g", vp, w, rdf(m), rdf(m + 20));
    if (!ws_on || vp > 4)
        return 1;
    built[vp].used = w == 640;
    if (w != 640)
        return 1;
    built[vp].orig = rdf(m);
    built[vp].set = widened(rdf(m));
    wrf(m, built[vp].set);
    if (built[vp].set != built[vp].orig && built[vp].set != 0.0f)
        ws_world_aspect = rdf(m + 20) / built[vp].set;
    return 1;
}

static void parse(void)
{
    const char *v = option_str("widescreen", "off");
    ws_want = 0;
    if (!strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcmp(v, "0")) {
        ws_on = 0;
    } else if (!strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcmp(v, "1")) {
        ws_on = 1;
    } else {
        float a = 0, b = 0;
        if (sscanf(v, "%f:%f", &a, &b) == 2 && a > 0 && b > 0) ws_want = a / b;
        else if (sscanf(v, "%f", &a) == 1 && a > 1.0f) ws_want = a;
        ws_on = ws_want > 0;
        if (!ws_on)
            LOG(LOG_ALWAYS, "options: widescreen \"%s\" is not on, off or an aspect like 16:9", v);
    }
    ws_aspect = ws_on && ws_want > 0 ? ws_want : 4.0f / 3.0f;
}

#define WRAP(name, counter, fn, before) \
    static int name(CPU *c) { before; counter++; fn##_orig(c); counter--; return 1; }
/* (TD6_WS_LOG: when the sun - 0x4473e0's projection, 0x62e1a0 visible, x/y at 0x62e1a4 - shows) */
static void sun_log(void)
{
    static int last = -1;
    int vis = R8(0x62e1a0u);
    if (ws_log && vis != last)
        LOG(LOG_ALWAYS, "sun %s (colour %u, flare %u) at %g, %g; sun %d %d %d camera %g %g %g", vis ? "in view" : "out of view",
            R32(0x49b868u), R8(0x49b88cu), rdf(0x62e1a4u), rdf(0x62e1a8u), (int)R32(0x49b880u), (int)R32(0x49b884u),
            (int)R32(0x49b888u), rdf(0x58546cu), rdf(0x585470u), rdf(0x585474u));
    last = vis;
}
/* VIDEO OPTIONS' RESOLUTION line (0x422fa0, ECX the item: the selected mode's index is the
   short at [0x65de9c + ECX * 4]; the game's mode list at [0x577224], 12 bytes an entry, its
   width, height and depth at + 8, + 0xc, + 0x10 [verified: disassembly of 0x422fa0, which
   formats them with "%d, %d, %d " (0x482130) and adds BIT]). With resolution = native the game's
   mode only sets its own coordinates and the picture fills the screen at the screen's
   resolution (vk.c res_update), so the line shows that size: written into the entry for the
   call, then put back. */
static int resolution_text_hook(CPU *c)
{
    int w = 0, h = 0;
    uint32_t list = R32(0x577224u), sel = R32(0x65de9cu + c->ecx * 4u);
    if (!vk_resolution_native() || !list || !sel || c->ecx > 16)
        return 0;
    vk_native_size(&w, &h);
    if (w <= 0 || h <= 0)
        return 0;
    uint32_t e = list + 12u * (uint32_t)(int16_t)R16(sel);
    uint32_t ow = R32(e + 8), oh = R32(e + 0xc);
    W32(e + 8, (uint32_t)w);
    W32(e + 0xc, (uint32_t)h);
    F_00422fa0_orig(c);
    W32(e + 8, ow);
    W32(e + 0xc, oh);
    return 1;
}

/* the race frame; after it, the cars' true heights back for the next ticks (camera.c) */
void camera_race_end(void);
static int race_frame_hook(CPU *c)
{
    refresh();
    sun_log();
    ws_race_depth++;
    F_00445850_orig(c);
    ws_race_depth--;
    camera_race_end();
    return 1;
}
WRAP(race_hud_hook, ws_hud_depth, F_00453cb0, )
WRAP(front_frame_hook, ws_menu_depth, F_0041d100, refresh())

void widescreen_init(void)
{
    ws_log = getenv("TD6_WS_LOG") != NULL;
    allowed = !deterministic || getenv("TD6_ENHANCED");
    parse();
    if (ws_on && !allowed) {
        LOG(LOG_ALWAYS, "widescreen is off in --test runs (it changes the game's camera)");
        ws_on = 0;
    }
    if (allowed || ws_log)
        hook_perspective = perspective_hook;
    if (allowed) {
        hook_race_frame = race_frame_hook;
        hook_race_hud = race_hud_hook;
        hook_front_frame = front_frame_hook;
        hook_view_cone = view_cone_hook;
        hook_view_cone_again = view_cone_again_hook;
        hook_resolution_text = resolution_text_hook;
    }
}

void widescreen_options(void)
{
    if (allowed) parse();
}

int widescreen_on(void) { return ws_on; }

/* The renderer tells us the shape of the picture area (window or screen). */
void widescreen_screen(int w, int h)
{
    if (!ws_on || ws_want > 0 || w <= 0 || h <= 0)
        return;
    float a = (float)w / (float)h;
    ws_aspect = a > 4.0f / 3.0f ? a : 4.0f / 3.0f;
}
