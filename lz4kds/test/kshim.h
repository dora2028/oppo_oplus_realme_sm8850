/* Userspace shim so the kernel LZ4KD codec compiles for the host test. */
#ifndef _KSHIM_H
#define _KSHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)

static inline void *kvzalloc(size_t n, unsigned g) { (void)g; return calloc(1, n); }
static inline void *kvmalloc(size_t n, unsigned g) { (void)g; return malloc(n); }
static inline void kvfree(void *p) { free(p); }

#endif
