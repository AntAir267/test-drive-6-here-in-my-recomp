/* COM objects handed to the program: a vtable of thunks in program memory and a pointer back
   to the runtime's own record of the object. */
#include <pthread.h>
#include <stdlib.h>
#include "runtime.h"

#define COM_MAGIC 0x4d4f4321u    /* "!COM" */

void com_class_init(ComClass *cls, const char *iface, int n, void (*const *methods)(CPU *),
                    const char *const *names)
{
    cls->iface = iface;
    cls->nmethods = n;
    cls->vtbl = galloc(4 * n);
    for (int i = 0; i < n; i++) {
        char *nm = malloc(strlen(iface) + strlen(names[i]) + 3);
        sprintf(nm, "%s::%s", iface, names[i]);
        W32(cls->vtbl + 4 * i, thunk_addr(methods[i], nm));
    }
}

/* The program's object holds an index into this table, not a host pointer, so program
   memory is the same on every run (tests compare it). */
static void **hosts;
static uint32_t nhosts, caphosts;
static pthread_mutex_t hlock = PTHREAD_MUTEX_INITIALIZER;

uint32_t com_new(ComClass *cls, void *host)
{
    pthread_mutex_lock(&hlock);
    if (nhosts == caphosts) {
        caphosts = caphosts ? caphosts * 2 : 256;
        hosts = realloc(hosts, caphosts * sizeof *hosts);
    }
    uint32_t id = nhosts++;
    hosts[id] = host;
    pthread_mutex_unlock(&hlock);
    uint32_t o = galloc(16);
    W32(o, cls->vtbl);
    W32(o + 4, COM_MAGIC);
    W32(o + 8, id);
    W32(o + 12, 0);
    return o;
}

void com_set_class(uint32_t obj, ComClass *cls)
{
    W32(obj, cls->vtbl);
}

void *com_host(uint32_t obj)
{
    if (!obj || R32(obj + 4) != COM_MAGIC || R32(obj + 8) >= nhosts)
        fatal("not a runtime COM object: %08x", obj);
    return hosts[R32(obj + 8)];
}

int guid_eq(uint32_t a, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8])
{
    if (!a)
        return 0;
    if (R32(a) != d1 || R16(a + 4) != d2 || R16(a + 6) != d3)
        return 0;
    for (int i = 0; i < 8; i++)
        if (R8(a + 8 + i) != d4[i])
            return 0;
    return 1;
}
