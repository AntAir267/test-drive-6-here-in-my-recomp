/* Controllers in TD6 (pad.h): menus by pad, a ready Xbox layout for races, keyboard beside it.

   Races. A player's controls are 16 entries per controller in their profile (0x65b688 +
   controller * 0x40: steer left, steer right, accelerate, brake, hand brake, horn, gear up, gear
   down, rear view, camera - the CONTROLLER OPTIONS screen's order; 0 the keyboard's key codes,
   else a joystick's: 1 / 2 X- / X+, 0x200 / 0x400 Y- / Y+, 0x40000 << b button b < 8). Entering a race (0x4450a0) the game turns the one the
   player chose (CONTROLLER in its options; the keyboard unless changed) into the live tables and
   0x409a50(ECX = joystick + 1, 0 the keyboard) applies them: with X on the steering and Y on the
   pedals it reads them as analog axes. A joystick it gives only the actions mapped on it from the
   keyboard; the rest stay keys.
   With a controller connected and the keyboard chosen, the race gets the XBOX layout here (hook on
   0x409a50): left stick steering, RT accelerate and LT brake (pad.c's Y), A hand brake, B gear up,
   X gear down, Y camera, LB rear view, RB horn; and every action keeps its key too. The race's
   ticks then read the controller in use - the one used last, so a second device for the same
   pad (Handheld Daemon's virtual one beside the built-in) or a second pad takes over as soon as
   it is used - with the keys, where the game would read the keyboard alone (hook on 0x408d00
   for its call from 0x446e50). A player who
   picks the controller in the game's options keeps their own mapping (the keys go on for what it
   leaves out, as the game does) - except that the game's untouched joystick default (X for
   steering, buttons 0-7 for the rest: A accelerating, B braking, no triggers) becomes the same
   XBOX layout in the profile, so the options screen shows what a race does and it can be changed
   there. That screen names a joystick's controls from DirectInput's object names ("X AXIS <",
   "BUTTON 0"; kept in the joystick's record, 0x40aaf0): they become the pad's (LS LEFT, RT, A).

   Menus. TD6's menus read the keyboard only (the pause menu also a joystick's X/Y and buttons in
   its digital mode). With a controller, outside races and in the pause menu (0x62e16c), the D-pad
   and the left stick are the arrow keys, A and Start Enter, B Escape; in a race Start is Escape
   (pause). The keys are pressed in the keyboard state the game reads, released when the
   controller lets go or the race goes on.

   Plugged in later. The game looks for joysticks once, at start-up (0x408eb0: EnumDevices with
   0x409ed0, which skips the ones it has and sets up a new one: device, data format, cooperative
   level); a controller connected after that is handed to that callback at a frame end, so it is
   the game's next joystick and the next race takes it. Unplugged, its device reads at rest and
   the keyboard goes on; one plugged back in takes the same joystick.

   In test runs none of this unless TD6_ENHANCED is set: the game's memory is then as shipped. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "image.h"
#include "input.h"
#include "pad.h"
#include "options.h"
#include "funcs.h"

#define RACE_PAUSED   0x62e16cu         /* the race is paused (its ticks skipped) */
#define JOY_COUNT     0x4e1f30u         /* joysticks the game set up */
#define LIVE_KEYS     0x4e1ef0u         /* the keyboard's 16 codes for the race (0x4450a0) */
#define LIVE_JOY      0x4e203cu         /* the joystick's 16 entries */
#define JOY_KEYS      0x509400u         /* the keys for what the joystick leaves out */
#define GAME_DI       0x4e2148u         /* the game's IDirectInput7 */
#define JOY_FOUND     0x409ed0u         /* its EnumDevices callback for joysticks */
#define CAR_JOYSTICK  0x5853d8u         /* per car: its joystick + 1 for force feedback (0x4317e0) */

static int allowed;
static int xbox_active;                 /* this race's controls are the XBOX layout */

/* Rumble (option "rumble", on unless off): controllers are force-feedback devices, so the game
   creates and plays its effects (dinput.c turns them into the motors' rumble; DESIGN, decision 2).
   It plays them for the cars that have a joystick (0x5853d8, set from the controller chosen):
   with the XBOX layout the player's car is given the joystick it reads there too. */
static int rumble_percent(void)         /* 0 off, -1 the game's own strength, else the percentage */
{
    const char *v = option_str("rumble", NULL);
    if (!v)
        return -1;
    int n = atoi(v);
    if (n > 0)
        return n > 100 ? 100 : n;
    return option_bool("rumble", 1) || !strcasecmp(v, "on") ? -1 : 0;
}

int ifc_rumble_wanted(void)
{
    return (!deterministic || getenv("TD6_ENHANCED")) && rumble_percent() != 0;
}

/* "rumble = <percent>": the game's force-feedback strength (0x50ae50, from its settings in 25 %
   steps, 0.75 unless changed) */
static void rumble_strength(void)
{
    int p = rumble_percent();
    if (p <= 0)
        return;
    float want = (float)p / 100.0f;
    uint32_t u;
    memcpy(&u, &want, 4);
    if (R32(0x50ae50u) != u)
        W32(0x50ae50u, u);
}

static const uint32_t xbox_layout[16] = {
    1, 2, 0x200, 0x400,                 /* X- X+ Y- Y+: steer, accelerate, brake */
    0x400000,                           /* A: hand brake */
    0x100000,                           /* B: gear up */
    0x200000,                           /* X: gear down */
    0x2000000,                          /* Y: camera */
    0x800000,                           /* LB: rear view */
    0x1000000,                          /* RB: horn */
    0, 0, 0, 0, 0, 0};

/* The game's joystick (0-based) for the controller in use - the one used last: a handheld can
   show one controller as more than one device (Handheld Daemon's virtual pad beside the built-in
   one) and a docked one has more; else the first. */
static int race_joystick(void)
{
    uint32_t n = R32(JOY_COUNT);
    int slot = pad_active_slot();
    for (uint32_t j = 0; j < n && j < 8 && slot >= 0; j++) {
        uint32_t rec = R32(0x4e20c4u + 4 * j);
        if (rec && dinput_device_slot(R32(rec)) == slot)
            return (int)j;
    }
    return 0;
}

static int race_joy = -1;               /* the joystick the XBOX layout reads now */

static void use_joystick(int j)
{
    if (j == race_joy)
        return;
    race_joy = j;
    if (ifc_rumble_wanted())
        W32(CAR_JOYSTICK, (uint32_t)j + 1);   /* the player's car rumbles on it */
    LOG(LOG_ALWAYS, "controller: the race reads joystick %d (the XBOX layout)", j + 1);
}

/* 0x409a50 at entry: ECX = the joystick + 1 chosen for the race, 0 the keyboard */
static int controls_apply_hook(CPU *c)
{
    xbox_active = 0;
    if (c->ecx != 0 || !pad_count() || (int32_t)R32(JOY_COUNT) < 1)
        return 0;
    for (int i = 0; i < 16; i++) {
        W32(JOY_KEYS + 4 * i, R32(LIVE_KEYS + 4 * i));     /* every action keeps its key */
        W32(LIVE_JOY + 4 * i, xbox_layout[i]);
    }
    race_joy = -1;
    use_joystick(race_joystick());
    c->ecx = (uint32_t)race_joy + 1;
    xbox_active = 1;
    LOG(LOG_INPUT, "padgame: the race takes the XBOX layout on joystick 1, the keys beside it");
    return 0;
}

/* 0x408d00, the keyboard's controls for a race tick, called by 0x446e50 when the player's
   controller is the keyboard: with the XBOX layout, joystick 1's instead (0x408cf0, which reads
   the keys for every action as well) */
static int race_keys_hook(CPU *c)
{
    if (!xbox_active || R32(c->esp) != 0x446e91u)
        return 0;
    use_joystick(race_joystick());
    c->ecx = (uint32_t)race_joy;
    F_00408cf0(c);
    return 1;
}

/* ------------------------------------------------------------------ the options screen */
#define JOY_RECORDS   0x4e20c4u         /* the joysticks' records */
#define PROFILE_CTL   0x65b688u         /* the player's controls, 0x40 per controller */

/* The game's default for a joystick, and the XBOX layout in the profile's terms */
static const uint32_t joy_default[10] = {1, 2, 0x40000, 0x80000, 0x100000, 0x200000, 0x400000, 0x800000,
                                         0x1000000, 0x2000000};
static const uint32_t joy_xbox[10] = {
    1, 2,                               /* steering: the left stick */
    0x200, 0x400,                       /* accelerate RT, brake LT (Y- / Y+) */
    0x40000 << 0,                       /* hand brake A */
    0x40000 << 5,                       /* horn RB */
    0x40000 << 1,                       /* gear up B */
    0x40000 << 2,                       /* gear down X */
    0x40000 << 4,                       /* rear view LB */
    0x40000 << 3};                      /* camera Y */

static const char *const axis_label[4] = {"LS LEFT", "LS RIGHT", "RT", "LT"};
static const char *const button_label[11] = {"A", "B", "X", "Y", "LB", "RB", "BACK", "START",
                                             "LS PRESS", "RS PRESS", "GUIDE"};

static void wstr(uint32_t a, const char *t)
{
    size_t n = strlen(t);
    memcpy(gptr(a), t, n + 1);
}

static void options_screen(void)
{
    uint32_t n = R32(JOY_COUNT);
    for (uint32_t j = 0; j < n && j < 8; j++) {
        uint32_t rec = R32(JOY_RECORDS + 4 * j);
        if (!rec || strcmp((const char *)gptr(rec + 0x2f8), "X AXIS <"))
            continue;
        for (int a = 0; a < 4; a++)
            wstr(rec + 0x2f8 + 0x1a * (uint32_t)a, axis_label[a]);
        for (int b = 0; b < 11; b++)
            wstr(rec + 0x360 + 0x1a * (uint32_t)b, button_label[b]);
        LOG(LOG_INPUT, "padgame: joystick %u's controls named as a pad's", j + 1);
    }
    for (uint32_t ctl = 1; ctl <= n && ctl <= 8; ctl++) {
        uint32_t p = PROFILE_CTL + 0x40 * ctl;
        int dflt = 1;
        for (int i = 0; i < 10 && dflt; i++)
            dflt = R32(p + 4 * (uint32_t)i) == joy_default[i];
        if (!dflt)
            continue;
        for (int i = 0; i < 10; i++)
            W32(p + 4 * (uint32_t)i, joy_xbox[i]);
        LOG(LOG_INPUT, "padgame: joystick %u's default controls made the XBOX layout", ctl);
    }
}

/* ------------------------------------------------------------------ menus by pad */
enum { MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_ENTER, MK_ESCAPE, MK_N };
static const int mk_dik[MK_N] = {0xc8, 0xd0, 0xcb, 0xcd, 0x1c, 0x01};
static int mk_down[MK_N];

static void mk_set(int k, int down)
{
    if (mk_down[k] == down)
        return;
    mk_down[k] = down;
    input_key(mk_dik[k], down);
}

/* A connected controller the game has no device for: through its own callback */
static void claim_joysticks(void)
{
    static int refused[INPUT_MAX_JOYS];
    int s = dinput_joy_unclaimed();
    if (s < 0 || refused[s] || !R32(GAME_DI) || !current_cpu())
        return;
    uint32_t n = R32(JOY_COUNT);
    if (n < 8) {
        uint32_t inst = dinput_joy_instance(s), caps = galloc(0x244);
        memset(gptr(caps), 0, 0x244);
        W32(caps, 0x244);
        guest_call(current_cpu(), JOY_FOUND, 2, inst, caps);
        gfree(caps);
        gfree(inst);
    }
    if (R32(JOY_COUNT) != n + 1) {
        refused[s] = 1;
        LOG(LOG_ALWAYS, "controller: joystick %d connected after start-up, but the game took no more joysticks", s);
        return;
    }
    LOG(LOG_ALWAYS, "controller: joystick %d connected after start-up is the game's joystick %u", s, n + 1);
}

/* TD6_AUTOPILOT=1 (tests): the AI drives the player's car, so a scripted run reaches a race's
   end. Each car has a kind for the race's ticks (0x62e634 + car * 0x4c, read by 0x428f00): 1 a
   player's (its controls from 0x656bc0, put on the car by 0x43a290), 2 an AI racer's; the
   player's car is made an AI racer. */
static void autopilot(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TD6_AUTOPILOT");
        on = e && *e && *e != '0';
    }
    if (on && R32(GAME_GAME_STATE) == 2 && R8(0x62e634u) == 1) {
        W8(0x62e634u, 2);
        LOG(LOG_ALWAYS, "TD6_AUTOPILOT: the AI drives the player's car");
    }
}

void padgame_frame(unsigned long frame)
{
    (void)frame;
    autopilot();
    if (!allowed)
        return;
    claim_joysticks();
    rumble_strength();
    if (R32(GAME_GAME_STATE) == 1)
        options_screen();
    int want[MK_N] = {0};
    if (pad_count()) {
        int state = (int)R32(GAME_GAME_STATE), paused = R32(RACE_PAUSED) != 0;
        int lx = pad_axis_value(PAD_LX), ly = pad_axis_value(PAD_LY);
        if (state != 2 || paused) {
            want[MK_UP] = pad_held(PAD_DUP) || ly < -20000;
            want[MK_DOWN] = pad_held(PAD_DDOWN) || ly > 20000;
            want[MK_LEFT] = pad_held(PAD_DLEFT) || lx < -20000;
            want[MK_RIGHT] = pad_held(PAD_DRIGHT) || lx > 20000;
            want[MK_ENTER] = pad_held(PAD_A) || (state != 2 && pad_held(PAD_START));
            want[MK_ESCAPE] = pad_held(PAD_B) || (paused && pad_held(PAD_START));
        } else
            want[MK_ESCAPE] = pad_held(PAD_START);
    }
    for (int k = 0; k < MK_N; k++)
        mk_set(k, want[k]);
}

void padgame_init(void)
{
    allowed = !deterministic || getenv("TD6_ENHANCED");
    if (allowed) {
        hook_controls_apply = controls_apply_hook;
        hook_race_keys = race_keys_hook;
    }
}
