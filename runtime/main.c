/* Start-up: map the user's TD6.exe at its own address, connect its imports to the runtime,
   and run its entry point on the main thread. Also: calls through pointers, callbacks into the
   program, and the error paths of the generated code. */
#define _GNU_SOURCE
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "runtime.h"
#include "image.h"
#include "input.h"
#include "options.h"
#include "pad.h"
#include "vk.h"

unsigned log_mask = LOG_ALWAYS;
char game_dir[1024];
char user_dir[1024];
uint32_t image_base = IMAGE_BASE;
CPU *main_cpu;
int headless = 1;
static FILE *logfp;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec t_start;

/* ------------------------------------------------------------------ logging */
/* Test mode (--test): a virtual clock that moves 1/60 s per presented frame and a microsecond
   per read (so busy-waits still end), timers run on the main thread at frame ends, nothing
   depends on the host's timing, and two runs with the same scripted input match exactly. */
int deterministic;
static double vclock;

static double real_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t_start.tv_sec) * 1000.0 + (t.tv_nsec - t_start.tv_nsec) / 1e6;
}

double now_ms(void)
{
    if (deterministic) {
        vclock += 0.001;
        return vclock;
    }
    return real_ms();
}

/* With a log file everything goes there; LOG_ALWAYS messages are also echoed to stderr. */
static void vlog(unsigned cat, const char *fmt, va_list ap)
{
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    CPU *c = current_cpu();
    pthread_mutex_lock(&log_lock);
    double t = deterministic ? vclock : real_ms();
    int th = c ? c->thread_index : -1;
    if (logfp) {
        fprintf(logfp, "%9.3f [%d] %s\n", t / 1000.0, th, buf);
        fflush(logfp);
    }
    if (!logfp || cat == LOG_ALWAYS)
        fprintf(stderr, "%9.3f [%d] %s\n", t / 1000.0, th, buf);
    pthread_mutex_unlock(&log_lock);
}

void logmsg(unsigned cat, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(cat, fmt, ap);
    va_end(ap);
}

static void dump_state(CPU *c)
{
    if (!c)
        return;
    logmsg(LOG_ALWAYS, "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x",
           c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    if (c->esp >= ARENA_START && c->esp < ARENA_END) {
        char line[256];
        int n = 0;
        for (int i = 0; i < 12; i++)
            n += snprintf(line + n, sizeof line - n, " %08x", R32(c->esp + 4 * i));
        logmsg(LOG_ALWAYS, "  stack:%s", line);
    }
    void *bt[64];
    int nb = backtrace(bt, 64);
    char **sym = backtrace_symbols(bt, nb);
    for (int i = 0; i < nb && sym; i++) {
        const char *p = strstr(sym[i], "(F_");
        const char *q = strstr(sym[i], "(w_");
        if (p || q || i < 6)
            logmsg(LOG_ALWAYS, "  at %s", sym[i]);
    }
    free(sym);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logmsg(LOG_ALWAYS, "FATAL: %s", buf);
    dump_state(current_cpu());
    if (logfp)
        fflush(logfp);
    _exit(2);
}

/* ------------------------------------------------------------------ calls through pointers */
static void (**text_fns)(CPU *);
static void (*thunk_fns[THUNK_MAX])(CPU *);
static const char *thunk_names[THUNK_MAX];
static int nthunks;
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t thunk_addr(void (*fn)(CPU *c), const char *name)
{
    pthread_mutex_lock(&thunk_lock);
    for (int i = 0; i < nthunks; i++)
        if (thunk_fns[i] == fn) {
            pthread_mutex_unlock(&thunk_lock);
            return THUNK_BASE + 16u * i;
        }
    if (nthunks >= (int)THUNK_MAX)
        fatal("out of thunks");
    thunk_fns[nthunks] = fn;
    thunk_names[nthunks] = name;
    uint32_t a = THUNK_BASE + 16u * nthunks++;
    pthread_mutex_unlock(&thunk_lock);
    return a;
}

const char *thunk_name(uint32_t a)
{
    if (a - THUNK_BASE < 16u * nthunks && !(a & 15))
        return thunk_names[(a - THUNK_BASE) >> 4];
    return NULL;
}

/* The reference build (ref_cpu.c) runs the original code in an emulator instead. */
void (*icall_hook)(CPU *c, uint32_t target);

int call_thunk(CPU *c, uint32_t t)
{
    if (t - THUNK_BASE < 16u * nthunks && !(t & 15)) {
        thunk_fns[(t - THUNK_BASE) >> 4](c);
        return 1;
    }
    return 0;
}

void icall(CPU *c, uint32_t t)
{
    if (icall_hook) {
        icall_hook(c, t);
        return;
    }
    if (t - TEXT_START < TEXT_END - TEXT_START) {
        void (*fn)(CPU *) = text_fns[t - TEXT_START];
        if (fn) {
            fn(c);
            return;
        }
        fatal("call to %08x: code the translator did not find as a function (add it to "
              "EXTRA_ENTRIES), return address %08x", t, R32(c->esp));
    }
    if (t - THUNK_BASE < 16u * nthunks && !(t & 15)) {
        thunk_fns[(t - THUNK_BASE) >> 4](c);
        return;
    }
    fatal("call to %08x: not code (return address %08x)", t, R32(c->esp));
}

#define SENTINEL 0x7efe0000u

uint32_t guest_callv(CPU *c, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t keep[8] = {c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi};
    uint32_t keep_ret = c->ret, keep_df = c->df, keep_fl = c->eflags;
    uint32_t esp = c->esp - 64;      /* leave the caller's outgoing area alone */
    for (int i = nargs - 1; i >= 0; i--) {
        esp -= 4;
        W32(esp, args[i]);
    }
    esp -= 4;
    W32(esp, SENTINEL);
    c->esp = esp;
    c->df = 0;
    icall(c, fn);
    if (c->ret != SENTINEL)
        fatal("callback %08x returned to %08x", fn, c->ret);
    uint32_t r = c->eax;
    c->eax = keep[0]; c->ecx = keep[1]; c->edx = keep[2]; c->ebx = keep[3];
    c->esp = keep[4]; c->ebp = keep[5]; c->esi = keep[6]; c->edi = keep[7];
    c->ret = keep_ret;
    c->df = keep_df;
    c->eflags = keep_fl;
    return r;
}

uint32_t guest_call(CPU *c, uint32_t fn, int nargs, ...)
{
    uint32_t args[16];
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++)
        args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return guest_callv(c, fn, nargs, args);
}

/* ------------------------------------------------------------------ error paths */
void bad_ret(CPU *c, uint32_t site, uint32_t expected)
{
    fatal("call at %08x returned to %08x instead of %08x", site, c->ret, expected);
}
void bad_jump(CPU *c, uint32_t site, uint32_t target)
{
    (void)c;
    fatal("jump table at %08x went to %08x, not a known case", site, target);
}
void nonlocal_jump(CPU *c, uint32_t site, uint32_t target)
{
    (void)c;
    fatal("exception/longjmp continuation at %08x to %08x is not supported yet", site, target);
}
void unsupported(CPU *c, uint32_t site, const char *what)
{
    (void)c;
    fatal("instruction at %08x not translated: %s", site, what);
}
void trap_int3(CPU *c, uint32_t site)
{
    (void)c;
    fatal("int3 at %08x", site);
}
void div_fault(CPU *c, uint32_t site)
{
    (void)c;
    fatal("integer divide fault at %08x", site);
}
void fell_off(CPU *c, uint32_t site)
{
    (void)c;
    fatal("ran past the end of translated code after %08x", site);
}
void returned_from_noreturn(CPU *c, uint32_t site)
{
    (void)c;
    fatal("call at %08x to a function that never returns came back", site);
}
void missing_import(CPU *c, const char *dll, const char *name)
{
    fatal("%s!%s is not implemented (called from %08x)", dll, name, R32(c->esp));
}

/* ------------------------------------------------------------------ threads' CPU records */
static __thread CPU *tls_cpu;
CPU *current_cpu(void) { return tls_cpu; }
void set_current_cpu(CPU *c) { tls_cpu = c; }
static int thread_counter;

CPU *cpu_new(uint32_t stack_size)
{
    CPU *c = aligned_alloc(64, (sizeof(CPU) + 63) & ~63u);
    memset(c, 0, sizeof *c);
    if (stack_size < 0x10000)
        stack_size = 0x10000;
    uint32_t stack = vm_alloc(0, stack_size, 0);
    if (!stack)
        fatal("no memory for a thread stack");
    c->stack_lo = stack;
    c->stack_hi = stack + stack_size;
    uint32_t teb = galloc(0x1000);
    memset(gptr(teb), 0, 0x1000);
    W32(teb + 0x00, 0xffffffffu);        /* end of the SEH chain */
    W32(teb + 0x04, c->stack_hi);
    W32(teb + 0x08, c->stack_lo);
    W32(teb + 0x18, teb);
    c->fs_base = teb;
    c->esp = c->stack_hi - 16;
    c->thread_index = __atomic_fetch_add(&thread_counter, 1, __ATOMIC_SEQ_CST);
    W32(teb + 0x24, 0x100 + 4 * c->thread_index);
    fpu_new(c);                          /* the host FPU is the caller's: not touched here */
    c->fcw = 0x27f;                      /* what Windows gives a new thread */
    return c;
}

/* ------------------------------------------------------------------ loading TD6.exe */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void load_image(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        fatal("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    fstat(fd, &st);
    uint8_t *f = malloc(st.st_size);
    if (read(fd, f, st.st_size) != st.st_size)
        fatal("cannot read %s", path);
    close(fd);
    uint32_t pe = rd32(f + 0x3c);
    if (memcmp(f + pe, "PE\0\0", 4))
        fatal("%s is not a Windows program", path);
    uint16_t nsec = rd16(f + pe + 6), optsz = rd16(f + pe + 20);
    const uint8_t *opt = f + pe + 24;
    if (rd32(opt + 28) != IMAGE_BASE || rd32(opt + 56) != IMAGE_SIZE || rd32(opt + 16) + IMAGE_BASE != IMAGE_ENTRY)
        fatal("%s is not the TD6.exe this build was translated from (%s)", path, BUILD_NAME);
    uint32_t size = (IMAGE_SIZE + 0xffff) & ~0xffffu;
    void *m = mmap((void *)(uintptr_t)IMAGE_BASE, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)(uintptr_t)IMAGE_BASE)
        fatal("cannot map the program at %08x: %s", IMAGE_BASE, strerror(errno));
    uint32_t hdrsize = rd32(opt + 60);
    memcpy(m, f, hdrsize < (uint32_t)st.st_size ? hdrsize : (uint32_t)st.st_size);
    const uint8_t *sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t vs = rd32(sh + 8), va = rd32(sh + 12), rs = rd32(sh + 16), ra = rd32(sh + 20);
        uint32_t n = rs < vs || !vs ? rs : vs;
        if (ra + n > (uint32_t)st.st_size)
            fatal("damaged section in %s", path);
        memcpy((uint8_t *)m + va, f + ra, n);
    }
    free(f);
}

static void connect_imports(void)
{
    for (int i = 0; i < import_count; i++) {
        const ImportEntry *e = &import_table[i];
        char *name = malloc(strlen(e->dll) + strlen(e->name) + 2);
        sprintf(name, "%s!%s", e->dll, e->name);
        W32(e->iat, thunk_addr(e->fn, name));
    }
}

static void build_function_index(void)
{
    text_fns = calloc(TEXT_END - TEXT_START, sizeof *text_fns);
    for (int i = 0; i < func_count; i++)
        text_fns[func_table[i].addr - TEXT_START] = func_table[i].fn;
}

/* ------------------------------------------------------------------ crashes */
static __attribute__((unused)) void on_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    static volatile int in;
    if (in++)
        _exit(3);
    fatal("signal %d (%s) at address %p", sig, strsignal(sig), si->si_addr);
}

void process_exit(int code)
{
    LOG(LOG_ALWAYS, "program exited with code %d", code);
    vk_stop();                          /* the render thread done, before atexit's teardown */
    ifc_exit();                         /* no motor left running */
    settings_save();                    /* the game's registry (registry.txt) */
    if (logfp)
        fflush(logfp);
    exit(code);
}

extern void sync_init(void);
extern void files_init(void);

/* Everything before the program's entry point runs: memory, the image, imports, devices. */
void runtime_setup(void)
{
    char exe[1100];
    snprintf(exe, sizeof exe, "%s/TD6.exe", game_dir);
    mem_init();
    load_image(exe);
    build_function_index();
    connect_imports();
    sync_init();
    files_init();
    settings_load();
    win_init();
    pad_init();
    ddraw_init();
    dsound_init();
    dinput_init();
    extern void bink_init(void);
    bink_init();
    extern void wma_init(void);
    wma_init();
    extern void widescreen_init(void);
    widescreen_init();
    padgame_init();
}

/* ------------------------------------------------------------------ main */
int run_frames_limit;
static unsigned long dump_frame, dump_call;
static char dump_path[1024];
double run_seconds_limit;
unsigned long frame_count;
static unsigned long wait_frames;       /* input_wait's frames */

extern void timers_run_due(double now);
static FILE *hashfp;
static unsigned long hash_every = 1;
unsigned long api_calls;
int api_trace_on;
static unsigned long trace_lo, trace_hi;
static FILE *tracefp;

/* --hash-calls FROM TO FILE: a memory hash after each runtime call in that range, with the
   call site, to find the first call after which two runs differ. */
void api_trace(CPU *c)
{
    if (dump_call && api_calls == dump_call) {
        mem_dump(dump_path);
        LOG(LOG_ALWAYS, "memory after call %lu (to %08x) written to %s", api_calls, c->ret, dump_path);
    }
    if (!tracefp || api_calls < trace_lo || api_calls > trace_hi)
        return;
    fprintf(tracefp, "%lu %08x %016llx\n", api_calls, c->ret, (unsigned long long)mem_hash());
    if (api_calls == trace_hi)
        fflush(tracefp);
}

/* FRAME LIMIT (Enhanced; options.txt frame_limit = off | screen | <fps>; not in test runs): a frame
   waits for its time before the game goes on (its input read after the wait, so what the next frame
   shows is as fresh as it can be). "screen" is 3 under the screen's refresh: with variable refresh
   (G-Sync, FreeSync) frames then stay inside the range the screen follows, never waiting for vsync.
   The wait sleeps to half a millisecond before and spins the rest (Windows' timers are not that
   exact). */
static double limit_ms;
static int limit_read;

void frame_limit_options(void) { limit_read = 0; }

int frame_limit_screen(void)
{
    int hz = host_refresh_hz();
    return hz > 33 ? hz - 3 : hz;
}

/* MENU FPS (options.txt menu_fps = 60 | <rate> | off; not in test runs): TD6's front end moves its
   animations a step a frame (it reads no clock; races run 30 Hz ticks and draw extrapolated), so
   outside races frames come no faster than 60 a second, as on the screens it was made for. */
static double menu_ms = -1;

static void frame_limit(void)
{
    static double next;
    if (deterministic)
        return;
    if (!limit_read) {
        const char *v = option_str("frame_limit", "off");
        int fps = !strcasecmp(v, "screen") ? frame_limit_screen() : atoi(v);
        limit_ms = fps >= 10 ? 1000.0 / fps : 0;
        const char *m = option_str("menu_fps", "60");
        int mf = atoi(m);
        menu_ms = mf >= 10 ? 1000.0 / mf : 0;
        limit_read = 1;
        if (fps >= 10)
            LOG(LOG_ALWAYS, "frame limit: %d fps", fps);
    }
    double want = limit_ms;
    if (menu_ms > want && R32(GAME_GAME_STATE) != 2)
        want = menu_ms;
    if (want <= 0)
        return;
    double now = real_ms();
    if (next > now) {
        if (next - now > 1.0) {
            struct timespec ts = {0, (long)((next - now - 0.5) * 1e6)};
            nanosleep(&ts, NULL);
        }
        while ((now = real_ms()) < next)
            __builtin_ia32_pause();
    }
    next = (next > now - want ? next : now) + want;    /* (a frame late: from now) */
}

void frame_presented(void)
{
    frame_count++;
    {   /* TD6_CPU_FROM=<frame>: the game thread's CPU time a frame from then, logged every 400 frames
           (timing changes on the game thread without its waits; work/cpu_far.sh) */
        static long from = -2;
        static double t0;
        if (from == -2)
            from = getenv("TD6_CPU_FROM") ? atol(getenv("TD6_CPU_FROM")) : -1;
        if (from >= 0 && frame_count >= (unsigned long)from) {
            struct timespec ts;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            double t = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
            if (frame_count == (unsigned long)from)
                t0 = t;
            else if ((frame_count - (unsigned long)from) % 400 == 0)
                LOG(LOG_ALWAYS, "game thread CPU %.3f ms a frame over %lu frames", (t - t0) / (frame_count - (unsigned long)from),
                    frame_count - (unsigned long)from);
        }
    }
    {   /* TD6_LOG_FRAME=<n>: the d3d and draw logs for that one frame */
        static long log_frame = -2;
        static unsigned saved_mask;
        if (log_frame == -2) {
            const char *e = getenv("TD6_LOG_FRAME");
            log_frame = e && *e ? atol(e) : -1;
        }
        if (log_frame >= 0 && frame_count == (unsigned long)log_frame) {
            saved_mask = log_mask;
            log_mask |= LOG_D3D | LOG_DRAW;
        } else if (log_frame >= 0 && frame_count == (unsigned long)log_frame + 1)
            log_mask = saved_mask;
    }
    if (deterministic) {
        /* TD6_TEST_FPS=<fps>[@<frame>]: the virtual clock's frame time from that frame on (60 fps
           otherwise: a race as a faster or slower machine plays it, still the same every run) */
        static double vstep = -1;
        static unsigned long vstep_from;
        if (vstep < 0) {
            const char *e = getenv("TD6_TEST_FPS");
            const char *at = e ? strchr(e, '@') : NULL;
            vstep = e && atof(e) > 0 ? 1000.0 / atof(e) : 1000.0 / 60.0;
            vstep_from = at ? strtoul(at + 1, NULL, 10) : 0;
        }
        vclock += frame_count >= vstep_from ? vstep : 1000.0 / 60.0;
        timers_run_due(vclock);
        extern void wma_run_due(double now);
        wma_run_due(vclock);
    }
    {
        /* TD6_PACE=<fps>: frames no faster than that in real time, as with vsync (timing a race
           as it is played: the CPU and GPU share the APU's power, which a flat-out run does not; and
           real-time runs off-screen, which have no vsync, at a monitor's rate) */
        static double pace_ms = -1, pace_next;
        if (pace_ms < 0) {
            const char *e = getenv("TD6_PACE");
            pace_ms = e && atof(e) > 0 ? 1000.0 / atof(e) : 0;
        }
        if (pace_ms > 0) {
            double now = real_ms();
            if (pace_next > now) {
                struct timespec ts = {0, (long)((pace_next - now) * 1e6)};
                nanosleep(&ts, NULL);
            }
            pace_next = (pace_next > now - pace_ms ? pace_next : now) + pace_ms;
        }
    }
    frame_limit();
    if (hashfp && frame_count % hash_every == 0) {
        fprintf(hashfp, "%lu %lu %016llx\n", frame_count, api_calls, (unsigned long long)mem_hash());
        fflush(hashfp);
    }
    if (dump_frame && frame_count == dump_frame) {
        char p[1200];
        snprintf(p, sizeof p, "%s", dump_path);
        mem_dump(p);
        LOG(LOG_ALWAYS, "memory of frame %lu written to %s", frame_count, p);
    }
    if (run_frames_limit && frame_count >= (unsigned long)run_frames_limit) {
        LOG(LOG_ALWAYS, "frame limit %d reached", run_frames_limit);
        process_exit(0);
    }
    if (run_seconds_limit > 0 && (deterministic ? vclock : real_ms()) > run_seconds_limit * 1000.0) {
        LOG(LOG_ALWAYS, "time limit reached after %lu frames", frame_count);
        process_exit(0);
    }
    input_frame(frame_count + wait_frames);
    win_pump();
}

/* The program reads its devices without presenting frames while it waits for a key (the
   controls menus' "press the control for ..." busy-waits on DirectInput): host events and
   scripted input keep coming as if frames passed - every 1/60 s, or with --test every 60 reads
   (no clock moves there). Scripted frame numbers count these too. */
void input_wait(void)
{
    static unsigned long reads, frame_seen;
    static double last;
    if (current_cpu() != main_cpu)
        return;
    if (frame_seen != frame_count) {
        frame_seen = frame_count;
        reads = 0;
        last = deterministic ? 0 : real_ms();
        return;
    }
    if (deterministic ? ++reads < 60 : real_ms() - last < 1000.0 / 60.0)
        return;
    reads = 0;
    last = deterministic ? 0 : real_ms();
    wait_frames++;
    LOG(LOG_INPUT, "input: the program waits on its devices (frame %lu + %lu)", frame_count, wait_frames);
    input_frame(frame_count + wait_frames);
    win_pump();
}

#ifndef RUNTIME_NO_MAIN
static void usage(void)
{
    fprintf(stderr,
            "usage: td6 [--game DIR] [--user DIR] [--log LIST] [--window|--fullscreen] [--frames N] [--seconds S]\n"
            "              [--test] [--hash FILE] [--hash-every N] [--dump-memory FRAME FILE]\n"
            "              [--option KEY=VALUE]   (see options.txt in the user folder)\n"
            "  --test  deterministic run: virtual clock, timers at frame ends, no real-time sound\n"
            "  --log  comma list: api,file,d3d,ddraw,sound,input,win,mem,thread,reg,draw,all\n");
    exit(1);
}

int main(int argc, char **argv)
{
    const char *cli_opts[16];
    int ncli = 0;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
#ifdef _WIN32
    const char *home = getenv("APPDATA");           /* %APPDATA%\td6-native, '/' separated */
    snprintf(user_dir, sizeof user_dir, "%s/td6-native", home ? home : ".");
    for (char *p = user_dir; *p; p++)
        if (*p == '\\') *p = '/';
#else
    const char *home = getenv("HOME");
    snprintf(user_dir, sizeof user_dir, "%s/.local/share/td6-native", home ? home : ".");
#endif
    snprintf(game_dir, sizeof game_dir, "%s/game", user_dir);
    const char *logpath = NULL;
#ifdef _WIN32
    /* Windows: started without arguments (the program double-clicked) it plays as the Start-menu entry
       does: in a window (options.txt's display decides full screen), the log in the user folder */
    static char default_log[1100];
    if (argc == 1) {
        headless = 0;
        snprintf(default_log, sizeof default_log, "%s/last-run.log", user_dir);
        logpath = default_log;
    }
#endif
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--game") && v) { snprintf(game_dir, sizeof game_dir, "%s", v); i++; }
        else if (!strcmp(a, "--user") && v) { snprintf(user_dir, sizeof user_dir, "%s", v); i++; }
        else if (!strcmp(a, "--logfile") && v) { logpath = v; i++; }
        else if (!strcmp(a, "--window")) headless = 0;
        else if (!strcmp(a, "--fullscreen")) { extern int start_fullscreen; headless = 0; start_fullscreen = 1; }
        else if (!strcmp(a, "--test")) deterministic = 1;
        else if (!strcmp(a, "--option") && v) { cli_opts[ncli++ & 15] = v; i++; }
        else if (!strcmp(a, "--hash") && v) {
            hashfp = fopen(v, "w");
            if (!hashfp) fatal("cannot write %s", v);
            i++;
        }
        else if (!strcmp(a, "--hash-calls") && i + 3 < argc) {
            trace_lo = strtoul(argv[i + 1], NULL, 10);
            trace_hi = strtoul(argv[i + 2], NULL, 10);
            tracefp = fopen(argv[i + 3], "w");
            if (!tracefp) fatal("cannot write %s", argv[i + 3]);
            api_trace_on = 1;
            i += 3;
        }
        else if (!strcmp(a, "--dump-memory-call") && i + 2 < argc) {
            dump_call = strtoul(argv[i + 1], NULL, 10);
            snprintf(dump_path, sizeof dump_path, "%s", argv[i + 2]);
            api_trace_on = 1;
            i += 2;
        }
        else if (!strcmp(a, "--hash-every") && v) { hash_every = strtoul(v, NULL, 10); if (!hash_every) hash_every = 1; i++; }
        else if (!strcmp(a, "--dump-memory") && i + 2 < argc) {
            dump_frame = strtoul(argv[i + 1], NULL, 10);
            snprintf(dump_path, sizeof dump_path, "%s", argv[i + 2]);
            i += 2;
        }
        else if (!strcmp(a, "--frames") && v) { run_frames_limit = atoi(v); i++; }
        else if (!strcmp(a, "--seconds") && v) { run_seconds_limit = atof(v); i++; }
        else if (!strcmp(a, "--log") && v) {
            static const struct { const char *n; unsigned m; } cats[] = {
                {"api", LOG_API}, {"file", LOG_FILE}, {"d3d", LOG_D3D}, {"ddraw", LOG_DDRAW},
                {"sound", LOG_SOUND}, {"input", LOG_INPUT}, {"win", LOG_WIN}, {"mem", LOG_MEM},
                {"thread", LOG_THREAD}, {"reg", LOG_REG}, {"draw", LOG_DRAW}, {"all", 0xffffu}};
            char tmp[256];
            snprintf(tmp, sizeof tmp, "%s", v);
            for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ","))
                for (size_t k = 0; k < sizeof cats / sizeof *cats; k++)
                    if (!strcmp(t, cats[k].n))
                        log_mask |= cats[k].m;
            i++;
        } else
            usage();
    }
    mkdir(user_dir, 0755);
    if (logpath) {
        logfp = fopen(logpath, "w");
        if (!logfp)
            fatal("cannot write %s", logpath);
    }
    struct sigaction sa = {0};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);

    options_load();
    for (int k = 0; k < ncli && k < 16; k++)
        option_parse(cli_opts[k]);
    if (deterministic) {
        extern int fpu_sw_exceptions;
        fpu_sw_exceptions = 0;          /* the reference emulator does not model them */
    }
    extern void prof_init(void);
    prof_init();
    runtime_setup();
    logmsg(LOG_ALWAYS, "TD6 native (%s): %d functions, game folder %s", BUILD_NAME, func_count, game_dir);

    CPU *c = cpu_new(STACK_RESERVE);
    main_cpu = c;
    set_current_cpu(c);
    fpu_activate(c);
    c->esp -= 4;
    W32(c->esp, SENTINEL);
    icall(c, IMAGE_ENTRY);
    process_exit((int)c->eax);
}
#endif
