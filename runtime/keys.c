/* Keyboard tables: Windows virtual keys <-> set-1 scan codes (= DirectInput DIK codes for
   the main block), key names as GetKeyNameText gives them (US layout). */
#include <ctype.h>
#include "runtime.h"
#include "win.h"

static const struct { uint8_t vk; uint16_t scan; const char *name; } keys[] = {
    {0x1b, 0x01, "Esc"}, {'1', 0x02, "1"}, {'2', 0x03, "2"}, {'3', 0x04, "3"}, {'4', 0x05, "4"},
    {'5', 0x06, "5"}, {'6', 0x07, "6"}, {'7', 0x08, "7"}, {'8', 0x09, "8"}, {'9', 0x0a, "9"},
    {'0', 0x0b, "0"}, {0xbd, 0x0c, "-"}, {0xbb, 0x0d, "="}, {0x08, 0x0e, "Backspace"},
    {0x09, 0x0f, "Tab"}, {'Q', 0x10, "Q"}, {'W', 0x11, "W"}, {'E', 0x12, "E"}, {'R', 0x13, "R"},
    {'T', 0x14, "T"}, {'Y', 0x15, "Y"}, {'U', 0x16, "U"}, {'I', 0x17, "I"}, {'O', 0x18, "O"},
    {'P', 0x19, "P"}, {0xdb, 0x1a, "["}, {0xdd, 0x1b, "]"}, {0x0d, 0x1c, "Enter"},
    {0x11, 0x1d, "Ctrl"}, {'A', 0x1e, "A"}, {'S', 0x1f, "S"}, {'D', 0x20, "D"}, {'F', 0x21, "F"},
    {'G', 0x22, "G"}, {'H', 0x23, "H"}, {'J', 0x24, "J"}, {'K', 0x25, "K"}, {'L', 0x26, "L"},
    {0xba, 0x27, ";"}, {0xde, 0x28, "'"}, {0xc0, 0x29, "`"}, {0x10, 0x2a, "Shift"},
    {0xdc, 0x2b, "\\"}, {'Z', 0x2c, "Z"}, {'X', 0x2d, "X"}, {'C', 0x2e, "C"}, {'V', 0x2f, "V"},
    {'B', 0x30, "B"}, {'N', 0x31, "N"}, {'M', 0x32, "M"}, {0xbc, 0x33, ","}, {0xbe, 0x34, "."},
    {0xbf, 0x35, "/"}, {0xa1, 0x36, "Right Shift"}, {0x6a, 0x37, "Num *"}, {0x12, 0x38, "Alt"},
    {0x20, 0x39, "Space"}, {0x14, 0x3a, "Caps Lock"}, {0x70, 0x3b, "F1"}, {0x71, 0x3c, "F2"},
    {0x72, 0x3d, "F3"}, {0x73, 0x3e, "F4"}, {0x74, 0x3f, "F5"}, {0x75, 0x40, "F6"},
    {0x76, 0x41, "F7"}, {0x77, 0x42, "F8"}, {0x78, 0x43, "F9"}, {0x79, 0x44, "F10"},
    {0x90, 0x45, "Pause"}, {0x91, 0x46, "Scroll Lock"}, {0x67, 0x47, "Num 7"}, {0x68, 0x48, "Num 8"},
    {0x69, 0x49, "Num 9"}, {0x6d, 0x4a, "Num -"}, {0x64, 0x4b, "Num 4"}, {0x65, 0x4c, "Num 5"},
    {0x66, 0x4d, "Num 6"}, {0x6b, 0x4e, "Num +"}, {0x61, 0x4f, "Num 1"}, {0x62, 0x50, "Num 2"},
    {0x63, 0x51, "Num 3"}, {0x60, 0x52, "Num 0"}, {0x6e, 0x53, "Num Del"}, {0x7a, 0x57, "F11"},
    {0x7b, 0x58, "F12"}, {0x26, 0x148, "Up"}, {0x25, 0x14b, "Left"}, {0x27, 0x14d, "Right"},
    {0x28, 0x150, "Down"}, {0x24, 0x147, "Home"}, {0x23, 0x14f, "End"}, {0x21, 0x149, "Page Up"},
    {0x22, 0x151, "Page Down"}, {0x2d, 0x152, "Insert"}, {0x2e, 0x153, "Delete"},
    {0x6f, 0x135, "Num /"}, {0xa3, 0x11d, "Right Ctrl"}, {0xa5, 0x138, "Right Alt"},
};
#define NKEYS (int)(sizeof keys / sizeof *keys)

uint32_t vk_to_scan(uint32_t vk)
{
    for (int i = 0; i < NKEYS; i++)
        if (keys[i].vk == vk) return keys[i].scan & 0xff;
    return 0;
}

uint32_t scan_to_vk(uint32_t scan)
{
    for (int i = 0; i < NKEYS; i++)
        if ((keys[i].scan & 0xff) == (scan & 0xff) && (keys[i].scan >> 8) == ((scan >> 8) & 1))
            return keys[i].vk;
    for (int i = 0; i < NKEYS; i++)
        if ((keys[i].scan & 0xff) == (scan & 0xff)) return keys[i].vk;
    return 0;
}

const char *scan_name(uint32_t scan)
{
    for (int i = 0; i < NKEYS; i++)
        if (keys[i].scan == scan) return keys[i].name;
    for (int i = 0; i < NKEYS; i++)
        if ((keys[i].scan & 0xff) == (scan & 0xff)) return keys[i].name;
    return "";
}

int vk_to_char(uint32_t vk, int shift)
{
    static const char *row = "1234567890";
    static const char *srow = "!@#$%^&*()";
    if (vk >= 'A' && vk <= 'Z') return shift ? (int)vk : tolower((int)vk);
    if (vk >= '0' && vk <= '9') return shift ? srow[strchr(row, (int)vk) - row] : (int)vk;
    switch (vk) {
    case 0x20: return ' ';
    case 0x0d: return '\r';
    case 0x08: return 8;
    case 0x09: return 9;
    case 0x1b: return 27;
    case 0xbd: return shift ? '_' : '-';
    case 0xbb: return shift ? '+' : '=';
    case 0xdb: return shift ? '{' : '[';
    case 0xdd: return shift ? '}' : ']';
    case 0xba: return shift ? ':' : ';';
    case 0xde: return shift ? '"' : '\'';
    case 0xc0: return shift ? '~' : '`';
    case 0xdc: return shift ? '|' : '\\';
    case 0xbc: return shift ? '<' : ',';
    case 0xbe: return shift ? '>' : '.';
    case 0xbf: return shift ? '?' : '/';
    }
    if (vk >= 0x60 && vk <= 0x69) return '0' + (int)(vk - 0x60);
    return 0;
}
