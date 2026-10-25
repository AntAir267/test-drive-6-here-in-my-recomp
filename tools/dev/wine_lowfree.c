/* (development tool, for testing the Windows build under Wine only)
   LD_PRELOAD into Wine: Wine's preloader reserves low memory (0x110000 up to 0x68000000) and
   ntdll places the process's own allocations there bottom-up, across the range TD6's program
   needs (0x400000-0x70000000, runtime/win/platform.c reserves it first thing); real Windows
   puts them elsewhere. This takes the range out of the preloader's list and holds it with a
   placeholder mapping; a request for the whole range at 0x400000 (the runtime's reservation)
   gets it, any other request inside it is refused, so Wine places its allocations elsewhere.
   Build: gcc -shared -fPIC -O2 -o build/wine_lowfree.so tools/dev/wine_lowfree.c -ldl */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

struct preload_info { void *addr; size_t size; };

#define LO 0x400000u
#define HI 0x70000000u
static int held, logit;

static void *sys_mmap(void *a, size_t n, int prot, int flags, int fd, off_t off)
{
    return (void *)syscall(SYS_mmap, a, n, prot, flags, fd, off);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    uintptr_t a = (uintptr_t)addr, e = a + len;
    if (held && addr && a < HI && e > LO) {
        if (a == LO && e >= HI - 0x100000) {      /* the runtime's reservation of the range */
            held = 0;
            syscall(SYS_munmap, (void *)LO, (size_t)(HI - LO));
            if (logit) fprintf(stderr, "lowfree: the range handed over (%lx-%lx)\n", (unsigned long)a, (unsigned long)e);
            return sys_mmap(addr, len, prot, flags, fd, off);
        }
        if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
            errno = EEXIST;
            return MAP_FAILED;
        }
        return sys_mmap(NULL, len, prot, flags, fd, off);   /* (a hint only: somewhere else) */
    }
    return sys_mmap(addr, len, prot, flags, fd, off);
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) __attribute__((alias("mmap")));

__attribute__((constructor)) static void lowfree(void)
{
    logit = getenv("LOWFREE_LOG") != NULL;
    struct preload_info *list = dlsym(RTLD_DEFAULT, "wine_main_preload_info");
    if (!list)
        return;
    for (struct preload_info *p = list; p->size; p++) {
        uintptr_t a = (uintptr_t)p->addr, e = a + p->size;
        if (e <= LO || a >= HI)
            continue;
        uintptr_t keep_end = a < LO ? LO : a;
        if (logit) fprintf(stderr, "lowfree: preload range %lx-%lx -> %lx-%lx\n", (unsigned long)a,
                           (unsigned long)e, (unsigned long)a, (unsigned long)keep_end);
        p->size = keep_end - a;
    }
    syscall(SYS_munmap, (void *)LO, (size_t)(HI - LO));
    if (sys_mmap((void *)LO, HI - LO, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == (void *)LO)
        held = 1;
}
