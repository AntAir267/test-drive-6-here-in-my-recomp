/* Smooth motion (options.txt smooth_camera = on | off; on by default, off in --test runs unless
   TD6_ENHANCED): races drawn moving evenly at frame rates above the game's 30 Hz physics.

   TD6 runs its physics in 30 Hz ticks and draws each frame extrapolated from the last tick by the
   tick's fraction (0x445850 stores it at 0x62e164: the low 16 bits of its clock in ticks times
   1/65536, the constant at 0x46d4f8): the cars, the camera and what it looks at are drawn at their
   tick's position plus their velocity times the fraction. But a tick moves them 1.25 times that
   velocity [verified 2026-11-14: the chase camera's target (the car) logged each frame of a
   real-time race, at steady speed: 1.2514 at 120 fps, 1.2515 at 165, over 853 ticks], so a drawn
   frame falls behind through each tick and catches up at the next: the frame a tick starts on
   moves 2 times as far as the others at 120 fps (2.4 at 165), seen on hills and jumps as the
   camera's height and everywhere else as judder of the whole picture. At 30 fps every frame is a
   tick and nothing shows, which is what the game was made for.

   So the drawing's fraction is made 1.25 times as large: the multiply at 0x4458ba reads 1.25/65536
   instead (here: the value at
   0x62e164 scaled by 1.25 when the frame's drawing starts, 0x457580), and everything extrapolated reaches
   the next tick's position as the tick comes, moving evenly and together. Only drawing functions
   read 0x62e164; the physics does not [verified: car records and race state identical with it on
   and off].

   Heights need more: a car's height through a tick comes from the road (in steps) and its
   suspension, not from the vertical velocity the drawing extrapolates by, and the camera's height
   over its car (0x5e38d0, one per view) is eased once a tick, so on hills and jumps the picture
   still stepped (the camera's height moved 1.7 times as far on a tick's frame as on the others at
   120 fps with the fraction fix; predicting by the last tick's change overshoots the steps). So
   heights are drawn between the last two ticks, a tick behind: when the frame's drawing starts
   (0x457580, after the ticks) each car (6 records of 0x330 bytes from 0x650d60) is given the
   height of a tick before (+0x1d8) and the tick's change / 1.25 as its vertical velocity (+0x1c0),
   and each camera height the value between its last two ticks' by the fraction; the true values
   go back when the race frame ends (widescreen.c's race_frame hook), so the ticks never see them
   [verified 2026-11-14: tick frame against the others 0.97 for the camera's and the car's height
   at 120 and 165 fps; physics identical, DESIGN.md]. A tick that changes a height by more than
   0x10000 (put back on the road, a new race) keeps the game's own. TD6_CAM_NO_HEIGHT=1: heights
   as the game's (tests). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "funcs.h"

#define GAME_STATE  0x63b740u           /* 2: a race (0x459740) */
#define TICK_FRAC   0x62e164u           /* the fraction of the physics tick a drawn frame is at */
#define TICK_STEP   1.25f               /* a tick's movement in units of the velocity extrapolated by */
#define TICK_COUNT  0x5e7be4u           /* physics ticks run (0x445850 counts them) */
#define CARS        0x650d60u
#define CAR_SIZE    0x330u
#define NCARS       6
#define CAR_Y       0x1d8u              /* height */
#define CAR_VY      0x1c0u              /* vertical velocity */
#define CAM_HEIGHT  0x5e38d0u           /* each view's camera height over its car (2: split screen) */
#define NCAMS       2

static struct { int32_t last, base, dy, y, vy; uint32_t tick; int known, own; } car[NCARS];
static struct { int32_t last, base, now; int known, own; } cam[NCAMS];
static uint32_t cam_tick;
static int swapped, heights = 1;

static int32_t to_drawing(int32_t dy) { float v = dy / TICK_STEP; return (int32_t)(v < 0 ? v - 0.5f : v + 0.5f); }

/* the frame's drawing starts: the fraction made 1.25 times as large; each car drawn between its
   last two ticks' heights, each camera height between its last two ticks' values */
static int race_draw_hook(CPU *c)
{
    (void)c;
    if (R32(GAME_STATE) != 2)
        return 0;
    uint32_t u = R32(TICK_FRAC);
    float f;
    memcpy(&f, &u, 4);
    float scaled = f * TICK_STEP;
    memcpy(&u, &scaled, 4);
    W32(TICK_FRAC, u);
    if (!heights)
        return 0;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    uint32_t t = R32(TICK_COUNT);
    for (int i = 0; i < NCARS; i++) {
        uint32_t rec = CARS + CAR_SIZE * (uint32_t)i;
        int32_t y = (int32_t)R32(rec + CAR_Y), vy = (int32_t)R32(rec + CAR_VY);
        if (!car[i].known || t != car[i].tick) {
            uint32_t n = t - car[i].tick;
            int64_t d = (int64_t)y - car[i].last;
            car[i].own = !car[i].known || n < 1 || n > 4 || d > 0x10000 || d < -0x10000;
            if (!car[i].own) {
                car[i].dy = (int32_t)(d / (int64_t)n);      /* the last tick's change */
                car[i].base = y - car[i].dy;                /* the height a tick before */
            }
            car[i].last = y;
            car[i].tick = t;
            car[i].known = 1;
        }
        car[i].y = y;
        car[i].vy = vy;
        if (!car[i].own) {
            W32(rec + CAR_Y, (uint32_t)car[i].base);
            W32(rec + CAR_VY, (uint32_t)to_drawing(car[i].dy));
        }
    }
    uint32_t n = t - cam_tick;
    for (int k = 0; k < NCAMS; k++) {
        int32_t v = (int32_t)R32(CAM_HEIGHT + 4u * (uint32_t)k);
        if (!cam[k].known || n) {
            int64_t d = (int64_t)v - cam[k].last;
            cam[k].own = !cam[k].known || n < 1 || n > 4 || d > 0x10000 || d < -0x10000;
            cam[k].base = cam[k].own ? v : (int32_t)(v - d / (int64_t)n);
            cam[k].last = v;
            cam[k].known = 1;
        }
        cam[k].now = v;
        if (!cam[k].own) {
            float h = cam[k].base + (float)(v - cam[k].base) * f;
            W32(CAM_HEIGHT + 4u * (uint32_t)k, (uint32_t)(int32_t)(h < 0 ? h - 0.5f : h + 0.5f));
        }
    }
    cam_tick = t;
    swapped = 1;
    return 0;
}

/* TD6_CAM_LOG=1 (tests): each race frame as drawn, the fraction, the camera's position
   (0x58546c) and the player's car's drawn height (+0x138) and its tick's height:
   "CAM <ms> <fraction> <x> <y> <z> <car drawn y> <car y>" */
static void cam_log(void)
{
    static int on = -1;
    if (on < 0) on = getenv("TD6_CAM_LOG") != NULL;
    if (!on || R32(GAME_STATE) != 2)
        return;
    float v[5];
    uint32_t rec = CARS + CAR_SIZE * R32(0x62e15cu);
    const uint32_t at[5] = {TICK_FRAC, 0x58546cu, 0x585470u, 0x585474u, rec + 0x138u};
    for (int i = 0; i < 5; i++) { uint32_t u = R32(at[i]); memcpy(&v[i], &u, 4); }
    LOG(LOG_ALWAYS, "CAM %.3f %.4f %.2f %.2f %.2f %.2f %d", now_ms(), v[0], v[1], v[2], v[3], v[4],
        swapped ? car[R32(0x62e15cu) % NCARS].y : (int32_t)R32(rec + CAR_Y));
}

/* the race frame ends: the true heights and velocities back */
void camera_race_end(void)
{
    cam_log();
    if (!swapped)
        return;
    for (int i = 0; i < NCARS; i++) {
        uint32_t rec = CARS + CAR_SIZE * (uint32_t)i;
        W32(rec + CAR_Y, (uint32_t)car[i].y);
        W32(rec + CAR_VY, (uint32_t)car[i].vy);
    }
    for (int k = 0; k < NCAMS; k++)
        W32(CAM_HEIGHT + 4u * (uint32_t)k, (uint32_t)cam[k].now);
    swapped = 0;
}

void camera_init(void)
{
    int allowed = !deterministic || getenv("TD6_ENHANCED");
    int on = allowed && strcasecmp(option_str("smooth_camera", "on"), "off") != 0;
    if (allowed)
        LOG(LOG_ALWAYS, "smooth camera: %s", on ? "on" : "off");
    if (!on)
        return;
    heights = !getenv("TD6_CAM_NO_HEIGHT");
    hook_race_draw = race_draw_hook;
}
