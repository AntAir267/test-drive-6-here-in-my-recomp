/* Game controllers (design point 5): how a controller becomes one of the DirectInput joysticks
   of input.h (pad.c), and what the port does with it in the game (padgame.c): the ready-made
   "XBOX CONTROLLER" layout, choosing it when a controller is connected, menu navigation. */
#pragma once
#include <stdint.h>

/* Buttons and axes, numbered as SDL3's SDL_GamepadButton and SDL_GamepadAxis. */
enum { PAD_A, PAD_B, PAD_X, PAD_Y, PAD_BACK, PAD_GUIDE, PAD_START, PAD_LSTICK, PAD_RSTICK, PAD_LB,
       PAD_RB, PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT, PAD_MISC1, PAD_BUTTONS = 26 };
enum { PAD_LX, PAD_LY, PAD_RX, PAD_RY, PAD_LT, PAD_RT, PAD_AXES };

/* The joystick the game sees: see pad.c (X left stick across, Y the triggers together, Xbox
   button order). */
int pad_attach(const char *id, const char *name);   /* returns the joystick slot, -1 if none free */
void pad_detach(int slot);
void pad_axis(int slot, int axis, int value);        /* sticks -32768..32767 (+ = right/down), triggers 0..32767 */
void pad_button(int slot, int button, int down);

/* The current state of every connected controller together (for menu navigation). */
int pad_held(int button);
int pad_axis_value(int axis);                         /* the one furthest from rest */
int pad_count(void);                                  /* controllers connected */
int pad_active_slot(void);                            /* the one used last (a button, or a stick or trigger well off rest), -1 none */

/* TD6_PAD: a scripted controller for tests, connected at start-up, fed through the same
   calls as SDL's gamepad events: "frame:BUTTON*hold,frame:AXIS=value*hold,...", e.g.
   "900:A*10,1600:LX=-32768*60,1700:RT=32767*300". Buttons: A B X Y BACK GUIDE START LS RS LB
   RB UP DOWN LEFT RIGHT (the D-pad); axes: LX LY RX RY LT RT (back to rest after the hold).
   "frame:PLUG", "frame:UNPLUG", "frame:PLUG2" (another controller, swapped in): if the first
   of these plugs one in, there is no controller at start-up.
   pad_init also reads controller_deadzone from options.txt; call it before the game starts. */
void pad_init(void);
void pad_frame(unsigned long frame);                 /* per presented frame, from input_frame */

/* padgame.c */
void padgame_init(void);    /* after the controllers present at start are attached */
void padgame_frame(unsigned long frame);   /* per frame (from pad_frame) */
