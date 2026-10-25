/* Host-side input state behind the DirectInput stand-in (dinput.c).
 *
 * The window layer feeds keyboard, mouse and controller events in here; the game reads them back
 * through IDirectInputDevice2A (GetDeviceState, buffered GetDeviceData). Every function is
 * thread-safe and may be called before dinput_init(). */
#pragma once
#include <stdint.h>

/* ------------------------------------------------------------------ keyboard */
/* DirectInput scan codes (DIK_*), as in the DirectX 7 SDK's dinput.h */
#define INPUT_DIK_LIST(KEYDEF) \
    KEYDEF(ESCAPE, 0x01) KEYDEF(1, 0x02) KEYDEF(2, 0x03) KEYDEF(3, 0x04) KEYDEF(4, 0x05) \
    KEYDEF(5, 0x06) KEYDEF(6, 0x07) KEYDEF(7, 0x08) KEYDEF(8, 0x09) KEYDEF(9, 0x0A) \
    KEYDEF(0, 0x0B) KEYDEF(MINUS, 0x0C) KEYDEF(EQUALS, 0x0D) KEYDEF(BACK, 0x0E) \
    KEYDEF(TAB, 0x0F) KEYDEF(Q, 0x10) KEYDEF(W, 0x11) KEYDEF(E, 0x12) KEYDEF(R, 0x13) \
    KEYDEF(T, 0x14) KEYDEF(Y, 0x15) KEYDEF(U, 0x16) KEYDEF(I, 0x17) KEYDEF(O, 0x18) \
    KEYDEF(P, 0x19) KEYDEF(LBRACKET, 0x1A) KEYDEF(RBRACKET, 0x1B) KEYDEF(RETURN, 0x1C) \
    KEYDEF(LCONTROL, 0x1D) KEYDEF(A, 0x1E) KEYDEF(S, 0x1F) KEYDEF(D, 0x20) KEYDEF(F, 0x21) \
    KEYDEF(G, 0x22) KEYDEF(H, 0x23) KEYDEF(J, 0x24) KEYDEF(K, 0x25) KEYDEF(L, 0x26) \
    KEYDEF(SEMICOLON, 0x27) KEYDEF(APOSTROPHE, 0x28) KEYDEF(GRAVE, 0x29) KEYDEF(LSHIFT, 0x2A) \
    KEYDEF(BACKSLASH, 0x2B) KEYDEF(Z, 0x2C) KEYDEF(X, 0x2D) KEYDEF(C, 0x2E) KEYDEF(V, 0x2F) \
    KEYDEF(B, 0x30) KEYDEF(N, 0x31) KEYDEF(M, 0x32) KEYDEF(COMMA, 0x33) KEYDEF(PERIOD, 0x34) \
    KEYDEF(SLASH, 0x35) KEYDEF(RSHIFT, 0x36) KEYDEF(MULTIPLY, 0x37) KEYDEF(LMENU, 0x38) \
    KEYDEF(SPACE, 0x39) KEYDEF(CAPITAL, 0x3A) KEYDEF(F1, 0x3B) KEYDEF(F2, 0x3C) KEYDEF(F3, 0x3D) \
    KEYDEF(F4, 0x3E) KEYDEF(F5, 0x3F) KEYDEF(F6, 0x40) KEYDEF(F7, 0x41) KEYDEF(F8, 0x42) \
    KEYDEF(F9, 0x43) KEYDEF(F10, 0x44) KEYDEF(NUMLOCK, 0x45) KEYDEF(SCROLL, 0x46) \
    KEYDEF(NUMPAD7, 0x47) KEYDEF(NUMPAD8, 0x48) KEYDEF(NUMPAD9, 0x49) KEYDEF(SUBTRACT, 0x4A) \
    KEYDEF(NUMPAD4, 0x4B) KEYDEF(NUMPAD5, 0x4C) KEYDEF(NUMPAD6, 0x4D) KEYDEF(ADD, 0x4E) \
    KEYDEF(NUMPAD1, 0x4F) KEYDEF(NUMPAD2, 0x50) KEYDEF(NUMPAD3, 0x51) KEYDEF(NUMPAD0, 0x52) \
    KEYDEF(DECIMAL, 0x53) KEYDEF(OEM_102, 0x56) KEYDEF(F11, 0x57) KEYDEF(F12, 0x58) \
    KEYDEF(F13, 0x64) KEYDEF(F14, 0x65) KEYDEF(F15, 0x66) KEYDEF(KANA, 0x70) \
    KEYDEF(ABNT_C1, 0x73) KEYDEF(CONVERT, 0x79) KEYDEF(NOCONVERT, 0x7B) KEYDEF(YEN, 0x7D) \
    KEYDEF(ABNT_C2, 0x7E) KEYDEF(NUMPADEQUALS, 0x8D) KEYDEF(PREVTRACK, 0x90) KEYDEF(AT, 0x91) \
    KEYDEF(COLON, 0x92) KEYDEF(UNDERLINE, 0x93) KEYDEF(KANJI, 0x94) KEYDEF(STOP, 0x95) \
    KEYDEF(AX, 0x96) KEYDEF(UNLABELED, 0x97) KEYDEF(NEXTTRACK, 0x99) KEYDEF(NUMPADENTER, 0x9C) \
    KEYDEF(RCONTROL, 0x9D) KEYDEF(MUTE, 0xA0) KEYDEF(CALCULATOR, 0xA1) KEYDEF(PLAYPAUSE, 0xA2) \
    KEYDEF(MEDIASTOP, 0xA4) KEYDEF(VOLUMEDOWN, 0xAE) KEYDEF(VOLUMEUP, 0xB0) \
    KEYDEF(WEBHOME, 0xB2) KEYDEF(NUMPADCOMMA, 0xB3) KEYDEF(DIVIDE, 0xB5) KEYDEF(SYSRQ, 0xB7) \
    KEYDEF(RMENU, 0xB8) KEYDEF(PAUSE, 0xC5) KEYDEF(HOME, 0xC7) KEYDEF(UP, 0xC8) \
    KEYDEF(PRIOR, 0xC9) KEYDEF(LEFT, 0xCB) KEYDEF(RIGHT, 0xCD) KEYDEF(END, 0xCF) \
    KEYDEF(DOWN, 0xD0) KEYDEF(NEXT, 0xD1) KEYDEF(INSERT, 0xD2) KEYDEF(DELETE, 0xD3) \
    KEYDEF(LWIN, 0xDB) KEYDEF(RWIN, 0xDC) KEYDEF(APPS, 0xDD) KEYDEF(POWER, 0xDE) \
    KEYDEF(SLEEP, 0xDF) KEYDEF(WAKE, 0xE3) KEYDEF(WEBSEARCH, 0xE5) KEYDEF(WEBFAVORITES, 0xE6) \
    KEYDEF(WEBREFRESH, 0xE7) KEYDEF(WEBSTOP, 0xE8) KEYDEF(WEBFORWARD, 0xE9) \
    KEYDEF(WEBBACK, 0xEA) KEYDEF(MYCOMPUTER, 0xEB) KEYDEF(MAIL, 0xEC) KEYDEF(MEDIASELECT, 0xED)
#define INPUT_DIK_ENUM(name, code) DIK_##name = code,
enum { INPUT_DIK_LIST(INPUT_DIK_ENUM) };

void input_key(int dik, int down);          /* repeats while held are ignored, as in DirectInput */
/* Which device the player used last (button prompts follow it): a key or the mouse (0) or a
   controller (1); pad.c keeps it. */
void input_used(int controller);
int input_controller_last(void);
void input_focus(int focused);              /* losing focus releases every key and mouse button */
int input_has_focus(void);                  /* the window is in front (headless: always) */
int input_dik_from_hid(unsigned usage);     /* USB HID keyboard usage (= SDL3 scancode) -> DIK, 0 = none */
int input_dik_from_name(const char *name);  /* "RETURN", "DIK_RETURN", "enter", "0x1c" -> DIK, 0 = unknown */
const char *input_dik_name(int dik);        /* "RETURN"; NULL for codes without a name */

/* ------------------------------------------------------------------ mouse */
void input_mouse_move(int dx, int dy);           /* relative motion in pixels (mickeys) */
void input_mouse_button(int button, int down);   /* 0 left, 1 right, 2 middle, 3 X1, 4 X2 */
void input_mouse_wheel(int delta);               /* Windows units: 120 per notch, + = away from the user */
/* Nonzero while the game holds an exclusive-mode mouse acquired (Windows hides and confines the
   pointer then): the window layer should switch to relative mouse mode. */
int input_mouse_captured(void);

/* ------------------------------------------------------------------ joysticks */
/* Each slot is one DirectInput joystick with a fixed instance GUID, so a controller that comes
   back to the same slot is the same device to the game (its JOY1/JOY2 bindings stay put). Only
   connected slots are listed by EnumDevices. Axes present are the first naxes of this order: */
enum { INPUT_AXIS_X, INPUT_AXIS_Y, INPUT_AXIS_Z, INPUT_AXIS_RX, INPUT_AXIS_RY, INPUT_AXIS_RZ,
       INPUT_AXIS_SLIDER0, INPUT_AXIS_SLIDER1 };
#define INPUT_MAX_JOYS 8
#define INPUT_JOY_AXES 8
#define INPUT_JOY_BUTTONS 32
#define INPUT_JOY_POVS 4
#define INPUT_POV_CENTERED (-1)

/* 1NSANE reads the first 6 axes, 16 buttons and 2 POVs of each joystick (only the four main POV
   directions count), and builds ONE data format from the last joystick it enumerates and uses it
   for all of them: so connect every controller with the same layout, at least 6 axes, 16 buttons
   and 2 POVs (unused ones just stay at rest), or the others lose axes/buttons.
   Controllers come through pad.c (pad.h): left stick X/Y, LT on Z and RT on Rx as half axes
   (0 at rest .. 32767 pulled), right stick Ry/Rz, D-pad as POV 0, buttons in SDL order. */
int input_joy_connect(int slot, const char *name, int naxes, int nbuttons, int npovs);  /* 0 / -1 */
/* Same, choosing the slot: a controller with a stable id (SDL GUID + serial or path) gets the slot
   it had before if that is free, else the lowest unused one. Returns the slot or -1. */
int input_joy_attach(const char *id, const char *name, int naxes, int nbuttons, int npovs);
void input_joy_disconnect(int slot);
int input_joy_connected(int slot);
/* Controllers connected after the game looked for joysticks: the lowest connected slot the game
   has no device for (-1 if none), and a DIDEVICEINSTANCEA for it in program memory (gfree it),
   to hand to the game's own EnumDevices callback. A controller attaching takes, after its own
   earlier slot, a free slot the game already has a device for (so a swapped-in controller is
   the game's same joystick), then a never-used one. */
int dinput_joy_unclaimed(void);
int dinput_device_slot(uint32_t devobj);   /* a program's joystick device -> its slot, else -1 */
uint32_t dinput_joy_instance(int slot);
/* Report the device as force-feedback capable (DIDC_FORCEFEEDBACK): a controller with RUMBLE on
   (pad.c). Call right after connecting, before the game creates its devices: the game checks the
   caps once at start-up (devices it already has change too; ifc21.c tells the game). */
void input_joy_set_ff(int slot, int capable);
void input_joy_axis(int slot, int axis, int value);    /* -32768..32767, 0 = centre */
void input_joy_button(int slot, int button, int down);
void input_joy_pov(int slot, int pov, int hundredths);  /* -1 = centred, else 0..35999 clockwise from up */

/* ------------------------------------------------------------------ force feedback */
/* low/high: strength of the low- and high-frequency motors (0..65535, as SDL_RumbleGamepad);
   ms: how long to keep it up (0xffffffff = until changed); 0, 0 stops. */
typedef void (*InputRumbleFn)(int slot, uint16_t low, uint16_t high, uint32_t ms);
void input_set_rumble_handler(InputRumbleFn fn);
void input_joy_rumble(int slot, uint16_t low, uint16_t high, uint32_t ms);   /* for the IFC21 stand-in */
/* The joystick slot behind a game IDirectInputDevice(2)A pointer (CImmDXDevice::Initialize gets
   one), or -1; whether that device reports force feedback. */
int dinput_joy_slot(uint32_t device);
int dinput_dev_ff(uint32_t device);

/* The IFC21 stand-in (ifc21.c): the game's force feedback as rumble. */
int ifc_rumble_wanted(void);            /* RUMBLE on (and the Enhanced options: not a plain --test run) */
void ifc_frame(unsigned long frame);    /* per frame (input_frame): the motors follow what plays */
void ifc_poll(struct CPU *c);           /* from the input poll in sessions with a controller */
void ifc_exit(void);                    /* at exit: motors off */

/* ------------------------------------------------------------------ scripted input */
/* Called once per presented frame (and per input_wait tick, which the frame numbers count too).
   TD6_KEYS="frame:KEY,frame:KEY*hold,..." presses the named key (input_dik_from_name) at those
   frame numbers for 3 frames (or `hold` frames). */
void input_frame(unsigned long frame);
/* Optional: also told about every scripted press/release (e.g. to post WM_KEYDOWN/WM_KEYUP). */
extern void (*input_script_key_hook)(int dik, int down);
