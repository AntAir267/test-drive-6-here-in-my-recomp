/* CPU state, memory access and helpers used by the generated C (tools/codegen.py).
 *
 * The program's code and data sit at their original addresses in this process (the image is
 * mapped at 0x400000 and every allocation the program sees is below 2 GB), so a program
 * address is a host pointer. Registers are C locals inside each generated function and are
 * written back to the CPU record around calls; the x87 stack lives in the CPU record and is
 * computed in the host's 80-bit long double with the program's own control word. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef long double f80;

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t ret;          /* return address popped by the last ret (checked by the caller) */
    uint32_t eflags;       /* flags handed between functions (CRT maths helpers) */
    uint32_t df;           /* direction flag */
    uint32_t eflags_id;    /* EFLAGS.ID as popf last set it (the CPUID test toggles it) */
    uint32_t fs_base;      /* thread information block */
    /* x87 */
    f80 st[8];
    uint32_t ftop;
    uint16_t fcw;          /* the program's control word (the host's has exceptions masked) */
    uint16_t fsw;          /* C0-C3 condition bits */
    /* thread */
    int thread_index;
    uint32_t stack_lo, stack_hi;
    void *thread;          /* runtime's per-thread record */
} CPU;

#define REGS_IN uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, \
    esp = c->esp, ebp = c->ebp, esi = c->esi, edi = c->edi
#define SAVE() do { c->eax = eax; c->ecx = ecx; c->edx = edx; c->ebx = ebx; \
    c->esp = esp; c->ebp = ebp; c->esi = esi; c->edi = edi; } while (0)
#define LOAD() do { eax = c->eax; ecx = c->ecx; edx = c->edx; ebx = c->ebx; \
    esp = c->esp; ebp = c->ebp; esi = c->esi; edi = c->edi; } while (0)

#define STOREFLAGS() (c->eflags = cf | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | (of << 11))
#define LOADFLAGS() do { cf = c->eflags & 1; pf = (c->eflags >> 2) & 1; af = (c->eflags >> 4) & 1; \
    zf = (c->eflags >> 6) & 1; sf = (c->eflags >> 7) & 1; of = (c->eflags >> 11) & 1; } while (0)

#define PARITY(v) ((uint32_t)!__builtin_parity((v) & 0xffu))

/* ------------------------------------------------------------------ memory */
typedef uint8_t u8a __attribute__((may_alias));
typedef uint16_t u16a __attribute__((may_alias, aligned(1)));
typedef uint32_t u32a __attribute__((may_alias, aligned(1)));
typedef uint64_t u64a __attribute__((may_alias, aligned(1)));
#define GP(a) ((uintptr_t)(uint32_t)(a))

/* The generated code (CPU_FAST_MEM) accesses program memory as ordinary memory, so the compiler
   can keep values in registers and merge accesses; each loop pass and each call still reads
   memory again (MEMBARRIER at backward jumps; calls are opaque), which is what the program's
   other thread (the music timer, guarded by InterlockedExchange) relies on. The runtime keeps
   volatile accesses. */
#ifdef CPU_FAST_MEM
#define GVOL
#else
#define GVOL volatile
#endif
#define MEMBARRIER() __asm__ __volatile__("" ::: "memory")

static inline uint32_t R8(uint32_t a) { return *(GVOL u8a *)GP(a); }
static inline uint32_t R16(uint32_t a) { return *(GVOL u16a *)GP(a); }
static inline uint32_t R32(uint32_t a) { return *(GVOL u32a *)GP(a); }
static inline uint64_t R64(uint32_t a) { return *(GVOL u64a *)GP(a); }
static inline void W8(uint32_t a, uint32_t v) { *(GVOL u8a *)GP(a) = (uint8_t)v; }
static inline void W16(uint32_t a, uint32_t v) { *(GVOL u16a *)GP(a) = (uint16_t)v; }
static inline void W32(uint32_t a, uint32_t v) { *(GVOL u32a *)GP(a) = v; }
static inline void W64(uint32_t a, uint64_t v) { *(GVOL u64a *)GP(a) = v; }

static inline float RF32(uint32_t a) { uint32_t u = R32(a); float f; memcpy(&f, &u, 4); return f; }
static inline double RF64(uint32_t a) { uint64_t u = R64(a); double f; memcpy(&f, &u, 8); return f; }
static inline f80 RF80(uint32_t a) { f80 v = 0; memcpy(&v, (const void *)GP(a), 10); return v; }
static inline void WF32(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); W32(a, u); }
static inline void WF64(uint32_t a, double f) { uint64_t u; memcpy(&u, &f, 8); W64(a, u); }
static inline void WF80(uint32_t a, f80 v) { memcpy((void *)GP(a), &v, 10); }

/* rep movs / rep stos on the generated function's locals */
void rep_movs(CPU *c, int n, uint32_t src, uint32_t dst, uint32_t count);
void rep_stos(CPU *c, int n, uint32_t dst, uint32_t value, uint32_t count);
#define REP_MOVS(n) do { if (ecx) { rep_movs(c, n, esi, edi, ecx); uint32_t _t = ecx * (n); \
    if (c->df) { esi -= _t; edi -= _t; } else { esi += _t; edi += _t; } ecx = 0; } } while (0)
#define REP_STOS(n) do { if (ecx) { rep_stos(c, n, edi, eax, ecx); uint32_t _t = ecx * (n); \
    if (c->df) edi -= _t; else edi += _t; ecx = 0; } } while (0)

/* ------------------------------------------------------------------ x87 */
#define ST(i) (c->st[(c->ftop + (i)) & 7])
#define FPUSH(v) do { f80 _v = (v); c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = _v; } while (0)
#define FPOP() (c->ftop = (c->ftop + 1) & 7)

static inline void fpu_com(CPU *c, f80 a, f80 b)
{
    uint16_t cc;
    if (__builtin_isnan(a) || __builtin_isnan(b)) cc = 0x4500;
    else if (a < b) cc = 0x0100;
    else if (a == b) cc = 0x4000;
    else cc = 0;
    c->fsw = (uint16_t)((c->fsw & ~0x4700) | cc);
}
static inline int16_t fpu_ist16(f80 v) { int16_t r; __asm__("fistps %0" : "=m"(r) : "t"(v) : "st"); return r; }
static inline int32_t fpu_ist32(f80 v) { int32_t r; __asm__("fistpl %0" : "=m"(r) : "t"(v) : "st"); return r; }
static inline int64_t fpu_ist64(f80 v) { int64_t r; __asm__("fistpll %0" : "=m"(r) : "t"(v) : "st"); return r; }
/* truncating store whatever the rounding mode (SSE3): what _ftol does with its fldcw pair */
static inline int64_t fpu_ftol(f80 v) { int64_t r; __asm__("fisttpll %0" : "=m"(r) : "t"(v) : "st"); return r; }
static inline f80 fpu_fsqrt(f80 v) { __asm__("fsqrt" : "+t"(v)); return v; }
static inline f80 fpu_fabs(f80 v) { __asm__("fabs" : "+t"(v)); return v; }
static inline f80 fpu_frndint(f80 v) { __asm__("frndint" : "+t"(v)); return v; }

void fpu_setcw(CPU *c, uint32_t cw);
extern int fpu_sw_exceptions;
static inline uint32_t fpu_sw(CPU *c)  /* FNSTSW: condition bits, top, the host's exception bits */
{
    uint16_t sw = 0;
    if (fpu_sw_exceptions)
        __asm__ volatile("fnstsw %0" : "=am"(sw));
    return (c->fsw & 0x4700u) | ((c->ftop & 7) << 11) | (sw & 0x3fu);
}
void fpu_clex(CPU *c);
void fpu_init(CPU *c);                 /* FNINIT on the running CPU */
void fpu_new(CPU *c);                  /* a new record (host FPU untouched) */
void fpu_activate(CPU *c);             /* the host thread now runs c */
void fpu_depth_fail(CPU *c, uint32_t at) __attribute__((noreturn));
void fpu_xam(CPU *c);
void fpu_ldconst(CPU *c, int which);
void fpu_f2xm1(CPU *c);
void fpu_fyl2x(CPU *c);
void fpu_fyl2xp1(CPU *c);
void fpu_fptan(CPU *c);
void fpu_fpatan(CPU *c);
void fpu_fxtract(CPU *c);
void fpu_fprem(CPU *c);
void fpu_fprem1(CPU *c);
void fpu_fsincos(CPU *c);
void fpu_fscale(CPU *c);
void fpu_fsin(CPU *c);
void fpu_fcos(CPU *c);
void fpu_ldenv(CPU *c, uint32_t a);
void fpu_stenv(CPU *c, uint32_t a);
void fpu_save(CPU *c, uint32_t a);
void fpu_rstor(CPU *c, uint32_t a);

/* ------------------------------------------------------------------ control flow */
/* cpuid (the processor the game is told it runs on) and rdtsc (a 1 GHz count from the runtime's
   clock, so the game's speed measurement and test runs agree with it) */
void x86_cpuid(CPU *c);
uint64_t x86_rdtsc(CPU *c);

void icall(CPU *c, uint32_t target);                 /* call through a pointer */
void bad_ret(CPU *c, uint32_t site, uint32_t expected);
void bad_jump(CPU *c, uint32_t site, uint32_t target);
void nonlocal_jump(CPU *c, uint32_t site, uint32_t target);
void unsupported(CPU *c, uint32_t site, const char *what);
void trap_int3(CPU *c, uint32_t site);
void div_fault(CPU *c, uint32_t site);
void fell_off(CPU *c, uint32_t site);
void returned_from_noreturn(CPU *c, uint32_t site);
void missing_import(CPU *c, const char *dll, const char *name);

#define CHKRET(site, expected) do { if (__builtin_expect(c->ret != (expected), 0)) bad_ret(c, site, expected); } while (0)
