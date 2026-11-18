// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LZ4KDS zram backend - registers the "lz4kds" algorithm.
 *
 * This file is part of the self-contained LZ4KDS feature: it lives in a new
 * file and is wired into the zram build by scripts/lz4kds_inject.py, so the
 * upstream zram sources are never edited in this repository.
 *
 * The backend keeps per-CPU scratch state in the zcomp context and one SDDC
 * reference table per zcomp (per device) in params->drv_data.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/err.h>

#include "lz4kds_zcomp.h"
#include "lz4kds_sddc.h"

static void lz4kds_release_params(struct zcomp_params *params)
{
	struct lz4kds_sddc *sddc = params->drv_data;

	if (!sddc)
		return;

	lz4kds_sddc_destroy(sddc);
	params->drv_data = NULL;
}

static int lz4kds_setup_params(struct zcomp_params *params)
{
	struct lz4kds_sddc *sddc;

	if (params->level == ZCOMP_PARAM_NO_LEVEL)
		params->level = 0;

	if (params->drv_data)
		return 0;

	/* setup_params() runs once per zcomp in process context. */
	sddc = lz4kds_sddc_create();
	if (IS_ERR(sddc))
		return PTR_ERR(sddc);

	params->drv_data = sddc;
	return 0;
}

static void lz4kds_destroy_ctx(struct zcomp_ctx *ctx)
{
	lz4kds_codec_destroy(ctx->context);
	ctx->context = NULL;
}

static int lz4kds_create_ctx(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct lz4kds_codec *codec;

	codec = lz4kds_codec_create();
	if (!codec)
		return -ENOMEM;

	ctx->context = codec;
	return 0;
}

static int lz4kds_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			   struct zcomp_req *req)
{
	struct lz4kds_sddc *sddc = params->drv_data;
	unsigned int out_len;
	int ret;

	if (!ctx->context || !sddc)
		return -EINVAL;

	out_len = req->dst_len;
	ret = lz4kds_sddc_compress(sddc, ctx->context, req->src, req->dst,
				   &out_len);
	if (ret)
		return ret;

	req->dst_len = out_len;
	return 0;
}

static int lz4kds_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			     struct zcomp_req *req)
{
	struct lz4kds_sddc *sddc = params->drv_data;
	int ret;

	if (!ctx->context || !sddc)
		return -EINVAL;

	ret = lz4kds_sddc_decompress(sddc, ctx->context, req->src,
				     req->src_len, req->dst);
	if (ret)
		return ret;

	return 0;
}

const struct zcomp_ops backend_lz4kds = {
	.compress	= lz4kds_compress,
	.decompress	= lz4kds_decompress,
	.create_ctx	= lz4kds_create_ctx,
	.destroy_ctx	= lz4kds_destroy_ctx,
	.setup_params	= lz4kds_setup_params,
	.release_params	= lz4kds_release_params,
	.name		= "lz4kds",
};
