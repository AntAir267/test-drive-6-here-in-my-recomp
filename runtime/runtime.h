/* Shared declarations of the Windows/DirectX stand-in. */
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include "cpu.h"

/* ------------------------------------------------------------------ generated tables */
typedef struct { uint32_t addr; void (*fn)(CPU *c); } FuncEntry;
typedef struct { uint32_t iat; const char *dll; const char *name; void (*fn)(CPU *c); } ImportEntry;
/* A fixed-size table the translated code reads through *reloc / checks against *limit. */
typedef struct { const char *name; uint32_t *reloc, *limit; uint32_t base, entry, count; } TableEntry;
extern const TableEntry table_list[];
extern const FuncEntry func_table[];
extern const int func_count;
extern const ImportEntry import_table[];
extern const int import_count;

/* ------------------------------------------------------------------ logging */
enum { LOG_API = 1, LOG_FILE = 2, LOG_D3D = 4, LOG_DDRAW = 8, LOG_SOUND = 16, LOG_INPUT = 32,
       LOG_WIN = 64, LOG_MEM = 128, LOG_THREAD = 256, LOG_REG = 512, LOG_DRAW = 1024,
       LOG_ALWAYS = 0x40000000 };
extern unsigned log_mask;
void logmsg(unsigned cat, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
#define LOGON(cat) (log_mask & (cat))
#define LOG(cat, ...) do { if (log_mask & (cat)) logmsg(cat, __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ calling conventions */
#define ARG(n) R32(c->esp + 4 + 4 * (n))
extern unsigned long api_calls;                /* every return from the runtime to the program */
extern int api_trace_on;
void api_trace(CPU *c);                        /* --hash-calls: memory hash at chosen calls */
static inline void api_return(CPU *c, uint32_t r, int nargs)
{
    c->eax = r;
    c->ret = R32(c->esp);
    c->esp += 4 + 4 * (uint32_t)nargs;
    api_calls++;
    if (__builtin_expect(api_trace_on, 0))
        api_trace(c);
}
/* A stdcall function the program calls (an import or a COM method): the body returns eax;
   nargs dwords are popped (for COM methods nargs includes `this`). */
#define HOSTFN(fname, nargs) \
    static uint32_t fname##_body(CPU *c); \
    void fname(CPU *c) { uint32_t _r = fname##_body(c); api_return(c, _r, nargs); } \
    static uint32_t fname##_body(CPU *c)
#define API(dll, name, nargs) HOSTFN(w_##dll##_##name, nargs)
/* Same, file-local: COM methods and callbacks handed to the program through thunks. */
#define METHOD(fname, nargs) \
    static uint32_t fname##_body(CPU *c); \
    static void fname(CPU *c) { uint32_t _r = fname##_body(c); api_return(c, _r, nargs); } \
    static uint32_t fname##_body(CPU *c)
/* cdecl: the caller pops the arguments */
#define API_CDECL(dll, name) HOSTFN(w_##dll##_##name, 0)

/* Call a function of the program from the runtime (window procedure, thread start,
   callbacks). Arguments are pushed right to left; returns eax. */
uint32_t guest_call(CPU *c, uint32_t fn, int nargs, ...);
uint32_t guest_callv(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
extern void (*icall_hook)(CPU *c, uint32_t target);
int call_thunk(CPU *c, uint32_t target);     /* run a runtime function by its thunk address */
/* A program address standing for a runtime function (vtables, IAT, GetProcAddress). */
uint32_t thunk_addr(void (*fn)(CPU *c), const char *name);
const char *thunk_name(uint32_t addr);

/* ------------------------------------------------------------------ memory */
#define ARENA_START 0x02400000u
#define ARENA_END   0x70000000u
#define THUNK_BASE  0x7f000000u
#define THUNK_MAX   0x10000u
void mem_init(void);
uint32_t vm_alloc(uint32_t addr, uint32_t size, int reserve_only);   /* 64 KB granular */
uint32_t vm_alloc_table(uint32_t bytes);   /* for a moved table: ends at an inaccessible page */
int vm_free(uint32_t addr, uint32_t size, int release);
uint32_t vm_query_base(uint32_t addr);
uint32_t heap_create(uint32_t initial, uint32_t max);
int heap_destroy(uint32_t heap);
uint32_t heap_alloc(uint32_t heap, uint32_t size, int zero);
int heap_free(uint32_t heap, uint32_t p);
uint32_t heap_realloc(uint32_t heap, uint32_t p, uint32_t size, int zero, int in_place_only);
uint32_t heap_size(uint32_t heap, uint32_t p);
extern uint32_t process_heap;
/* runtime-owned allocations visible to the program */
uint64_t mem_hash(void);                     /* hash of program-visible memory (tests) */
void mem_dump(const char *path);              /* all program memory to a file (tests) */
extern int deterministic;                     /* --test */
uint32_t galloc(uint32_t size);
void gfree(uint32_t p);
uint32_t gstrdup(const char *s);
static inline char *gptr(uint32_t a) { return (char *)(uintptr_t)a; }
static inline const char *gstr(uint32_t a) { return a ? (const char *)(uintptr_t)a : NULL; }
void gstrncpy(uint32_t dst, const char *src, uint32_t n);   /* always terminates if n > 0 */

/* ------------------------------------------------------------------ process / threads */
typedef struct Thread Thread;
extern CPU *main_cpu;
extern char game_dir[];      /* host folder holding Game.exe and Data/ */
extern char user_dir[];      /* host folder for saves, settings, logs */
extern uint32_t image_base;
extern int headless;
CPU *cpu_new(uint32_t stack_size);
void runtime_setup(void);
void set_current_cpu(CPU *c);
CPU *current_cpu(void);
uint32_t current_thread_id(void);
void set_last_error(uint32_t e);
uint32_t get_last_error(void);
void process_exit(int code) __attribute__((noreturn));
void frame_limit_options(void);     /* main.c: "frame_limit" read again */
int frame_limit_screen(void);       /* ... its "screen" rate: 3 under the screen's refresh */
int host_refresh_hz(void);          /* win.c: the screen's refresh rate (60 when unknown) */
double now_ms(void);         /* milliseconds since start */

/* ------------------------------------------------------------------ handles */
enum { H_FREE = 0, H_FILE, H_EVENT, H_MUTEX, H_THREAD, H_FIND, H_SEMAPHORE, H_MISC };
typedef struct HandleObj HandleObj;
uint32_t handle_new(int type, void *obj);
void *handle_get(uint32_t h, int type);
int handle_type(uint32_t h);
void handle_close(uint32_t h);
int wait_handle(uint32_t h, uint32_t ms);              /* 0 = signalled, 0x102 = timeout */
int wait_handles(int n, const uint32_t *h, int all, uint32_t ms);

/* ------------------------------------------------------------------ files */
/* Translate a Windows path the program uses to a host path. For reading, a file present in
   the user folder wins over the game folder; writing always goes to the user folder. */
int path_to_host(const char *winpath, char *out, size_t outsz, int for_write);
const char *virtual_game_root(void);   /* e.g. "C:\\1NSANE" */

/* ------------------------------------------------------------------ resources */
int resource_find(uint32_t type, uint32_t name, uint32_t *data, uint32_t *size);

/* ------------------------------------------------------------------ registry/settings */
void settings_load(void);
void settings_save(void);
/* The runtime's own reads and writes of the store, e.g. keeping values of its own under another
   key. settings_get returns the value's size (0 if there is none) and copies up to cap bytes. */
uint32_t settings_get(const char *key, const char *name, uint32_t *type, void *buf, uint32_t cap);
int settings_has(const char *key, const char *name);
void settings_put(const char *key, const char *name, uint32_t type, const void *data, uint32_t size);
void settings_delete(const char *key, const char *name);

/* ------------------------------------------------------------------ COM objects */
/* A COM object as the program sees it: [vtable][host object pointer (low 32 bits unused)].
   The host side keeps its own struct; com_host() finds it from the program's pointer. */
typedef struct { const char *iface; int nmethods; uint32_t vtbl; } ComClass;
uint32_t com_new(ComClass *cls, void *host);
void *com_host(uint32_t obj);
void com_set_class(uint32_t obj, ComClass *cls);
void com_class_init(ComClass *cls, const char *iface, int n, void (*const *methods)(CPU *),
                    const char *const *names);
#define GUID_FMT "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x"
#define GUID_ARGS(a) R32(a), R16((a) + 4), R16((a) + 6), R8((a) + 8), R8((a) + 9), R8((a) + 10), \
    R8((a) + 11), R8((a) + 12), R8((a) + 13), R8((a) + 14), R8((a) + 15)
int guid_eq(uint32_t a, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]);

/* Called by the display layer once per presented frame (Flip, or a Blt to the primary
   surface): counts frames, applies --frames/--seconds limits, pumps host window events. */
void frame_presented(void);
/* The program reads an input device (DirectInput): if it does so without presenting frames, it
   is waiting for a key, and input must keep flowing (main.c). */
void input_wait(void);
extern int run_frames_limit;
extern double run_seconds_limit;
extern unsigned long frame_count;

/* windows / input / display shared state */
void win_init(void);
void win_pump(void);            /* host events -> window messages (no-op when headless) */
extern uint32_t main_hwnd;
void ddraw_init(void);
void dsound_init(void);
void dinput_init(void);
void ifc_init(void);
void video_init(void);
void video_skip_press(void);         /* video.c: Escape or a controller button while an intro video plays */
int video_skip_taken(void);           /* ... one is waiting for the game (taken once) */
