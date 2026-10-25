/* DirectInput 7 stand-in: IDirectInputA (also answers as 2A/7A) and IDirectInputDeviceA/2A/7A
   over the host input state of input.h: system keyboard, system mouse and up to INPUT_MAX_JOYS
   joysticks. SetDataFormat takes whatever DIDATAFORMAT the game builds (1NSANE makes its own
   48-byte joystick layout from EnumObjects); every struct is read and written by its 32-bit
   Windows offsets. Force feedback: effect objects on devices flagged FF, played as rumble.
   Also TD6_KEYS, the scripted key presses for automated boot tests (TD6_PAD, a scripted
   controller, is in pad.c). */
#define _GNU_SOURCE
#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "pad.h"

/* ------------------------------------------------------------------ DirectInput constants */
#define DI_OK                        0x00000000u
#define DI_FALSE                     0x00000001u   /* S_FALSE: NOTATTACHED, BUFFEROVERFLOW, NOEFFECT */
#define DIERR_UNSUPPORTED            0x80004001u
#define DIERR_NOINTERFACE            0x80004002u
#define DIERR_NOAGGREGATION          0x80040110u
#define DIERR_DEVICENOTREG           0x80040154u
#define DIERR_MOREDATA               0x80040202u
#define DIERR_NOTEXCLUSIVEACQUIRED   0x80040205u
#define DIERR_NOTBUFFERED            0x80040207u
#define DIERR_UNPLUGGED              0x80040209u
#define DIERR_NOTFOUND               0x80070002u
#define DIERR_NOTACQUIRED            0x8007000cu
#define DIERR_INPUTLOST              0x8007001eu
#define DIERR_INVALIDPARAM           0x80070057u
#define DIERR_ACQUIRED               0x800700aau
#define DIERR_OLDDIRECTINPUTVERSION  0x8007047eu
#define DIERR_BETADIRECTINPUTVERSION 0x80070481u

#define DIDFT_RELAXIS         0x01u
#define DIDFT_ABSAXIS         0x02u
#define DIDFT_PSHBUTTON       0x04u
#define DIDFT_TGLBUTTON       0x08u
#define DIDFT_POV             0x10u
#define DIDFT_ANYINSTANCE     0x00ffff00u
#define DIDFT_COLLECTION      0x40u
#define DIDFT_NODATA          0x80u
#define DIDFT_FFACTUATOR      0x01000000u
#define DIDFT_FFEFFECTTRIGGER 0x02000000u
#define DIDFT_ATTRS           0x1f000000u    /* FFACTUATOR FFEFFECTTRIGGER VENDORDEFINED ALIAS OUTPUT */
#define DIDFT_OPTIONAL        0x80000000u
#define INSTANCE(n)           ((uint32_t)(n) << 8)
#define GETINSTANCE(t)        (((t) >> 8) & 0xffffu)
#define DIDOI_FFACTUATOR      0x01u
#define DIDOI_FFEFFECTTRIGGER 0x02u
#define DIDOI_ASPECTPOSITION  0x100u
#define DIDF_ABSAXIS          1u
#define DIDF_RELAXIS          2u
#define DISCL_EXCLUSIVE       1u
#define DISCL_NONEXCLUSIVE    2u
#define DISCL_FOREGROUND      4u
#define DISCL_BACKGROUND      8u
#define DISCL_NOWINKEY        0x10u
#define DIDC_ATTACHED         1u
#define DIDC_FORCEFEEDBACK    0x100u
#define DIDEVTYPE_MOUSE       2u
#define DIDEVTYPE_KEYBOARD    3u
#define DIDEVTYPE_JOYSTICK    4u
#define DIDEVTYPE_HID         0x10000u
#define DIEDFL_ATTACHEDONLY   1u
#define DIEDFL_FORCEFEEDBACK  0x100u
#define DIGDD_PEEK            1u
#define DIPH_DEVICE           0u
#define DIPH_BYOFFSET         1u
#define DIPH_BYID             2u
#define DIPH_BYUSAGE          3u
enum { DIPROP_BUFFERSIZE = 1, DIPROP_AXISMODE, DIPROP_GRANULARITY, DIPROP_RANGE, DIPROP_DEADZONE,
       DIPROP_SATURATION, DIPROP_FFGAIN, DIPROP_FFLOAD, DIPROP_AUTOCENTER, DIPROP_CALIBRATIONMODE,
       DIPROP_CALIBRATION, DIPROP_GUIDANDPATH, DIPROP_INSTANCENAME, DIPROP_PRODUCTNAME,
       DIPROP_JOYSTICKID, DIPROP_LAST = 26 };
#define DIEP_DURATION          0x001u
#define DIEP_SAMPLEPERIOD      0x002u
#define DIEP_GAIN              0x004u
#define DIEP_TRIGGERBUTTON     0x008u
#define DIEP_TRIGGERREPEATINTERVAL 0x010u
#define DIEP_AXES              0x020u
#define DIEP_DIRECTION         0x040u
#define DIEP_ENVELOPE          0x080u
#define DIEP_TYPESPECIFICPARAMS 0x100u
#define DIEP_STARTDELAY        0x200u
#define DIEP_ALLPARAMS         0x3ffu
#define DIEP_START             0x20000000u
#define DIES_SOLO              1u
#define DIEGES_PLAYING         1u
#define DISFFC_RESET           0x01u
#define DISFFC_STOPALL         0x02u
#define DISFFC_PAUSE           0x04u
#define DISFFC_CONTINUE        0x08u
#define DISFFC_SETACTUATORSON  0x10u
#define DISFFC_SETACTUATORSOFF 0x20u
#define DIGFFS_EMPTY           0x01u
#define DIGFFS_STOPPED         0x02u
#define DIGFFS_PAUSED          0x04u
#define DIGFFS_ACTUATORSON     0x10u
#define DIGFFS_ACTUATORSOFF    0x20u
#define DIGFFS_POWERON         0x40u
#define DIEFT_CONSTANTFORCE    1u
#define DIEFT_RAMPFORCE        2u
#define DIEFT_PERIODIC         3u
#define INFINITE               0xffffffffu

/* struct sizes (32-bit layouts) */
#define SZ_DEVINST_DX3  0x234u     /* DIDEVICEINSTANCEA */
#define SZ_DEVINST      0x244u
#define SZ_OBJINST_DX3  0x124u     /* DIDEVICEOBJECTINSTANCEA */
#define SZ_OBJINST      0x13cu
#define SZ_DEVCAPS_DX3  0x18u      /* DIDEVCAPS */
#define SZ_DEVCAPS      0x2cu
#define SZ_EFFINFO      0x124u     /* DIEFFECTINFOA */
#define SZ_EFFECT_DX5   0x34u      /* DIEFFECT */
#define SZ_EFFECT       0x38u
#define SZ_PROPDWORD    0x14u
#define SZ_PROPRANGE    0x18u
#define SZ_PROPSTRING   0x218u     /* header + WCHAR[260] */

/* ------------------------------------------------------------------ logging */
static int verbose;      /* TD6_LOG_INPUT_VERBOSE: per-frame polling calls too */
#define LOGA(...) logmsg(LOG_ALWAYS, __VA_ARGS__)
#define LOGV(...) do { if (verbose && LOGON(LOG_INPUT)) logmsg(LOG_INPUT, __VA_ARGS__); } while (0)

static const char *hr_str(uint32_t r)
{
    switch (r) {
    case DI_OK: return "DI_OK";
    case DI_FALSE: return "S_FALSE";
    case DIERR_UNSUPPORTED: return "DIERR_UNSUPPORTED";
    case DIERR_NOINTERFACE: return "DIERR_NOINTERFACE";
    case DIERR_NOAGGREGATION: return "DIERR_NOAGGREGATION";
    case DIERR_DEVICENOTREG: return "DIERR_DEVICENOTREG";
    case DIERR_MOREDATA: return "DIERR_MOREDATA";
    case DIERR_NOTEXCLUSIVEACQUIRED: return "DIERR_NOTEXCLUSIVEACQUIRED";
    case DIERR_NOTBUFFERED: return "DIERR_NOTBUFFERED";
    case DIERR_UNPLUGGED: return "DIERR_UNPLUGGED";
    case DIERR_NOTFOUND: return "DIERR_NOTFOUND";
    case DIERR_NOTACQUIRED: return "DIERR_NOTACQUIRED";
    case DIERR_INPUTLOST: return "DIERR_INPUTLOST";
    case DIERR_INVALIDPARAM: return "DIERR_INVALIDPARAM";
    case DIERR_ACQUIRED: return "DIERR_ACQUIRED";
    case DIERR_OLDDIRECTINPUTVERSION: return "DIERR_OLDDIRECTINPUTVERSION";
    case DIERR_BETADIRECTINPUTVERSION: return "DIERR_BETADIRECTINPUTVERSION";
    }
    return "error";
}

static const char *const prop_names[DIPROP_LAST + 1] = {
    NULL, "BUFFERSIZE", "AXISMODE", "GRANULARITY", "RANGE", "DEADZONE", "SATURATION", "FFGAIN",
    "FFLOAD", "AUTOCENTER", "CALIBRATIONMODE", "CALIBRATION", "GUIDANDPATH", "INSTANCENAME",
    "PRODUCTNAME", "JOYSTICKID", "GETPORTDISPLAYNAME", "17", "PHYSICALRANGE", "LOGICALRANGE",
    "KEYNAME", "CPOINTS", "APPDATA", "SCANCODE", "VIDPID", "USERNAME", "TYPENAME"};

static const char *type_str(uint32_t t, char *buf, size_t n)
{
    static const char *const base[] = {"ALL", "RELAXIS", "ABSAXIS", "AXIS", "PSHBUTTON", "?5", "?6",
                                       "?7", "TGLBUTTON", "?9", "?10", "?11", "BUTTON"};
    uint32_t b = t & 0xff;
    const char *bs = b < 13 ? base[b] : b == DIDFT_POV ? "POV" : b == DIDFT_COLLECTION ? "COLLECTION"
                   : b == DIDFT_NODATA ? "NODATA" : "?";
    int k = snprintf(buf, n, "%s", bs);
    if (GETINSTANCE(t) == 0xffff)
        k += snprintf(buf + k, n - k, "|any");
    else
        k += snprintf(buf + k, n - k, "#%u", GETINSTANCE(t));
    if (t & DIDFT_FFACTUATOR) k += snprintf(buf + k, n - k, "|FFACTUATOR");
    if (t & DIDFT_FFEFFECTTRIGGER) k += snprintf(buf + k, n - k, "|FFEFFECTTRIGGER");
    if (t & DIDFT_OPTIONAL) snprintf(buf + k, n - k, "|OPTIONAL");
    return buf;
}

static const char *coop_str(uint32_t f, char *buf, size_t n)
{
    snprintf(buf, n, "%s%s%s%s%s", f & DISCL_EXCLUSIVE ? "EXCLUSIVE " : "",
             f & DISCL_NONEXCLUSIVE ? "NONEXCLUSIVE " : "", f & DISCL_FOREGROUND ? "FOREGROUND " : "",
             f & DISCL_BACKGROUND ? "BACKGROUND " : "", f & DISCL_NOWINKEY ? "NOWINKEY " : "");
    size_t k = strlen(buf);
    if (k)
        buf[k - 1] = 0;
    return buf;
}

/* ------------------------------------------------------------------ GUIDs */
typedef struct { const char *name; uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } GuidDef;
enum { G_XAXIS, G_YAXIS, G_ZAXIS, G_RXAXIS, G_RYAXIS, G_RZAXIS, G_SLIDER, G_BUTTON, G_KEY, G_POV,
       G_UNKNOWN, G_SYSMOUSE, G_SYSKEYBOARD, G_JOYSTICK, G_IUNKNOWN, G_IDI_A, G_IDI_W, G_IDI2_A,
       G_IDI2_W, G_IDI7_A, G_IDI7_W, G_IDI8_A, G_IDEV_A, G_IDEV_W, G_IDEV2_A, G_IDEV2_W, G_IDEV7_A,
       G_IDEV7_W, G_IDEV8_A, G_IEFFECT, G_CONSTANT, G_RAMP, G_SQUARE, G_SINE, G_TRIANGLE, G_SAWUP,
       G_SAWDOWN, G_SPRING, G_DAMPER, G_INERTIA, G_FRICTION, G_CUSTOM, G_COUNT };
#define DI_D4 {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}
#define FF_D4 {0x9a, 0xd0, 0x00, 0xa0, 0xc9, 0xa0, 0x6e, 0x35}
#define DX7_D4 {0x8e, 0x9d, 0x00, 0xc0, 0x4f, 0x68, 0x44, 0xae}
static const GuidDef guids[G_COUNT] = {
    [G_XAXIS] = {"GUID_XAxis", 0xa36d02e0, 0xc9f3, 0x11cf, DI_D4},
    [G_YAXIS] = {"GUID_YAxis", 0xa36d02e1, 0xc9f3, 0x11cf, DI_D4},
    [G_ZAXIS] = {"GUID_ZAxis", 0xa36d02e2, 0xc9f3, 0x11cf, DI_D4},
    [G_RXAXIS] = {"GUID_RxAxis", 0xa36d02f4, 0xc9f3, 0x11cf, DI_D4},
    [G_RYAXIS] = {"GUID_RyAxis", 0xa36d02f5, 0xc9f3, 0x11cf, DI_D4},
    [G_RZAXIS] = {"GUID_RzAxis", 0xa36d02e3, 0xc9f3, 0x11cf, DI_D4},
    [G_SLIDER] = {"GUID_Slider", 0xa36d02e4, 0xc9f3, 0x11cf, DI_D4},
    [G_BUTTON] = {"GUID_Button", 0xa36d02f0, 0xc9f3, 0x11cf, DI_D4},
    [G_KEY] = {"GUID_Key", 0x55728220, 0xd33c, 0x11cf, DI_D4},
    [G_POV] = {"GUID_POV", 0xa36d02f2, 0xc9f3, 0x11cf, DI_D4},
    [G_UNKNOWN] = {"GUID_Unknown", 0xa36d02f3, 0xc9f3, 0x11cf, DI_D4},
    [G_SYSMOUSE] = {"GUID_SysMouse", 0x6f1d2b60, 0xd5a0, 0x11cf, DI_D4},
    [G_SYSKEYBOARD] = {"GUID_SysKeyboard", 0x6f1d2b61, 0xd5a0, 0x11cf, DI_D4},
    [G_JOYSTICK] = {"GUID_Joystick", 0x6f1d2b70, 0xd5a0, 0x11cf, DI_D4},
    [G_IUNKNOWN] = {"IID_IUnknown", 0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}},
    [G_IDI_A] = {"IID_IDirectInputA", 0x89521360, 0xaa8a, 0x11cf, DI_D4},
    [G_IDI_W] = {"IID_IDirectInputW", 0x89521361, 0xaa8a, 0x11cf, DI_D4},
    [G_IDI2_A] = {"IID_IDirectInput2A", 0x5944e662, 0xaa8a, 0x11cf, DI_D4},
    [G_IDI2_W] = {"IID_IDirectInput2W", 0x5944e663, 0xaa8a, 0x11cf, DI_D4},
    [G_IDI7_A] = {"IID_IDirectInput7A", 0x9a4cb684, 0x236d, 0x11d3, DX7_D4},
    [G_IDI7_W] = {"IID_IDirectInput7W", 0x9a4cb685, 0x236d, 0x11d3, DX7_D4},
    [G_IDI8_A] = {"IID_IDirectInput8A", 0xbf798030, 0x483a, 0x4da2, {0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00}},
    [G_IDEV_A] = {"IID_IDirectInputDeviceA", 0x5944e680, 0xc92e, 0x11cf, DI_D4},
    [G_IDEV_W] = {"IID_IDirectInputDeviceW", 0x5944e681, 0xc92e, 0x11cf, DI_D4},
    [G_IDEV2_A] = {"IID_IDirectInputDevice2A", 0x5944e682, 0xc92e, 0x11cf, DI_D4},
    [G_IDEV2_W] = {"IID_IDirectInputDevice2W", 0x5944e683, 0xc92e, 0x11cf, DI_D4},
    [G_IDEV7_A] = {"IID_IDirectInputDevice7A", 0x57d7c6bc, 0x2356, 0x11d3, DX7_D4},
    [G_IDEV7_W] = {"IID_IDirectInputDevice7W", 0x57d7c6bd, 0x2356, 0x11d3, DX7_D4},
    [G_IDEV8_A] = {"IID_IDirectInputDevice8A", 0x54d41080, 0xdc15, 0x4833, {0xa4, 0x1b, 0x74, 0x8f, 0x73, 0xa3, 0x81, 0x79}},
    [G_IEFFECT] = {"IID_IDirectInputEffect", 0xe7e1f7c0, 0x88d2, 0x11d0, FF_D4},
    [G_CONSTANT] = {"GUID_ConstantForce", 0x13541c20, 0x8e33, 0x11d0, FF_D4},
    [G_RAMP] = {"GUID_RampForce", 0x13541c21, 0x8e33, 0x11d0, FF_D4},
    [G_SQUARE] = {"GUID_Square", 0x13541c22, 0x8e33, 0x11d0, FF_D4},
    [G_SINE] = {"GUID_Sine", 0x13541c23, 0x8e33, 0x11d0, FF_D4},
    [G_TRIANGLE] = {"GUID_Triangle", 0x13541c24, 0x8e33, 0x11d0, FF_D4},
    [G_SAWUP] = {"GUID_SawtoothUp", 0x13541c25, 0x8e33, 0x11d0, FF_D4},
    [G_SAWDOWN] = {"GUID_SawtoothDown", 0x13541c26, 0x8e33, 0x11d0, FF_D4},
    [G_SPRING] = {"GUID_Spring", 0x13541c27, 0x8e33, 0x11d0, FF_D4},
    [G_DAMPER] = {"GUID_Damper", 0x13541c28, 0x8e33, 0x11d0, FF_D4},
    [G_INERTIA] = {"GUID_Inertia", 0x13541c29, 0x8e33, 0x11d0, FF_D4},
    [G_FRICTION] = {"GUID_Friction", 0x13541c2a, 0x8e33, 0x11d0, FF_D4},
    [G_CUSTOM] = {"GUID_CustomForce", 0x13541c2b, 0x8e33, 0x11d0, FF_D4},
};

static int guid_id(uint32_t a)
{
    if (!a)
        return -1;
    for (int g = 0; g < G_COUNT; g++)
        if (guid_eq(a, guids[g].d1, guids[g].d2, guids[g].d3, guids[g].d4))
            return g;
    return -1;
}

static void guid_write(uint32_t p, int g)
{
    W32(p, guids[g].d1);
    W16(p + 4, guids[g].d2);
    W16(p + 6, guids[g].d3);
    memcpy(gptr(p + 8), guids[g].d4, 8);
}

/* Joystick slots get fixed instance and product GUIDs of our own. */
static const uint8_t joy_d4[2][8] = {{'1', 'N', 'S', 'A', 'N', 'E', 'J', 'I'},
                                     {'1', 'N', 'S', 'A', 'N', 'E', 'J', 'P'}};
#define JOY_GUID_D1 0x1a5e0000u

static void joy_guid_write(uint32_t p, int slot, int product)
{
    W32(p, JOY_GUID_D1 + (uint32_t)slot);
    W16(p + 4, 1);
    W16(p + 6, 1);
    memcpy(gptr(p + 8), joy_d4[product], 8);
}

static int joy_guid_slot(uint32_t a, int product)
{
    if (!a)
        return -1;
    uint32_t s = R32(a) - JOY_GUID_D1;
    if (s >= INPUT_MAX_JOYS || !guid_eq(a, R32(a), 1, 1, joy_d4[product]))
        return -1;
    return (int)s;
}

static const char *guid_str(uint32_t a, char *buf, size_t n)
{
    if (!a)
        return "NULL";
    int g = guid_id(a);
    if (g >= 0)
        return guids[g].name;
    int s = joy_guid_slot(a, 0);
    if (s >= 0) {
        snprintf(buf, n, "joystick#%d", s);
        return buf;
    }
    if ((s = joy_guid_slot(a, 1)) >= 0) {
        snprintf(buf, n, "joystick#%d product", s);
        return buf;
    }
    snprintf(buf, n, "{" GUID_FMT "}", GUID_ARGS(a));
    return buf;
}

/* ------------------------------------------------------------------ key names */
#define DIK_NAME(name, code) [code] = #name,
static const char *const dik_names[256] = { INPUT_DIK_LIST(DIK_NAME) };

static const struct { const char *name; uint8_t dik; } dik_aliases[] = {
    {"ENTER", DIK_RETURN}, {"ESC", DIK_ESCAPE}, {"BACKSPACE", DIK_BACK}, {"NUMPADSTAR", DIK_MULTIPLY},
    {"LALT", DIK_LMENU}, {"RALT", DIK_RMENU}, {"ALT", DIK_LMENU}, {"CAPSLOCK", DIK_CAPITAL},
    {"NUMPADMINUS", DIK_SUBTRACT}, {"NUMPADPLUS", DIK_ADD}, {"NUMPADPERIOD", DIK_DECIMAL},
    {"NUMPADSLASH", DIK_DIVIDE}, {"UPARROW", DIK_UP}, {"DOWNARROW", DIK_DOWN}, {"LEFTARROW", DIK_LEFT},
    {"RIGHTARROW", DIK_RIGHT}, {"PGUP", DIK_PRIOR}, {"PGDN", DIK_NEXT}, {"PAGEUP", DIK_PRIOR},
    {"PAGEDOWN", DIK_NEXT}, {"CIRCUMFLEX", DIK_PREVTRACK}, {"LCTRL", DIK_LCONTROL},
    {"RCTRL", DIK_RCONTROL}, {"CTRL", DIK_LCONTROL}, {"SHIFT", DIK_LSHIFT}, {"DEL", DIK_DELETE},
    {"INS", DIK_INSERT}, {"PRINTSCREEN", DIK_SYSRQ},
};

const char *input_dik_name(int dik)
{
    return dik > 0 && dik < 256 ? dik_names[dik] : NULL;
}

int input_dik_from_name(const char *name)
{
    if (!name)
        return 0;
    while (isspace((unsigned char)*name))
        name++;
    if (!strncasecmp(name, "DIK_", 4))
        name += 4;
    if (!*name)
        return 0;
    if (isdigit((unsigned char)name[0]) && name[1]) {    /* "0x1c" or "28"; "1" is the key */
        char *end;
        long v = strtol(name, &end, 0);
        return *end == 0 && v > 0 && v < 256 ? (int)v : 0;
    }
    for (int k = 1; k < 256; k++)
        if (dik_names[k] && !strcasecmp(dik_names[k], name))
            return k;
    for (size_t k = 0; k < sizeof dik_aliases / sizeof *dik_aliases; k++)
        if (!strcasecmp(dik_aliases[k].name, name))
            return dik_aliases[k].dik;
    return 0;
}

/* USB HID keyboard usages (SDL3's scancode numbers) to DIK, by physical position. */
static const uint8_t hid_dik[232] = {
    [4] = DIK_A, [5] = DIK_B, [6] = DIK_C, [7] = DIK_D, [8] = DIK_E, [9] = DIK_F, [10] = DIK_G,
    [11] = DIK_H, [12] = DIK_I, [13] = DIK_J, [14] = DIK_K, [15] = DIK_L, [16] = DIK_M, [17] = DIK_N,
    [18] = DIK_O, [19] = DIK_P, [20] = DIK_Q, [21] = DIK_R, [22] = DIK_S, [23] = DIK_T, [24] = DIK_U,
    [25] = DIK_V, [26] = DIK_W, [27] = DIK_X, [28] = DIK_Y, [29] = DIK_Z,
    [30] = DIK_1, [31] = DIK_2, [32] = DIK_3, [33] = DIK_4, [34] = DIK_5, [35] = DIK_6, [36] = DIK_7,
    [37] = DIK_8, [38] = DIK_9, [39] = DIK_0,
    [40] = DIK_RETURN, [41] = DIK_ESCAPE, [42] = DIK_BACK, [43] = DIK_TAB, [44] = DIK_SPACE,
    [45] = DIK_MINUS, [46] = DIK_EQUALS, [47] = DIK_LBRACKET, [48] = DIK_RBRACKET,
    [49] = DIK_BACKSLASH, [50] = DIK_BACKSLASH, [51] = DIK_SEMICOLON, [52] = DIK_APOSTROPHE,
    [53] = DIK_GRAVE, [54] = DIK_COMMA, [55] = DIK_PERIOD, [56] = DIK_SLASH, [57] = DIK_CAPITAL,
    [58] = DIK_F1, [59] = DIK_F2, [60] = DIK_F3, [61] = DIK_F4, [62] = DIK_F5, [63] = DIK_F6,
    [64] = DIK_F7, [65] = DIK_F8, [66] = DIK_F9, [67] = DIK_F10, [68] = DIK_F11, [69] = DIK_F12,
    [70] = DIK_SYSRQ, [71] = DIK_SCROLL, [72] = DIK_PAUSE, [73] = DIK_INSERT, [74] = DIK_HOME,
    [75] = DIK_PRIOR, [76] = DIK_DELETE, [77] = DIK_END, [78] = DIK_NEXT, [79] = DIK_RIGHT,
    [80] = DIK_LEFT, [81] = DIK_DOWN, [82] = DIK_UP, [83] = DIK_NUMLOCK, [84] = DIK_DIVIDE,
    [85] = DIK_MULTIPLY, [86] = DIK_SUBTRACT, [87] = DIK_ADD, [88] = DIK_NUMPADENTER,
    [89] = DIK_NUMPAD1, [90] = DIK_NUMPAD2, [91] = DIK_NUMPAD3, [92] = DIK_NUMPAD4, [93] = DIK_NUMPAD5,
    [94] = DIK_NUMPAD6, [95] = DIK_NUMPAD7, [96] = DIK_NUMPAD8, [97] = DIK_NUMPAD9, [98] = DIK_NUMPAD0,
    [99] = DIK_DECIMAL, [100] = DIK_OEM_102, [101] = DIK_APPS, [102] = DIK_POWER,
    [103] = DIK_NUMPADEQUALS, [104] = DIK_F13, [105] = DIK_F14, [106] = DIK_F15,
    [127] = DIK_MUTE, [128] = DIK_VOLUMEUP, [129] = DIK_VOLUMEDOWN, [133] = DIK_NUMPADCOMMA,
    [135] = DIK_ABNT_C1, [136] = DIK_KANA, [137] = DIK_YEN, [138] = DIK_CONVERT, [139] = DIK_NOCONVERT,
    [224] = DIK_LCONTROL, [225] = DIK_LSHIFT, [226] = DIK_LMENU, [227] = DIK_LWIN,
    [228] = DIK_RCONTROL, [229] = DIK_RSHIFT, [230] = DIK_RMENU, [231] = DIK_RWIN,
};

int input_dik_from_hid(unsigned usage)
{
    return usage < sizeof hid_dik ? hid_dik[usage] : 0;
}

/* ------------------------------------------------------------------ devices and objects */
enum { DEV_KEYBOARD, DEV_MOUSE, DEV_JOY };
enum { OBJ_AXIS, OBJ_BUTTON, OBJ_POV, OBJ_KEY };
#define MOUSE_BUTTONS 5
#define MOUSE_OBJS (3 + MOUSE_BUTTONS)
#define KBD_OBJS 255

typedef struct {
    uint8_t kind, index, guid;      /* index: axis id, button/POV number, or DIK */
    uint32_t type;                  /* DIDFT_* | INSTANCE(n) */
    uint32_t flags;                 /* DIDOI_* */
    uint32_t native_ofs;            /* offset in the standard format (c_dfDIJoystick etc.) */
    uint16_t usage_page, usage;
    char name[24];
} DevObj;

typedef struct { int32_t min, max; uint32_t dead, sat; } AxisProp;
typedef struct { uint32_t ofs, data, time, seq; } InEvent;
typedef struct Effect Effect;

typedef struct Device {
    struct Device *next;
    uint32_t obj, version;
    int kind, slot, refs, alive;
    char label[12];
    int acquired, lost;
    uint32_t coop, hwnd, event;
    const DevObj *objs;
    int nobjs, naxes, npovs, nbuttons, first_pov, first_button;
    AxisProp *ap;                   /* per object (axes only) */
    int32_t *fmt_ofs;               /* per object: offset in the data format, -1 = not in it */
    uint32_t *last;                 /* per object: value at the last buffered event */
    uint32_t *rel_last;             /* per object: value at the last GetDeviceState (relative axes) */
    int has_format, rel_axes, npov_fill;
    uint32_t data_size, pov_fill[8];
    uint32_t bufsize, head, count;
    int overflow;
    InEvent *buf;
    int ff, ff_off, ff_paused;
    uint32_t ffgain, autocenter;
    Effect *effects;
} Device;

typedef struct {
    int connected, ff;
    char name[64], id[128];
    int naxes, nbuttons, npovs;
    int axis[INPUT_JOY_AXES];
    uint8_t button[INPUT_JOY_BUTTONS];
    int pov[INPUT_JOY_POVS];
} JoySlot;

static pthread_mutex_t in_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t keys[256];
static uint32_t mouse_pos[3];
static uint8_t mouse_btn[MOUSE_BUTTONS];
static JoySlot joys[INPUT_MAX_JOYS];
static Device *devices;
static uint32_t seq_counter;
static InputRumbleFn rumble_fn;
void (*input_script_key_hook)(int dik, int down);

static DevObj kbd_objs[KBD_OBJS], mouse_objs[MOUSE_OBJS];
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

static uint8_t dik_hid(int dik)
{
    for (unsigned u = 0; u < sizeof hid_dik; u++)
        if (hid_dik[u] == dik)
            return (uint8_t)u;
    return 0;
}

static void tables_init(void)
{
    for (int k = 1; k <= KBD_OBJS; k++) {
        DevObj *o = &kbd_objs[k - 1];
        o->kind = OBJ_KEY;
        o->index = (uint8_t)k;
        o->guid = G_KEY;
        o->type = DIDFT_PSHBUTTON | INSTANCE(k);
        o->native_ofs = (uint32_t)k;
        o->usage_page = 7;
        o->usage = dik_hid(k);
        if (dik_names[k])
            snprintf(o->name, sizeof o->name, "%s", dik_names[k]);
        else
            snprintf(o->name, sizeof o->name, "Key 0x%02X", k);
    }
    static const char *const mnames[3] = {"X-axis", "Y-axis", "Wheel"};
    for (int a = 0; a < 3; a++) {
        DevObj *o = &mouse_objs[a];
        o->kind = OBJ_AXIS;
        o->index = (uint8_t)a;
        o->guid = (uint8_t)(G_XAXIS + a);
        o->type = DIDFT_RELAXIS | INSTANCE(a);
        o->native_ofs = 4u * a;
        o->usage_page = 1;
        o->usage = a == 2 ? 0x38 : 0x30 + a;
        snprintf(o->name, sizeof o->name, "%s", mnames[a]);
    }
    for (int b = 0; b < MOUSE_BUTTONS; b++) {
        DevObj *o = &mouse_objs[3 + b];
        o->kind = OBJ_BUTTON;
        o->index = (uint8_t)b;
        o->guid = G_BUTTON;
        o->type = DIDFT_PSHBUTTON | INSTANCE(b);
        o->native_ofs = 12u + b;
        o->usage_page = 9;
        o->usage = (uint16_t)(b + 1);
        snprintf(o->name, sizeof o->name, "Button %d", b);
    }
}

/* axes, then POVs, then buttons; FF devices have actuators on X/Y and FF trigger buttons */
static DevObj *joy_objs(const JoySlot *j, int ff, int *count)
{
    static const uint8_t ag[INPUT_JOY_AXES] = {G_XAXIS, G_YAXIS, G_ZAXIS, G_RXAXIS, G_RYAXIS,
                                               G_RZAXIS, G_SLIDER, G_SLIDER};
    static const char *const an[INPUT_JOY_AXES] = {"X Axis", "Y Axis", "Z Axis", "X Rotation",
                                                   "Y Rotation", "Z Rotation", "Slider", "Slider"};
    int n = j->naxes + j->npovs + j->nbuttons;
    DevObj *objs = calloc(n ? n : 1, sizeof *objs), *o = objs;
    for (int a = 0; a < j->naxes; a++, o++) {
        int act = ff && a < 2;
        o->kind = OBJ_AXIS;
        o->index = (uint8_t)a;
        o->guid = ag[a];
        o->type = DIDFT_ABSAXIS | INSTANCE(a) | (act ? DIDFT_FFACTUATOR : 0);
        o->flags = DIDOI_ASPECTPOSITION | (act ? DIDOI_FFACTUATOR : 0);
        o->native_ofs = 4u * a;
        o->usage_page = 1;
        o->usage = (uint16_t)(a < 6 ? 0x30 + a : 0x36);
        snprintf(o->name, sizeof o->name, "%s", an[a]);
    }
    for (int p = 0; p < j->npovs; p++, o++) {
        o->kind = OBJ_POV;
        o->index = (uint8_t)p;
        o->guid = G_POV;
        o->type = DIDFT_POV | INSTANCE(p);
        o->native_ofs = 32u + 4u * p;
        o->usage_page = 1;
        o->usage = 0x39;
        snprintf(o->name, sizeof o->name, "Hat Switch %d", p);
    }
    for (int b = 0; b < j->nbuttons; b++, o++) {
        o->kind = OBJ_BUTTON;
        o->index = (uint8_t)b;
        o->guid = G_BUTTON;
        o->type = DIDFT_PSHBUTTON | INSTANCE(b) | (ff ? DIDFT_FFEFFECTTRIGGER : 0);
        o->flags = ff ? DIDOI_FFEFFECTTRIGGER : 0;
        o->native_ofs = 48u + b;
        o->usage_page = 9;
        o->usage = (uint16_t)(b + 1);
        snprintf(o->name, sizeof o->name, "Button %d", b);
    }
    *count = n;
    return objs;
}

static Device *dev_new(int kind, int slot, uint32_t version)    /* under in_lock */
{
    Device *d = calloc(1, sizeof *d);
    d->kind = kind;
    d->slot = slot;
    d->version = version;
    d->refs = 1;
    d->alive = 1;
    d->coop = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    d->ffgain = 10000;
    d->autocenter = 1;
    if (kind == DEV_KEYBOARD) {
        d->objs = kbd_objs;
        d->nobjs = d->nbuttons = KBD_OBJS;
        snprintf(d->label, sizeof d->label, "keyboard");
    } else if (kind == DEV_MOUSE) {
        d->objs = mouse_objs;
        d->nobjs = MOUSE_OBJS;
        d->naxes = d->first_pov = d->first_button = 3;
        d->nbuttons = MOUSE_BUTTONS;
        d->rel_axes = 1;
        snprintf(d->label, sizeof d->label, "mouse");
    } else {
        JoySlot *j = &joys[slot];
        d->ff = j->ff;
        d->objs = joy_objs(j, d->ff, &d->nobjs);
        d->naxes = j->naxes;
        d->npovs = j->npovs;
        d->nbuttons = j->nbuttons;
        d->first_pov = j->naxes;
        d->first_button = j->naxes + j->npovs;
        snprintf(d->label, sizeof d->label, "joy%d", slot);
    }
    int n = d->nobjs;
    d->ap = calloc(n, sizeof *d->ap);
    d->fmt_ofs = malloc(n * sizeof *d->fmt_ofs);
    d->last = calloc(n, sizeof *d->last);
    d->rel_last = calloc(n, sizeof *d->rel_last);
    for (int i = 0; i < n; i++) {
        d->ap[i] = (AxisProp){0, 65535, 0, 10000};
        d->fmt_ofs[i] = -1;
    }
    d->next = devices;
    devices = d;
    return d;
}

static Device *dev_of(uint32_t obj)
{
    return com_host(obj);
}

/* ------------------------------------------------------------------ values */
static uint32_t timestamp(void)
{
    return (uint32_t)now_ms();
}

/* host axis (-32768..32767) -> device range, after dead zone and saturation (0..10000 of the
   distance from the centre) */
static int32_t scale_axis(const AxisProp *p, int raw)
{
    double f = raw < 0 ? raw / 32768.0 : raw / 32767.0;
    if (f < -1)
        f = -1;
    double a = (f < 0 ? -f : f) * 10000.0;
    if (a <= p->dead)
        f = 0;
    else if (a >= p->sat)
        f = f < 0 ? -1 : 1;
    else
        f = (f < 0 ? -1 : 1) * (a - p->dead) / ((double)p->sat - p->dead);
    double span = (double)p->max - p->min;
    return (int32_t)(p->min + (int64_t)((f + 1) * 0.5 * span));
}

/* absolute value of an object: key/button 0x80 or 0, axis position, POV angle or -1 */
static uint32_t obj_abs(const Device *d, int i)
{
    const DevObj *o = &d->objs[i];
    if (d->kind == DEV_KEYBOARD)
        return keys[o->index] ? 0x80 : 0;
    if (d->kind == DEV_MOUSE)
        return o->kind == OBJ_BUTTON ? (mouse_btn[o->index] ? 0x80u : 0) : mouse_pos[o->index];
    const JoySlot *j = &joys[d->slot];
    if (o->kind == OBJ_BUTTON)
        return j->connected && j->button[o->index] ? 0x80 : 0;
    if (o->kind == OBJ_POV)
        return j->connected ? (uint32_t)j->pov[o->index] : 0xffffffffu;
    return (uint32_t)scale_axis(&d->ap[i], j->connected ? j->axis[o->index] : 0);
}

/* as GetDeviceState reports it: relative axes give the motion since the last read */
static uint32_t obj_read(Device *d, int i)
{
    uint32_t v = obj_abs(d, i);
    if (!d->rel_axes || d->objs[i].kind != OBJ_AXIS)
        return v;
    uint32_t r = v - d->rel_last[i];
    d->rel_last[i] = v;
    return r;
}

/* current values become the reference for relative reads and buffered changes */
static void dev_baseline(Device *d)
{
    for (int i = 0; i < d->nobjs; i++)
        d->rel_last[i] = d->last[i] = obj_abs(d, i);
}

extern void event_signal(uint32_t handle) __attribute__((weak));

/* a change of object i: buffered event if the device keeps a buffer and the format has it */
static void dev_event(Device *d, int i, uint32_t data, uint32_t t, uint32_t seq)
{
    if (d->bufsize && d->fmt_ofs[i] >= 0) {
        if (d->count == d->bufsize)
            d->overflow = 1;
        else
            d->buf[(d->head + d->count++) % d->bufsize] = (InEvent){(uint32_t)d->fmt_ofs[i], data, t, seq};
    }
    if (d->event && event_signal)
        event_signal(d->event);
}

/* ------------------------------------------------------------------ host input (input.h) */
static void key_set(int dik, int down)     /* under in_lock */
{
    if (keys[dik] == down)
        return;
    keys[dik] = (uint8_t)down;
    uint32_t t = timestamp(), seq = ++seq_counter;
    for (Device *d = devices; d; d = d->next)
        if (d->kind == DEV_KEYBOARD && d->acquired)
            dev_event(d, dik - 1, down ? 0x80 : 0, t, seq);
}

void input_key(int dik, int down)
{
    if (dik <= 0 || dik > 255)
        return;
    pthread_mutex_lock(&in_lock);
    key_set(dik, down != 0);
    pthread_mutex_unlock(&in_lock);
    if (down)
        input_used(0);
}

static void mouse_button_set(int b, int down)     /* under in_lock */
{
    if (mouse_btn[b] == down)
        return;
    mouse_btn[b] = (uint8_t)down;
    uint32_t t = timestamp(), seq = ++seq_counter;
    for (Device *d = devices; d; d = d->next)
        if (d->kind == DEV_MOUSE && d->acquired)
            dev_event(d, 3 + b, down ? 0x80 : 0, t, seq);
}

static int window_focused = 1;
int input_has_focus(void) { return __atomic_load_n(&window_focused, __ATOMIC_RELAXED); }

void input_focus(int focused)
{
    __atomic_store_n(&window_focused, focused != 0, __ATOMIC_RELAXED);
    if (focused)
        return;
    pthread_mutex_lock(&in_lock);
    for (int k = 1; k < 256; k++)
        key_set(k, 0);
    for (int b = 0; b < MOUSE_BUTTONS; b++)
        mouse_button_set(b, 0);
    pthread_mutex_unlock(&in_lock);
}

static void mouse_axes(const int delta[3])
{
    pthread_mutex_lock(&in_lock);
    uint32_t t = timestamp(), seq = ++seq_counter;
    for (int a = 0; a < 3; a++)
        mouse_pos[a] += (uint32_t)delta[a];
    for (Device *d = devices; d; d = d->next)
        if (d->kind == DEV_MOUSE && d->acquired)
            for (int a = 0; a < 3; a++)
                if (delta[a]) {
                    dev_event(d, a, d->rel_axes ? (uint32_t)delta[a] : mouse_pos[a], t, seq);
                    d->last[a] = mouse_pos[a];
                }
    pthread_mutex_unlock(&in_lock);
}

void input_mouse_move(int dx, int dy)
{
    if (dx || dy)
        mouse_axes((const int[3]){dx, dy, 0});
    if (abs(dx) + abs(dy) > 2)
        input_used(0);
}

void input_mouse_wheel(int delta)
{
    if (delta)
        mouse_axes((const int[3]){0, 0, delta});
    if (delta)
        input_used(0);
}

void input_mouse_button(int button, int down)
{
    if (button < 0 || button >= MOUSE_BUTTONS)
        return;
    pthread_mutex_lock(&in_lock);
    mouse_button_set(button, down != 0);
    pthread_mutex_unlock(&in_lock);
    if (down)
        input_used(0);
}

int input_mouse_captured(void)
{
    int r = 0;
    pthread_mutex_lock(&in_lock);
    for (Device *d = devices; d; d = d->next)
        if (d->kind == DEV_MOUSE && d->acquired && (d->coop & DISCL_EXCLUSIVE))
            r = 1;
    pthread_mutex_unlock(&in_lock);
    return r;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void joy_reset(JoySlot *j)
{
    memset(j->axis, 0, sizeof j->axis);
    memset(j->button, 0, sizeof j->button);
    for (int p = 0; p < INPUT_JOY_POVS; p++)
        j->pov[p] = -1;
}

int input_joy_connect(int slot, const char *name, int naxes, int nbuttons, int npovs)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS)
        return -1;
    pthread_mutex_lock(&in_lock);
    JoySlot *j = &joys[slot];
    snprintf(j->name, sizeof j->name, "%s", name && *name ? name : "Joystick");
    j->naxes = clampi(naxes, 0, INPUT_JOY_AXES);
    j->nbuttons = clampi(nbuttons, 0, INPUT_JOY_BUTTONS);
    j->npovs = clampi(npovs, 0, INPUT_JOY_POVS);
    j->ff = 0;
    joy_reset(j);
    j->connected = 1;
    LOG(LOG_INPUT, "input: joystick %d connected: \"%s\", %d axes, %d buttons, %d POVs", slot, j->name,
        j->naxes, j->nbuttons, j->npovs);
    pthread_mutex_unlock(&in_lock);
    return 0;
}

/* the game has a device on the slot (it created one for it) */
static int joy_claimed(int slot)          /* under in_lock */
{
    for (Device *d = devices; d; d = d->next)
        if (d->kind == DEV_JOY && d->slot == slot)
            return 1;
    return 0;
}

int input_joy_attach(const char *id, const char *name, int naxes, int nbuttons, int npovs)
{
    int slot = -1;
    pthread_mutex_lock(&in_lock);
    for (int s = 0; s < INPUT_MAX_JOYS && slot < 0 && id && *id; s++)
        if (!joys[s].connected && !strcmp(joys[s].id, id))
            slot = s;
    /* a controller swapped for another takes the slot the game already has a device for */
    for (int s = 0; s < INPUT_MAX_JOYS && slot < 0; s++)
        if (!joys[s].connected && joy_claimed(s))
            slot = s;
    for (int s = 0; s < INPUT_MAX_JOYS && slot < 0; s++)
        if (!joys[s].connected && !joys[s].id[0])
            slot = s;
    for (int s = 0; s < INPUT_MAX_JOYS && slot < 0; s++)
        if (!joys[s].connected)
            slot = s;
    if (slot >= 0)
        snprintf(joys[slot].id, sizeof joys[slot].id, "%s", id && *id ? id : "?");
    pthread_mutex_unlock(&in_lock);
    if (slot < 0) {
        LOGA("input: no free joystick slot for \"%s\"", name ? name : "?");
        return -1;
    }
    input_joy_connect(slot, name, naxes, nbuttons, npovs);
    return slot;
}

int dinput_joy_unclaimed(void)
{
    int r = -1;
    pthread_mutex_lock(&in_lock);
    for (int s = 0; s < INPUT_MAX_JOYS && r < 0; s++)
        if (joys[s].connected && !joy_claimed(s))
            r = s;
    pthread_mutex_unlock(&in_lock);
    return r;
}

/* The joystick slot behind one of the program's joystick devices (-1 if it is not one) */
int dinput_device_slot(uint32_t devobj)
{
    Device *d = devobj ? dev_of(devobj) : NULL;
    return d && d->kind == DEV_JOY ? d->slot : -1;
}

static void fill_devinst(uint32_t p, uint32_t size, int kind, int slot);

uint32_t dinput_joy_instance(int slot)
{
    uint32_t p = galloc(SZ_DEVINST);
    pthread_mutex_lock(&in_lock);
    fill_devinst(p, SZ_DEVINST, DEV_JOY, slot);
    pthread_mutex_unlock(&in_lock);
    return p;
}

void input_joy_disconnect(int slot)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS)
        return;
    pthread_mutex_lock(&in_lock);
    joys[slot].connected = 0;
    joy_reset(&joys[slot]);
    for (Device *d = devices; d; d = d->next)
        if (d->kind == DEV_JOY && d->slot == slot && d->acquired) {
            d->acquired = 0;
            d->lost = 1;
        }
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "input: joystick %d disconnected", slot);
}

int input_joy_connected(int slot)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS)
        return 0;
    pthread_mutex_lock(&in_lock);
    int r = joys[slot].connected;
    pthread_mutex_unlock(&in_lock);
    return r;
}

void input_joy_set_ff(int slot, int capable)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS)
        return;
    pthread_mutex_lock(&in_lock);
    joys[slot].ff = capable != 0;
    /* a device the game already has (its objects keep the attributes they were made with) */
    for (Device *d = devices; d; d = d->next)
        if (d->kind == DEV_JOY && d->slot == slot)
            d->ff = capable != 0;
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "input: joystick %d %s force feedback", slot, capable ? "reports" : "does not report");
}

/* object i of every acquired device on the slot changed */
static void joy_changed(int slot, int kind, int index)   /* under in_lock */
{
    uint32_t t = timestamp(), seq = ++seq_counter;
    for (Device *d = devices; d; d = d->next) {
        if (d->kind != DEV_JOY || d->slot != slot || !d->acquired)
            continue;
        int i = kind == OBJ_AXIS ? (index < d->naxes ? index : -1)
              : kind == OBJ_POV ? (index < d->npovs ? d->first_pov + index : -1)
              : (index < d->nbuttons ? d->first_button + index : -1);
        if (i < 0)
            continue;
        uint32_t v = obj_abs(d, i);
        if (v == d->last[i])
            continue;
        uint32_t data = kind == OBJ_AXIS && d->rel_axes ? v - d->last[i] : v;
        d->last[i] = v;
        dev_event(d, i, data, t, seq);
    }
}

void input_joy_axis(int slot, int axis, int value)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || axis < 0 || axis >= INPUT_JOY_AXES)
        return;
    pthread_mutex_lock(&in_lock);
    if (joys[slot].connected && joys[slot].axis[axis] != value) {
        joys[slot].axis[axis] = clampi(value, -32768, 32767);
        joy_changed(slot, OBJ_AXIS, axis);
    }
    pthread_mutex_unlock(&in_lock);
}

void input_joy_button(int slot, int button, int down)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || button < 0 || button >= INPUT_JOY_BUTTONS)
        return;
    pthread_mutex_lock(&in_lock);
    if (joys[slot].connected && joys[slot].button[button] != (down != 0)) {
        joys[slot].button[button] = down != 0;
        joy_changed(slot, OBJ_BUTTON, button);
    }
    pthread_mutex_unlock(&in_lock);
}

void input_joy_pov(int slot, int pov, int hundredths)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || pov < 0 || pov >= INPUT_JOY_POVS)
        return;
    int v = hundredths < 0 ? -1 : hundredths % 36000;
    pthread_mutex_lock(&in_lock);
    if (joys[slot].connected && joys[slot].pov[pov] != v) {
        joys[slot].pov[pov] = v;
        joy_changed(slot, OBJ_POV, pov);
    }
    pthread_mutex_unlock(&in_lock);
}

/* ------------------------------------------------------------------ rumble */
void input_set_rumble_handler(InputRumbleFn fn)
{
    __atomic_store_n(&rumble_fn, fn, __ATOMIC_RELEASE);
}

void input_joy_rumble(int slot, uint16_t low, uint16_t high, uint32_t ms)
{
    InputRumbleFn fn = __atomic_load_n(&rumble_fn, __ATOMIC_ACQUIRE);
    if (fn && slot >= 0 && slot < INPUT_MAX_JOYS)
        fn(slot, low, high, ms);
}

int dinput_dev_ff(uint32_t device)
{
    int r = 0;
    pthread_mutex_lock(&in_lock);
    for (Device *d = devices; d; d = d->next)
        if (d->obj == device && d->kind == DEV_JOY)
            r = d->ff;
    pthread_mutex_unlock(&in_lock);
    return r;
}

int dinput_joy_slot(uint32_t device)
{
    int r = -1;
    pthread_mutex_lock(&in_lock);
    for (Device *d = devices; d; d = d->next)
        if (d->obj == device && d->kind == DEV_JOY)
            r = d->slot;
    pthread_mutex_unlock(&in_lock);
    return r;
}

/* ------------------------------------------------------------------ structs handed to the game */
static uint32_t devtype(const Device *d)
{
    if (d->kind == DEV_KEYBOARD)
        return DIDEVTYPE_KEYBOARD | 4u << 8;     /* DIDEVTYPEKEYBOARD_PCENH */
    if (d->kind == DEV_MOUSE)
        return DIDEVTYPE_MOUSE | 2u << 8;        /* DIDEVTYPEMOUSE_TRADITIONAL */
    return DIDEVTYPE_JOYSTICK | 4u << 8 | DIDEVTYPE_HID;   /* DIDEVTYPEJOYSTICK_GAMEPAD */
}

/* DIDEVICEINSTANCEA; under in_lock (joystick names) */
static void fill_devinst(uint32_t p, uint32_t size, int kind, int slot)
{
    memset(gptr(p), 0, size);
    W32(p, size);
    Device tmp = {.kind = kind, .slot = slot};
    const char *name;
    if (kind == DEV_KEYBOARD) {
        guid_write(p + 4, G_SYSKEYBOARD);
        guid_write(p + 0x14, G_SYSKEYBOARD);
        name = "Keyboard";
    } else if (kind == DEV_MOUSE) {
        guid_write(p + 4, G_SYSMOUSE);
        guid_write(p + 0x14, G_SYSMOUSE);
        name = "Mouse";
    } else {
        joy_guid_write(p + 4, slot, 0);
        joy_guid_write(p + 0x14, slot, 1);
        name = joys[slot].name;
    }
    W32(p + 0x24, devtype(&tmp));
    gstrncpy(p + 0x28, name, 260);
    gstrncpy(p + 0x12c, name, 260);
    if (size >= SZ_DEVINST) {
        W16(p + 0x240, 1);                       /* HID generic desktop: keyboard 6, mouse 2, pad 5 */
        W16(p + 0x242, kind == DEV_KEYBOARD ? 6 : kind == DEV_MOUSE ? 2 : 5);
    }
}

/* DIDEVICEOBJECTINSTANCEA; under in_lock */
static void fill_objinst(const Device *d, int i, uint32_t p, uint32_t size)
{
    const DevObj *o = &d->objs[i];
    memset(gptr(p), 0, size);
    W32(p, size);
    guid_write(p + 4, o->guid);
    W32(p + 0x14, d->fmt_ofs[i] >= 0 ? (uint32_t)d->fmt_ofs[i] : o->native_ofs);
    W32(p + 0x18, o->type);
    W32(p + 0x1c, o->flags);
    gstrncpy(p + 0x20, o->name, 260);
    if (size >= SZ_OBJINST) {
        if (o->flags & DIDOI_FFACTUATOR) {
            W32(p + 0x124, 10000);               /* dwFFMaxForce */
            W32(p + 0x128, 1);                   /* dwFFForceResolution */
        }
        W16(p + 0x130, o->usage_page);
        W16(p + 0x132, o->usage);
    }
}

static int enum_filter(const DevObj *o, uint32_t flags)
{
    uint32_t t = flags & 0xff;
    if (t && !(o->type & t))
        return 0;
    return !(flags & DIDFT_ATTRS & ~o->type);
}

/* the object a DIPROPHEADER / GetObjectInfo names, -1 if none */
static int find_obj(const Device *d, uint32_t obj, uint32_t how)
{
    for (int i = 0; i < d->nobjs; i++) {
        const DevObj *o = &d->objs[i];
        if (how == DIPH_BYOFFSET && d->fmt_ofs[i] >= 0 && (uint32_t)d->fmt_ofs[i] == obj)
            return i;
        if (how == DIPH_BYID && GETINSTANCE(obj) == GETINSTANCE(o->type) && (obj & o->type & 0xff))
            return i;
        if (how == DIPH_BYUSAGE && (obj & 0xffff) == o->usage && (obj >> 16) == o->usage_page)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ force feedback effects */
struct Effect {
    Effect *next;
    Device *dev;
    uint32_t obj;
    int refs, alive, gid, playing;
    uint32_t flags, duration, sample_period, gain, trigger, trigger_repeat, start_delay;
    uint32_t naxes, axes[4];
    int32_t dir[4];
    int has_env;
    uint32_t env[4];
    uint32_t tsp_size;
    uint8_t tsp[64];
    double started;
    uint32_t iterations;
};

typedef struct { int slot, send; uint16_t low, high; uint32_t ms; } Rumble;

static const struct { int gid; uint32_t type; } eff_types[] = {
    {G_CONSTANT, DIEFT_CONSTANTFORCE}, {G_RAMP, DIEFT_RAMPFORCE}, {G_SQUARE, DIEFT_PERIODIC},
    {G_SINE, DIEFT_PERIODIC}, {G_TRIANGLE, DIEFT_PERIODIC}, {G_SAWUP, DIEFT_PERIODIC},
    {G_SAWDOWN, DIEFT_PERIODIC},
};
#define NEFFTYPES (int)(sizeof eff_types / sizeof *eff_types)

static int eff_type_index(int gid)
{
    for (int k = 0; k < NEFFTYPES; k++)
        if (eff_types[k].gid == gid)
            return k;
    return -1;
}

static int32_t tsp_i32(const Effect *e, uint32_t ofs)
{
    int32_t v = 0;
    if (ofs + 4 <= e->tsp_size)
        memcpy(&v, e->tsp + ofs, 4);
    return v;
}

static double absd(double v) { return v < 0 ? -v : v; }

/* ms of play left (INFINITE = no end), 0 = finished */
static uint32_t eff_left(const Effect *e, double now)
{
    if (e->duration == INFINITE || e->iterations == INFINITE)
        return INFINITE;
    double left = (e->duration / 1000.0) * e->iterations + e->start_delay / 1000.0 - (now - e->started);
    return left <= 0 ? 0 : left < 1 ? 1 : (uint32_t)left;
}

/* what the device's playing effects add up to on the two motors */
static Rumble ff_rumble(Device *d)       /* under in_lock */
{
    double lo = 0, hi = 0, now = now_ms();
    uint32_t ms = 0;
    for (Effect *e = d->effects; e && !d->ff_off && !d->ff_paused; e = e->next) {
        if (!e->playing)
            continue;
        uint32_t left = eff_left(e, now);
        if (!left) {
            e->playing = 0;
            continue;
        }
        double m;
        int high = 0;
        if (e->gid == G_CONSTANT)
            m = absd(tsp_i32(e, 0));
        else if (e->gid == G_RAMP)
            m = absd(tsp_i32(e, 0)) > absd(tsp_i32(e, 4)) ? absd(tsp_i32(e, 0)) : absd(tsp_i32(e, 4));
        else {
            m = (uint32_t)tsp_i32(e, 0) + absd(tsp_i32(e, 4));
            uint32_t period = (uint32_t)tsp_i32(e, 12);
            high = period && period < 60000;     /* faster than ~16 Hz: the small motor */
        }
        m = m / 10000.0 * (e->gain / 10000.0) * (d->ffgain / 10000.0);
        if (m > 1)
            m = 1;
        if (high && m > hi)
            hi = m;
        if (!high && m > lo)
            lo = m;
        if (left > ms)
            ms = left;
    }
    Rumble r = {d->slot, 1, (uint16_t)(lo * 65535), (uint16_t)(hi * 65535), ms};
    if (!r.low && !r.high)
        r.ms = 0;
    return r;
}

static void rumble_send(Rumble r)
{
    if (!r.send)
        return;
    LOG(LOG_INPUT, "rumble: joy%d low %u high %u for %u ms", r.slot, r.low, r.high, r.ms);
    input_joy_rumble(r.slot, r.low, r.high, r.ms);
}

static void ff_stop_all(Device *d)       /* under in_lock */
{
    for (Effect *e = d->effects; e; e = e->next)
        e->playing = 0;
}

static Effect *eff_of(uint32_t obj)
{
    return com_host(obj);
}

/* DIEFFECT -> effect, for the DIEP_* fields named */
static uint32_t eff_set_params(Effect *e, uint32_t p, uint32_t fl)    /* under in_lock */
{
    uint32_t size = R32(p);
    if (size != SZ_EFFECT && size != SZ_EFFECT_DX5)
        return DIERR_INVALIDPARAM;
    if (fl & (DIEP_AXES | DIEP_DIRECTION)) {
        uint32_t n = R32(p + 0x1c);
        if (n > 4)
            return DIERR_INVALIDPARAM;
        e->flags = R32(p + 4);
        if ((fl & DIEP_AXES) && n && !R32(p + 0x20))
            return DIERR_INVALIDPARAM;
        if ((fl & DIEP_DIRECTION) && n && !R32(p + 0x24))
            return DIERR_INVALIDPARAM;
        if (fl & DIEP_AXES) {
            e->naxes = n;
            for (uint32_t k = 0; k < n; k++)
                e->axes[k] = R32(R32(p + 0x20) + 4 * k);
        }
        if (fl & DIEP_DIRECTION)
            for (uint32_t k = 0; k < n; k++)
                e->dir[k] = (int32_t)R32(R32(p + 0x24) + 4 * k);
    }
    if (fl & DIEP_DURATION)
        e->duration = R32(p + 8);
    if (fl & DIEP_SAMPLEPERIOD)
        e->sample_period = R32(p + 0xc);
    if (fl & DIEP_GAIN)
        e->gain = R32(p + 0x10) > 10000 ? 10000 : R32(p + 0x10);
    if (fl & DIEP_TRIGGERBUTTON)
        e->trigger = R32(p + 0x14);
    if (fl & DIEP_TRIGGERREPEATINTERVAL)
        e->trigger_repeat = R32(p + 0x18);
    if (fl & DIEP_ENVELOPE) {
        uint32_t env = R32(p + 0x28);
        e->has_env = env != 0;
        for (int k = 0; k < 4 && env; k++)
            e->env[k] = R32(env + 4 + 4 * k);
    }
    if (fl & DIEP_TYPESPECIFICPARAMS) {
        uint32_t cb = R32(p + 0x2c), src = R32(p + 0x30);
        if (cb > sizeof e->tsp || (cb && !src))
            return DIERR_INVALIDPARAM;
        memcpy(e->tsp, gptr(src), cb);
        e->tsp_size = cb;
    }
    if ((fl & DIEP_STARTDELAY) && size >= SZ_EFFECT)
        e->start_delay = R32(p + 0x34);
    return DI_OK;
}

static uint32_t eff_start(Effect *e, uint32_t iterations, uint32_t flags)  /* under in_lock */
{
    Device *d = e->dev;
    if (!d->acquired || !(d->coop & DISCL_EXCLUSIVE))
        return DIERR_NOTEXCLUSIVEACQUIRED;
    if (flags & DIES_SOLO)
        ff_stop_all(d);
    e->playing = 1;
    e->started = now_ms();
    e->iterations = iterations ? iterations : 1;
    return DI_OK;
}

METHOD(eff_QueryInterface, 3)
{
    Effect *e = eff_of(ARG(0));
    uint32_t ppv = ARG(2);
    char gb[64];
    int g = guid_id(ARG(1));
    uint32_t r = DIERR_NOINTERFACE;
    if (g == G_IUNKNOWN || g == G_IEFFECT) {
        pthread_mutex_lock(&in_lock);
        e->refs++;
        pthread_mutex_unlock(&in_lock);
        r = DI_OK;
    }
    if (ppv)
        W32(ppv, r == DI_OK ? ARG(0) : 0);
    LOG(LOG_INPUT, "IDirectInputEffect::QueryInterface(%08x, %s) -> %s", ARG(0), guid_str(ARG(1), gb, sizeof gb),
        hr_str(r));
    return r;
}

METHOD(eff_AddRef, 1)
{
    Effect *e = eff_of(ARG(0));
    pthread_mutex_lock(&in_lock);
    int n = ++e->refs;
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "IDirectInputEffect::AddRef(%08x) -> %d", ARG(0), n);
    return (uint32_t)n;
}

METHOD(eff_Release, 1)
{
    Effect *e = eff_of(ARG(0));
    Rumble r = {0};
    pthread_mutex_lock(&in_lock);
    int n = e->refs > 0 ? --e->refs : 0;
    if (!n && e->alive) {
        e->alive = 0;
        int was_playing = e->playing;
        e->playing = 0;
        for (Effect **pp = &e->dev->effects; *pp; pp = &(*pp)->next)
            if (*pp == e) {
                *pp = e->next;
                break;
            }
        if (was_playing)
            r = ff_rumble(e->dev);
    }
    pthread_mutex_unlock(&in_lock);
    rumble_send(r);
    LOG(LOG_INPUT, "IDirectInputEffect::Release(%08x) -> %d", ARG(0), n);
    return (uint32_t)n;
}

METHOD(eff_Initialize, 4)
{
    char gb[64];
    LOG(LOG_INPUT, "IDirectInputEffect::Initialize(%08x, hinst %08x, version %x, %s) -> DI_OK", ARG(0), ARG(1),
        ARG(2), guid_str(ARG(3), gb, sizeof gb));
    return DI_OK;
}

METHOD(eff_GetEffectGuid, 2)
{
    Effect *e = eff_of(ARG(0));
    if (!ARG(1))
        return DIERR_INVALIDPARAM;
    guid_write(ARG(1), e->gid);
    LOG(LOG_INPUT, "IDirectInputEffect::GetEffectGuid(%08x) -> %s", ARG(0), guids[e->gid].name);
    return DI_OK;
}

METHOD(eff_GetParameters, 3)
{
    Effect *e = eff_of(ARG(0));
    uint32_t p = ARG(1), fl = ARG(2), r = DI_OK;
    if (!p || (R32(p) != SZ_EFFECT && R32(p) != SZ_EFFECT_DX5)) {
        r = DIERR_INVALIDPARAM;
        goto out;
    }
    pthread_mutex_lock(&in_lock);
    if (fl & DIEP_DURATION) W32(p + 8, e->duration);
    if (fl & DIEP_SAMPLEPERIOD) W32(p + 0xc, e->sample_period);
    if (fl & DIEP_GAIN) W32(p + 0x10, e->gain);
    if (fl & DIEP_TRIGGERBUTTON) W32(p + 0x14, e->trigger);
    if (fl & DIEP_TRIGGERREPEATINTERVAL) W32(p + 0x18, e->trigger_repeat);
    if (fl & (DIEP_AXES | DIEP_DIRECTION)) {
        if (R32(p + 0x1c) < e->naxes) {
            W32(p + 0x1c, e->naxes);
            r = DIERR_MOREDATA;
        } else {
            W32(p + 0x4, e->flags);
            W32(p + 0x1c, e->naxes);
            for (uint32_t k = 0; k < e->naxes; k++) {
                if ((fl & DIEP_AXES) && R32(p + 0x20))
                    W32(R32(p + 0x20) + 4 * k, e->axes[k]);
                if ((fl & DIEP_DIRECTION) && R32(p + 0x24))
                    W32(R32(p + 0x24) + 4 * k, (uint32_t)e->dir[k]);
            }
        }
    }
    if (fl & DIEP_ENVELOPE) {
        if (!e->has_env)
            W32(p + 0x28, 0);
        else if (R32(p + 0x28))
            for (int k = 0; k < 4; k++)
                W32(R32(p + 0x28) + 4 + 4 * k, e->env[k]);
    }
    if (fl & DIEP_TYPESPECIFICPARAMS) {
        if (R32(p + 0x2c) < e->tsp_size)
            r = DIERR_MOREDATA;
        else if (R32(p + 0x30))
            memcpy(gptr(R32(p + 0x30)), e->tsp, e->tsp_size);
        W32(p + 0x2c, e->tsp_size);
    }
    if ((fl & DIEP_STARTDELAY) && R32(p) >= SZ_EFFECT)
        W32(p + 0x34, e->start_delay);
    pthread_mutex_unlock(&in_lock);
out:
    LOG(LOG_INPUT, "IDirectInputEffect::GetParameters(%08x, %08x, flags %x) -> %s", ARG(0), p, fl, hr_str(r));
    return r;
}

METHOD(eff_SetParameters, 3)
{
    Effect *e = eff_of(ARG(0));
    uint32_t p = ARG(1), fl = ARG(2), r;
    Rumble rum = {0};
    if (!p) {
        r = DIERR_INVALIDPARAM;
    } else {
        pthread_mutex_lock(&in_lock);
        r = eff_set_params(e, p, fl);
        if (r == DI_OK && (fl & DIEP_START))
            r = eff_start(e, 1, 0);
        if (r == DI_OK && e->playing)
            rum = ff_rumble(e->dev);
        pthread_mutex_unlock(&in_lock);
    }
    rumble_send(rum);
    LOG(LOG_INPUT, "IDirectInputEffect::SetParameters(%08x, %08x, flags %x) -> %s", ARG(0), p, fl, hr_str(r));
    return r;
}

METHOD(eff_Start, 3)
{
    Effect *e = eff_of(ARG(0));
    Rumble rum = {0};
    pthread_mutex_lock(&in_lock);
    uint32_t r = eff_start(e, ARG(1), ARG(2));
    if (r == DI_OK)
        rum = ff_rumble(e->dev);
    pthread_mutex_unlock(&in_lock);
    rumble_send(rum);
    LOG(LOG_INPUT, "IDirectInputEffect::Start(%08x, %u, flags %x) -> %s", ARG(0), ARG(1), ARG(2), hr_str(r));
    return r;
}

METHOD(eff_Stop, 1)
{
    Effect *e = eff_of(ARG(0));
    Rumble rum = {0};
    pthread_mutex_lock(&in_lock);
    if (e->playing) {
        e->playing = 0;
        rum = ff_rumble(e->dev);
    }
    pthread_mutex_unlock(&in_lock);
    rumble_send(rum);
    LOG(LOG_INPUT, "IDirectInputEffect::Stop(%08x) -> DI_OK", ARG(0));
    return DI_OK;
}

METHOD(eff_GetEffectStatus, 2)
{
    Effect *e = eff_of(ARG(0));
    if (!ARG(1))
        return DIERR_INVALIDPARAM;
    pthread_mutex_lock(&in_lock);
    uint32_t st = e->playing && eff_left(e, now_ms()) ? DIEGES_PLAYING : 0;
    pthread_mutex_unlock(&in_lock);
    W32(ARG(1), st);
    LOGV("IDirectInputEffect::GetEffectStatus(%08x) -> %x", ARG(0), st);
    return DI_OK;
}

METHOD(eff_Download, 1)
{
    LOG(LOG_INPUT, "IDirectInputEffect::Download(%08x) -> DI_OK", ARG(0));
    return DI_OK;
}

METHOD(eff_Unload, 1)
{
    Effect *e = eff_of(ARG(0));
    Rumble rum = {0};
    pthread_mutex_lock(&in_lock);
    if (e->playing) {
        e->playing = 0;
        rum = ff_rumble(e->dev);
    }
    pthread_mutex_unlock(&in_lock);
    rumble_send(rum);
    LOG(LOG_INPUT, "IDirectInputEffect::Unload(%08x) -> DI_OK", ARG(0));
    return DI_OK;
}

METHOD(eff_Escape, 2)
{
    LOGA("IDirectInputEffect::Escape(%08x, %08x): not supported", ARG(0), ARG(1));
    return DIERR_UNSUPPORTED;
}

/* ------------------------------------------------------------------ IDirectInputDevice2A */
static ComClass cls_dev, cls_eff;

static int is_device_iid(int g)
{
    return g == G_IUNKNOWN || g == G_IDEV_A || g == G_IDEV2_A || g == G_IDEV7_A;
}

METHOD(dev_QueryInterface, 3)
{
    Device *d = dev_of(ARG(0));
    uint32_t ppv = ARG(2), r = DIERR_NOINTERFACE;
    char gb[64];
    if (is_device_iid(guid_id(ARG(1)))) {
        pthread_mutex_lock(&in_lock);
        d->refs++;
        pthread_mutex_unlock(&in_lock);
        r = DI_OK;
    }
    if (ppv)
        W32(ppv, r == DI_OK ? ARG(0) : 0);
    LOG(LOG_INPUT, "%s::QueryInterface(%s, %s) -> %s", d->label, guid_str(ARG(1), gb, sizeof gb),
        r == DI_OK ? "same object" : "-", hr_str(r));
    return r;
}

METHOD(dev_AddRef, 1)
{
    Device *d = dev_of(ARG(0));
    pthread_mutex_lock(&in_lock);
    int n = ++d->refs;
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "%s::AddRef -> %d", d->label, n);
    return (uint32_t)n;
}

METHOD(dev_Release, 1)
{
    Device *d = dev_of(ARG(0));
    Rumble rum = {0};
    pthread_mutex_lock(&in_lock);
    int n = d->refs > 0 ? --d->refs : 0;
    if (!n && d->alive) {
        /* gone for the game; the record stays so a stale pointer can't crash us */
        d->alive = d->acquired = d->lost = 0;
        for (Device **pp = &devices; *pp; pp = &(*pp)->next)
            if (*pp == d) {
                *pp = d->next;
                break;
            }
        if (d->ff && d->effects) {     /* (none: the IFC21 stand-in's rumble is not ours to stop) */
            ff_stop_all(d);
            rum = ff_rumble(d);
        }
        for (Effect *e = d->effects; e; e = e->next)
            e->alive = 0;
        d->effects = NULL;
        free(d->buf);
        d->buf = NULL;
        d->bufsize = d->count = 0;
    }
    pthread_mutex_unlock(&in_lock);
    rumble_send(rum);
    LOG(LOG_INPUT, "%s::Release -> %d", d->label, n);
    return (uint32_t)n;
}

METHOD(dev_GetCapabilities, 2)
{
    Device *d = dev_of(ARG(0));
    uint32_t p = ARG(1), r = DI_OK;
    uint32_t size = p ? R32(p) : 0;
    if (size != SZ_DEVCAPS && size != SZ_DEVCAPS_DX3) {
        r = DIERR_INVALIDPARAM;
    } else {
        pthread_mutex_lock(&in_lock);
        int attached = d->kind != DEV_JOY || joys[d->slot].connected;
        uint32_t flags = (attached ? DIDC_ATTACHED : 0) | (d->ff ? DIDC_FORCEFEEDBACK : 0);
        pthread_mutex_unlock(&in_lock);
        W32(p + 4, flags);
        W32(p + 8, devtype(d));
        W32(p + 12, (uint32_t)d->naxes);
        W32(p + 16, (uint32_t)d->nbuttons);
        W32(p + 20, (uint32_t)d->npovs);
        if (size >= SZ_DEVCAPS) {
            W32(p + 24, d->ff ? 1000 : 0);      /* dwFFSamplePeriod, us */
            W32(p + 28, d->ff ? 1000 : 0);      /* dwFFMinTimeResolution, us */
            W32(p + 32, 0);
            W32(p + 36, 0);
            W32(p + 40, 0);
        }
    }
    LOG(LOG_INPUT, "%s::GetCapabilities(size %#x) -> %s (flags %x, type %x, %d axes, %d buttons, %d POVs)",
        d->label, size, hr_str(r), r ? 0 : R32(p + 4), devtype(d), d->naxes, d->nbuttons, d->npovs);
    return r;
}

METHOD(dev_EnumObjects, 4)
{
    Device *d = dev_of(ARG(0));
    uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3);
    char tb[96];
    if (!cb) {
        LOG(LOG_INPUT, "%s::EnumObjects(NULL callback) -> DIERR_INVALIDPARAM", d->label);
        return DIERR_INVALIDPARAM;
    }
    uint32_t size = d->version >= 0x500 ? SZ_OBJINST : SZ_OBJINST_DX3;
    uint32_t p = galloc(size);
    int n = 0;
    for (int i = 0; i < d->nobjs; i++) {
        if (!enum_filter(&d->objs[i], flags))
            continue;
        pthread_mutex_lock(&in_lock);
        fill_objinst(d, i, p, size);
        pthread_mutex_unlock(&in_lock);
        n++;
        LOG(LOG_INPUT, "  %s: %s, %s", d->objs[i].name, guids[d->objs[i].guid].name, type_str(d->objs[i].type, tb, sizeof tb));
        if (!guest_call(c, cb, 2, p, ref))
            break;
    }
    gfree(p);
    LOG(LOG_INPUT, "%s::EnumObjects(cb %08x, ref %08x, flags %s) -> DI_OK, %d objects", d->label, cb, ref,
        type_str(flags | DIDFT_ANYINSTANCE, tb, sizeof tb), n);
    return DI_OK;
}

static uint32_t write_wstr(uint32_t p, const char *s, uint32_t nchars)
{
    uint32_t k = 0;
    for (; s[k] && k + 1 < nchars; k++)
        W16(p + 2 * k, (unsigned char)s[k]);
    W16(p + 2 * k, 0);
    return k;
}

/* which object a property names: -1 = the device, -2 = not found */
static int prop_target(Device *d, uint32_t ph, uint32_t *err)
{
    uint32_t obj = R32(ph + 8), how = R32(ph + 12);
    if (how == DIPH_DEVICE) {
        if (obj) {
            *err = DIERR_INVALIDPARAM;
            return -2;
        }
        return -1;
    }
    int i = how <= DIPH_BYUSAGE ? find_obj(d, obj, how) : -1;
    if (i < 0)
        *err = how <= DIPH_BYUSAGE ? DIERR_NOTFOUND : DIERR_INVALIDPARAM;
    return i < 0 ? -2 : i;
}

static const char *prop_name(uint32_t prop, char *buf, size_t n)
{
    if (prop <= DIPROP_LAST && prop_names[prop])
        snprintf(buf, n, "DIPROP_%s", prop_names[prop]);
    else if (prop < 0x10000)
        snprintf(buf, n, "DIPROP_%u", prop);
    else
        guid_str(prop, buf, n);
    return buf;
}

static int is_axis(const Device *d, int i)
{
    return i >= 0 && d->objs[i].kind == OBJ_AXIS;
}

METHOD(dev_GetProperty, 3)
{
    Device *d = dev_of(ARG(0));
    uint32_t prop = ARG(1), ph = ARG(2), r = DI_OK;
    char pb[64], vb[96] = "";
    int unknown = 0;
    if (!ph || R32(ph + 4) != 16) {
        r = DIERR_INVALIDPARAM;
        goto out;
    }
    if (prop >= 0x10000) {
        r = DIERR_UNSUPPORTED;
        unknown = 1;
        goto out;
    }
    pthread_mutex_lock(&in_lock);
    uint32_t size = R32(ph);
    int i = prop_target(d, ph, &r);
    if (i == -2)
        goto unlock;
    int axis = is_axis(d, i) || (i == -1 && d->naxes);
    int first_axis = i >= 0 ? i : 0;
    switch (prop) {
    case DIPROP_BUFFERSIZE: case DIPROP_AXISMODE: case DIPROP_FFGAIN: case DIPROP_FFLOAD:
    case DIPROP_AUTOCENTER: case DIPROP_CALIBRATIONMODE: case DIPROP_JOYSTICKID:
        if (size != SZ_PROPDWORD) {
            r = DIERR_INVALIDPARAM;
            break;
        }
        if (prop == DIPROP_BUFFERSIZE) W32(ph + 16, d->bufsize);
        else if (prop == DIPROP_AXISMODE) W32(ph + 16, (uint32_t)d->rel_axes);
        else if (prop == DIPROP_CALIBRATIONMODE) W32(ph + 16, 0);
        else if (prop == DIPROP_JOYSTICKID) {
            if (d->kind != DEV_JOY) r = DIERR_UNSUPPORTED;
            else W32(ph + 16, (uint32_t)d->slot);
        } else if (!d->ff) r = DIERR_UNSUPPORTED;
        else W32(ph + 16, prop == DIPROP_FFGAIN ? d->ffgain : prop == DIPROP_AUTOCENTER ? d->autocenter : 0);
        if (r == DI_OK) snprintf(vb, sizeof vb, " = %u", R32(ph + 16));
        break;
    case DIPROP_GRANULARITY:
        if (size != SZ_PROPDWORD || i < 0) {
            r = DIERR_INVALIDPARAM;
            break;
        }
        W32(ph + 16, d->kind == DEV_MOUSE && d->objs[i].index == 2 ? 120 : 1);
        snprintf(vb, sizeof vb, " = %u", R32(ph + 16));
        break;
    case DIPROP_RANGE:
        if (size != SZ_PROPRANGE) r = DIERR_INVALIDPARAM;
        else if (d->kind != DEV_JOY || !axis) r = DIERR_UNSUPPORTED;
        else {
            W32(ph + 16, (uint32_t)d->ap[first_axis].min);
            W32(ph + 20, (uint32_t)d->ap[first_axis].max);
            snprintf(vb, sizeof vb, " = [%d, %d]", d->ap[first_axis].min, d->ap[first_axis].max);
        }
        break;
    case DIPROP_DEADZONE: case DIPROP_SATURATION:
        if (size != SZ_PROPDWORD) r = DIERR_INVALIDPARAM;
        else if (d->kind != DEV_JOY || !axis) r = DIERR_UNSUPPORTED;
        else {
            W32(ph + 16, prop == DIPROP_DEADZONE ? d->ap[first_axis].dead : d->ap[first_axis].sat);
            snprintf(vb, sizeof vb, " = %u", R32(ph + 16));
        }
        break;
    case DIPROP_INSTANCENAME: case DIPROP_PRODUCTNAME: {
        if (size != SZ_PROPSTRING || i != -1) {
            r = DIERR_INVALIDPARAM;
            break;
        }
        const char *name = d->kind == DEV_KEYBOARD ? "Keyboard" : d->kind == DEV_MOUSE ? "Mouse" : joys[d->slot].name;
        write_wstr(ph + 16, name, 260);
        snprintf(vb, sizeof vb, " = \"%s\"", name);
        break;
    }
    default:
        r = DIERR_UNSUPPORTED;
        unknown = 1;
    }
unlock:
    pthread_mutex_unlock(&in_lock);
out:
    if (unknown)
        LOGA("%s::GetProperty(%s): not supported", d->label, prop_name(prop, pb, sizeof pb));
    LOG(LOG_INPUT, "%s::GetProperty(%s, how %u obj %#x) -> %s%s", d->label, prop_name(prop, pb, sizeof pb),
        ph ? R32(ph + 12) : 0, ph ? R32(ph + 8) : 0, hr_str(r), vb);
    return r;
}

static int set_buffer(Device *d, uint32_t n)      /* under in_lock */
{
    if (n > 65536)
        n = 65536;
    InEvent *b = n ? calloc(n, sizeof *b) : NULL;
    if (n && !b)
        return 0;
    free(d->buf);
    d->buf = b;
    d->bufsize = n;
    d->head = d->count = 0;
    d->overflow = 0;
    return 1;
}

METHOD(dev_SetProperty, 3)
{
    Device *d = dev_of(ARG(0));
    uint32_t prop = ARG(1), ph = ARG(2), r = DI_OK;
    char pb[64], vb[96] = "";
    int unknown = 0;
    if (!ph || R32(ph + 4) != 16) {
        r = DIERR_INVALIDPARAM;
        goto out;
    }
    if (prop >= 0x10000) {
        r = DIERR_UNSUPPORTED;
        unknown = 1;
        goto out;
    }
    pthread_mutex_lock(&in_lock);
    uint32_t size = R32(ph), v = size >= SZ_PROPDWORD ? R32(ph + 16) : 0;
    int i = prop_target(d, ph, &r);
    if (i == -2)
        goto unlock;
    int lo = i >= 0 ? i : 0, hi = i >= 0 ? i + 1 : d->naxes;     /* objects the value applies to */
    switch (prop) {
    case DIPROP_BUFFERSIZE: case DIPROP_AXISMODE: case DIPROP_FFGAIN: case DIPROP_AUTOCENTER:
    case DIPROP_CALIBRATIONMODE:
        snprintf(vb, sizeof vb, " = %u", v);
        if (size != SZ_PROPDWORD || i != -1) {
            r = DIERR_INVALIDPARAM;
        } else if (prop == DIPROP_BUFFERSIZE) {
            if (d->acquired) r = DIERR_ACQUIRED;
            else if (!set_buffer(d, v)) r = DIERR_INVALIDPARAM;
        } else if (prop == DIPROP_AXISMODE) {
            if (d->acquired) r = DIERR_ACQUIRED;
            else if (v > 1) r = DIERR_INVALIDPARAM;
            else d->rel_axes = (int)v;
        } else if (prop == DIPROP_CALIBRATIONMODE) {
            /* raw vs cooked: we only have cooked data */
        } else if (!d->ff) {
            r = DIERR_UNSUPPORTED;
        } else if (prop == DIPROP_FFGAIN) {
            if (v > 10000) r = DIERR_INVALIDPARAM;
            else d->ffgain = v;
        } else {
            d->autocenter = v != 0;
        }
        break;
    case DIPROP_RANGE: {
        int32_t mn = (int32_t)(size >= SZ_PROPRANGE ? R32(ph + 16) : 0);
        int32_t mx = (int32_t)(size >= SZ_PROPRANGE ? R32(ph + 20) : 0);
        snprintf(vb, sizeof vb, " = [%d, %d]", mn, mx);
        if (size != SZ_PROPRANGE || mn >= mx) r = DIERR_INVALIDPARAM;
        else if (d->kind != DEV_JOY || (i >= 0 && !is_axis(d, i))) r = DIERR_UNSUPPORTED;
        else
            for (int k = lo; k < hi; k++) {
                d->ap[k].min = mn;
                d->ap[k].max = mx;
            }
        break;
    }
    case DIPROP_DEADZONE: case DIPROP_SATURATION:
        snprintf(vb, sizeof vb, " = %u", v);
        if (size != SZ_PROPDWORD || v > 10000) r = DIERR_INVALIDPARAM;
        else if (d->kind != DEV_JOY || (i >= 0 && !is_axis(d, i))) r = DIERR_UNSUPPORTED;
        else
            for (int k = lo; k < hi; k++) {
                if (prop == DIPROP_DEADZONE) d->ap[k].dead = v;
                else d->ap[k].sat = v;
            }
        break;
    default:
        r = DIERR_UNSUPPORTED;
        unknown = 1;
    }
    if (r == DI_OK && d->acquired)
        for (int k = lo; k < hi && k < d->nobjs; k++)
            d->last[k] = obj_abs(d, k);
unlock:
    pthread_mutex_unlock(&in_lock);
out:
    if (unknown)
        LOGA("%s::SetProperty(%s): not supported", d->label, prop_name(prop, pb, sizeof pb));
    LOG(LOG_INPUT, "%s::SetProperty(%s, how %u obj %#x%s) -> %s", d->label, prop_name(prop, pb, sizeof pb),
        ph ? R32(ph + 12) : 0, ph ? R32(ph + 8) : 0, vb, hr_str(r));
    return r;
}

METHOD(dev_Acquire, 1)
{
    Device *d = dev_of(ARG(0));
    uint32_t r;
    pthread_mutex_lock(&in_lock);
    if (!d->has_format)
        r = DIERR_INVALIDPARAM;
    else if (d->kind == DEV_JOY && !joys[d->slot].connected) {
        d->lost = 0;
        r = DIERR_UNPLUGGED;
    } else if (d->acquired)
        r = DI_FALSE;
    else {
        d->acquired = 1;
        d->lost = 0;
        d->head = d->count = 0;
        d->overflow = 0;
        dev_baseline(d);
        r = DI_OK;
    }
    pthread_mutex_unlock(&in_lock);
    if (r == DIERR_UNPLUGGED || r == DI_FALSE)
        LOGV("%s::Acquire -> %s", d->label, hr_str(r));
    else
        LOG(LOG_INPUT, "%s::Acquire -> %s", d->label, hr_str(r));
    return r;
}

METHOD(dev_Unacquire, 1)
{
    Device *d = dev_of(ARG(0));
    Rumble rum = {0};
    pthread_mutex_lock(&in_lock);
    uint32_t r = d->acquired || d->lost ? DI_OK : DI_FALSE;
    d->acquired = d->lost = 0;
    if (d->ff && d->effects && r == DI_OK) {
        ff_stop_all(d);         /* DirectInput stops a device's effects when it is unacquired */
        rum = ff_rumble(d);
    }
    pthread_mutex_unlock(&in_lock);
    rumble_send(rum);
    LOG(LOG_INPUT, "%s::Unacquire -> %s", d->label, hr_str(r));
    return r;
}

static void log_state(const Device *d, uint32_t p, uint32_t cb)
{
    char buf[512];
    int k = 0;
    if (d->kind == DEV_KEYBOARD) {
        for (uint32_t i = 0; i < cb && k < 400; i++)
            if (R8(p + i) & 0x80)
                k += snprintf(buf + k, sizeof buf - k, " %s", dik_names[i & 0xff] ? dik_names[i & 0xff] : "?");
    } else {
        for (uint32_t i = 0; i + 4 <= cb && k < 400; i += 4)
            k += snprintf(buf + k, sizeof buf - k, " %08x", R32(p + i));
    }
    buf[k] = 0;
    logmsg(LOG_INPUT, "  state:%s", k ? buf : " (none)");
}

METHOD(dev_GetDeviceState, 3)
{
    input_wait();
    Device *d = dev_of(ARG(0));
    uint32_t cb = ARG(1), p = ARG(2), r = DI_OK;
    pthread_mutex_lock(&in_lock);
    if (d->lost)
        r = DIERR_INPUTLOST;
    else if (!d->acquired)
        r = DIERR_NOTACQUIRED;
    else if (!p || cb != d->data_size)
        r = DIERR_INVALIDPARAM;
    else {
        memset(gptr(p), 0, cb);
        for (int k = 0; k < d->npov_fill; k++)
            W32(p + d->pov_fill[k], 0xffffffffu);
        for (int i = 0; i < d->nobjs; i++) {
            if (d->fmt_ofs[i] < 0)
                continue;
            uint32_t v = obj_read(d, i), at = p + (uint32_t)d->fmt_ofs[i];
            if (d->objs[i].kind == OBJ_AXIS || d->objs[i].kind == OBJ_POV)
                W32(at, v);
            else
                W8(at, v);
        }
    }
    pthread_mutex_unlock(&in_lock);
    if (verbose && LOGON(LOG_INPUT)) {
        logmsg(LOG_INPUT, "%s::GetDeviceState(%u, %08x) -> %s", d->label, cb, p, hr_str(r));
        if (r == DI_OK)
            log_state(d, p, cb);
    } else if (r == DIERR_INVALIDPARAM)
        LOG(LOG_INPUT, "%s::GetDeviceState(%u, %08x) -> %s (format size %u)", d->label, cb, p, hr_str(r),
            d->data_size);
    return r;
}

static const char *ofs_name(const Device *d, uint32_t ofs)
{
    for (int i = 0; i < d->nobjs; i++)
        if (d->fmt_ofs[i] >= 0 && (uint32_t)d->fmt_ofs[i] == ofs)
            return d->objs[i].name;
    return "?";
}

METHOD(dev_GetDeviceData, 5)
{
    Device *d = dev_of(ARG(0));
    uint32_t cbobj = ARG(1), out = ARG(2), pn = ARG(3), flags = ARG(4), r = DI_OK, n = 0;
    uint32_t want = pn ? R32(pn) : 0;
    pthread_mutex_lock(&in_lock);
    if (!pn || (out && cbobj < 16))
        r = DIERR_INVALIDPARAM;
    else if (d->lost)
        r = DIERR_INPUTLOST;
    else if (!d->acquired)
        r = DIERR_NOTACQUIRED;
    else if (!d->bufsize)
        r = DIERR_NOTBUFFERED;
    else {
        n = want < d->count ? want : d->count;
        for (uint32_t k = 0; k < n && out; k++) {
            const InEvent *e = &d->buf[(d->head + k) % d->bufsize];
            uint32_t q = out + k * cbobj;
            W32(q, e->ofs);
            W32(q + 4, e->data);
            W32(q + 8, e->time);
            W32(q + 12, e->seq);
            if (cbobj > 16)
                memset(gptr(q + 16), 0, cbobj - 16);
        }
        if (d->overflow)
            r = DI_FALSE;                        /* DI_BUFFEROVERFLOW */
        if (!(flags & DIGDD_PEEK)) {
            d->head = (d->head + n) % d->bufsize;
            d->count -= n;
            d->overflow = 0;
        }
        W32(pn, n);
    }
    pthread_mutex_unlock(&in_lock);
    if (n && out && LOGON(LOG_INPUT)) {
        logmsg(LOG_INPUT, "%s::GetDeviceData(%u, %08x, %u, flags %x) -> %s, %u events", d->label, cbobj, out, want,
               flags, hr_str(r), n);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t q = out + k * cbobj;
            logmsg(LOG_INPUT, "  %s = %#x (seq %u, t %u)", ofs_name(d, R32(q)), R32(q + 4), R32(q + 12), R32(q + 8));
        }
    } else if (n && !out) {
        LOG(LOG_INPUT, "%s::GetDeviceData(NULL, %u, flags %x) -> %s, %u events %s", d->label, want, flags, hr_str(r), n,
            flags & DIGDD_PEEK ? "waiting" : "flushed");
    } else if (r != DI_OK && r != DIERR_NOTACQUIRED && r != DIERR_INPUTLOST) {
        LOG(LOG_INPUT, "%s::GetDeviceData(%u, %08x, %u, flags %x) -> %s", d->label, cbobj, out, want, flags, hr_str(r));
    } else {
        LOGV("%s::GetDeviceData(%u, %08x, %u, flags %x) -> %s, %u events", d->label, cbobj, out, want, flags,
             hr_str(r), n);
    }
    return r;
}

static const char *format_name(uint32_t size, uint32_t nobjs, uint32_t flags)
{
    if (size == 256 && nobjs == 256) return "c_dfDIKeyboard";
    if (size == 16 && nobjs == 7 && flags == DIDF_RELAXIS) return "c_dfDIMouse";
    if (size == 20 && nobjs == 11 && flags == DIDF_RELAXIS) return "c_dfDIMouse2";
    if (size == 80 && nobjs == 44 && flags == DIDF_ABSAXIS) return "c_dfDIJoystick";
    if (size == 272 && nobjs == 164 && flags == DIDF_ABSAXIS) return "c_dfDIJoystick2";
    return "custom";
}

static int fmt_matches(const DevObj *o, int any_guid, int g, uint32_t type)
{
    if (!any_guid && o->guid != g)
        return 0;
    if ((type & 0xff) && !(o->type & type & 0xff))
        return 0;
    if (GETINSTANCE(type) != 0xffff && GETINSTANCE(type) != GETINSTANCE(o->type))
        return 0;
    return !(type & DIDFT_ATTRS & ~o->type);
}

METHOD(dev_SetDataFormat, 2)
{
    Device *d = dev_of(ARG(0));
    uint32_t f = ARG(1), r = DI_OK;
    char tb[96], gb[64];
    if (!f || R32(f) != 0x18 || R32(f + 4) != 0x10 || R32(f + 16) > 4096 || R32(f + 12) > 0x10000 ||
        (R32(f + 16) && !R32(f + 20))) {
        LOG(LOG_INPUT, "%s::SetDataFormat(%08x) -> DIERR_INVALIDPARAM", d->label, f);
        return DIERR_INVALIDPARAM;
    }
    uint32_t flags = R32(f + 8), size = R32(f + 12), nobjs = R32(f + 16), rg = R32(f + 20);
    const char *fname = format_name(size, nobjs, flags);
    int custom = !strcmp(fname, "custom");
    pthread_mutex_lock(&in_lock);
    if (d->acquired) {
        pthread_mutex_unlock(&in_lock);
        LOG(LOG_INPUT, "%s::SetDataFormat(%s) -> DIERR_ACQUIRED", d->label, fname);
        return DIERR_ACQUIRED;
    }
    int naxes = 0, nbut = 0, npov = 0, missing = 0;
    d->npov_fill = 0;
    for (int i = 0; i < d->nobjs; i++)
        d->fmt_ofs[i] = -1;
    for (uint32_t k = 0; k < nobjs; k++) {
        uint32_t e = rg + 16 * k, pg = R32(e), ofs = R32(e + 4), type = R32(e + 8);
        int g = pg ? guid_id(pg) : -1, found = -1;
        if (!pg || g >= 0)
            for (int i = 0; i < d->nobjs && found < 0; i++)
                if (d->fmt_ofs[i] < 0 && fmt_matches(&d->objs[i], !pg, g, type))
                    found = i;
        if (found >= 0) {
            uint32_t width = d->objs[found].kind == OBJ_AXIS || d->objs[found].kind == OBJ_POV ? 4 : 1;
            if (ofs + width > size) {
                LOGA("%s::SetDataFormat: object %u at offset %u is outside the %u-byte format", d->label, k, ofs, size);
                found = -1;
            } else {
                d->fmt_ofs[found] = (int32_t)ofs;
                int kind = d->objs[found].kind;
                naxes += kind == OBJ_AXIS;
                npov += kind == OBJ_POV;
                nbut += kind == OBJ_BUTTON || kind == OBJ_KEY;
            }
        }
        if (found < 0 && (type & 0xff & DIDFT_POV) && ofs + 4 <= size && d->npov_fill < 8)
            d->pov_fill[d->npov_fill++] = ofs;
        if (found < 0 && !(type & DIDFT_OPTIONAL)) {
            missing++;
            LOGA("%s::SetDataFormat: no %s object for %s (offset %u); left empty instead of failing",
                 d->label, guid_str(pg, gb, sizeof gb), type_str(type, tb, sizeof tb), ofs);
        }
        if (custom && LOGON(LOG_INPUT))
            logmsg(LOG_INPUT, "  [%u] %s ofs %u %s -> %s", k, guid_str(pg, gb, sizeof gb), ofs,
                   type_str(type, tb, sizeof tb), found >= 0 ? d->objs[found].name : "(none)");
    }
    d->has_format = 1;
    d->data_size = size;
    if (flags & DIDF_ABSAXIS)
        d->rel_axes = 0;
    else if (flags & DIDF_RELAXIS)
        d->rel_axes = 1;
    d->head = d->count = 0;
    d->overflow = 0;
    dev_baseline(d);
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "%s::SetDataFormat(%s, %u bytes, %u objects, %s) -> %s (%d axes, %d buttons, %d POVs mapped%s)",
        d->label, fname, size, nobjs, flags & DIDF_ABSAXIS ? "ABSAXIS" : flags & DIDF_RELAXIS ? "RELAXIS" : "0",
        hr_str(r), naxes, nbut, npov, missing ? ", some missing" : "");
    return r;
}

METHOD(dev_SetEventNotification, 2)
{
    Device *d = dev_of(ARG(0));
    uint32_t r = DI_OK;
    pthread_mutex_lock(&in_lock);
    if (d->acquired)
        r = DIERR_ACQUIRED;
    else
        d->event = ARG(1);
    pthread_mutex_unlock(&in_lock);
    if (r == DI_OK && ARG(1) && !event_signal)
        LOGA("%s::SetEventNotification: the runtime has no event_signal(); the event will never be set", d->label);
    LOG(LOG_INPUT, "%s::SetEventNotification(%x) -> %s", d->label, ARG(1), hr_str(r));
    return r;
}

METHOD(dev_SetCooperativeLevel, 3)
{
    Device *d = dev_of(ARG(0));
    uint32_t hwnd = ARG(1), fl = ARG(2), r = DI_OK;
    char cb[96];
    int ex = !!(fl & DISCL_EXCLUSIVE), nex = !!(fl & DISCL_NONEXCLUSIVE);
    int fg = !!(fl & DISCL_FOREGROUND), bg = !!(fl & DISCL_BACKGROUND);
    pthread_mutex_lock(&in_lock);
    if (ex == nex || fg == bg)
        r = DIERR_INVALIDPARAM;
    else if (d->kind == DEV_KEYBOARD && ex && bg)
        r = DIERR_UNSUPPORTED;
    else if (d->acquired)
        r = DIERR_ACQUIRED;
    else {
        d->coop = fl;
        d->hwnd = hwnd;
    }
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "%s::SetCooperativeLevel(hwnd %x, %s) -> %s", d->label, hwnd, coop_str(fl, cb, sizeof cb), hr_str(r));
    return r;
}

METHOD(dev_GetObjectInfo, 4)
{
    Device *d = dev_of(ARG(0));
    uint32_t p = ARG(1), obj = ARG(2), how = ARG(3), r = DI_OK;
    uint32_t size = p ? R32(p) : 0;
    pthread_mutex_lock(&in_lock);
    int i = how == DIPH_DEVICE || how > DIPH_BYUSAGE ? -1 : find_obj(d, obj, how);
    if (size != SZ_OBJINST && size != SZ_OBJINST_DX3)
        r = DIERR_INVALIDPARAM;
    else if (how == DIPH_DEVICE || how > DIPH_BYUSAGE)
        r = DIERR_INVALIDPARAM;
    else if (i < 0)
        r = DIERR_NOTFOUND;
    else
        fill_objinst(d, i, p, size);
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "%s::GetObjectInfo(obj %#x, how %u) -> %s%s%s", d->label, obj, how, hr_str(r),
        i >= 0 ? ", " : "", i >= 0 ? d->objs[i].name : "");
    return r;
}

METHOD(dev_GetDeviceInfo, 2)
{
    Device *d = dev_of(ARG(0));
    uint32_t p = ARG(1), r = DI_OK;
    uint32_t size = p ? R32(p) : 0;
    if (size != SZ_DEVINST && size != SZ_DEVINST_DX3)
        r = DIERR_INVALIDPARAM;
    else {
        pthread_mutex_lock(&in_lock);
        fill_devinst(p, size, d->kind, d->slot);
        pthread_mutex_unlock(&in_lock);
    }
    LOG(LOG_INPUT, "%s::GetDeviceInfo(size %#x) -> %s", d->label, size, hr_str(r));
    return r;
}

METHOD(dev_RunControlPanel, 3)
{
    LOG(LOG_INPUT, "%s::RunControlPanel(hwnd %x, flags %x) -> DI_OK (no control panel)", dev_of(ARG(0))->label,
        ARG(1), ARG(2));
    return DI_OK;
}

METHOD(dev_Initialize, 4)
{
    char gb[64];
    LOG(LOG_INPUT, "%s::Initialize(hinst %x, version %x, %s) -> DI_OK", dev_of(ARG(0))->label, ARG(1), ARG(2),
        guid_str(ARG(3), gb, sizeof gb));
    return DI_OK;
}

METHOD(dev_CreateEffect, 5)
{
    Device *d = dev_of(ARG(0));
    uint32_t rguid = ARG(1), peff = ARG(2), ppeff = ARG(3), outer = ARG(4), r = DI_OK;
    char gb[64];
    int g = guid_id(rguid);
    Effect *e = NULL;
    Rumble rum = {0};
    if (ppeff)
        W32(ppeff, 0);
    if (!d->ff)
        r = DIERR_UNSUPPORTED;
    else if (!ppeff)
        r = DIERR_INVALIDPARAM;
    else if (outer)
        r = DIERR_NOAGGREGATION;
    else if (eff_type_index(g) < 0)
        r = DIERR_DEVICENOTREG;
    else {
        e = calloc(1, sizeof *e);
        e->dev = d;
        e->gid = g;
        e->refs = 1;
        e->alive = 1;
        e->gain = 10000;
        e->trigger = INFINITE;                   /* DIEB_NOTRIGGER */
        e->iterations = 1;
        pthread_mutex_lock(&in_lock);
        if (peff)
            r = eff_set_params(e, peff, DIEP_ALLPARAMS);
        if (r == DI_OK) {
            e->obj = com_new(&cls_eff, e);
            e->next = d->effects;
            d->effects = e;
            W32(ppeff, e->obj);
            if (peff && (R32(peff + 4) & DIEP_START)) {
                eff_start(e, 1, 0);
                rum = ff_rumble(d);
            }
        }
        pthread_mutex_unlock(&in_lock);
        if (r != DI_OK)
            free(e);
    }
    rumble_send(rum);
    if (r == DIERR_UNSUPPORTED)
        LOG(LOG_INPUT, "%s::CreateEffect(%s): not a force-feedback device", d->label, guid_str(rguid, gb, sizeof gb));
    LOG(LOG_INPUT, "%s::CreateEffect(%s, %08x) -> %s %08x", d->label, guid_str(rguid, gb, sizeof gb), peff, hr_str(r),
        ppeff ? R32(ppeff) : 0);
    return r;
}

static void fill_effinfo(uint32_t p, int k)
{
    memset(gptr(p), 0, SZ_EFFINFO);
    W32(p, SZ_EFFINFO);
    guid_write(p + 4, eff_types[k].gid);
    W32(p + 0x14, eff_types[k].type);
    W32(p + 0x18, DIEP_ALLPARAMS);
    W32(p + 0x1c, DIEP_ALLPARAMS);
    gstrncpy(p + 0x20, guids[eff_types[k].gid].name + 5, 260);     /* without "GUID_" */
}

METHOD(dev_EnumEffects, 4)
{
    Device *d = dev_of(ARG(0));
    uint32_t cb = ARG(1), ref = ARG(2), type = ARG(3);
    int n = 0;
    if (!cb)
        return DIERR_INVALIDPARAM;
    if (d->ff) {
        uint32_t p = galloc(SZ_EFFINFO);
        for (int k = 0; k < NEFFTYPES; k++) {
            if ((type & 0xff) && (type & 0xff) != eff_types[k].type)
                continue;
            fill_effinfo(p, k);
            n++;
            if (!guest_call(c, cb, 2, p, ref))
                break;
        }
        gfree(p);
    }
    LOG(LOG_INPUT, "%s::EnumEffects(cb %08x, ref %08x, type %x) -> DI_OK, %d effects", d->label, cb, ref, type, n);
    return DI_OK;
}

METHOD(dev_GetEffectInfo, 3)
{
    Device *d = dev_of(ARG(0));
    uint32_t p = ARG(1), r = DI_OK;
    char gb[64];
    int k = d->ff ? eff_type_index(guid_id(ARG(2))) : -1;
    if (!p || R32(p) != SZ_EFFINFO)
        r = DIERR_INVALIDPARAM;
    else if (k < 0)
        r = DIERR_DEVICENOTREG;
    else
        fill_effinfo(p, k);
    LOG(LOG_INPUT, "%s::GetEffectInfo(%s) -> %s", d->label, guid_str(ARG(2), gb, sizeof gb), hr_str(r));
    return r;
}

METHOD(dev_GetForceFeedbackState, 2)
{
    Device *d = dev_of(ARG(0));
    uint32_t p = ARG(1), r = DI_OK, st = 0;
    pthread_mutex_lock(&in_lock);
    if (!d->ff)
        r = DIERR_UNSUPPORTED;
    else if (!p)
        r = DIERR_INVALIDPARAM;
    else if (!d->acquired || !(d->coop & DISCL_EXCLUSIVE))
        r = DIERR_NOTEXCLUSIVEACQUIRED;
    else {
        int playing = 0;
        double now = now_ms();
        for (Effect *e = d->effects; e; e = e->next)
            playing |= e->playing && eff_left(e, now);
        st = DIGFFS_POWERON | (d->ff_off ? DIGFFS_ACTUATORSOFF : DIGFFS_ACTUATORSON) |
             (d->ff_paused ? DIGFFS_PAUSED : 0) | (d->effects ? 0 : DIGFFS_EMPTY) | (playing ? 0 : DIGFFS_STOPPED);
        W32(p, st);
    }
    pthread_mutex_unlock(&in_lock);
    LOGV("%s::GetForceFeedbackState -> %s %x", d->label, hr_str(r), st);
    return r;
}

METHOD(dev_SendForceFeedbackCommand, 2)
{
    Device *d = dev_of(ARG(0));
    uint32_t fl = ARG(1), r = DI_OK;
    Rumble rum = {0};
    pthread_mutex_lock(&in_lock);
    if (!d->ff)
        r = DIERR_UNSUPPORTED;
    else if (!d->acquired || !(d->coop & DISCL_EXCLUSIVE))
        r = DIERR_NOTEXCLUSIVEACQUIRED;
    else {
        if (fl & (DISFFC_RESET | DISFFC_STOPALL))
            ff_stop_all(d);
        if (fl & DISFFC_RESET)
            d->ff_paused = d->ff_off = 0;
        if (fl & DISFFC_PAUSE)
            d->ff_paused = 1;
        if (fl & DISFFC_CONTINUE)
            d->ff_paused = 0;
        if (fl & DISFFC_SETACTUATORSON)
            d->ff_off = 0;
        if (fl & DISFFC_SETACTUATORSOFF)
            d->ff_off = 1;
        rum = ff_rumble(d);
    }
    pthread_mutex_unlock(&in_lock);
    rumble_send(rum);
    LOG(LOG_INPUT, "%s::SendForceFeedbackCommand(%x) -> %s", d->label, fl, hr_str(r));
    return r;
}

METHOD(dev_EnumCreatedEffectObjects, 4)
{
    Device *d = dev_of(ARG(0));
    uint32_t cb = ARG(1), ref = ARG(2);
    if (!cb)
        return DIERR_INVALIDPARAM;
    uint32_t list[64];
    int n = 0;
    pthread_mutex_lock(&in_lock);
    for (Effect *e = d->effects; e && n < 64; e = e->next)
        list[n++] = e->obj;
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "%s::EnumCreatedEffectObjects(cb %08x, ref %08x) -> DI_OK, %d effects", d->label, cb, ref, n);
    for (int k = 0; k < n; k++)
        if (!guest_call(c, cb, 2, list[k], ref))
            break;
    return DI_OK;
}

METHOD(dev_Escape, 2)
{
    LOGA("%s::Escape(%08x): not supported", dev_of(ARG(0))->label, ARG(1));
    return DIERR_UNSUPPORTED;
}

METHOD(dev_Poll, 1)
{
    Device *d = dev_of(ARG(0));
    pthread_mutex_lock(&in_lock);
    uint32_t r = d->lost ? DIERR_INPUTLOST : !d->acquired ? DIERR_NOTACQUIRED : DI_FALSE;   /* DI_NOEFFECT */
    pthread_mutex_unlock(&in_lock);
    LOGV("%s::Poll -> %s", d->label, hr_str(r));
    return r;
}

METHOD(dev_SendDeviceData, 5)
{
    LOGA("%s::SendDeviceData(%u, %08x, %08x, flags %x): not supported", dev_of(ARG(0))->label, ARG(1), ARG(2),
         ARG(3), ARG(4));
    if (ARG(3))
        W32(ARG(3), 0);
    return DIERR_UNSUPPORTED;
}

METHOD(dev_EnumEffectsInFile, 5)
{
    LOGA("%s::EnumEffectsInFile(\"%s\"): not supported", dev_of(ARG(0))->label, ARG(1) ? gstr(ARG(1)) : "");
    return DIERR_UNSUPPORTED;
}

METHOD(dev_WriteEffectToFile, 5)
{
    LOGA("%s::WriteEffectToFile(\"%s\"): not supported", dev_of(ARG(0))->label, ARG(1) ? gstr(ARG(1)) : "");
    return DIERR_UNSUPPORTED;
}

/* ------------------------------------------------------------------ IDirectInputA */
typedef struct { uint32_t obj, version; int refs; } DInput;
static ComClass cls_di;

static DInput *di_of(uint32_t obj)
{
    return com_host(obj);
}

/* device GUID -> kind and slot; 0 if unknown. Under in_lock. */
static int device_for_guid(uint32_t a, int *kind, int *slot)
{
    int g = guid_id(a);
    *slot = -1;
    if (g == G_SYSKEYBOARD) {
        *kind = DEV_KEYBOARD;
        return 1;
    }
    if (g == G_SYSMOUSE) {
        *kind = DEV_MOUSE;
        return 1;
    }
    *kind = DEV_JOY;
    if (g == G_JOYSTICK) {                       /* the first joystick, as DirectInput 3 had it */
        for (int s = 0; s < INPUT_MAX_JOYS && *slot < 0; s++)
            if (joys[s].connected)
                *slot = s;
        return *slot >= 0;
    }
    *slot = joy_guid_slot(a, 0);
    return *slot >= 0;
}

static uint32_t create_device(DInput *di, uint32_t rguid, uint32_t *out)
{
    int kind, slot;
    *out = 0;
    pthread_mutex_lock(&in_lock);
    pthread_once(&tables_once, tables_init);
    if (!device_for_guid(rguid, &kind, &slot)) {
        pthread_mutex_unlock(&in_lock);
        return DIERR_DEVICENOTREG;
    }
    if (kind == DEV_JOY && !joys[slot].connected) {
        pthread_mutex_unlock(&in_lock);
        return DIERR_DEVICENOTREG;
    }
    Device *d = dev_new(kind, slot, di->version);
    d->obj = com_new(&cls_dev, d);
    *out = d->obj;
    pthread_mutex_unlock(&in_lock);
    return DI_OK;
}

METHOD(di_QueryInterface, 3)
{
    DInput *di = di_of(ARG(0));
    uint32_t ppv = ARG(2), r = DIERR_NOINTERFACE;
    char gb[64];
    int g = guid_id(ARG(1));
    if (g == G_IUNKNOWN || g == G_IDI_A || g == G_IDI2_A || g == G_IDI7_A) {
        pthread_mutex_lock(&in_lock);
        di->refs++;
        pthread_mutex_unlock(&in_lock);
        r = DI_OK;
    }
    if (ppv)
        W32(ppv, r == DI_OK ? ARG(0) : 0);
    LOG(LOG_INPUT, "IDirectInputA::QueryInterface(%s) -> %s", guid_str(ARG(1), gb, sizeof gb), hr_str(r));
    return r;
}

METHOD(di_AddRef, 1)
{
    DInput *di = di_of(ARG(0));
    pthread_mutex_lock(&in_lock);
    int n = ++di->refs;
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "IDirectInputA::AddRef -> %d", n);
    return (uint32_t)n;
}

METHOD(di_Release, 1)
{
    DInput *di = di_of(ARG(0));
    pthread_mutex_lock(&in_lock);
    int n = di->refs > 0 ? --di->refs : 0;
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "IDirectInputA::Release -> %d", n);
    return (uint32_t)n;
}

METHOD(di_CreateDevice, 4)
{
    DInput *di = di_of(ARG(0));
    uint32_t rguid = ARG(1), pp = ARG(2), outer = ARG(3), r, obj = 0;
    char gb[64];
    if (!pp || !rguid)
        r = DIERR_INVALIDPARAM;
    else if (outer)
        r = DIERR_NOAGGREGATION;
    else
        r = create_device(di, rguid, &obj);
    if (pp)
        W32(pp, obj);
    LOG(LOG_INPUT, "IDirectInputA::CreateDevice(%s) -> %s %08x%s%s", guid_str(rguid, gb, sizeof gb), hr_str(r), obj,
        obj ? " = " : "", obj ? dev_of(obj)->label : "");
    return r;
}

static const char *devtype_filter_str(uint32_t t)
{
    switch (t & 0xff) {
    case 0: return "ALL";
    case 1: return "DEVICE";
    case DIDEVTYPE_MOUSE: return "MOUSE";
    case DIDEVTYPE_KEYBOARD: return "KEYBOARD";
    case DIDEVTYPE_JOYSTICK: return "JOYSTICK";
    }
    return "?";
}

METHOD(di_EnumDevices, 5)
{
    DInput *di = di_of(ARG(0));
    uint32_t type = ARG(1) & 0xff, cb = ARG(2), ref = ARG(3), flags = ARG(4);
    if (!cb) {
        LOG(LOG_INPUT, "IDirectInputA::EnumDevices(NULL callback) -> DIERR_INVALIDPARAM");
        return DIERR_INVALIDPARAM;
    }
    int kinds[2 + INPUT_MAX_JOYS], slots[2 + INPUT_MAX_JOYS], n = 0;
    pthread_mutex_lock(&in_lock);
    if (!(flags & DIEDFL_FORCEFEEDBACK)) {
        if (!type || type == DIDEVTYPE_MOUSE)
            kinds[n] = DEV_MOUSE, slots[n++] = -1;
        if (!type || type == DIDEVTYPE_KEYBOARD)
            kinds[n] = DEV_KEYBOARD, slots[n++] = -1;
    }
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        if (joys[s].connected && (!type || type == DIDEVTYPE_JOYSTICK) && (joys[s].ff || !(flags & DIEDFL_FORCEFEEDBACK)))
            kinds[n] = DEV_JOY, slots[n++] = s;
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "IDirectInputA::EnumDevices(%s, cb %08x, ref %08x, flags %x%s%s) -> %d devices",
        devtype_filter_str(type), cb, ref, flags, flags & DIEDFL_ATTACHEDONLY ? " ATTACHEDONLY" : "",
        flags & DIEDFL_FORCEFEEDBACK ? " FORCEFEEDBACK" : "", n);
    uint32_t size = di->version >= 0x500 ? SZ_DEVINST : SZ_DEVINST_DX3;
    uint32_t p = galloc(size);
    for (int k = 0; k < n; k++) {
        pthread_mutex_lock(&in_lock);
        int ok = kinds[k] != DEV_JOY || joys[slots[k]].connected;
        if (ok)
            fill_devinst(p, size, kinds[k], slots[k]);
        pthread_mutex_unlock(&in_lock);
        if (!ok)
            continue;
        LOG(LOG_INPUT, "  %s \"%s\"", kinds[k] == DEV_JOY ? "joystick" : kinds[k] == DEV_MOUSE ? "mouse" : "keyboard",
            gstr(p + 0x28));
        if (!guest_call(c, cb, 2, p, ref))
            break;
    }
    gfree(p);
    return DI_OK;
}

METHOD(di_GetDeviceStatus, 2)
{
    char gb[64];
    int kind, slot;
    pthread_mutex_lock(&in_lock);
    int known = ARG(1) && device_for_guid(ARG(1), &kind, &slot);
    uint32_t r = !known ? DIERR_DEVICENOTREG : kind == DEV_JOY && !joys[slot].connected ? DI_FALSE : DI_OK;
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "IDirectInputA::GetDeviceStatus(%s) -> %s", guid_str(ARG(1), gb, sizeof gb),
        r == DI_FALSE ? "DI_NOTATTACHED" : hr_str(r));
    return r;
}

METHOD(di_RunControlPanel, 3)
{
    LOG(LOG_INPUT, "IDirectInputA::RunControlPanel(hwnd %x, flags %x) -> DI_OK (no control panel)", ARG(1), ARG(2));
    return DI_OK;
}

METHOD(di_Initialize, 3)
{
    LOG(LOG_INPUT, "IDirectInputA::Initialize(hinst %x, version %x) -> DI_OK", ARG(1), ARG(2));
    return DI_OK;
}

METHOD(di_FindDevice, 4)
{
    char gb[64];
    uint32_t name = ARG(2), out = ARG(3), r = DIERR_DEVICENOTREG;
    if (!name || !out)
        r = DIERR_INVALIDPARAM;
    else {
        pthread_mutex_lock(&in_lock);
        if (!strcmp(gstr(name), "Keyboard"))
            guid_write(out, G_SYSKEYBOARD), r = DI_OK;
        else if (!strcmp(gstr(name), "Mouse"))
            guid_write(out, G_SYSMOUSE), r = DI_OK;
        for (int s = 0; s < INPUT_MAX_JOYS && r != DI_OK; s++)
            if (joys[s].connected && !strcmp(gstr(name), joys[s].name))
                joy_guid_write(out, s, 0), r = DI_OK;
        pthread_mutex_unlock(&in_lock);
    }
    LOG(LOG_INPUT, "IDirectInput2A::FindDevice(%s, \"%s\") -> %s", guid_str(ARG(1), gb, sizeof gb),
        name ? gstr(name) : "", hr_str(r));
    return r;
}

METHOD(di_CreateDeviceEx, 5)
{
    DInput *di = di_of(ARG(0));
    uint32_t rguid = ARG(1), riid = ARG(2), pp = ARG(3), outer = ARG(4), r, obj = 0;
    char gb[64], ib[64];
    if (!pp || !rguid)
        r = DIERR_INVALIDPARAM;
    else if (outer)
        r = DIERR_NOAGGREGATION;
    else if (!is_device_iid(guid_id(riid)))
        r = DIERR_NOINTERFACE;
    else
        r = create_device(di, rguid, &obj);
    if (pp)
        W32(pp, obj);
    LOG(LOG_INPUT, "IDirectInput7A::CreateDeviceEx(%s, %s) -> %s %08x", guid_str(rguid, gb, sizeof gb),
        guid_str(riid, ib, sizeof ib), hr_str(r), obj);
    return r;
}

API(DINPUT, DirectInputCreateA, 4)
{
    uint32_t hinst = ARG(0), version = ARG(1), pp = ARG(2), outer = ARG(3), r, obj = 0;
    if (!pp)
        r = DIERR_INVALIDPARAM;
    else if (outer)
        r = DIERR_NOAGGREGATION;
    else if (version > 0x700)
        r = DIERR_OLDDIRECTINPUTVERSION;        /* asks for a newer DirectInput than ours */
    else if (version < 0x300)
        r = DIERR_BETADIRECTINPUTVERSION;
    else {
        DInput *di = calloc(1, sizeof *di);
        di->version = version;
        di->refs = 1;
        di->obj = obj = com_new(&cls_di, di);
        r = DI_OK;
    }
    if (pp)
        W32(pp, obj);
    LOG(LOG_INPUT, "DirectInputCreateA(hinst %x, version %x, outer %x) -> %s %08x", hinst, version, outer, hr_str(r),
        obj);
    return r;
}

/* DirectX 7's own entry (TD6): the interface asked for by its IID; our object answers as
   IDirectInputA, 2A and 7A alike */
API(DINPUT, DirectInputCreateEx, 5)
{
    uint32_t hinst = ARG(0), version = ARG(1), riid = ARG(2), pp = ARG(3), outer = ARG(4), r, obj = 0;
    if (!pp)
        r = DIERR_INVALIDPARAM;
    else if (outer)
        r = DIERR_NOAGGREGATION;
    else if (version > 0x700)
        r = DIERR_OLDDIRECTINPUTVERSION;
    else if (version < 0x300)
        r = DIERR_BETADIRECTINPUTVERSION;
    else {
        DInput *di = calloc(1, sizeof *di);
        di->version = version;
        di->refs = 1;
        di->obj = obj = com_new(&cls_di, di);
        r = DI_OK;
    }
    if (pp)
        W32(pp, obj);
    LOG(LOG_INPUT, "DirectInputCreateEx(hinst %x, version %x, iid %08x, outer %x) -> %s %08x", hinst, version,
        riid ? R32(riid) : 0, outer, hr_str(r), obj);
    return r;
}

/* ------------------------------------------------------------------ scripted keys (TD6_KEYS) */
typedef struct { unsigned long at, release; int dik, hold, state; } ScriptKey;
static ScriptKey *script;
static int nscript;
static pthread_once_t script_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t script_lock = PTHREAD_MUTEX_INITIALIZER;

static void script_load(void)
{
    const char *env = getenv("TD6_KEYS");
    if (!env || !*env)
        return;
    char *copy = strdup(env), *save = NULL;
    for (char *tok = strtok_r(copy, ", \t\n", &save); tok; tok = strtok_r(NULL, ", \t\n", &save)) {
        char *colon = strchr(tok, ':'), *end;
        unsigned long at = colon ? strtoul(tok, &end, 10) : 0;
        if (!colon || end != colon) {
            LOGA("TD6_KEYS: \"%s\" is not frame:KEY", tok);
            continue;
        }
        char *name = colon + 1, *star = strchr(name, '*');
        int hold = 3;
        if (star) {
            *star = 0;
            hold = atoi(star + 1) > 0 ? atoi(star + 1) : 1;
        }
        int dik = input_dik_from_name(name);
        if (!dik) {
            LOGA("TD6_KEYS: unknown key \"%s\"", name);
            continue;
        }
        script = realloc(script, (nscript + 1) * sizeof *script);
        script[nscript++] = (ScriptKey){at, 0, dik, hold, 0};
    }
    free(copy);
    LOGA("TD6_KEYS: %d scripted key presses", nscript);
}

void input_frame(unsigned long frame)
{
    pad_frame(frame);
    pthread_once(&script_once, script_load);
    if (!nscript)
        return;
    pthread_mutex_lock(&script_lock);
    for (int k = 0; k < nscript; k++) {
        ScriptKey *s = &script[k];
        int down = -1;
        if (s->state == 0 && frame >= s->at) {
            s->state = 1;
            s->release = frame + (unsigned long)s->hold;
            down = 1;
        } else if (s->state == 1 && frame >= s->release) {
            s->state = 2;
            down = 0;
        }
        if (down < 0)
            continue;
        LOGA("TD6_KEYS: frame %lu: %s %s", frame, dik_names[s->dik] ? dik_names[s->dik] : "?", down ? "down" : "up");
        input_key(s->dik, down);
        if (input_script_key_hook)
            input_script_key_hook(s->dik, down);
    }
    pthread_mutex_unlock(&script_lock);
}

/* ------------------------------------------------------------------ init */
#define DI_VTBL(X) X(QueryInterface) X(AddRef) X(Release) X(CreateDevice) X(EnumDevices) \
    X(GetDeviceStatus) X(RunControlPanel) X(Initialize) X(FindDevice) X(CreateDeviceEx)
#define DEV_VTBL(X) X(QueryInterface) X(AddRef) X(Release) X(GetCapabilities) X(EnumObjects) \
    X(GetProperty) X(SetProperty) X(Acquire) X(Unacquire) X(GetDeviceState) X(GetDeviceData) \
    X(SetDataFormat) X(SetEventNotification) X(SetCooperativeLevel) X(GetObjectInfo) \
    X(GetDeviceInfo) X(RunControlPanel) X(Initialize) X(CreateEffect) X(EnumEffects) \
    X(GetEffectInfo) X(GetForceFeedbackState) X(SendForceFeedbackCommand) \
    X(EnumCreatedEffectObjects) X(Escape) X(Poll) X(SendDeviceData) X(EnumEffectsInFile) \
    X(WriteEffectToFile)
#define EFF_VTBL(X) X(QueryInterface) X(AddRef) X(Release) X(Initialize) X(GetEffectGuid) \
    X(GetParameters) X(SetParameters) X(Start) X(Stop) X(GetEffectStatus) X(Download) X(Unload) \
    X(Escape)
#define NAME_OF(m) #m,
#define DI_FN(m) di_##m,
#define DEV_FN(m) dev_##m,
#define EFF_FN(m) eff_##m,

void dinput_init(void)
{
    static void (*const di_fns[])(CPU *) = { DI_VTBL(DI_FN) };
    static const char *const di_names[] = { DI_VTBL(NAME_OF) };
    static void (*const dev_fns[])(CPU *) = { DEV_VTBL(DEV_FN) };
    static const char *const dev_names[] = { DEV_VTBL(NAME_OF) };
    static void (*const eff_fns[])(CPU *) = { EFF_VTBL(EFF_FN) };
    static const char *const eff_names[] = { EFF_VTBL(NAME_OF) };
    _Static_assert(sizeof di_fns / sizeof *di_fns == 10, "IDirectInput7A has 10 methods");
    _Static_assert(sizeof dev_fns / sizeof *dev_fns == 29, "IDirectInputDevice7A has 29 methods");
    _Static_assert(sizeof eff_fns / sizeof *eff_fns == 13, "IDirectInputEffect has 13 methods");
    /* one vtable per object kind, laid out as the newest interface (IDirectInput7A,
       IDirectInputDevice7A): the older interfaces are prefixes of it */
    com_class_init(&cls_di, "IDirectInputA", 10, di_fns, di_names);
    com_class_init(&cls_dev, "IDirectInputDevice2A", 29, dev_fns, dev_names);
    com_class_init(&cls_eff, "IDirectInputEffect", 13, eff_fns, eff_names);
    pthread_once(&tables_once, tables_init);
    const char *v = getenv("TD6_LOG_INPUT_VERBOSE");
    verbose = v && *v && strcmp(v, "0");
    pthread_once(&script_once, script_load);
    int nj = 0;
    pthread_mutex_lock(&in_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        nj += joys[s].connected;
    pthread_mutex_unlock(&in_lock);
    LOG(LOG_INPUT, "dinput: ready (%d joysticks connected%s)", nj, verbose ? ", verbose" : "");
}

