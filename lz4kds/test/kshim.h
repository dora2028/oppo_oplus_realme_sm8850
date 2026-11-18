/* Userspace shims so the real kernel sources compile for the host test. */
#ifndef _KSHIM_H
#define _KSHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

#define SZ_4K 4096u
#define PAGE_SIZE 4096u

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)

#define GFP_KERNEL  0u
#define GFP_ATOMIC  1u

#define IS_ENABLED(c) (c)
#define IS_ERR(p) 0
#define IS_ERR_VALUE(v) 0
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define EINVAL 22
#define ENOMEM 12
#define EOPNOTSUPP 95

/* --- allocators ------------------------------------------------------- */
static inline void *kvzalloc(size_t n, unsigned g) { (void)g; return calloc(1, n); }
static inline void *kvcalloc(size_t n, size_t s, unsigned g) { (void)g; return calloc(n, s); }
static inline void *kvmalloc(size_t n, unsigned g) { (void)g; return malloc(n); }
static inline void *kzalloc(size_t n, unsigned g) { (void)g; return calloc(1, n); }
static inline void *kcalloc(size_t n, size_t s, unsigned g) { (void)g; return calloc(n, s); }
static inline void kvfree(void *p) { free(p); }
static inline void kfree(void *p) { free(p); }

/* --- atomics (single threaded in the test) ---------------------------- */
typedef struct { int v; } atomic_t;
typedef struct { long v; } atomic64_t;

static inline void atomic_set(atomic_t *a, int v) { a->v = v; }
static inline int  atomic_read(const atomic_t *a) { return a->v; }
static inline void atomic_inc(atomic_t *a) { a->v++; }
static inline void atomic_dec(atomic_t *a) { a->v--; }
static inline void atomic64_set(atomic64_t *a, long v) { a->v = v; }
static inline long atomic64_read(const atomic64_t *a) { return a->v; }
static inline void atomic64_add(long d, atomic64_t *a) { a->v += d; }
static inline int atomic_dec_if_positive(atomic_t *a) { if (a->v > 0) a->v--; return a->v; }

/* --- spinlock (no-op single threaded) -------------------------------- */
typedef struct { int dummy; } spinlock_t;
#define spin_lock_init(l) do { (l)->dummy = 0; } while (0)
#define spin_lock(l)      do { (void)(l); } while (0)
#define spin_unlock(l)    do { (void)(l); } while (0)

/* --- jhash: real algorithm, from the kernel -------------------------- */
#define JHASH_INITVAL 0xdeadbeef
static inline u32 jhash(const void *key, u32 length, u32 initval)
{
	u32 a, b, c;
	const u8 *k = key;

	a = b = c = JHASH_INITVAL + length + initval;
	while (length > 12) {
		a += k[0] + ((u32)k[1] << 8) + ((u32)k[2] << 16) + ((u32)k[3] << 24);
		b += k[4] + ((u32)k[5] << 8) + ((u32)k[6] << 16) + ((u32)k[7] << 24);
		c += k[8] + ((u32)k[9] << 8) + ((u32)k[10] << 16) + ((u32)k[11] << 24);
		/* __jhash_mix */
		a -= c; a ^= (c << 4) | (c >> 28); c += b;
		b -= a; b ^= (a << 6) | (a >> 26); a += c;
		c -= b; c ^= (b << 8) | (b >> 24); b += a;
		a -= c; a ^= (c << 16) | (c >> 16); c += b;
		b -= a; b ^= (a << 19) | (a >> 13); a += c;
		c -= b; c ^= (b << 4) | (b >> 28); b += a;
		length -= 12;
		k += 12;
	}
	switch (length) {
	case 12: c += ((u32)k[11]) << 24; /* fallthrough */
	case 11: c += ((u32)k[10]) << 16; /* fallthrough */
	case 10: c += ((u32)k[9]) << 8;   /* fallthrough */
	case 9:  c += k[8];               /* fallthrough */
	case 8:  b += ((u32)k[7]) << 24;  /* fallthrough */
	case 7:  b += ((u32)k[6]) << 16;  /* fallthrough */
	case 6:  b += ((u32)k[5]) << 8;   /* fallthrough */
	case 5:  b += k[4];               /* fallthrough */
	case 4:  a += ((u32)k[3]) << 24;  /* fallthrough */
	case 3:  a += ((u32)k[2]) << 16;  /* fallthrough */
	case 2:  a += ((u32)k[1]) << 8;   /* fallthrough */
	case 1:  a += k[0];
	}
	/* __jhash_final */
	c ^= b; c -= (b << 14) | (b >> 18);
	a ^= c; a -= (c << 11) | (c >> 21);
	b ^= a; b -= (a << 25) | (a >> 7);
	c ^= b; c -= (b << 16) | (b >> 16);
	a ^= c; a -= (c << 4)  | (c >> 28);
	b ^= a; b -= (a << 14) | (a >> 18);
	c ^= b; c -= (b << 24) | (b >> 8);
	return c;
}

#endif
