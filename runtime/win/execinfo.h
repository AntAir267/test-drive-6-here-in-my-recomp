/* Windows stand-in (platform.c): stack backtraces for crash reports and the debug logs. */
#pragma once

int backtrace(void **buf, int size);
/* one malloc'd block, free() it: "module(F_<address>+0x..) [address]" per entry */
char **backtrace_symbols(void *const *buf, int size);
