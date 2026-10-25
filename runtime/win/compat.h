/* Windows host (MSYS2 UCRT64, WINDOWS.md): included before every runtime source by the
   Makefile's Windows block. The runtime is written against Linux; the few POSIX pieces MinGW
   lacks are stood in for here and in the headers next to this one (sys/mman.h, dlfcn.h,
   spawn.h, ...), implemented in platform.c, so the sources stay as they are on Linux. */
#pragma once
#ifndef _WIN32
#error "runtime/win is for Windows builds only"
#endif

/* the system's declarations first, so the macros below do not rewrite them */
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* mkdir takes no mode on Windows */
#define mkdir(path, mode) _mkdir(path)

/* handles are not inherited by child processes (spawn.h passes the ones it means to) */
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif

#define _SC_NPROCESSORS_ONLN 84
long win_sysconf(int name);
#define sysconf win_sysconf

ssize_t win_pread(int fd, void *buf, size_t n, int64_t off);
#define pread win_pread
int win_pipe(int fd[2]);
#define pipe win_pipe

/* exit runs the C runtime's atexit work and flushes files, then ends the process without the DLLs'
   own teardown: Vulkan drivers and layers can deadlock there (seen with the validation layer) */
void win_exit(int code) __attribute__((noreturn));
#define exit win_exit

/* rename replaces an existing file, as on Linux (the runtime writes x.new, then renames it over x) */
#include <stdio.h>
int win_rename(const char *from, const char *to);
#define rename win_rename

char *win_strcasestr(const char *hay, const char *needle);
#define strcasestr win_strcasestr
void *win_memmem(const void *hay, size_t hn, const void *needle, size_t nn);
#define memmem win_memmem
const char *win_strsignal(int sig);
#define strsignal win_strsignal

/* what posix_spawnp's callers pass as the environment (the child gets this process's anyway) */
#undef environ
#define environ win_environ
extern char **win_environ;

/* C11's aligned_alloc is missing from the UCRT; the runtime never frees these blocks */
#define aligned_alloc(align, size) _aligned_malloc((size), (align))

/* Sleeps as precise as on Linux: Windows' default timer rounds them up to 15.6 ms */
int win_nanosleep(const struct timespec *req, struct timespec *rem);
#define nanosleep win_nanosleep
int win_clock_nanosleep(clockid_t clock, int flags, const struct timespec *req, struct timespec *rem);
#define clock_nanosleep win_clock_nanosleep
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

/* threads get Linux's default 8 MB stack when the caller does not say (the translated code
   recurses on the host stack as deep as the game does) */
int win_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
#define pthread_create win_pthread_create
