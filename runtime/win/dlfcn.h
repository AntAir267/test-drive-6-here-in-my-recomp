/* Windows stand-in (platform.c): libraries loaded at run time ("libfoo.so.N" opens libfoo-N.dll),
   and dladdr for the debug logs' call chains (a translated function's name from func_table). */
#pragma once

#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_LOCAL  0
#define RTLD_GLOBAL 0x100

typedef struct {
    const char *dli_fname;              /* the module's path */
    void *dli_fbase;                    /* where it is loaded */
    const char *dli_sname;              /* F_<address> for translated code, else NULL */
    void *dli_saddr;
} Dl_info;

void *dlopen(const char *name, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
const char *dlerror(void);
int dladdr(const void *addr, Dl_info *info);
