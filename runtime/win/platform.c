/* Windows host (WINDOWS.md): the POSIX pieces the runtime uses that MinGW lacks (declared in
   compat.h and the headers next to it), and what a Windows process needs before main:

   - The program's memory below 2 GB (its image at 0x400000, the arena up to 0x70000000) must
     be free for it. A Windows process may already have something there by the time it runs (a
     heap, a thread stack, a DLL an overlay injected), so the whole range is reserved first
     thing; if any of it is taken, the program starts itself again with the range reserved in
     the new process before any of that process's own code runs, and passes on its exit code.
     TD6_WIN_RELAUNCH=1 always takes that path (to test it).
   - Sleeps to the microsecond (a high-resolution waitable timer; plain Sleep rounds to the
     system's 15.6 ms tick), and the system timer at 1 ms for everything else that waits. */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fnmatch.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include "runtime.h"
#include "image.h"
#include "funcs.h"

#undef exit
#undef pthread_create
#undef nanosleep
#undef clock_nanosleep
#undef pread
#undef pipe
#undef sysconf

/* ------------------------------------------------------------------ the low range */
#define LOW_START ((uintptr_t)IMAGE_BASE)
#define LOW_END   ((uintptr_t)ARENA_END)

static int low_range_ours(void)
{
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery((void *)LOW_START, &mi, sizeof mi) && mi.State == MEM_RESERVE &&
           (uintptr_t)mi.AllocationBase == LOW_START && (uintptr_t)mi.BaseAddress + mi.RegionSize >= LOW_END;
}

static void describe_low_range(void)
{
    fprintf(stderr, "td6: the program's memory range %08llx-%08llx is in use:\n",
            (unsigned long long)LOW_START, (unsigned long long)LOW_END);
    for (uintptr_t a = LOW_START; a < LOW_END;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)a, &mi, sizeof mi)) break;
        if (mi.State != MEM_FREE) {
            char mod[MAX_PATH] = "";
            if (mi.Type == MEM_IMAGE) GetModuleFileNameA((HMODULE)mi.AllocationBase, mod, sizeof mod);
            fprintf(stderr, "  %08llx-%08llx %s %s\n", (unsigned long long)(uintptr_t)mi.BaseAddress,
                    (unsigned long long)((uintptr_t)mi.BaseAddress + mi.RegionSize),
                    mi.State == MEM_COMMIT ? "committed" : "reserved", mod);
        }
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
}

/* Start this program again, suspended, reserve the range in it, let it run; returns 1 with its
   exit code, or 0 when every try found the range taken there too. */
static int relaunch(DWORD *exit_code)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    SetEnvironmentVariableA("TD6_WIN_CHILD", "1");
    for (int attempt = 0; attempt < 8; attempt++) {
        STARTUPINFOW si = {.cb = sizeof si};
        PROCESS_INFORMATION pi;
        if (!CreateProcessW(exe, GetCommandLineW(), NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
            return 0;
        if (VirtualAllocEx(pi.hProcess, (void *)LOW_START, LOW_END - LOW_START, MEM_RESERVE, PAGE_NOACCESS)) {
            ResumeThread(pi.hThread);
            WaitForSingleObject(pi.hProcess, INFINITE);
            *exit_code = 1;
            GetExitCodeProcess(pi.hProcess, exit_code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 1;
        }
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return 0;
}

static void exit_last(void);

__attribute__((constructor(101))) static void win_start(void)
{
    const char *child = getenv("TD6_WIN_CHILD");
    const char *force = getenv("TD6_WIN_RELAUNCH");
    int ok = 0;
    if (child && *child)
        ok = low_range_ours();          /* the parent reserved it */
    else if (!(force && *force == '1'))
        ok = VirtualAlloc((void *)LOW_START, LOW_END - LOW_START, MEM_RESERVE, PAGE_NOACCESS) != NULL;
    if (!ok) {
        if (child && *child) {
            describe_low_range();
            ExitProcess(2);
        }
        DWORD code;
        if (!relaunch(&code)) {
            describe_low_range();
            fprintf(stderr, "td6: could not get the range in a new process either\n");
            ExitProcess(2);
        }
        ExitProcess(code);
    }
    /* a windowed program (no console of its own): started from a terminal, its output goes there; with
       its output redirected, there as given */
    if (GetFileType(GetStdHandle(STD_ERROR_HANDLE)) == FILE_TYPE_UNKNOWN && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    {   /* the helper programs beside this one (ffmpeg, 7z: Makefile HELPERS) first on PATH */
        static char path[32768];
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = n ? strrchr(exe, '\\') : NULL;
        const char *old = getenv("PATH");
        if (slash) {
            *slash = 0;
            snprintf(path, sizeof path, "%s;%s", exe, old ? old : "");
            _putenv_s("PATH", path);
        }
    }
    _set_fmode(_O_BINARY);              /* open() and fopen() without 'b' as on Linux */
    timeBeginPeriod(1);
    atexit(exit_last);                  /* (registered first: runs after every other atexit function) */
}

/* ------------------------------------------------------------------ exit */
static volatile UINT exit_code;

static void exit_last(void)
{
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), exit_code);
}

void win_exit(int code)
{
    exit_code = (UINT)code;
    exit(code);
}

static int in_low(const void *p, size_t len)
{
    uintptr_t a = (uintptr_t)p;
    return a >= LOW_START && a + len <= LOW_END;
}

/* ------------------------------------------------------------------ sys/mman.h */
static DWORD prot_flags(int prot)
{
    if (prot & PROT_EXEC) return prot & PROT_WRITE ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    if (prot & PROT_READ) return PAGE_READONLY;
    return PAGE_NOACCESS;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    if (!len) { errno = EINVAL; return MAP_FAILED; }
    if (flags & MAP_ANONYMOUS) {
        DWORD pf = prot_flags(prot);
        if (addr && in_low(addr, len)) {
            /* NOREPLACE: every page of it still only reserved */
            for (uintptr_t a = (uintptr_t)addr; a < (uintptr_t)addr + len;) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_RESERVE) {
                    errno = EEXIST;
                    return MAP_FAILED;
                }
                a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
            }
            void *m = VirtualAlloc(addr, len, MEM_COMMIT, pf);
            if (!m) { errno = ENOMEM; return MAP_FAILED; }
            return m;
        }
        void *m = VirtualAlloc(addr, len, MEM_RESERVE | MEM_COMMIT, pf);
        if (!m || (addr && (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && m != addr)) {
            errno = addr ? EEXIST : ENOMEM;
            return MAP_FAILED;
        }
        return m;
    }
    /* a read-only view of a file */
    if (prot & PROT_WRITE) { errno = ENOTSUP; return MAP_FAILED; }
    HANDLE fh = (HANDLE)_get_osfhandle(fd);
    if (fh == INVALID_HANDLE_VALUE) { errno = EBADF; return MAP_FAILED; }
    HANDLE map = CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!map) { errno = EACCES; return MAP_FAILED; }
    void *v = MapViewOfFile(map, FILE_MAP_READ, (DWORD)((uint64_t)off >> 32), (DWORD)off, len);
    CloseHandle(map);                   /* the view keeps the mapping */
    if (!v) { errno = ENOMEM; return MAP_FAILED; }
    return v;
}

int munmap(void *addr, size_t len)
{
    if (UnmapViewOfFile(addr))
        return 0;
    if (in_low(addr, len))
        return VirtualFree(addr, len, MEM_DECOMMIT) ? 0 : -1;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

int mprotect(void *addr, size_t len, int prot)
{
    DWORD old;
    if (prot != PROT_NONE) {
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery(addr, &mi, sizeof mi) && mi.State == MEM_RESERVE)
            return VirtualAlloc(addr, len, MEM_COMMIT, prot_flags(prot)) ? 0 : -1;
    }
    /* PROT_NONE on reserved-only pages: they fault already */
    MEMORY_BASIC_INFORMATION mi;
    if (prot == PROT_NONE && VirtualQuery(addr, &mi, sizeof mi) && mi.State == MEM_RESERVE)
        return 0;
    return VirtualProtect(addr, len, prot_flags(prot), &old) ? 0 : -1;
}

/* The pages read as zeros afterwards, as on Linux for private anonymous memory */
int madvise(void *addr, size_t len, int advice)
{
    if (advice != MADV_DONTNEED) return 0;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(addr, &mi, sizeof mi)) return -1;
    DWORD prot = mi.Protect ? mi.Protect : PAGE_READWRITE;
    if (!VirtualFree(addr, len, MEM_DECOMMIT)) return -1;
    return VirtualAlloc(addr, len, MEM_COMMIT, prot) ? 0 : -1;
}

/* ------------------------------------------------------------------ dlfcn.h */
static char dl_err[256];

void *dlopen(const char *name, int flags)
{
    (void)flags;
    char dll[MAX_PATH];
    const char *so = strstr(name, ".so");
    if (so) {
        /* libarchive.so.13 -> libarchive-13.dll, libfoo.so -> libfoo.dll */
        int base = (int)(so - name);
        const char *ver = so[3] == '.' ? so + 4 : NULL;
        if (ver && *ver) {
            int vl = (int)strcspn(ver, ".");
            snprintf(dll, sizeof dll, "%.*s-%.*s.dll", base, name, vl, ver);
        } else
            snprintf(dll, sizeof dll, "%.*s.dll", base, name);
    } else
        snprintf(dll, sizeof dll, "%s", name);
    HMODULE h = LoadLibraryA(dll);
    if (!h)
        snprintf(dl_err, sizeof dl_err, "%s: error %lu", dll, (unsigned long)GetLastError());
    return h;
}

void *dlsym(void *handle, const char *name)
{
    void *p = (void *)GetProcAddress((HMODULE)handle, name);
    if (!p) snprintf(dl_err, sizeof dl_err, "%s: not found", name);
    return p;
}

int dlclose(void *handle) { return FreeLibrary((HMODULE)handle) ? 0 : -1; }
const char *dlerror(void) { return dl_err[0] ? dl_err : NULL; }

/* Names for code addresses: translated functions (F_<address>) and the runtime's stand-ins for
   the game's imports (w_<DLL>_<name>), from the tables the translator writes. */
typedef struct { uintptr_t at; const char *name; } Sym;
static Sym *syms;
static int nsyms;
static INIT_ONCE syms_once = INIT_ONCE_STATIC_INIT;

static int cmp_sym(const void *a, const void *b)
{
    uintptr_t x = ((const Sym *)a)->at, y = ((const Sym *)b)->at;
    return x < y ? -1 : x > y;
}

static BOOL CALLBACK syms_build(PINIT_ONCE once, void *param, void **ctx)
{
    (void)once; (void)param; (void)ctx;
    syms = malloc((func_count + import_count) * sizeof *syms);
    char *names = malloc((size_t)func_count * 11 + (size_t)import_count * 80);
    char *p = names;
    for (int i = 0; i < func_count; i++) {
        syms[nsyms].at = (uintptr_t)func_table[i].fn;
        syms[nsyms++].name = p;
        p += sprintf(p, "F_%08x", func_table[i].addr) + 1;
    }
    for (int i = 0; i < import_count; i++) {
        const char *dot = strchr(import_table[i].dll, '.');
        int dl = dot ? (int)(dot - import_table[i].dll) : (int)strlen(import_table[i].dll);
        syms[nsyms].at = (uintptr_t)import_table[i].fn;
        syms[nsyms++].name = p;
        p += snprintf(p, 80, "w_%.*s_%.60s", dl, import_table[i].dll, import_table[i].name) + 1;
    }
    qsort(syms, nsyms, sizeof *syms, cmp_sym);
    return TRUE;
}

/* the named function an address is in: the nearest start below it, if no other starts between
   (and not past a sensible function size: the runtime's own code has no names here) */
static const Sym *sym_of(uintptr_t a)
{
    InitOnceExecuteOnce(&syms_once, syms_build, NULL, NULL);
    int lo = 0, hi = nsyms - 1, k = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (syms[mid].at <= a) { k = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (k < 0 || a - syms[k].at > 0x40000) return NULL;
    return &syms[k];
}

int dladdr(const void *addr, Dl_info *info)
{
    static __thread char mod[MAX_PATH];
    HMODULE h;
    memset(info, 0, sizeof *info);
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (const char *)addr, &h))
        return 0;
    GetModuleFileNameA(h, mod, sizeof mod);
    info->dli_fname = mod;
    info->dli_fbase = h;
    if (h == GetModuleHandleA(NULL)) {
        const Sym *s = sym_of((uintptr_t)addr);
        if (s) { info->dli_sname = s->name; info->dli_saddr = (void *)s->at; }
    }
    return 1;
}

/* ------------------------------------------------------------------ execinfo.h */
int backtrace(void **buf, int size)
{
    return RtlCaptureStackBackTrace(1, (DWORD)(size > 62 ? 62 : size), buf, NULL);
}

char **backtrace_symbols(void *const *buf, int size)
{
    char **out = malloc(size * (sizeof(char *) + 400));
    if (!out) return NULL;
    char *p = (char *)(out + size);
    for (int i = 0; i < size; i++) {
        Dl_info di;
        out[i] = p;
        const char *base = "?";
        if (dladdr(buf[i], &di) && di.dli_fname) {
            base = strrchr(di.dli_fname, '\\');
            base = base ? base + 1 : di.dli_fname;
        }
        if (di.dli_sname)
            snprintf(p, 400, "%s(%s+0x%llx) [%p]", base, di.dli_sname,
                     (unsigned long long)((uintptr_t)buf[i] - (uintptr_t)di.dli_saddr), buf[i]);
        else
            snprintf(p, 400, "%s(+0x%llx) [%p]", base,
                     (unsigned long long)((uintptr_t)buf[i] - (uintptr_t)di.dli_fbase), buf[i]);
        p += 400;
    }
    return out;
}

/* ------------------------------------------------------------------ spawn.h, sys/wait.h */
enum { SA_OPEN = 1, SA_DUP2, SA_CLOSE };

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa) { fa->n = 0; return 0; }
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa) { (void)fa; return 0; }

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path, int oflag, mode_t mode)
{
    (void)mode;
    if (fa->n >= WIN_SPAWN_ACTIONS) return ENOMEM;
    fa->a[fa->n].kind = SA_OPEN;
    fa->a[fa->n].fd = fd;
    fa->a[fa->n].oflag = oflag;
    snprintf(fa->a[fa->n].path, sizeof fa->a[0].path, "%s", strcmp(path, "/dev/null") ? path : "NUL");
    fa->n++;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int src, int fd)
{
    if (fa->n >= WIN_SPAWN_ACTIONS) return ENOMEM;
    fa->a[fa->n].kind = SA_DUP2;
    fa->a[fa->n].src = src;
    fa->a[fa->n].fd = fd;
    fa->n++;
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd)
{
    (void)fa; (void)fd;                 /* nothing is inherited unless passed */
    return 0;
}

/* one argument quoted for CommandLineToArgvW and the C runtime's own parsing */
static size_t quote_arg(char *out, size_t cap, size_t k, const char *a)
{
    int plain = *a && !strpbrk(a, " \t\"");
    if (plain) return k + snprintf(out + k, k < cap ? cap - k : 0, "%s", a);
    if (k < cap) out[k] = '"';
    k++;
    for (const char *s = a;; s++) {
        int bs = 0;
        while (*s == '\\') { bs++; s++; }
        int reps = !*s ? bs * 2 : *s == '"' ? bs * 2 + 1 : bs;
        for (int i = 0; i < reps; i++) { if (k < cap) out[k] = '\\'; k++; }
        if (!*s) break;
        if (k < cap) out[k] = *s;
        k++;
    }
    if (k < cap) out[k] = '"';
    return k + 1;
}

typedef struct { pid_t pid; HANDLE h; } Child;
static Child children[32];
static SRWLOCK child_lock = SRWLOCK_INIT;

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    (void)attr; (void)envp;             /* the child gets this process's environment */
    char cmd[32768];
    size_t k = quote_arg(cmd, sizeof cmd, 0, file);
    for (int i = 1; argv[i]; i++) {
        if (k < sizeof cmd) cmd[k] = ' ';
        k = quote_arg(cmd, sizeof cmd, k + 1, argv[i]);
    }
    if (k >= sizeof cmd) return E2BIG;
    cmd[k] = 0;
    HANDLE std[3] = {0, 0, 0}, own[3] = {0, 0, 0};
    SECURITY_ATTRIBUTES inh = {sizeof inh, NULL, TRUE};
    for (int i = 0; fa && i < fa->n; i++) {
        int fd = fa->a[i].fd;
        if (fd < 0 || fd > 2) continue;
        if (own[fd]) { CloseHandle(own[fd]); own[fd] = 0; }
        if (fa->a[i].kind == SA_OPEN) {
            int of = fa->a[i].oflag, acc = of & (O_WRONLY | O_RDWR);
            DWORD access = acc == O_WRONLY ? GENERIC_WRITE : acc == O_RDWR ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
            DWORD disp = (of & O_CREAT) ? ((of & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS)
                                        : ((of & O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING);
            own[fd] = CreateFileA(fa->a[i].path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inh, disp,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
            if (own[fd] == INVALID_HANDLE_VALUE) own[fd] = 0;
        } else if (fa->a[i].kind == SA_DUP2) {
            HANDLE s = (HANDLE)_get_osfhandle(fa->a[i].src);
            if (s == INVALID_HANDLE_VALUE ||
                !DuplicateHandle(GetCurrentProcess(), s, GetCurrentProcess(), &own[fd], 0, TRUE, DUPLICATE_SAME_ACCESS))
                own[fd] = 0;
        }
        std[fd] = own[fd];
    }
    static const DWORD which[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int fd = 0; fd < 3; fd++)
        if (!std[fd]) std[fd] = GetStdHandle(which[fd]);
    STARTUPINFOA si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES,
                       .hStdInput = std[0], .hStdOutput = std[1], .hStdError = std[2]};
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD err = GetLastError();
    for (int fd = 0; fd < 3; fd++)
        if (own[fd]) CloseHandle(own[fd]);
    if (!ok) return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
    CloseHandle(pi.hThread);
    AcquireSRWLockExclusive(&child_lock);
    int slot = -1;
    for (int i = 0; i < 32; i++)
        if (!children[i].h) { slot = i; break; }
    if (slot >= 0) { children[slot].pid = (pid_t)pi.dwProcessId; children[slot].h = pi.hProcess; }
    ReleaseSRWLockExclusive(&child_lock);
    if (slot < 0) { CloseHandle(pi.hProcess); return EAGAIN; }
    if (pid) *pid = (pid_t)pi.dwProcessId;
    return 0;
}

static HANDLE child_handle(pid_t pid, int take)
{
    HANDLE h = 0;
    AcquireSRWLockExclusive(&child_lock);
    for (int i = 0; i < 32; i++)
        if (children[i].h && children[i].pid == pid) {
            h = children[i].h;
            if (take) children[i].h = 0;
            break;
        }
    ReleaseSRWLockExclusive(&child_lock);
    return h;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    HANDLE h = child_handle(pid, 0);
    if (!h) { errno = ECHILD; return -1; }
    if (WaitForSingleObject(h, options & WNOHANG ? 0 : INFINITE) != WAIT_OBJECT_0)
        return 0;
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    child_handle(pid, 1);
    CloseHandle(h);
    if (status) *status = (int)((code & 0xff) << 8);
    return pid;
}

int kill(pid_t pid, int sig)
{
    HANDLE h = child_handle(pid, 0);
    if (!h) { errno = ESRCH; return -1; }
    if (sig) TerminateProcess(h, 128 + sig);
    return 0;
}

/* ------------------------------------------------------------------ fnmatch.h */
static int fold(int c, int flags) { return flags & FNM_CASEFOLD ? tolower((unsigned char)c) : (unsigned char)c; }

/* a [...] set at p (after the '['): 1/0 match, -1 not a set; *end past its ']' */
static int match_set(const char *p, int c, int flags, const char **end)
{
    int neg = *p == '!' || *p == '^';
    if (neg) p++;
    int hit = 0, first = 1;
    for (; *p && (first || *p != ']'); first = 0) {
        int lo = (unsigned char)*p++;
        if (lo == '\\' && !(flags & FNM_NOESCAPE) && *p) lo = (unsigned char)*p++;
        int hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            hi = (unsigned char)p[1];
            p += 2;
            if (hi == '\\' && !(flags & FNM_NOESCAPE) && *p) hi = (unsigned char)*p++;
        }
        if (fold(lo, flags) <= c && c <= fold(hi, flags)) hit = 1;
        else if (flags & FNM_CASEFOLD && lo <= toupper(c) && toupper(c) <= hi) hit = 1;
    }
    if (*p != ']') return -1;
    *end = p + 1;
    return hit != neg;
}

int fnmatch(const char *pat, const char *s, int flags)
{
    const char *star_p = NULL, *star_s = NULL;
    for (;;) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            star_p = pat;
            star_s = s;
            continue;
        }
        if (!*s) {
            if (!*pat) return 0;
            return FNM_NOMATCH;
        }
        int ok = 0;
        const char *next = pat + 1;
        if (*pat == '?') ok = 1;
        else if (*pat == '[') {
            int r = match_set(pat + 1, fold(*s, flags), flags, &next);
            if (r < 0) { ok = *s == '['; next = pat + 1; }
            else ok = r;
        } else {
            int pc = *pat;
            if (pc == '\\' && !(flags & FNM_NOESCAPE) && pat[1]) { pc = pat[1]; next = pat + 2; }
            ok = pc && fold(pc, flags) == fold(*s, flags);
        }
        if (ok) { pat = next; s++; continue; }
        if (!star_p) return FNM_NOMATCH;
        pat = star_p;
        s = ++star_s;
    }
}

/* ------------------------------------------------------------------ signal.h: crash handlers */
static struct sigaction handlers[32];

static LONG WINAPI on_exception(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    int sig;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_IN_PAGE_ERROR: case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case EXCEPTION_GUARD_PAGE: case EXCEPTION_DATATYPE_MISALIGNMENT:
        sig = SIGSEGV; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
        sig = SIGILL; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_INT_OVERFLOW: case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION: case EXCEPTION_FLT_OVERFLOW: case EXCEPTION_FLT_UNDERFLOW:
    case EXCEPTION_FLT_INEXACT_RESULT: case EXCEPTION_FLT_DENORMAL_OPERAND: case EXCEPTION_FLT_STACK_CHECK:
        sig = SIGFPE; break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    struct sigaction *h = &handlers[sig];
    siginfo_t si = {.si_signo = sig};
    si.si_addr = code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2
                     ? (void *)ep->ExceptionRecord->ExceptionInformation[1]
                     : ep->ExceptionRecord->ExceptionAddress;
    if (h->sa_flags & SA_SIGINFO && h->sa_sigaction)
        h->sa_sigaction(sig, &si, ep->ContextRecord);
    else if (h->sa_handler && h->sa_handler != SIG_DFL && h->sa_handler != SIG_IGN)
        h->sa_handler(sig);
    else
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int sigaction(int sig, const struct sigaction *sa, struct sigaction *old)
{
    if (sig <= 0 || sig >= 32) { errno = EINVAL; return -1; }
    if (old) *old = handlers[sig];
    if (sa) {
        handlers[sig] = *sa;
        if (sig == SIGSEGV || sig == SIGILL || sig == SIGFPE || sig == SIGBUS) {
            static int installed;
            if (!installed++) SetUnhandledExceptionFilter(on_exception);
        }
    }
    return 0;
}

/* setitimer(ITIMER_PROF) for prof.c: a thread samples the thread that set the timer (the game's main
   thread) at the interval: suspended, its instruction pointer handed to the SIGPROF handler as Linux's
   signal would, resumed. Wall-clock time, so its waits show too (in the system's DLLs). */
static void sleep_100ns(int64_t t);
static HANDLE prof_target, prof_thread;
static volatile LONG prof_run;
static int64_t prof_step;               /* 100 ns units */

static int prof_caller;
static DWORD64 prog_lo, prog_hi;        /* this program's image */

/* The loaded modules' function tables, for unwinding the suspended thread. Not RtlLookupFunctionEntry:
   it takes the loader's lock, which the thread may hold (suspended in LdrLoadDll: both waited for
   ever). Read while the thread runs, every so often; a module gone since is skipped (VirtualQuery). */
#define PROF_MODS 256
static struct { DWORD64 lo, hi; RUNTIME_FUNCTION *fn; DWORD n; } prof_mods[PROF_MODS];
static int prof_nmods;

static void prof_mods_read(void)
{
    HMODULE m[PROF_MODS];
    DWORD need = 0;
    prof_nmods = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), m, sizeof m, &need))
        return;
    for (DWORD i = 0; i < need / sizeof m[0] && i < PROF_MODS; i++) {
        char *b = (char *)m[i];
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
        IMAGE_DATA_DIRECTORY *d = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!d->VirtualAddress || !d->Size)
            continue;
        prof_mods[prof_nmods].lo = (DWORD64)b;
        prof_mods[prof_nmods].hi = (DWORD64)b + nt->OptionalHeader.SizeOfImage;
        prof_mods[prof_nmods].fn = (RUNTIME_FUNCTION *)(b + d->VirtualAddress);
        prof_mods[prof_nmods].n = d->Size / sizeof(RUNTIME_FUNCTION);
        prof_nmods++;
    }
}

/* pc's function entry (and its module's base), or NULL: not in a function with one (a leaf), or in no
   module known */
static RUNTIME_FUNCTION *prof_lookup(DWORD64 pc, DWORD64 *base, int *known)
{
    *known = 0;
    for (int i = 0; i < prof_nmods; i++) {
        if (pc < prof_mods[i].lo || pc >= prof_mods[i].hi)
            continue;
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)prof_mods[i].lo, &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Type != MEM_IMAGE)
            return NULL;
        *known = 1;
        *base = prof_mods[i].lo;
        DWORD rva = (DWORD)(pc - prof_mods[i].lo), lo = 0, hi = prof_mods[i].n;
        while (lo < hi) {
            DWORD mid = (lo + hi) / 2;
            RUNTIME_FUNCTION *f = &prof_mods[i].fn[mid];
            if (rva < f->BeginAddress)
                hi = mid;
            else if (rva >= f->EndAddress)
                lo = mid + 1;
            else
                return f;
        }
        return NULL;
    }
    return NULL;
}

static DWORD WINAPI prof_sampler(void *arg)
{
    (void)arg;
    const char *e = getenv("TD6_PROFILE_CALLER");
    prof_caller = e && *e == '1';
    HMODULE self = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)self + ((IMAGE_DOS_HEADER *)self)->e_lfanew);
    prog_lo = (DWORD64)self;
    prog_hi = prog_lo + nt->OptionalHeader.SizeOfImage;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (unsigned long k = 0; prof_run; k++) {
        sleep_100ns(prof_step);
        if (prof_caller && k % 1000 == 0)
            prof_mods_read();               /* (the thread running: the loader's lock is free to take) */
        if (SuspendThread(prof_target) == (DWORD)-1)
            break;
        CONTEXT ctx = {.ContextFlags = prof_caller ? CONTEXT_FULL : CONTEXT_CONTROL};
        if (GetThreadContext(prof_target, &ctx) && handlers[SIGPROF].sa_sigaction) {
            /* TD6_PROFILE_CALLER=1: a sample in a DLL counts for the program's function that called into it */
            for (int up = 0; prof_caller && up < 32 && (ctx.Rip < prog_lo || ctx.Rip >= prog_hi); up++) {
                DWORD64 base;
                int known;
                PRUNTIME_FUNCTION fe = prof_lookup(ctx.Rip, &base, &known);
                if (!known)                     /* (a module not read yet, or gone: where it is will do) */
                    break;
                if (!fe) {                      /* a leaf: its return address is on the stack */
                    ctx.Rip = *(DWORD64 *)ctx.Rsp;
                    ctx.Rsp += 8;
                    continue;
                }
                void *hd;
                DWORD64 est;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fe, &ctx, &hd, &est, NULL);
            }
            ucontext_t uc;
            memset(&uc, 0, sizeof uc);
            uc.uc_mcontext.gregs[REG_RIP] = (int64_t)ctx.Rip;
            siginfo_t si = {.si_signo = SIGPROF};
            handlers[SIGPROF].sa_sigaction(SIGPROF, &si, &uc);
        }
        ResumeThread(prof_target);
    }
    return 0;
}

int setitimer(int which, const struct itimerval *nv, struct itimerval *old)
{
    if (old) memset(old, 0, sizeof *old);
    if (which != ITIMER_PROF) {
        errno = ENOSYS;
        return -1;
    }
    if (prof_thread) {                  /* stop (and wait: the samples are read next) */
        InterlockedExchange(&prof_run, 0);
        WaitForSingleObject(prof_thread, INFINITE);
        CloseHandle(prof_thread);
        CloseHandle(prof_target);
        prof_thread = prof_target = NULL;
    }
    int64_t us = nv ? (int64_t)nv->it_interval.tv_sec * 1000000 + nv->it_interval.tv_usec : 0;
    if (us <= 0)
        return 0;
    prof_step = us * 10;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &prof_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0))
        return -1;
    InterlockedExchange(&prof_run, 1);
    prof_thread = CreateThread(NULL, 0, prof_sampler, NULL, 0, NULL);
    return prof_thread ? 0 : -1;
}

const char *win_strsignal(int sig)
{
    switch (sig) {
    case SIGSEGV: return "memory fault";
    case SIGILL: return "illegal instruction";
    case SIGFPE: return "arithmetic fault";
    case SIGBUS: return "bus error";
    case SIGABRT: return "aborted";
    case SIGINT: return "interrupted";
    case SIGTERM: return "terminated";
    default: return "signal";
    }
}

/* ------------------------------------------------------------------ compat.h */
char **win_environ;


long win_sysconf(int name)
{
    if (name == _SC_NPROCESSORS_ONLN)
        return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    errno = EINVAL;
    return -1;
}

ssize_t win_pread(int fd, void *buf, size_t n, int64_t off)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    OVERLAPPED o = {0};
    o.Offset = (DWORD)off;
    o.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)(n > 0x7fffffff ? 0x7fffffff : n), &got, &o)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        errno = EIO;
        return -1;
    }
    return (ssize_t)got;
}

int win_rename(const char *from, const char *to)
{
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        return 0;
    DWORD e = GetLastError();
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
          : e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ? EACCES : EIO;
    return -1;
}

int win_pipe(int fd[2]) { return _pipe(fd, 1 << 20, _O_BINARY | _O_NOINHERIT); }

char *win_strcasestr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return (char *)hay;
    return n ? NULL : (char *)hay;
}

void *win_memmem(const void *hay, size_t hn, const void *needle, size_t nn)
{
    if (!nn) return (void *)hay;
    const unsigned char *h = hay, *e = h + hn;
    for (; (size_t)(e - h) >= nn; h++) {
        h = memchr(h, *(const unsigned char *)needle, (size_t)(e - h) - nn + 1);
        if (!h) return NULL;
        if (!memcmp(h, needle, nn)) return (void *)h;
    }
    return NULL;
}

static void sleep_100ns(int64_t t)
{
    static __thread HANDLE timer;
    if (t <= 0) { SwitchToThread(); return; }
    if (!timer)
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) { Sleep((DWORD)((t + 9999) / 10000)); return; }
    LARGE_INTEGER due = {.QuadPart = -t};
    if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep((DWORD)((t + 9999) / 10000));
}

int win_nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) { errno = EINVAL; return -1; }
    sleep_100ns((int64_t)req->tv_sec * 10000000 + (req->tv_nsec + 99) / 100);
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}

int win_clock_nanosleep(clockid_t clock, int flags, const struct timespec *req, struct timespec *rem)
{
    if (!(flags & TIMER_ABSTIME))
        return win_nanosleep(req, rem) ? errno : 0;
    struct timespec now;
    clock_gettime(clock, &now);
    int64_t ns = (int64_t)(req->tv_sec - now.tv_sec) * 1000000000 + (req->tv_nsec - now.tv_nsec);
    if (ns > 0) sleep_100ns((ns + 99) / 100);
    return 0;
}

int win_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    if (attr)
        return pthread_create(t, attr, fn, arg);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 8 << 20);
    int r = pthread_create(t, &at, fn, arg);
    pthread_attr_destroy(&at);
    return r;
}
