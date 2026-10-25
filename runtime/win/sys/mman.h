/* Windows stand-in (platform.c): the mmap uses of the runtime. Anonymous memory at a fixed address
   (the program's image and arena, inside the low range platform.c reserves at start-up), read-only
   views of whole files, page protection and giving pages back (they read as zeros again). */
#pragma once
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_ANON            MAP_ANONYMOUS
#define MAP_NORESERVE       0x4000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED          ((void *)-1)

#define MADV_DONTNEED 4

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
int madvise(void *addr, size_t len, int advice);
