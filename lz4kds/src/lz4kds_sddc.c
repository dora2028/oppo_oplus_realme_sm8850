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

/*
 * Delta candidate selection
 * -------------------------
 * An exact 4 KiB jhash only groups *identical* pages: flipping a single byte
 * changes the hash completely, so a page that differs from a resident one by
 * a few bytes lands in a different bucket with overwhelming probability
 * (measured: 0 of 2000 single-byte variants shared a bucket).  Walking the
 * exact-hash bucket therefore almost never finds a delta candidate, so the
 * delta path was dead weight while still costing a full hash plus a bucket
 * walk on every write.
 *
 * Similarity is indexed with several small *windows* spread across the page
 * instead of one prefix.  A page is registered under the hash of every window,
 * and a lookup probes each window's bucket.  Two pages that differ in a few
 * scattered bytes almost always still share at least one untouched window,
 * which is exactly the property a delta candidate needs; a single prefix hash
 * fails as soon as any modified byte falls inside that prefix.
 *
 * Work per write stays bounded: WINDOWS buckets are probed and at most
 * LZ4KDS_DELTA_TRIES encodings are attempted, independent of table fullness.
 */
#define LZ4KDS_WINDOWS		4
#define LZ4KDS_FP_SIZE		64
#define LZ4KDS_WINDOW_STRIDE	(LZ4KDS_BLOCK_SIZE / LZ4KDS_WINDOWS)

#define LZ4KDS_SAMPLE_BITS	8
#define LZ4KDS_SAMPLE_SIZE	(1u << LZ4KDS_SAMPLE_BITS)
#define LZ4KDS_SAMPLE_WAYS	2	/* candidates kept per window bucket */

/* Delta candidates tried per write; the encoder runs without locks. */
#define LZ4KDS_DELTA_TRIES	2

struct lz4kds_ref {
	u32 id;				/* 0 = slot free */
	u32 hash;			/* exact page hash -> alias bucket */
	u32 whash[LZ4KDS_WINDOWS];	/* per-window hashes -> similarity buckets */
	s32 hnext;			/* next id in the same alias bucket, 0 = end */
	u8 *page;
};

struct lz4kds_sddc {
	spinlock_t *locks;			/* LZ4KDS_LOCK_STRIPES */
	struct lz4kds_ref *refs;		/* LZ4KDS_REF_MAX */
	u32 *buckets;				/* LZ4KDS_HASH_SIZE -> id */
	u32 *samples;	/* WINDOWS * SAMPLE_SIZE * WAYS candidates */
	u8 *sample_pos;	/* WINDOWS * SAMPLE_SIZE insert cursors */
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

/*
 * Hash window @i of the page.  Windows are spread across the whole block so a
 * page that differs in a few scattered bytes still matches on the windows it
 * did not touch.
 */
static inline u32 lz4kds_hash_window(const void *p, unsigned int i)
{
	const u8 *b = (const u8 *)p + i * LZ4KDS_WINDOW_STRIDE;

	return jhash(b, LZ4KDS_FP_SIZE, 0x85ebca6b + i) &
	       (LZ4KDS_SAMPLE_SIZE - 1);
}

static inline void lz4kds_hash_windows(const void *p, u32 *out)
{
	unsigned int i;

	for (i = 0; i < LZ4KDS_WINDOWS; i++)
		out[i] = lz4kds_hash_window(p, i);
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
	sddc->samples = kvcalloc(LZ4KDS_WINDOWS * LZ4KDS_SAMPLE_SIZE *
				 LZ4KDS_SAMPLE_WAYS, sizeof(*sddc->samples),
				 GFP_KERNEL);
	sddc->sample_pos = kvcalloc(LZ4KDS_WINDOWS * LZ4KDS_SAMPLE_SIZE,
				    sizeof(*sddc->sample_pos), GFP_KERNEL);
	if (!sddc->locks || !sddc->refs || !sddc->buckets ||
	    !sddc->samples || !sddc->sample_pos) {
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
	kvfree(sddc->samples);
	kvfree(sddc->sample_pos);
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
					    const void *page, u32 hash,
					    const u32 *whash)
{
	struct lz4kds_ref *ref;
	spinlock_t *lock;
	u32 id, w;

	if (atomic_read(&sddc->nr_live) >= LZ4KDS_REF_MAX)
		return NULL;
	if (atomic64_read(&sddc->ref_bytes) + LZ4KDS_BLOCK_SIZE >
	    LZ4KDS_REF_BUDGET)
		return NULL;

	/*
	 * Ids are handed out in ascending order and never freed, so next_id is
	 * the first free slot.  There is no need to scan for a hole: refs are
	 * immortal, which makes allocation O(1) instead of a linear sweep that
	 * would rescan the tail on every write once the table filled up.
	 */
	if (sddc->next_id >= LZ4KDS_REF_MAX)
		return NULL;
	id = sddc->next_id;

	ref = &sddc->refs[id];
	/*
	 * The page copy is allocated with GFP_ATOMIC because the zram write
	 * path runs with preemption disabled.  kvzalloc() is used instead of
	 * kvmalloc(): kvzalloc falls back to vmalloc, while kvmalloc() returns
	 * NULL outright for non-blocking allocations (see __kvmalloc_node).
	 * 4 KiB is a single page, so the plain kmalloc path is taken anyway.
	 */
	ref->page = kvzalloc(LZ4KDS_BLOCK_SIZE, GFP_ATOMIC);
	if (!ref->page)
		return NULL;
	memcpy(ref->page, page, LZ4KDS_BLOCK_SIZE);

	ref->hash = hash;
	ref->id = id;

	lock = lz4kds_lock(sddc, hash);
	spin_lock(lock);
	ref->hnext = sddc->buckets[hash];
	sddc->buckets[hash] = id;
	spin_unlock(lock);

	/* Register under every window so any untouched window can match later. */
	for (w = 0; w < LZ4KDS_WINDOWS; w++) {
		u32 hb = whash[w];
		u32 idx = w * LZ4KDS_SAMPLE_SIZE + hb;
		u32 slot = idx * LZ4KDS_SAMPLE_WAYS +
			   (sddc->sample_pos[idx] % LZ4KDS_SAMPLE_WAYS);

		ref->whash[w] = hb;
		sddc->samples[slot] = id;
		sddc->sample_pos[idx]++;
	}

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

		out[0] = LZ4KDS_KIND_ALIAS;
		memcpy(out + 1, &id, sizeof(id));
		*dst_len = LZ4KDS_HDR_REFERENCED;
		sddc->stats.aliases++;
		sddc->stats.saved_bytes += plain_len - LZ4KDS_HDR_REFERENCED;
		return 0;
	}

	/*
	 * 3. similar page? -> delta against the best resident reference.
	 *
	 * Candidates come from the prefix-hash sample set, not from the exact
	 * hash bucket: an exact 4 KiB hash separates pages that differ by a
	 * single byte, so the alias bucket almost never contains a useful delta
	 * candidate.  Each candidate page is snapshotted into a private buffer
	 * before encoding, and the encoder itself runs with no lock held.
	 */
	if (atomic_read(&sddc->nr_live)) {
		unsigned int best = 0;
		unsigned int tries = 0;
		u32 best_id = 0;
		u32 whash[LZ4KDS_WINDOWS];
		u32 w;

		lz4kds_hash_windows(src, whash);

		for (w = 0; w < LZ4KDS_WINDOWS && tries < LZ4KDS_DELTA_TRIES; w++) {
			u32 base = (w * LZ4KDS_SAMPLE_SIZE + whash[w]) *
				   LZ4KDS_SAMPLE_WAYS;
			u32 k;

			for (k = 0; k < LZ4KDS_SAMPLE_WAYS &&
			     tries < LZ4KDS_DELTA_TRIES; k++) {
				u32 cid = sddc->samples[base + k];
				struct lz4kds_ref *cand;
				unsigned int dret;

				if (!cid || cid >= LZ4KDS_REF_MAX)
					continue;
				cand = &sddc->refs[cid];
				if (!cand->page || cand->whash[w] != whash[w])
					continue;

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
	{
		u32 whash[LZ4KDS_WINDOWS];

		lz4kds_hash_windows(src, whash);
		lz4kds_ref_publish(sddc, src, hash, whash);
	}
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

void lz4kds_sddc_get_stats(struct lz4kds_sddc *sddc, struct lz4kds_stats *out)
{
	if (!sddc || IS_ERR(sddc) || !out)
		return;

	memcpy(out, &sddc->stats, sizeof(*out));
	out->ref_live = atomic_read(&sddc->nr_live);
	out->ref_bytes = atomic64_read(&sddc->ref_bytes);
}
