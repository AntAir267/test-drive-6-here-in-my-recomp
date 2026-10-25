/* Windows stand-in (platform.c): waiting for a program spawn.h started; its exit code as Linux
   reports a normal exit. */
#pragma once
#include <sys/types.h>

#define WNOHANG 1
#define WIFEXITED(s)   1
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) 0
#define WTERMSIG(s)    0

pid_t waitpid(pid_t pid, int *status, int options);
int kill(pid_t pid, int sig);
