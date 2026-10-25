/* The processor as the program sees it: CPUID and the time-stamp counter.

   TD6 identifies the processor at start-up (0x40fa30: the EFLAGS.ID test, CPUID leaves 0, 1 and 3,
   then its clock speed from RDTSC over a timed wait) and writes it to TD6.log. It is told it runs
   on a Pentium III (family 6 model 7: MMX and SSE, "KNI" in its log), the kind of machine it was
   made for; the counter runs at 1 GHz on the runtime's clock, so the measured speed is the same
   on every machine and in test runs. */
#include "runtime.h"

void x86_cpuid(CPU *c)
{
    static const char vendor[13] = "GenuineIntel";
    uint32_t leaf = c->eax;
    uint32_t v[3];
    memcpy(v, vendor, 12);
    if (leaf == 0) {
        c->eax = 1;
        c->ebx = v[0];
        c->edx = v[1];
        c->ecx = v[2];
    } else if (leaf == 1) {
        c->eax = 0x673;             /* family 6, model 7, stepping 3 */
        c->ebx = 0;
        c->ecx = 0;
        c->edx = 0x0383fbffu;       /* FPU ... CMOV, MMX, FXSR, SSE; no serial number */
    } else {
        c->eax = c->ebx = c->ecx = c->edx = 0;
    }
}

uint64_t x86_rdtsc(CPU *c)
{
    (void)c;
    return (uint64_t)(now_ms() * 1e6);
}
