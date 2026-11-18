// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LZ4KDS - LZ4KD plus SDDC (similarity/delta) compression for zram.
 *
 * Payload layout (stored in zsmalloc, first byte is the marker):
 *
 *   0x00  LZ4KD single-page stream (ordinary compression)
 *   0x41  u32 reference id; page is byte identical to the reference
 *   0x44  u32 reference id + LZ4KD delta stream against that reference
 *
 * A page that does not compress below one page is reported as PAGE_SIZE and
 * stored verbatim by zram; it carries no marker and never reaches decode.
 *
 * Why references are never reclaimed
 * ----------------------------------
 * An alias/delta payload may already be on swap or on the backing device, and
 * it only carries a numeric reference id.  If the reference behind that id
 * were freed, the payload would decode to garbage.  References are therefore
 * immortal for the lifetime of the device and memory is bounded by
 * LZ4KDS_REF_BUDGET instead: once the budget is exhausted, new pages stop
 * being promoted (they still compress normally), but every existing id stays
 * valid.  This trades a fixed memory ceiling for provable correctness.
 *
 * Why this is fast
 * ----------------
 * The reference array is indexed directly by id, so decompression -- the hot
 * path -- resolves a reference in O(1) with one lock per stripe.  Page
 * lookup uses a hash index so compression is O(1) on average as well.  The
 * encoder never runs while a lock is held.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/err.h>
#include <linux/jhash.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

#include "lz4kds_sddc.h"
#include <linux/lz4kd.h>

#define LZ4KDS_BLOCK_SIZE	SZ_4K
#define LZ4KDS_WINDOW_SIZE	(2 * LZ4KDS_BLOCK_SIZE)

#define LZ4KDS_HDR_ORDINARY	1
#define LZ4KDS_HDR_REFERENCED	(1 + sizeof(u32))

#define LZ4KDS_KIND_ORDINARY	0x00
#define LZ4KDS_KIND_ALIAS	0x41
#define LZ4KDS_KIND_DELTA	0x44

/*
 * lz4kd_encode_state_bytes_min() reports the single-page encoder's need
 * (8 KiB), but the delta encoder indexes a 1<<13 hash table plus a 1<<5
 * offset chain as uint16_t: (8192 + 32) * 2 = 16448 bytes, writing past the
 * reported minimum.  Size the delta-capable state to 32 KiB.
 */
#define LZ4KDS_ENCODE_STATE_BYTES	(1u << 15)

#define LZ4KDS_REF_MAX		4096
#define LZ4KDS_REF_BUDGET	(16u * 1024 * 1024)
#define LZ4KDS_LOCK_STRIPES	64
#define LZ4KDS_HASH_BITS	12
#define LZ4KDS_HASH_SIZE	(1u << LZ4KDS_HASH_BITS)

/* Delta candidates tried per write; the encoder runs without locks. */
#define LZ4KDS_DELTA_TRIES	4

struct lz4kds_ref {
	u32 id;			/* 0 = slot free */
	u32 hash;
	s32 hnext;		/* next id in the same hash bucket, 0 = end */
	atomic_t users;		/* accounting only; never gates freeing */
	u8 *page;
};

struct lz4kds_sddc {
	spinlock_t *locks;			/* LZ4KDS_LOCK_STRIPES */
	struct lz4kds_ref *refs;		/* LZ4KDS_REF_MAX */
	u32 *buckets;				/* LZ4KDS_HASH_SIZE -> id */
	u32 next_id;
	atomic_t nr_live;
	atomic64_t ref_bytes;
	struct lz4kds_stats stats;
};

static inline spinlock_t *lz4kds_lock(struct lz4kds_sddc *sddc, u32 v)
{
	return &sddc->locks[v & (LZ4KDS_LOCK_STRIPES - 1)];
}

static inline u32 lz4kds_hash_page(const void *p)
{
	return jhash(p, LZ4KDS_BLOCK_SIZE, 0x9e3779b9) & (LZ4KDS_HASH_SIZE - 1);
}

struct lz4kds_codec *lz4kds_codec_create(void)
{
	struct lz4kds_codec *codec;

	codec = kzalloc(sizeof(*codec), GFP_KERNEL);
	if (!codec)
		return NULL;

	codec->encode_state = kvzalloc(LZ4KDS_ENCODE_STATE_BYTES, GFP_KERNEL);
	codec->window = kvzalloc(LZ4KDS_WINDOW_SIZE, GFP_KERNEL);
	codec->scratch = kvzalloc(2 * LZ4KDS_BLOCK_SIZE, GFP_KERNEL);
	codec->trial = kvzalloc(2 * LZ4KDS_BLOCK_SIZE, GFP_KERNEL);
	codec->best = kvzalloc(2 * LZ4KDS_BLOCK_SIZE, GFP_KERNEL);
	if (!codec->encode_state || !codec->window || !codec->scratch ||
	    !codec->trial || !codec->best) {
		lz4kds_codec_destroy(codec);
		return NULL;
	}

	return codec;
}

void lz4kds_codec_destroy(struct lz4kds_codec *codec)
{
	if (!codec)
		return;

	kvfree(codec->best);
	kvfree(codec->trial);
	kvfree(codec->scratch);
	kvfree(codec->window);
	kvfree(codec->encode_state);
	kfree(codec);
}

struct lz4kds_sddc *lz4kds_sddc_create(void)
{
	struct lz4kds_sddc *sddc;
	int i;

	/* The delta codec requires 4 KiB pages. */
	if (PAGE_SIZE != LZ4KDS_BLOCK_SIZE)
		return ERR_PTR(-EOPNOTSUPP);

	sddc = kzalloc(sizeof(*sddc), GFP_KERNEL);
	if (!sddc)
		return ERR_PTR(-ENOMEM);

	sddc->locks = kcalloc(LZ4KDS_LOCK_STRIPES, sizeof(*sddc->locks),
			      GFP_KERNEL);
	sddc->refs = kvcalloc(LZ4KDS_REF_MAX, sizeof(*sddc->refs), GFP_KERNEL);
	sddc->buckets = kvcalloc(LZ4KDS_HASH_SIZE, sizeof(*sddc->buckets),
				 GFP_KERNEL);
	if (!sddc->locks || !sddc->refs || !sddc->buckets) {
		lz4kds_sddc_destroy(sddc);
		return ERR_PTR(-ENOMEM);
	}

	for (i = 0; i < LZ4KDS_LOCK_STRIPES; i++)
		spin_lock_init(&sddc->locks[i]);

	atomic_set(&sddc->nr_live, 0);
	atomic64_set(&sddc->ref_bytes, 0);
	sddc->next_id = 1;

	return sddc;
}

void lz4kds_sddc_destroy(struct lz4kds_sddc *sddc)
{
	u32 i;

	if (!sddc || IS_ERR(sddc))
		return;

	if (sddc->refs) {
		for (i = 0; i < LZ4KDS_REF_MAX; i++)
			kvfree(sddc->refs[i].page);
		kvfree(sddc->refs);
	}
	kvfree(sddc->buckets);
	kfree(sddc->locks);
	kfree(sddc);
}

/* O(1) by id.  Never fails for a live id because refs are immortal. */
static struct lz4kds_ref *lz4kds_ref_by_id(struct lz4kds_sddc *sddc, u32 id)
{
	struct lz4kds_ref *ref;

	if (!id || id >= LZ4KDS_REF_MAX)
		return NULL;

	ref = &sddc->refs[id];
	return ref->id == id ? ref : NULL;
}

/* O(1) average: walk only the bucket for this page's hash. */
static struct lz4kds_ref *lz4kds_ref_find_page(struct lz4kds_sddc *sddc,
					      const void *src, u32 hash)
{
	s32 id;

	for (id = sddc->buckets[hash]; id > 0; id = sddc->refs[id].hnext) {
		struct lz4kds_ref *ref = &sddc->refs[id];

		if (ref->hash == hash &&
		    !memcmp(ref->page, src, LZ4KDS_BLOCK_SIZE))
			return ref;
	}

	return NULL;
}

/* Publish @page as a new reference.  Returns NULL when the budget is full. */
static struct lz4kds_ref *lz4kds_ref_publish(struct lz4kds_sddc *sddc,
					    const void *page, u32 hash)
{
	struct lz4kds_ref *ref;
	spinlock_t *lock;
	u32 id;

	if (atomic_read(&sddc->nr_live) >= LZ4KDS_REF_MAX)
		return NULL;
	if (atomic64_read(&sddc->ref_bytes) + LZ4KDS_BLOCK_SIZE >
	    LZ4KDS_REF_BUDGET)
		return NULL;

	for (id = sddc->next_id; id < LZ4KDS_REF_MAX; id++)
		if (!sddc->refs[id].id)
			break;
	if (id >= LZ4KDS_REF_MAX)
		return NULL;

	ref = &sddc->refs[id];
	ref->page = kvmalloc(LZ4KDS_BLOCK_SIZE, GFP_ATOMIC);
	if (!ref->page)
		return NULL;
	memcpy(ref->page, page, LZ4KDS_BLOCK_SIZE);

	lock = lz4kds_lock(sddc, hash);
	spin_lock(lock);
	ref->hash = hash;
	ref->hnext = sddc->buckets[hash];
	ref->id = id;
	atomic_set(&ref->users, 1);
	sddc->buckets[hash] = id;
	spin_unlock(lock);

	sddc->next_id = id + 1;
	atomic_inc(&sddc->nr_live);
	atomic64_add(LZ4KDS_BLOCK_SIZE, &sddc->ref_bytes);
	sddc->stats.ref_allocs++;

	return ref;
}

int lz4kds_sddc_compress(struct lz4kds_sddc *sddc, struct lz4kds_codec *codec,
			 const void *src, void *dst, unsigned int *dst_len)
{
	unsigned int out_max = *dst_len;
	struct lz4kds_ref *ref;
	unsigned int plain_len;
	u8 *out = dst;
	u32 hash;
	int ret;

	if (!sddc || IS_ERR(sddc) || !codec || !src || !dst || !dst_len)
		return -EINVAL;
	if (out_max < 2 * LZ4KDS_BLOCK_SIZE)
		return -EINVAL;

	/* 1. plain LZ4KD compression (no lock held) */
	ret = lz4kd_encode(codec->encode_state, src, codec->scratch,
			   LZ4KDS_BLOCK_SIZE, out_max - LZ4KDS_HDR_ORDINARY, 0);
	if (ret <= 0) {
		*dst_len = LZ4KDS_BLOCK_SIZE;
		sddc->stats.raw++;
		return 0;
	}
	plain_len = ret;

	hash = lz4kds_hash_page(src);

	/* 2. identical page already resident? -> alias */
	ref = lz4kds_ref_find_page(sddc, src, hash);
	if (ref) {
		u32 id = ref->id;

		atomic_inc(&ref->users);
		out[0] = LZ4KDS_KIND_ALIAS;
		memcpy(out + 1, &id, sizeof(id));
		*dst_len = LZ4KDS_HDR_REFERENCED;
		sddc->stats.aliases++;
		sddc->stats.saved_bytes += plain_len - LZ4KDS_HDR_REFERENCED;
		return 0;
	}

	/*
	 * 3. similar page? -> delta against the best resident reference.
	 * Candidates come from the same hash bucket, are snapshotted into a
	 * private buffer, and the encoder runs without any lock.
	 */
	if (atomic_read(&sddc->nr_live)) {
		unsigned int best = 0;
		unsigned int tries = 0;
		u32 best_id = 0;
		s32 id;

		for (id = sddc->buckets[hash];
		     id > 0 && tries < LZ4KDS_DELTA_TRIES;
		     id = sddc->refs[id].hnext) {
			struct lz4kds_ref *cand = &sddc->refs[id];
			unsigned int dret;

			memcpy(codec->window, cand->page, LZ4KDS_BLOCK_SIZE);
			memcpy(codec->window + LZ4KDS_BLOCK_SIZE, src,
			       LZ4KDS_BLOCK_SIZE);

			tries++;
			sddc->stats.delta_attempts++;
			dret = lz4kd_encode_delta(codec->encode_state,
						  codec->window,
						  codec->window + LZ4KDS_BLOCK_SIZE,
						  codec->trial,
						  LZ4KDS_BLOCK_SIZE,
						  LZ4KDS_BLOCK_SIZE, 0);
			if ((int)dret <= 0)
				continue;
			if (dret + LZ4KDS_HDR_REFERENCED >= plain_len)
				continue;
			if (!best || dret < best) {
				memcpy(codec->best, codec->trial, dret);
				best = dret;
				best_id = cand->id;
			}
		}

		if (best) {
			out[0] = LZ4KDS_KIND_DELTA;
			memcpy(out + 1, &best_id, sizeof(best_id));
			memcpy(out + LZ4KDS_HDR_REFERENCED, codec->best, best);
			*dst_len = LZ4KDS_HDR_REFERENCED + best;
			sddc->stats.deltas++;
			sddc->stats.saved_bytes +=
				plain_len - LZ4KDS_HDR_REFERENCED - best;
			return 0;
		}
		sddc->stats.delta_no_gain++;
	}

	/* 4. plain path; promote this page so later pages can alias/delta */
	memcpy(out + LZ4KDS_HDR_ORDINARY, codec->scratch, plain_len);
	out[0] = LZ4KDS_KIND_ORDINARY;
	*dst_len = LZ4KDS_HDR_ORDINARY + plain_len;
	lz4kds_ref_publish(sddc, src, hash);
	sddc->stats.ordinary++;

	return 0;
}

int lz4kds_sddc_decompress(struct lz4kds_sddc *sddc, struct lz4kds_codec *codec,
			   const void *src, unsigned int src_len, void *dst)
{
	const u8 *in = src;
	struct lz4kds_ref *ref;
	u32 id;
	int ret;

	if (!sddc || IS_ERR(sddc) || !codec || !src || !src_len || !dst)
		return -EINVAL;

	if (in[0] == LZ4KDS_KIND_ORDINARY) {
		ret = lz4kd_decode(in + LZ4KDS_HDR_ORDINARY, dst,
				   src_len - LZ4KDS_HDR_ORDINARY,
				   LZ4KDS_BLOCK_SIZE);
		if (ret != LZ4KDS_BLOCK_SIZE) {
			sddc->stats.decode_failures++;
			return -EINVAL;
		}
		return 0;
	}

	if (in[0] != LZ4KDS_KIND_ALIAS && in[0] != LZ4KDS_KIND_DELTA) {
		sddc->stats.decode_failures++;
		return -EINVAL;
	}
	if (src_len < LZ4KDS_HDR_REFERENCED)
		return -EINVAL;

	memcpy(&id, in + 1, sizeof(id));
	ref = lz4kds_ref_by_id(sddc, id);	/* O(1) */
	if (!ref) {
		sddc->stats.decode_failures++;
		return -EINVAL;
	}

	if (in[0] == LZ4KDS_KIND_ALIAS) {
		memcpy(dst, ref->page, LZ4KDS_BLOCK_SIZE);
		return 0;
	}

	{
		u8 *cur;

		memcpy(codec->window, ref->page, LZ4KDS_BLOCK_SIZE);
		cur = codec->window + LZ4KDS_BLOCK_SIZE;
		memset(cur, 0, LZ4KDS_BLOCK_SIZE);
		ret = lz4kd_decode_delta(in + LZ4KDS_HDR_REFERENCED,
					 codec->window, cur,
					 src_len - LZ4KDS_HDR_REFERENCED,
					 LZ4KDS_BLOCK_SIZE);
		if (ret != LZ4KDS_BLOCK_SIZE) {
			sddc->stats.decode_failures++;
			return -EINVAL;
		}
		memcpy(dst, cur, LZ4KDS_BLOCK_SIZE);
	}

	return 0;
}

void lz4kds_sddc_put_payload(struct lz4kds_sddc *sddc, const void *src,
			     unsigned int src_len)
{
	const u8 *in = src;
	struct lz4kds_ref *ref;
	u32 id;

	if (!sddc || IS_ERR(sddc) || !src || src_len < LZ4KDS_HDR_REFERENCED)
		return;
	if (in[0] != LZ4KDS_KIND_ALIAS && in[0] != LZ4KDS_KIND_DELTA)
		return;

	/* References are immortal; only the usage counter changes. */
	memcpy(&id, in + 1, sizeof(id));
	ref = lz4kds_ref_by_id(sddc, id);
	if (ref)
		atomic_dec_if_positive(&ref->users);
}

void lz4kds_sddc_get_stats(struct lz4kds_sddc *sddc, struct lz4kds_stats *out)
{
	if (!sddc || IS_ERR(sddc) || !out)
		return;

	memcpy(out, &sddc->stats, sizeof(*out));
	out->ref_live = atomic_read(&sddc->nr_live);
	out->ref_bytes = atomic64_read(&sddc->ref_bytes);
}
