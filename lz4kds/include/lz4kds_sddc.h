/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LZ4KDS - LZ4KD plus SDDC (similarity/delta) compression for zram.
 *
 * This header is self-contained: it declares everything the implementation
 * needs without pulling in zram internals, so the whole feature lives in
 * brand-new files and upstream sources stay untouched.
 *
 * Design notes (performance)
 * --------------------------
 * The first implementation scanned the whole reference table under a single
 * spinlock for every decompression, which made zram reads pathologically
 * slow.  This version instead:
 *
 *   - keeps references in a fixed-size array addressed directly by id, so a
 *     reference lookup is O(1) instead of O(nr_refs);
 *   - splits the lock into one spinlock per lock stripe, so unrelated
 *     references do not serialise;
 *   - never runs the encoder while holding a lock: delta candidates are
 *     snapshotted first, encoding happens unlocked, then the result is
 *     published only if the candidate is still alive;
 *   - caches the "no references yet" state so the common early-boot path
 *     costs a single atomic read.
 *
 * The representation is always opportunistic: any failure falls back to
 * plain single-page LZ4KD, so the data path stays correct under memory
 * pressure.
 */
#ifndef _LZ4KDS_SDDC_H
#define _LZ4KDS_SDDC_H

#include <linux/types.h>

struct lz4kds_sddc;

/* Per-CPU scratch state.  zram's write path runs with preemption disabled. */
struct lz4kds_codec {
	void *encode_state;	/* LZ4KD encoder state (delta capable) */
	u8 *window;		/* 2 pages: reference block + current block */
	u8 *scratch;		/* 2 pages: plain compression output */
	u8 *trial;		/* 2 pages: delta candidate output */
	u8 *best;		/* 2 pages: winning delta payload */
};

struct lz4kds_stats {
	u64 aliases;
	u64 deltas;
	u64 ordinary;
	u64 raw;
	u64 delta_attempts;
	u64 delta_no_gain;
	u64 saved_bytes;
	u64 decode_failures;
	u64 ref_allocs;
	u64 ref_frees;
	u64 ref_live;
	u64 ref_bytes;
};

struct lz4kds_codec *lz4kds_codec_create(void);
void lz4kds_codec_destroy(struct lz4kds_codec *codec);

struct lz4kds_sddc *lz4kds_sddc_create(void);
void lz4kds_sddc_destroy(struct lz4kds_sddc *sddc);

/*
 * Compress one page.  On success @dst_len holds the payload size, which is
 * PAGE_SIZE when the page must be stored verbatim (no marker, no reference).
 */
int lz4kds_sddc_compress(struct lz4kds_sddc *sddc, struct lz4kds_codec *codec,
			 const void *src, void *dst, unsigned int *dst_len);

/* Decompress a payload produced by lz4kds_sddc_compress() into one page. */
int lz4kds_sddc_decompress(struct lz4kds_sddc *sddc, struct lz4kds_codec *codec,
			   const void *src, unsigned int src_len, void *dst);

/* Drop the reference owned by a payload that is about to be freed. */
void lz4kds_sddc_put_payload(struct lz4kds_sddc *sddc, const void *src,
			     unsigned int src_len);

void lz4kds_sddc_get_stats(struct lz4kds_sddc *sddc, struct lz4kds_stats *out);

#endif /* _LZ4KDS_SDDC_H */
