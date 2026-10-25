/* x87 operations the generated code calls out of line. They run the real x87 instruction on
   the host so results match the original bit for bit (the host's control word mirrors the
   program's, with exceptions masked). */
#include <math.h>
#include "runtime.h"

static inline void host_fldcw(uint16_t cw) { __asm__ volatile("fldcw %0" : : "m"(cw)); }
static inline uint16_t host_fnstsw(void) { uint16_t sw; __asm__ volatile("fnstsw %0" : "=am"(sw)); return sw; }

void fpu_setcw(CPU *c, uint32_t cw)
{
    c->fcw = (uint16_t)cw;
    host_fldcw((uint16_t)(cw | 0x3f));
}

/* A new CPU record's x87 state; the host FPU is not touched (the record may be for another
   thread): fpu_activate loads it once the record runs on a thread. */
void fpu_new(CPU *c)
{
    c->ftop = 0;
    c->fsw = 0;
    for (int i = 0; i < 8; i++)
        c->st[i] = 0;
    c->fcw = 0x37f;
}

/* This host thread now runs c: its FPU takes c's control word. */
void fpu_activate(CPU *c)
{
    host_fldcw((uint16_t)(c->fcw | 0x3f));
}

/* FNINIT on the running CPU */
void fpu_init(CPU *c)
{
    __asm__ volatile("fninit");
    c->ftop = 0;
    c->fsw = 0;
    for (int i = 0; i < 8; i++)
        c->st[i] = 0;
    fpu_setcw(c, 0x37f);
}

/* The exception bits come from the host x87, which ran the same operations. Tests against
   an emulator that does not model them can turn them off. (fpu_sw itself is inline, cpu.h.) */
int fpu_sw_exceptions = 1;

void fpu_clex(CPU *c)
{
    (void)c;
    __asm__ volatile("fnclex");
}

static void setcc(CPU *c, uint16_t sw, uint16_t mask)
{
    c->fsw = (uint16_t)((c->fsw & ~mask) | (sw & mask));
}

void fpu_xam(CPU *c)
{
    f80 v = ST(0);
    uint16_t sw;
    __asm__("fxam; fnstsw %0" : "=am"(sw) : "t"(v));
    setcc(c, sw, 0x4700);
}

void fpu_ldconst(CPU *c, int which)
{
    f80 v;
    switch (which) {
    case 0: __asm__("fld1" : "=t"(v)); break;
    case 1: __asm__("fldl2t" : "=t"(v)); break;
    case 2: __asm__("fldl2e" : "=t"(v)); break;
    case 3: __asm__("fldpi" : "=t"(v)); break;
    case 4: __asm__("fldlg2" : "=t"(v)); break;
    default: __asm__("fldln2" : "=t"(v)); break;
    }
    FPUSH(v);
}

void fpu_f2xm1(CPU *c) { f80 v = ST(0); __asm__("f2xm1" : "+t"(v)); ST(0) = v; }

void fpu_fyl2x(CPU *c)
{
    f80 x = ST(0), y = ST(1), r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    FPOP();
    ST(0) = r;
}

void fpu_fyl2xp1(CPU *c)
{
    f80 x = ST(0), y = ST(1), r;
    __asm__("fyl2xp1" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    FPOP();
    ST(0) = r;
}

void fpu_fpatan(CPU *c)
{
    f80 x = ST(0), y = ST(1), r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    FPOP();
    ST(0) = r;
}

void fpu_fptan(CPU *c)
{
    f80 x = ST(0), one;
    uint16_t sw;
    __asm__("fptan; fnstsw %2" : "=t"(one), "=u"(x), "=am"(sw) : "0"(x));
    setcc(c, sw, 0x0600);
    if (sw & 0x0400)                 /* out of range: operand unchanged, nothing pushed */
        return;
    ST(0) = x;
    FPUSH(one);
}

void fpu_fsincos(CPU *c)
{
    f80 x = ST(0), s, co;
    uint16_t sw;
    __asm__("fsincos; fnstsw %2" : "=t"(co), "=u"(s), "=am"(sw) : "0"(x));
    setcc(c, sw, 0x0600);
    if (sw & 0x0400)
        return;
    ST(0) = s;
    FPUSH(co);
}

void fpu_fsin(CPU *c)
{
    f80 x = ST(0);
    uint16_t sw;
    __asm__("fsin; fnstsw %1" : "+t"(x), "=am"(sw));
    setcc(c, sw, 0x0600);
    ST(0) = x;
}

void fpu_fcos(CPU *c)
{
    f80 x = ST(0);
    uint16_t sw;
    __asm__("fcos; fnstsw %1" : "+t"(x), "=am"(sw));
    setcc(c, sw, 0x0600);
    ST(0) = x;
}

void fpu_fscale(CPU *c)
{
    f80 x = ST(0), y = ST(1);
    __asm__("fscale" : "+t"(x) : "u"(y));
    ST(0) = x;
}

void fpu_fxtract(CPU *c)
{
    f80 x = ST(0), sig, ex;
    __asm__("fxtract" : "=t"(sig), "=u"(ex) : "0"(x));
    ST(0) = ex;
    FPUSH(sig);
}

void fpu_fprem(CPU *c)
{
    f80 x = ST(0), y = ST(1);
    uint16_t sw;
    __asm__("fprem; fnstsw %1" : "+t"(x), "=am"(sw) : "u"(y));
    setcc(c, sw, 0x4700);
    ST(0) = x;
}

void fpu_fprem1(CPU *c)
{
    f80 x = ST(0), y = ST(1);
    uint16_t sw;
    __asm__("fprem1; fnstsw %1" : "+t"(x), "=am"(sw) : "u"(y));
    setcc(c, sw, 0x4700);
    ST(0) = x;
}

/* 28-byte protected-mode environment: CW, SW, TW, then instruction/operand pointers. */
void fpu_stenv(CPU *c, uint32_t a)
{
    W32(a, c->fcw | 0xffff0000u);
    W32(a + 4, fpu_sw(c) | 0xffff0000u);
    W32(a + 8, 0xffffffffu);
    for (int i = 12; i < 28; i += 4)
        W32(a + i, 0);
}

void fpu_ldenv(CPU *c, uint32_t a)
{
    fpu_setcw(c, R16(a));
    uint16_t sw = (uint16_t)R16(a + 4);
    c->fsw = sw & 0x4700;
    c->ftop = (sw >> 11) & 7;
}

void fpu_save(CPU *c, uint32_t a)
{
    fpu_stenv(c, a);
    W32(a + 8, 0);                    /* all registers tagged valid */
    for (int i = 0; i < 8; i++)
        WF80(a + 28 + 10 * i, ST(i));
    fpu_init(c);
}

void fpu_rstor(CPU *c, uint32_t a)
{
    fpu_ldenv(c, a);
    for (int i = 0; i < 8; i++)
        ST(i) = RF80(a + 28 + 10 * i);
}

/* Generated code keeping the x87 stack in locals assumed a call through a pointer leaves the
   stack depth as it was (tools/fpustack.py); it did not. */
void fpu_depth_fail(CPU *c, uint32_t at)
{
    fatal("x87 stack depth changed by the call at %08x (top %u): add the callee's effect to the "
          "translator (tools/fpustack.py)", at, c->ftop);
}
