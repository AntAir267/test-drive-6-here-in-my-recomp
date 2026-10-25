/* Windows stand-in over the system's sys/time.h: ITIMER_PROF for the profiler (prof.c), which
   platform.c runs as a thread sampling the game's main thread. */
#pragma once
#include_next <sys/time.h>

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct itimerval {
    struct timeval it_interval, it_value;
};

int setitimer(int which, const struct itimerval *nv, struct itimerval *old);
