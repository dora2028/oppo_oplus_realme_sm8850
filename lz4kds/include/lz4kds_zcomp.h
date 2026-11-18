/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * zcomp interface view used by the self-contained LZ4KDS backend.
 *
 * When built inside the kernel tree the real drivers/block/zram/zcomp.h is
 * used, so the backend always sees the exact upstream struct layout.  A
 * standalone copy is provided only so the file can be read/reviewed outside
 * a kernel tree.
 *
 * The in-tree path is selected by __KERNEL__, not by a manually defined
 * macro: a previous revision gated it on LZ4KDS_IN_TREE, which the injector
 * never defined, so the backend silently compiled against the local struct
 * copies below.  Those copies happened to match upstream, but any future
 * upstream field addition would have produced a silently corrupt vtable
 * instead of a compile error.
 */
#ifndef _LZ4KDS_ZCOMP_H
#define _LZ4KDS_ZCOMP_H

#ifdef __KERNEL__
/* Building inside the kernel tree: use the real zcomp interface. */
#include "zcomp.h"
#else
/* Standalone view, for review and host-side checking only. */
#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

typedef int32_t s32;

#define ZCOMP_PARAM_NO_LEVEL	INT_MIN

struct zcomp_params {
	void *dict;
	size_t dict_sz;
	s32 level;
	void *drv_data;
};

struct zcomp_ctx {
	void *context;
};

struct zcomp_req {
	const unsigned char *src;
	const size_t src_len;
	unsigned char *dst;
	size_t dst_len;
};

struct zcomp_ops {
	int (*compress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req);
	int (*decompress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req);
	int (*create_ctx)(struct zcomp_params *params, struct zcomp_ctx *ctx);
	void (*destroy_ctx)(struct zcomp_ctx *ctx);
	int (*setup_params)(struct zcomp_params *params);
	void (*release_params)(struct zcomp_params *params);

	const char *name;
};

extern const struct zcomp_ops backend_lz4kds;
#endif /* __KERNEL__ */

#endif /* _LZ4KDS_ZCOMP_H */
