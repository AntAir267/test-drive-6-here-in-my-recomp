/* KERNEL32: process, memory, strings, code pages, modules, errors. Files are in files.c,
   threads and synchronisation in sync.c. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"
#include "image.h"

extern int close_file_handle(uint32_t h);
extern void set_current_cpu(CPU *c);

/* ------------------------------------------------------------------ errors */
void set_last_error(uint32_t e)
{
    CPU *c = current_cpu();
    if (c) W32(c->fs_base + 0x34, e);
}

uint32_t get_last_error(void)
{
    CPU *c = current_cpu();
    return c ? R32(c->fs_base + 0x34) : 0;
}

API(KERNEL32, GetLastError, 0) { return get_last_error(); }

API(KERNEL32, CloseHandle, 1)
{
    uint32_t h = ARG(0);
    switch (handle_type(h)) {
    case H_FILE: return close_file_handle(h);
    case H_EVENT: case H_MUTEX: case H_THREAD: case H_SEMAPHORE: case H_MISC:
        LOG(LOG_THREAD, "CloseHandle(%x)", h);
        handle_close(h);               /* the objects stay allocated: waiters may still hold them */
        return 1;
    default:
        set_last_error(6);
        return 0;
    }
}

/* ------------------------------------------------------------------ process */
static uint32_t cmdline_a, cmdline_w_env, env_a, module_name;

API(KERNEL32, GetCommandLineA, 0)
{
    if (!cmdline_a) {
        char b[600];
        snprintf(b, sizeof b, "\"%s\\TD6.exe\" NoProps", virtual_game_root());
        cmdline_a = gstrdup(b);
    }
    return cmdline_a;
}

API(KERNEL32, GetEnvironmentStrings, 0)
{
    if (!env_a) {
        static const char env[] = "WINDIR=C:\\WINDOWS\0\0";
        env_a = galloc(sizeof env);
        memcpy(gptr(env_a), env, sizeof env);
    }
    return env_a;
}

API(KERNEL32, GetEnvironmentStringsW, 0)
{
    if (!cmdline_w_env) {
        static const char env[] = "WINDIR=C:\\WINDOWS";
        uint32_t n = sizeof env;          /* includes one terminator; add the block's second */
        cmdline_w_env = galloc(2 * (n + 1));
        for (uint32_t i = 0; i < n; i++)
            W16(cmdline_w_env + 2 * i, (uint8_t)env[i]);
        W16(cmdline_w_env + 2 * n, 0);
    }
    return cmdline_w_env;
}

API(KERNEL32, FreeEnvironmentStringsA, 1) { return 1; }
API(KERNEL32, FreeEnvironmentStringsW, 1) { return 1; }

API(KERNEL32, GetStartupInfoA, 1)
{
    memset(gptr(ARG(0)), 0, 68);
    W32(ARG(0), 68);
    return 0;
}

API(KERNEL32, GetVersion, 0)
{
    return 0x0a280105u;                  /* Windows XP (5.1 build 2600): GOG's compatibility mode */
}

API(KERNEL32, GetModuleHandleA, 1)
{
    const char *n = gstr(ARG(0));
    LOG(LOG_API, "GetModuleHandleA(%s)", n ? n : "NULL");
    if (!n)
        return image_base;
    if (!strncasecmp(n, "kernel32", 8)) return 0x7c800000u;
    if (!strncasecmp(n, "user32", 6)) return 0x7e410000u;
    set_last_error(126);
    return 0;
}

API(KERNEL32, GetModuleFileNameA, 3)
{
    char b[600];
    snprintf(b, sizeof b, "%s\\TD6.exe", virtual_game_root());
    uint32_t n = ARG(2);
    if (!n) return 0;
    gstrncpy(ARG(1), b, n);
    (void)module_name;
    return strlen(gptr(ARG(1)));
}

API(KERNEL32, ExitProcess, 1)
{
    process_exit((int)ARG(0));
}

API(KERNEL32, GetCurrentProcess, 0) { return 0xffffffffu; }

API(KERNEL32, TerminateProcess, 2)
{
    if (ARG(0) == 0xffffffffu)
        process_exit((int)ARG(1));
    return 0;
}

API(KERNEL32, GetComputerNameA, 2)
{
    const char *name = "INSANE";
    uint32_t cap = R32(ARG(1));
    uint32_t n = strlen(name);
    if (cap <= n) { W32(ARG(1), n + 1); set_last_error(111); return 0; }
    gstrncpy(ARG(0), name, cap);
    W32(ARG(1), n);
    return 1;
}

API(KERNEL32, OutputDebugStringA, 1)
{
    LOG(LOG_ALWAYS, "debug output: %s", gstr(ARG(0)));
    return 0;
}

/* ------------------------------------------------------------------ libraries */
#define FAKE_USER32 0x7e410000u

static uint32_t user32_proc(const char *name);

API(KERNEL32, LoadLibraryA, 1)
{
    const char *n = gstr(ARG(0));
    uint32_t r = 0;
    if (!strcasecmp(n, "user32.dll") || !strcasecmp(n, "user32"))
        r = FAKE_USER32;
    LOG(LOG_ALWAYS, "LoadLibraryA(\"%s\") -> %x", n, r);
    if (!r)
        set_last_error(126);              /* ERROR_MOD_NOT_FOUND: GameComms, Glide, EForum */
    return r;
}

API(KERNEL32, FreeLibrary, 1) { return 1; }

API(KERNEL32, GetProcAddress, 2)
{
    uint32_t h = ARG(0), np = ARG(1);
    const char *name = np < 0x10000 ? "(ordinal)" : gstr(np);
    uint32_t r = 0;
    if (h == FAKE_USER32 && np >= 0x10000)
        r = user32_proc(name);
    LOG(LOG_ALWAYS, "GetProcAddress(%x, \"%s\") -> %x", h, name, r);
    if (!r) set_last_error(127);
    return r;
}

static uint32_t user32_proc(const char *name)
{
    for (int i = 0; i < import_count; i++)
        if (!strcasecmp(import_table[i].dll, "USER32.dll") && !strcmp(import_table[i].name, name))
            return R32(import_table[i].iat);
    return 0;
}

/* ------------------------------------------------------------------ memory */
API(KERNEL32, HeapCreate, 3)
{
    uint32_t h = heap_create(ARG(1), ARG(2));
    LOG(LOG_MEM, "HeapCreate(%x, %x, %x) -> %x", ARG(0), ARG(1), ARG(2), h);
    return h;
}

API(KERNEL32, HeapDestroy, 1) { return heap_destroy(ARG(0)); }

API(KERNEL32, HeapAlloc, 3)
{
    return heap_alloc(ARG(0), ARG(2), (ARG(1) & 8) != 0);
}

API(KERNEL32, HeapFree, 3)
{
    return heap_free(ARG(0), ARG(2));
}

API(KERNEL32, HeapReAlloc, 4)
{
    return heap_realloc(ARG(0), ARG(2), ARG(3), (ARG(1) & 8) != 0, (ARG(1) & 0x10) != 0);
}

API(KERNEL32, HeapSize, 3)
{
    return heap_size(ARG(0), ARG(2));
}

API(KERNEL32, VirtualAlloc, 4)
{
    uint32_t r = vm_alloc(ARG(0), ARG(1), !(ARG(2) & 0x1000));
    LOG(LOG_MEM, "VirtualAlloc(%08x, %x, %x, %x) -> %08x", ARG(0), ARG(1), ARG(2), ARG(3), r);
    if (!r) set_last_error(8);
    return r;
}

API(KERNEL32, VirtualFree, 3)
{
    return vm_free(ARG(0), ARG(1), (ARG(2) & 0x8000) != 0);
}

API(KERNEL32, GlobalAlloc, 2)
{
    uint32_t p = heap_alloc(process_heap, ARG(1) ? ARG(1) : 1, (ARG(0) & 0x40) != 0);
    LOG(LOG_MEM, "GlobalAlloc(%x, %u) -> %08x", ARG(0), ARG(1), p);
    return p;
}

API(KERNEL32, GlobalLock, 1) { return ARG(0); }
API(KERNEL32, GlobalUnlock, 1) { return 1; }
API(KERNEL32, GlobalHandle, 1) { return ARG(0); }
API(KERNEL32, GlobalFree, 1) { return heap_free(process_heap, ARG(0)) ? 0 : ARG(0); }

static int mapped(uint32_t p, uint32_t n)
{
    if (p >= IMAGE_BASE && (uint64_t)p + n <= IMAGE_BASE + (uint64_t)IMAGE_SIZE)
        return 1;
    uint32_t b = vm_query_base(p);
    return b != 0 && (n == 0 || vm_query_base(p + n - 1) == b);
}

API(KERNEL32, IsBadReadPtr, 2) { return ARG(1) && !mapped(ARG(0), ARG(1)); }
API(KERNEL32, IsBadWritePtr, 2) { return ARG(1) && !mapped(ARG(0), ARG(1)); }
API(KERNEL32, IsBadCodePtr, 1)
{
    uint32_t p = ARG(0);
    return !(p >= TEXT_START && p < TEXT_END) && !thunk_name(p);
}

/* ------------------------------------------------------------------ strings */
API(KERNEL32, lstrlenA, 1) { return ARG(0) ? strlen(gptr(ARG(0))) : 0; }

API(KERNEL32, lstrcpyA, 2)
{
    strcpy(gptr(ARG(0)), gptr(ARG(1)));
    return ARG(0);
}

/* lstrcmpA compares with the user locale's word sort; for the program's ASCII names a
   case-sensitive comparison with lower case first is close enough. */
API(KERNEL32, lstrcmpA, 2)
{
    const unsigned char *a = (const unsigned char *)gptr(ARG(0)), *b = (const unsigned char *)gptr(ARG(1));
    if (!a || !b) return a ? 1 : b ? -1 : 0;
    for (;; a++, b++) {
        int x = tolower(*a), y = tolower(*b);
        if (x != y) return x < y ? -1 : 1;
        if (*a != *b) return islower(*a) ? -1 : 1;
        if (!*a) return 0;
    }
}

/* Windows-1252 bytes 0x80-0x9f */
static const uint16_t cp1252_hi[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039,
    0x0152, 0x008d, 0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178};

static uint16_t to_wide(uint8_t b) { return b >= 0x80 && b < 0xa0 ? cp1252_hi[b - 0x80] : b; }
static uint8_t to_narrow(uint16_t w)
{
    if (w < 0x80 || (w >= 0xa0 && w < 0x100)) return (uint8_t)w;
    for (int i = 0; i < 32; i++)
        if (cp1252_hi[i] == w) return 0x80 + i;
    return '?';
}

API(KERNEL32, MultiByteToWideChar, 6)
{
    uint32_t src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = strlen(gptr(src)) + 1;
    if (!cap) return n;
    if ((uint32_t)n > cap) { set_last_error(122); return 0; }
    for (int i = 0; i < n; i++)
        W16(dst + 2 * i, to_wide(R8(src + i)));
    return n;
}

API(KERNEL32, WideCharToMultiByte, 8)
{
    uint32_t src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) { n = 0; while (R16(src + 2 * n)) n++; n++; }
    if (ARG(7)) W32(ARG(7), 0);
    if (!cap) return n;
    if ((uint32_t)n > cap) { set_last_error(122); return 0; }
    for (int i = 0; i < n; i++)
        W8(dst + i, to_narrow(R16(src + 2 * i)));
    return n;
}

API(KERNEL32, GetACP, 0) { return 1252; }
API(KERNEL32, GetOEMCP, 0) { return 437; }

API(KERNEL32, GetCPInfo, 2)
{
    uint32_t p = ARG(1);
    memset(gptr(p), 0, 20);
    W32(p, 1);
    W8(p + 4, '?');
    return 1;
}

static uint16_t ctype1(uint16_t ch)
{
    uint16_t f = 0;
    unsigned c = ch;
    if (c < 0x20 || c == 0x7f || (c >= 0x80 && c < 0xa0)) f |= 0x20;        /* C1_CNTRL */
    if (c == ' ' || (c >= 9 && c <= 13) || c == 0xa0) f |= 0x08;             /* C1_SPACE */
    if (c == ' ' || c == '\t' || c == 0xa0) f |= 0x40;                       /* C1_BLANK */
    if (c >= '0' && c <= '9') f |= 0x04 | 0x80;                              /* DIGIT|XDIGIT */
    if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) f |= 0x80;
    int upper = (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7) ||
                c == 0x160 || c == 0x152 || c == 0x17d || c == 0x178;
    int lower = (c >= 'a' && c <= 'z') || (c >= 0xdf && c <= 0xff && c != 0xf7) ||
                c == 0x161 || c == 0x153 || c == 0x17e || c == 0x192 || c == 0xaa || c == 0xb5 || c == 0xba;
    if (upper) f |= 0x01 | 0x100;
    if (lower) f |= 0x02 | 0x100;
    if (!(f & (0x20 | 0x08 | 0x04 | 0x100)) && c > ' ' && c != 0x7f) f |= 0x10;   /* C1_PUNCT */
    if (c == 0xa0) f &= ~0x10;
    return f;
}

API(KERNEL32, GetStringTypeA, 5)
{
    uint32_t type = ARG(1), src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = strlen(gptr(src)) + 1;
    for (int i = 0; i < n; i++)
        W16(out + 2 * i, type == 1 ? ctype1(to_wide(R8(src + i))) : 0);
    return 1;
}

API(KERNEL32, GetStringTypeW, 4)
{
    uint32_t type = ARG(0), src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) { n = 0; while (R16(src + 2 * n)) n++; n++; }
    for (int i = 0; i < n; i++)
        W16(out + 2 * i, type == 1 ? ctype1(R16(src + 2 * i)) : 0);
    return 1;
}

static uint16_t case_map(uint16_t ch, uint32_t flags)
{
    if (flags & 0x100) {                 /* LCMAP_LOWERCASE */
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 0xc0 && ch <= 0xde && ch != 0xd7)) return ch + 0x20;
    } else if (flags & 0x200) {          /* LCMAP_UPPERCASE */
        if ((ch >= 'a' && ch <= 'z') || (ch >= 0xe0 && ch <= 0xfe && ch != 0xf7)) return ch - 0x20;
    }
    return ch;
}

API(KERNEL32, LCMapStringA, 6)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = strlen(gptr(src)) + 1;
    if (flags & ~0x300u)
        LOG(LOG_ALWAYS, "LCMapStringA: flags %x not supported", flags);
    if (!cap) return n;
    if ((uint32_t)n > cap) { set_last_error(122); return 0; }
    for (int i = 0; i < n; i++)
        W8(dst + i, to_narrow(case_map(to_wide(R8(src + i)), flags)));
    return n;
}

API(KERNEL32, LCMapStringW, 6)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), cap = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) { n = 0; while (R16(src + 2 * n)) n++; n++; }
    if (flags & ~0x300u)
        LOG(LOG_ALWAYS, "LCMapStringW: flags %x not supported", flags);
    if (!cap) return n;
    if ((uint32_t)n > cap) { set_last_error(122); return 0; }
    for (int i = 0; i < n; i++)
        W16(dst + 2 * i, case_map(R16(src + 2 * i), flags));
    return n;
}

/* ------------------------------------------------------------------ exceptions */
static uint32_t unhandled_filter;

API(KERNEL32, SetUnhandledExceptionFilter, 1)
{
    uint32_t old = unhandled_filter;
    unhandled_filter = ARG(0);
    return old;
}

API(KERNEL32, UnhandledExceptionFilter, 1) { return 0; }

API(KERNEL32, RaiseException, 4)
{
    uint32_t code = ARG(0);
    if (code == 0xe06d7363u && ARG(2) >= 3) {
        /* C++ throw: args = magic, object, ThrowInfo -> CatchableTypeArray -> type name */
        uint32_t args = ARG(3), ti = R32(args + 8);
        const char *tname = "?";
        if (ti) {
            uint32_t cta = R32(ti + 12);
            if (cta && R32(cta) > 0) {
                uint32_t ct = R32(cta + 4), td = R32(ct + 4);
                if (td) tname = gstr(td + 8);
            }
        }
        fatal("the program threw a C++ exception of type %s (exceptions are not supported yet)", tname);
    }
    fatal("RaiseException(%08x) (exceptions are not supported yet)", code);
}

API(KERNEL32, RtlUnwind, 4)
{
    fatal("RtlUnwind (exceptions are not supported yet)");
}

/* ------------------------------------------------------------------ TD6's further needs */
API(KERNEL32, SetLastError, 1) { set_last_error(ARG(0)); return 0; }

API(KERNEL32, GetVersionExA, 1)
{
    uint32_t p = ARG(0);
    uint32_t size = R32(p);
    if (size < 148) return 0;
    memset(gptr(p + 4), 0, size - 4);
    W32(p + 4, 5);                       /* XP, as GetVersion says */
    W32(p + 8, 1);
    W32(p + 12, 2600);
    W32(p + 16, 2);                      /* VER_PLATFORM_WIN32_NT */
    gstrncpy(p + 20, "Service Pack 3", 128);
    return 1;
}

API(KERNEL32, GetCurrentThreadId, 0)
{
    CPU *c2 = current_cpu();
    return 0x100 + 4 * (c2 ? (uint32_t)c2->thread_index : 0);
}

/* Thread-local slots: the TEB's own 64 (fs:[0xe10]), as Windows keeps them */
static uint64_t tls_used;
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
API(KERNEL32, TlsAlloc, 0)
{
    pthread_mutex_lock(&tls_lock);
    uint32_t r = 0xffffffffu;
    for (int i = 0; i < 64; i++)
        if (!(tls_used & (1ull << i))) {
            tls_used |= 1ull << i;
            r = (uint32_t)i;
            break;
        }
    pthread_mutex_unlock(&tls_lock);
    return r;
}
API(KERNEL32, TlsGetValue, 1)
{
    if (ARG(0) >= 64) { set_last_error(87); return 0; }
    set_last_error(0);
    return R32(c->fs_base + 0xe10 + 4 * ARG(0));
}
API(KERNEL32, TlsSetValue, 2)
{
    if (ARG(0) >= 64) { set_last_error(87); return 0; }
    W32(c->fs_base + 0xe10 + 4 * ARG(0), ARG(1));
    return 1;
}

API(KERNEL32, InterlockedIncrement, 1) { return __atomic_add_fetch((uint32_t *)gptr(ARG(0)), 1, __ATOMIC_SEQ_CST); }
API(KERNEL32, InterlockedDecrement, 1) { return __atomic_sub_fetch((uint32_t *)gptr(ARG(0)), 1, __ATOMIC_SEQ_CST); }

/* The performance counter: microseconds of the runtime's clock (virtual in --test runs) */
API(KERNEL32, QueryPerformanceFrequency, 1)
{
    W32(ARG(0), 1000000);
    W32(ARG(0) + 4, 0);
    return 1;
}
API(KERNEL32, QueryPerformanceCounter, 1)
{
    uint64_t t = (uint64_t)(now_ms() * 1000.0);
    W32(ARG(0), (uint32_t)t);
    W32(ARG(0) + 4, (uint32_t)(t >> 32));
    return 1;
}

API(KERNEL32, GetSystemInfo, 1)
{
    uint32_t p = ARG(0);
    memset(gptr(p), 0, 36);
    W32(p + 4, 0x1000);                  /* page size */
    W32(p + 8, 0x10000);
    W32(p + 12, 0x7ffeffff);
    W32(p + 16, 1);                      /* active processor mask */
    W32(p + 20, 1);                      /* one processor, as the game expects of 1999 */
    W32(p + 24, 586);                    /* PROCESSOR_INTEL_PENTIUM */
    W32(p + 28, 0x10000);
    W16(p + 32, 6);                      /* family 6 */
    W16(p + 34, 0x0703);
    return 0;
}

API(KERNEL32, GlobalMemoryStatus, 1)
{
    uint32_t p = ARG(0);
    W32(p, 32);
    W32(p + 4, 30);                      /* memory load % */
    W32(p + 8, 512u << 20);              /* 512 MB of RAM: plenty for 1999, no overflow */
    W32(p + 12, 384u << 20);
    W32(p + 16, 1024u << 20);
    W32(p + 20, 1024u << 20);
    W32(p + 24, 0x7ffe0000u);
    W32(p + 28, 0x7f000000u);
    return 0;
}

/* ---- time. --test runs see a fixed date (2000-01-01 12:00 UTC) moving with the virtual clock */
static int64_t unix_now_100ns(void)
{
    if (deterministic)
        return (int64_t)946728000 * 10000000 + (int64_t)(now_ms() * 10000.0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 10000000 + ts.tv_nsec / 100;
}
#define FT_EPOCH 116444736000000000ll    /* 1601 -> 1970 in 100 ns */
static void put_ft(uint32_t a, int64_t ft) { W32(a, (uint32_t)ft); W32(a + 4, (uint32_t)((uint64_t)ft >> 32)); }
static int64_t get_ft(uint32_t a) { return (int64_t)(((uint64_t)R32(a + 4) << 32) | R32(a)); }
static void put_systemtime(uint32_t p, int64_t ft)
{
    int64_t u100 = ft - FT_EPOCH;
    time_t t = (time_t)(u100 / 10000000);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    W16(p, tm.tm_year + 1900);
    W16(p + 2, tm.tm_mon + 1);
    W16(p + 4, tm.tm_wday);
    W16(p + 6, tm.tm_mday);
    W16(p + 8, tm.tm_hour);
    W16(p + 10, tm.tm_min);
    W16(p + 12, tm.tm_sec);
    W16(p + 14, (uint32_t)((u100 / 10000) % 1000));
}
static int64_t local_offset_100ns(void)
{
    if (deterministic) return 0;
    time_t t = time(NULL);
#ifdef _WIN32
    struct tm g, l;                     /* (no tm_gmtoff: the difference of the two readings) */
    gmtime_s(&g, &t);
    localtime_s(&l, &t);
    l.tm_isdst = g.tm_isdst = 0;
    return (int64_t)(mktime(&l) - mktime(&g)) * 10000000;
#else
    struct tm tm;
    localtime_r(&t, &tm);
    return (int64_t)tm.tm_gmtoff * 10000000;
#endif
}
API(KERNEL32, GetSystemTimeAsFileTime, 1) { put_ft(ARG(0), unix_now_100ns() + FT_EPOCH); return 0; }
API(KERNEL32, GetSystemTime, 1) { put_systemtime(ARG(0), unix_now_100ns() + FT_EPOCH); return 0; }
API(KERNEL32, GetLocalTime, 1) { put_systemtime(ARG(0), unix_now_100ns() + FT_EPOCH + local_offset_100ns()); return 0; }
API(KERNEL32, FileTimeToLocalFileTime, 2) { put_ft(ARG(1), get_ft(ARG(0)) + local_offset_100ns()); return 1; }
API(KERNEL32, FileTimeToSystemTime, 2) { put_systemtime(ARG(1), get_ft(ARG(0))); return 1; }
API(KERNEL32, GetTimeZoneInformation, 1)
{
    memset(gptr(ARG(0)), 0, 172);
    W32(ARG(0), (uint32_t)(int32_t)(-local_offset_100ns() / 600000000));   /* bias, minutes */
    return 0;                            /* TIME_ZONE_ID_UNKNOWN: no daylight rules */
}

API(KERNEL32, SetEnvironmentVariableA, 2) { return 1; }

API(KERNEL32, lstrcatA, 2)
{
    if (!ARG(0)) return 0;
    char *d = gptr(ARG(0));
    strcat(d, ARG(1) ? (const char *)gptr(ARG(1)) : "");
    return ARG(0);
}
API(KERNEL32, lstrcmpiA, 2)
{
    const char *a = ARG(0) ? gstr(ARG(0)) : "", *b = ARG(1) ? gstr(ARG(1)) : "";
    int r = strcasecmp(a, b);
    return r < 0 ? 0xffffffffu : r > 0;
}

/* CompareString: 1 less, 2 equal, 3 greater; NORM_IGNORECASE (1) honoured */
static uint32_t cmp_result(int r) { return r < 0 ? 1 : r > 0 ? 3 : 2; }
API(KERNEL32, CompareStringA, 6)
{
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    const char *a = gptr(ARG(2)), *b = gptr(ARG(4));
    size_t l1 = n1 < 0 ? strlen(a) : (size_t)n1, l2 = n2 < 0 ? strlen(b) : (size_t)n2;
    size_t n = l1 < l2 ? l1 : l2;
    int r = (ARG(1) & 1) ? strncasecmp(a, b, n) : strncmp(a, b, n);
    if (!r) r = (l1 > l2) - (l1 < l2);
    return cmp_result(r);
}
API(KERNEL32, CompareStringW, 6)
{
    int32_t n1 = (int32_t)ARG(3), n2 = (int32_t)ARG(5);
    uint32_t a = ARG(2), b = ARG(4);
    uint32_t l1 = 0, l2 = 0;
    if (n1 < 0) while (R16(a + 2 * l1)) l1++; else l1 = (uint32_t)n1;
    if (n2 < 0) while (R16(b + 2 * l2)) l2++; else l2 = (uint32_t)n2;
    for (uint32_t i = 0; i < l1 && i < l2; i++) {
        uint32_t x = R16(a + 2 * i), y = R16(b + 2 * i);
        if (ARG(1) & 1) { if (x < 128) x = (uint32_t)tolower((int)x); if (y < 128) y = (uint32_t)tolower((int)y); }
        if (x != y) return cmp_result(x < y ? -1 : 1);
    }
    return cmp_result((l1 > l2) - (l1 < l2));
}

/* Drives: C: the game's (fixed); nothing else */
API(KERNEL32, GetDriveTypeA, 1)
{
    const char *p = ARG(0) ? gstr(ARG(0)) : "C:\\";
    return (p[0] == 'C' || p[0] == 'c') ? 3 : 1;    /* DRIVE_FIXED / DRIVE_NO_ROOT_DIR */
}
