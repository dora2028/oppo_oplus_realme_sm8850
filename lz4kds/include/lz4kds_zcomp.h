/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Minimal zcomp interface used by the self-contained LZ4KDS backend.
 *
 * The upstream drivers/block/zram/zcomp.h already declares these types; this
 * header exists only so lz4kds_backend.c can be compiled and read in
 * isolation.  When built inside the kernel tree it includes the real header
 * instead of redeclaring anything.
 */
#ifndef _LZ4KDS_ZCOMP_H
#define _LZ4KDS_ZCOMP_H

#ifdef LZ4KDS_IN_TREE
/* Building inside the kernel tree: use the real zcomp interface. */
#include "zcomp.h"
#else
/* Standalone view, for review and host-side checking. */
#include <linux/types.h>

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
	size_t src_len;
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
	void (*lz4kds_release)(struct zcomp_params *params, const void *src,
			       unsigned int src_len);
	const char *name;
};

extern const struct zcomp_ops backend_lz4kds;
#endif /* LZ4KDS_IN_TREE */

#endif /* _LZ4KDS_ZCOMP_H */
