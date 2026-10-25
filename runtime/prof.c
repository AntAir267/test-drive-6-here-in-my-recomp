/* Sampling profiler for development (perf is not available without root): TD6_PROFILE=<file>
   samples the program counter of whichever thread is using the CPU every millisecond of CPU
   time and, at exit, writes "count module offset" lines; tools/profile.py turns them into a
   per-function report. TD6_PROFILE_FROM=<frame> leaves out what comes before that frame (the start
   and loading). On Windows (platform.c) the samples are the game thread's, every millisecond of wall
   time, waits included. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include "runtime.h"

#define MAX_SAMPLES (1u << 22)
static uintptr_t *samples;
static volatile unsigned nsamples;
static char outpath[1024];
static unsigned long from_frame;        /* TD6_PROFILE_FROM: samples before that frame are left out */

static void on_prof(int sig, siginfo_t *si, void *ucv)
{
    (void)sig; (void)si;
    ucontext_t *uc = ucv;
    if (frame_count < from_frame)
        return;
    unsigned i = __atomic_fetch_add(&nsamples, 1, __ATOMIC_RELAXED);
    if (i < MAX_SAMPLES)
        samples[i] = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
}

static int cmp_ptr(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

static void prof_write(void)
{
    struct itimerval off = {0};
    setitimer(ITIMER_PROF, &off, NULL);
    unsigned n = nsamples < MAX_SAMPLES ? nsamples : MAX_SAMPLES;
    FILE *f = fopen(outpath, "w");
    if (!f) return;
    qsort(samples, n, sizeof *samples, cmp_ptr);
    for (unsigned i = 0; i < n;) {
        unsigned j = i;
        while (j < n && samples[j] == samples[i]) j++;
        Dl_info di;
        if (dladdr((void *)samples[i], &di) && di.dli_fname)
            fprintf(f, "%u %s %lx\n", j - i, di.dli_fname, (unsigned long)(samples[i] - (uintptr_t)di.dli_fbase));
        else
            fprintf(f, "%u ? %lx\n", j - i, (unsigned long)samples[i]);
        i = j;
    }
    fclose(f);
    LOG(LOG_ALWAYS, "profile: %u samples written to %s", n, outpath);
}

void prof_init(void)
{
    const char *p = getenv("TD6_PROFILE");
    if (!p || !*p) return;
    snprintf(outpath, sizeof outpath, "%s", p);
    const char *from = getenv("TD6_PROFILE_FROM");
    from_frame = from ? strtoul(from, NULL, 10) : 0;
    samples = calloc(MAX_SAMPLES, sizeof *samples);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &it, NULL);
    atexit(prof_write);
}
