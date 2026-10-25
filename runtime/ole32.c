/* ole32: COM start-up and object creation. DirectSound and the intro videos' DirectShow
   streams are created this way; DirectPlay is
   parked until multiplayer (design point 6): its creation fails as if not installed. */
#include "runtime.h"
#include "sound.h"


API(OLE32, CoInitialize, 1) { return 0; }
API(OLE32, CoUninitialize, 0) { return 0; }

API(OLE32, CoCreateGuid, 1)
{
    static uint32_t n;
    uint32_t g = ARG(0);
    W32(g, 0x1d5a0000u + ++n);
    W16(g + 4, 0x4e53);
    W16(g + 6, 0x4111);
    for (int i = 0; i < 8; i++)
        W8(g + 8 + i, (uint8_t)(0x80 + i + n));
    return 0;
}

API(OLE32, CoCreateInstance, 5)
{
    uint32_t clsid = ARG(0), iid = ARG(3);
    uint32_t hr = dsound_cocreate(clsid, iid, ARG(4));
    if (hr != 0x80040154u)
        return hr;
    LOG(LOG_ALWAYS, "CoCreateInstance({" GUID_FMT "}, iid {" GUID_FMT "}): not available",
        GUID_ARGS(clsid), GUID_ARGS(iid));
    if (ARG(4)) W32(ARG(4), 0);
    return 0x80040154u;                  /* REGDB_E_CLASSNOTREG */
}
