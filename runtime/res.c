/* Resources in the mapped Game.exe (.rsrc): bitmaps, icons, dialogs, version info. */
#include <strings.h>
#include "runtime.h"

static uint32_t rsrc_base(void)
{
    uint32_t pe = image_base + R32(image_base + 0x3c);
    return image_base + R32(pe + 24 + 96 + 8 * 2);
}

static uint32_t dir_lookup(uint32_t root, uint32_t dir, uint32_t id, int any)
{
    uint32_t named = R16(dir + 12), ids = R16(dir + 14);
    for (uint32_t i = 0; i < named + ids; i++) {
        uint32_t e = dir + 16 + 8 * i;
        uint32_t name = R32(e), off = R32(e + 4);
        if (any || (!(name & 0x80000000u) && name == id))
            return off;
    }
    (void)root;
    return 0xffffffffu;
}

int resource_find(uint32_t type, uint32_t name, uint32_t *data, uint32_t *size)
{
    uint32_t root = rsrc_base();
    if (root == image_base || name >= 0x10000)
        return 0;
    uint32_t o = dir_lookup(root, root, type, 0);
    if (o == 0xffffffffu || !(o & 0x80000000u)) return 0;
    o = dir_lookup(root, root + (o & 0x7fffffffu), name, 0);
    if (o == 0xffffffffu || !(o & 0x80000000u)) return 0;
    o = dir_lookup(root, root + (o & 0x7fffffffu), 0, 1);      /* first language */
    if (o == 0xffffffffu || (o & 0x80000000u)) return 0;
    uint32_t de = root + o;
    *data = image_base + R32(de);
    *size = R32(de + 4);
    return 1;
}

/* ------------------------------------------------------------------ VERSION.dll
   Only Game.exe's own version block is available (the game reads its version number). */
static int version_block(const char *path, uint32_t *data, uint32_t *size)
{
    const char *base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    if (strcasecmp(base, "TD6.exe") && strcasecmp(base, "TD6.exe\""))
        return 0;
    return resource_find(16, 1, data, size);
}

API(VERSION, GetFileVersionInfoSizeA, 2)
{
    uint32_t d, n;
    if (ARG(1)) W32(ARG(1), 0);
    int ok = version_block(gstr(ARG(0)), &d, &n);
    LOG(LOG_API, "GetFileVersionInfoSizeA(\"%s\") -> %u", gstr(ARG(0)), ok ? n : 0);
    return ok ? n : 0;
}

API(VERSION, GetFileVersionInfoA, 4)
{
    uint32_t d, n;
    if (!version_block(gstr(ARG(0)), &d, &n))
        return 0;
    memcpy(gptr(ARG(3)), gptr(d), n < ARG(2) ? n : ARG(2));
    return 1;
}

API(VERSION, VerQueryValueA, 4)
{
    const char *q = gstr(ARG(1));
    uint32_t blk = ARG(0);
    if (!strcmp(q, "\\")) {
        /* VS_VERSIONINFO: wLength, wValueLength, wType, L"VS_VERSION_INFO", pad -> VS_FIXEDFILEINFO */
        uint32_t ffi = blk + 40;
        if (R32(ffi) != 0xfeef04bdu) return 0;
        W32(ARG(2), ffi);
        W32(ARG(3), R16(blk + 2));
        return 1;
    }
    LOG(LOG_ALWAYS, "VerQueryValueA(\"%s\") not supported", q);
    return 0;
}
