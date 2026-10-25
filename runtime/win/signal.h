/* Windows stand-in (platform.c) over the system's signal.h: sigaction for the crash handlers
   (memory faults, illegal instructions and arithmetic faults arrive as Windows exceptions the
   program did not handle). SIGPROF (the profiler) comes from platform.c's sampling thread
   (setitimer). */
#pragma once
#include_next <signal.h>
#include <sys/types.h>

#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGPROF
#define SIGPROF 27
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif

#define SA_SIGINFO 4
#define SA_RESTART 0x10000000

typedef struct {
    int si_signo, si_code;
    void *si_addr;
} siginfo_t;

typedef unsigned long win_sigset_t;
struct sigaction {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t *, void *);
    win_sigset_t sa_mask;
    int sa_flags;
};

int sigaction(int sig, const struct sigaction *sa, struct sigaction *old);
