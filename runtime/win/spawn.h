/* Windows stand-in (platform.c): starting a helper program (ffmpeg for the intro videos, the RAR
   tool for old self-extracting fan downloads) with its standard handles redirected. Only what
   the runtime uses: open a file (/dev/null is NUL) or dup2 a descriptor onto 0, 1 or 2. The
   program is looked up on PATH as CreateProcess does. pid_t values work with waitpid and kill. */
#pragma once
#include <sys/types.h>

#define WIN_SPAWN_ACTIONS 8
typedef struct {
    int n;
    struct { int kind, fd, src, oflag; char path[1024]; } a[WIN_SPAWN_ACTIONS];
} posix_spawn_file_actions_t;
typedef struct { int unused; } posix_spawnattr_t;

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path, int oflag, mode_t mode);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int src, int fd);
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd);
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[]);
#define posix_spawn posix_spawnp
