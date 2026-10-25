/* Function-level check of the translation: each function of TD6.exe is run on random inputs
   twice, as the original machine code in the Unicorn emulator and as the translated C, and
   the results (registers, x87 stack, every byte written) must be identical.

     fncheck --game DIR [--seeds N] [--only ADDR] [--verbose]

   Functions that call into Windows/DirectX, fault on the random inputs, or run too long are
   skipped (they are covered by whole-program runs instead). The native run happens in a
   forked child so a crash only fails that one test. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unicorn/unicorn.h>
#include "runtime.h"
#include "image.h"

#define SENTINEL 0x7efe0000u
#define MAX_INSNS 3000000

static uc_engine *uc;
static uint32_t scratch, stack_top, teb, gdt_base;
static CPU *cpu;

/* ------------------------------------------------------------------ write journal */
typedef struct { uint32_t addr; uint8_t old; } Undo;
static Undo *journal;
static size_t njournal, capjournal;

static void hook_write(uc_engine *u, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud)
{
    (void)u; (void)type; (void)value; (void)ud;
    int ok = (addr >= IMAGE_BASE && addr + size <= IMAGE_BASE + (uint64_t)IMAGE_SIZE) ||
             (addr >= ARENA_START && addr + size <= ARENA_END);
    if (!ok)
        return;                          /* unmapped: the emulator faults right after */
    for (int i = 0; i < size; i++) {
        if (njournal == capjournal) {
            capjournal = capjournal ? capjournal * 2 : 65536;
            journal = realloc(journal, capjournal * sizeof *journal);
        }
        journal[njournal].addr = (uint32_t)addr + i;
        journal[njournal].old = R8((uint32_t)addr + i);
        njournal++;
    }
}

static bool hook_fetch_unmapped(uc_engine *u, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud)
{
    (void)u; (void)type; (void)addr; (void)size; (void)value; (void)ud;
    return false;
}

/* ------------------------------------------------------------------ x87 transcendentals
   Unicorn's fsin/fcos/fpatan/... differ from real hardware in the last bits (and so do the
   branches that follow them): run those instructions on the host CPU, like the port does. */
extern const uint32_t x87_special_addr[];
extern const uint8_t x87_special_op[];
extern const int x87_special_count;

static void hook_x87(uc_engine *u, uint64_t addr, uint32_t size, void *ud)
{
    uint8_t op = (uint8_t)(uintptr_t)ud;
    uint16_t sw, cw, tag;
    uc_reg_read(u, UC_X86_REG_FPSW, &sw);
    uc_reg_read(u, UC_X86_REG_FPCW, &cw);
    uc_reg_read(u, UC_X86_REG_FPTAG, &tag);
    CPU t;
    memset(&t, 0, sizeof t);
    t.ftop = (sw >> 11) & 7;
    t.fsw = sw & 0x4700;
    for (int i = 0; i < 8; i++) {
        uint8_t v[16] = {0};
        uc_reg_read(u, UC_X86_REG_FP0 + i, v);
        memcpy(&t.st[i], v, 10);
    }
    fpu_setcw(&t, cw);
    uint32_t top0 = t.ftop;
    switch (op) {
    case 0xfe: fpu_fsin(&t); break;
    case 0xff: fpu_fcos(&t); break;
    case 0xfb: fpu_fsincos(&t); break;
    case 0xf2: fpu_fptan(&t); break;
    case 0xf3: fpu_fpatan(&t); break;
    case 0xf1: fpu_fyl2x(&t); break;
    case 0xf9: fpu_fyl2xp1(&t); break;
    case 0xf0: fpu_f2xm1(&t); break;
    }
    for (int i = 0; i < 8; i++) {
        uint8_t v[16] = {0};
        memcpy(v, &t.st[i], 10);
        uc_reg_write(u, UC_X86_REG_FP0 + i, v);
    }
    if (t.ftop != top0) {
        if (((top0 - 1) & 7) == t.ftop) tag &= ~(3u << (2 * t.ftop));   /* pushed: valid */
        else tag |= 3u << (2 * top0);                                   /* popped: empty */
    }
    sw = (uint16_t)((sw & ~0x7f00u) | t.fsw | ((t.ftop & 7) << 11));
    uc_reg_write(u, UC_X86_REG_FPSW, &sw);
    uc_reg_write(u, UC_X86_REG_FPTAG, &tag);
    uint32_t next = (uint32_t)addr + size;
    uc_reg_write(u, UC_X86_REG_EIP, &next);
}

/* ------------------------------------------------------------------ GDT so fs: works */
static void set_gdt_entry(uint32_t at, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    uint64_t d = (limit & 0xffff) | ((uint64_t)(base & 0xffffff) << 16) | ((uint64_t)access << 40) |
                 ((uint64_t)((limit >> 16) & 0xf) << 48) | ((uint64_t)flags << 52) | ((uint64_t)(base >> 24) << 56);
    W64(at, d);
}

static void uc_setup(void)
{
    if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc)) fatal("uc_open");
    uint32_t isz = (IMAGE_SIZE + 0xfff) & ~0xfffu;
    if (uc_mem_map_ptr(uc, IMAGE_BASE, isz, UC_PROT_ALL, (void *)(uintptr_t)IMAGE_BASE)) fatal("map image");
    if (uc_mem_map_ptr(uc, ARENA_START, ARENA_END - ARENA_START, UC_PROT_ALL, (void *)(uintptr_t)ARENA_START))
        fatal("map arena");
    if (uc_mem_map(uc, SENTINEL, 0x1000, UC_PROT_ALL)) fatal("map sentinel");
    uc_hook h1, h2;
    uc_hook_add(uc, &h1, UC_HOOK_MEM_WRITE, hook_write, NULL, 1, 0);
    uc_hook_add(uc, &h2, UC_HOOK_MEM_FETCH_UNMAPPED, hook_fetch_unmapped, NULL, 1, 0);
    for (int i = 0; i < x87_special_count && !getenv("FNCHECK_NO_X87_HOOK"); i++) {
        uc_hook h;
        uc_hook_add(uc, &h, UC_HOOK_CODE, hook_x87, (void *)(uintptr_t)x87_special_op[i],
                    x87_special_addr[i], x87_special_addr[i]);
    }
    /* GDT: 14 code (ring 3), 15 data (ring 3), 16 fs -> TEB, 17 stack (ring 0) */
    gdt_base = vm_alloc(0, 0x10000, 0);
    set_gdt_entry(gdt_base + 14 * 8, 0, 0xfffff, 0xfb, 0xc);
    set_gdt_entry(gdt_base + 15 * 8, 0, 0xfffff, 0xf3, 0xc);
    set_gdt_entry(gdt_base + 16 * 8, teb, 0xfff, 0xf3, 0x4);
    set_gdt_entry(gdt_base + 17 * 8, 0, 0xfffff, 0x93, 0xc);
    uc_x86_mmr gdtr = {0, gdt_base, 31 * 8 - 1, 0};
    uc_reg_write(uc, UC_X86_REG_GDTR, &gdtr);
    int sel;
    sel = 0x73; uc_reg_write(uc, UC_X86_REG_CS, &sel);
    sel = 0x7b; uc_reg_write(uc, UC_X86_REG_DS, &sel);
    sel = 0x7b; uc_reg_write(uc, UC_X86_REG_ES, &sel);
    sel = 0x83; uc_reg_write(uc, UC_X86_REG_FS, &sel);
    sel = 0x88; uc_reg_write(uc, UC_X86_REG_SS, &sel);
}

/* ------------------------------------------------------------------ random inputs */
static uint64_t rng;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static int tame;          /* odd seeds: only small numbers and pointers into the scratch area */

static uint32_t rnd_value(void)
{
    if (tame)
        return rnd() % 3 ? scratch + (rnd() % 0xf000 & ~3u) : rnd() % 32;
    switch (rnd() % 6) {
    case 0: return 0;
    case 1: return rnd() % 16;
    case 2: return rnd() % 256;
    case 3: return scratch + (rnd() % 0x8000) * 4 % 0xf000;
    case 4: return 0x3f800000u;          /* 1.0f */
    default: return rnd();
    }
}

static void fill_scratch(void)
{
    for (uint32_t i = 0; i < 0x10000; i += 4)
        W32(scratch + i, (rnd() % 4 == 0) ? scratch + (rnd() % 0xf000 & ~3u) : rnd() % 3 == 0 ? rnd() % 64 : rnd());
}

/* ------------------------------------------------------------------ one test */
enum { R_PASS, R_SKIP_IMPORT, R_SKIP_FAULT, R_SKIP_LONG, R_FAIL };
static const char *rname[] = {"pass", "import", "fault", "long", "FAIL"};

typedef struct { uint32_t r[8]; uint32_t top; uint8_t st[8][10]; uint16_t tag; } State;
extern int fpu_sw_exceptions;

static int regid[8] = {UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
                       UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI};

static int verbose;

static int run_one(uint32_t fn, uint64_t seed, char *why, size_t whysz)
{
    rng = seed * 0x9e3779b97f4a7c15ull + fn;
    if (!rng) rng = 1;
    tame = seed & 1;
    fill_scratch();
    State in = {0};
    for (int i = 0; i < 8; i++) in.r[i] = rnd_value();
    in.r[4] = stack_top - 0x800;
    for (int i = 0; i < 16; i++)
        W32(in.r[4] + 4 + 4 * i, rnd_value());
    W32(in.r[4], SENTINEL);
    uint32_t teb_seh = R32(teb);
    /* two values on the x87 stack (functions like _ftol take their argument there) */
    long double fin[2];
    for (int i = 0; i < 2; i++)
        fin[i] = (long double)((int32_t)(rnd() % 2000000) - 1000000) / 1000.0L;

    /* ---- original code under emulation */
    njournal = 0;
    for (int i = 0; i < 8; i++) uc_reg_write(uc, regid[i], &in.r[i]);
    uint32_t fl = 0x202; uc_reg_write(uc, UC_X86_REG_EFLAGS, &fl);
    uint16_t cw = 0x37f, sw = 6 << 11, tag = 0x0fff;   /* top = 6: phys 6, 7 in use */
    for (int i = 0; i < 8; i++) {
        uint8_t v[16] = {0};
        if (i >= 6) memcpy(v, &fin[i - 6], 10);
        uc_reg_write(uc, UC_X86_REG_FP0 + i, v);
    }
    uc_reg_write(uc, UC_X86_REG_FPCW, &cw);
    uc_reg_write(uc, UC_X86_REG_FPSW, &sw);
    uc_reg_write(uc, UC_X86_REG_FPTAG, &tag);
    uc_err e = uc_emu_start(uc, fn, SENTINEL, 0, MAX_INSNS);
    uint32_t pc = 0;
    uc_reg_read(uc, UC_X86_REG_EIP, &pc);
    int result = R_PASS;
    if (e == UC_ERR_FETCH_UNMAPPED && thunk_name(pc)) result = R_SKIP_IMPORT;
    else if (e != UC_ERR_OK) result = R_SKIP_FAULT;
    else if (pc != SENTINEL) result = R_SKIP_LONG;
    State out = {0};
    for (int i = 0; i < 8; i++) uc_reg_read(uc, regid[i], &out.r[i]);
    uc_reg_read(uc, UC_X86_REG_FPSW, &sw);
    uc_reg_read(uc, UC_X86_REG_FPTAG, &out.tag);
    out.top = (sw >> 11) & 7;
    for (int i = 0; i < 8; i++) {
        uint8_t v[16] = {0};
        uc_reg_read(uc, UC_X86_REG_FP0 + i, v);
        memcpy(out.st[i], v, 10);
    }
    /* final values of written bytes, then undo the writes */
    size_t nw = njournal;
    Undo *writes = malloc((nw ? nw : 1) * sizeof *writes);
    for (size_t i = 0; i < nw; i++) {
        writes[i].addr = journal[i].addr;
        writes[i].old = R8(journal[i].addr);
    }
    for (size_t i = nw; i-- > 0;)
        W8(journal[i].addr, journal[i].old);
    W32(teb, teb_seh);
    if (result != R_PASS) {
        free(writes);
        return result;
    }

    /* ---- translated code in a child process */
    int pfd[2];
    if (pipe(pfd)) fatal("pipe");
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        alarm(10);
        for (int i = 0; i < 8; i++) (&cpu->eax)[i] = in.r[i];
        cpu->ftop = 6;
        cpu->fsw = 0;
        for (int i = 0; i < 8; i++) cpu->st[i] = i >= 6 ? fin[i - 6] : 0;
        fpu_sw_exceptions = 0;
        fpu_setcw(cpu, 0x37f);
        cpu->df = 0;
        icall(cpu, fn);
        char msg[4096];
        int n = 0;
        if (cpu->ret != SENTINEL)
            n += snprintf(msg + n, sizeof msg - n, "returned to %08x; ", cpu->ret);
        static const char *rn[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
        for (int i = 0; i < 8; i++)
            if ((&cpu->eax)[i] != out.r[i])
                n += snprintf(msg + n, sizeof msg - n, "%s %08x vs %08x; ", rn[i], (&cpu->eax)[i], out.r[i]);
        if ((cpu->ftop & 7) != out.top)
            n += snprintf(msg + n, sizeof msg - n, "fpu top %u vs %u; ", cpu->ftop & 7, out.top);
        else
            for (int p = out.top; p < 8; p++) {
                if (memcmp(&cpu->st[p], out.st[p], 10)) {
                    long double uv;
                    memset(&uv, 0, sizeof uv);
                    memcpy(&uv, out.st[p], 10);
                    n += snprintf(msg + n, sizeof msg - n, "st phys%d %.20Lg vs %.20Lg; ", p, cpu->st[p], uv);
                }
            }
        size_t bad = 0;
        for (size_t i = 0; i < nw && n < 3500; i++) {
            uint8_t want = writes[i].old;      /* the emulator's final value at this address */
            if (R8(writes[i].addr) != want) {
                if (bad++ < 8)
                    n += snprintf(msg + n, sizeof msg - n, "mem %08x %02x vs %02x; ", writes[i].addr, R8(writes[i].addr), want);
            }
        }
        if (bad > 8)
            n += snprintf(msg + n, sizeof msg - n, "(%zu bytes differ) ", bad);
        if (write(pfd[1], msg, n) < 0) {}
        _exit(n ? 1 : 0);
    }
    close(pfd[1]);
    int len = (int)read(pfd[0], why, whysz - 1);
    close(pfd[0]);
    why[len > 0 ? len : 0] = 0;
    int status;
    waitpid(pid, &status, 0);
    free(writes);
    if (WIFSIGNALED(status)) {
        snprintf(why, whysz, "translated code died with signal %d", WTERMSIG(status));
        return R_FAIL;
    }
    if (WEXITSTATUS(status) != 0)
        return R_FAIL;
    return R_PASS;
}

int main(int argc, char **argv)
{
    int seeds = 4;
    uint32_t only = 0;
    const char *home = getenv("HOME");
    snprintf(user_dir, 1024, "%s/.local/share/td6-native", home);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) snprintf(game_dir, 1024, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) snprintf(user_dir, 1024, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--seeds") && i + 1 < argc) seeds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
    }
    log_mask = 0;
    deterministic = 1;                  /* the game as shipped: no Enhanced hooks, no mods */
    runtime_setup();
    log_mask = LOG_ALWAYS;
    cpu = cpu_new(0x100000);
    set_current_cpu(cpu);
    teb = cpu->fs_base;
    stack_top = cpu->stack_hi - 0x10000;
    scratch = vm_alloc(0, 0x10000, 0);
    uc_setup();
    int count[5] = {0};
    for (int k = 0; k < func_count; k++) {
        uint32_t fn = func_table[k].addr;
        if (only && fn != only) continue;
        int worst = -1;
        char why[4096] = "";
        for (int s = 0; s < seeds; s++) {
            char w[4096];
            int r = run_one(fn, s + 1, w, sizeof w);
            count[r]++;
            if (r == R_FAIL && worst != R_FAIL) {
                worst = R_FAIL;
                snprintf(why, sizeof why, "seed %d: %s", s + 1, w);
            } else if (r == R_PASS && worst < 0) worst = R_PASS;
            if (verbose)
                printf("%08x seed %d: %s %s\n", fn, s + 1, rname[r], r == R_FAIL ? w : "");
        }
        if (worst == R_FAIL)
            printf("FAIL %08x: %s\n", fn, why);
        fflush(stdout);
    }
    printf("runs: %d pass, %d skipped (import %d, fault %d, long %d), %d FAIL\n", count[R_PASS],
           count[1] + count[2] + count[3], count[1], count[2], count[3], count[R_FAIL]);
    return count[R_FAIL] ? 1 : 0;
}
