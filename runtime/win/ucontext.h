/* Windows stand-in: enough of ucontext_t for prof.c (platform.c's sampler fills in RIP). */
#pragma once
#include <stdint.h>

#define REG_RIP 16
typedef struct { struct { int64_t gregs[23]; } uc_mcontext; } ucontext_t;
