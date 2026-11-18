/*
 * Userspace harness for the LZ4KDS algorithm logic.
 *
 * It compiles the real Huawei LZ4KD codec (the same sources the kernel patch
 * uses) and mirrors the SDDC decision logic of lz4kds_sddc.c, so the payload
 * format and round-trip behaviour can be verified without a kernel tree.
 *
 * Reference model: SDDC keeps *original pages* (4 KiB), because the LZ4KD
 * delta codec operates on raw blocks.  References are reference counted and
 * never evicted while a payload still names them.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lz4kd.h"

#define BLOCK 4096
#define WINDOW (2 * BLOCK)
#define HDR_ORD 1
#define HDR_REF (1 + sizeof(uint32_t))
#define KIND_ORD 0x00
#define KIND_ALIAS 0x41
#define KIND_DELTA 0x44
#define REF_MAX 1024
#define DELTA_TRIES 8

/* The delta encoder needs (1<<13 + 1<<5) * 2 bytes, not the 8 KiB minimum. */
#define STATE_BYTES (1u << 15)

struct ref {
	int used;
	uint32_t id;
	uint32_t hash;
	int users;
	uint8_t *page;
};

static struct ref refs[REF_MAX];
static int nr_refs;
static uint32_t next_id = 1;
static uint8_t window[WINDOW];
static uint8_t scratch[2 * BLOCK];
static uint8_t trial[2 * BLOCK];
static uint8_t bestbuf[2 * BLOCK];

static uint32_t hash32(const void *p, unsigned len)
{
	const uint8_t *b = p;
	uint32_t h = 0x9e3779b9u;
	for (unsigned i = 0; i < len; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

static struct ref *ref_lookup(const void *page, uint32_t h)
{
	for (int i = 0; i < REF_MAX; i++)
		if (refs[i].used && refs[i].hash == h &&
		    !memcmp(refs[i].page, page, BLOCK))
			return &refs[i];
	return NULL;
}

static struct ref *ref_alloc(const void *page, uint32_t h)
{
	for (int i = 0; i < REF_MAX; i++) {
		if (refs[i].used)
			continue;
		refs[i].page = malloc(BLOCK);
		if (!refs[i].page)
			return NULL;
		memcpy(refs[i].page, page, BLOCK);
		refs[i].used = 1;
		refs[i].hash = h;
		refs[i].users = 1;
		refs[i].id = next_id++;
		if (!refs[i].id)
			refs[i].id = next_id++;
		nr_refs++;
		return &refs[i];
	}
	return NULL;
}

static struct ref *ref_by_id(uint32_t id)
{
	for (int i = 0; i < REF_MAX; i++)
		if (refs[i].used && refs[i].id == id)
			return &refs[i];
	return NULL;
}

static void ref_put(struct ref *r)
{
	/* References are immortal: only usage accounting changes. */
	if (r && r->users > 0)
		r->users--;
}

/* Mirrors lz4kds_sddc_compress(). */
static int sddc_compress(void *state, const void *src, void *dst,
			 unsigned *dst_len)
{
	unsigned out_max = *dst_len, plain_len;
	uint8_t *out = dst;
	struct ref *r;
	uint32_t h;
	int ret;

	ret = lz4kd_encode(state, src, scratch, BLOCK, out_max - HDR_ORD, 0);
	if (ret <= 0) {
		*dst_len = BLOCK;      /* raw page, stored verbatim by zram */
		return 0;
	}
	plain_len = ret;
	h = hash32(src, BLOCK);

	r = ref_lookup(src, h);
	if (r) {
		uint32_t id = r->id;
		r->users++;
		out[0] = KIND_ALIAS;
		memcpy(out + 1, &id, sizeof(id));
		*dst_len = HDR_REF;
		return 0;
	}

	if (nr_refs) {
		unsigned best = 0, tries = 0;
		uint32_t id = 0;
		struct ref *pick = NULL;

		for (int i = 0; i < REF_MAX && tries < DELTA_TRIES; i++) {
			if (!refs[i].used)
				continue;
			memcpy(window, refs[i].page, BLOCK);
			memcpy(window + BLOCK, src, BLOCK);
			tries++;
			ret = lz4kd_encode_delta(state, window, window + BLOCK,
						 trial, BLOCK, BLOCK, 0);
			if (ret <= 0)
				continue;
			if ((unsigned)ret + HDR_REF >= plain_len)
				continue;
			if (!best || (unsigned)ret < best) {
				memcpy(bestbuf, trial, ret);
				best = ret;
				id = refs[i].id;
				pick = &refs[i];
			}
		}
		if (best) {
			pick->users++;
			out[0] = KIND_DELTA;
			memcpy(out + 1, &id, sizeof(id));
			memcpy(out + HDR_REF, bestbuf, best);
			*dst_len = HDR_REF + best;
			return 0;
		}
	}

	memcpy(out + HDR_ORD, scratch, plain_len);
	out[0] = KIND_ORD;
	*dst_len = HDR_ORD + plain_len;
	ref_alloc(src, h);
	return 0;
}

/* Mirrors lz4kds_sddc_decompress(). */
static int sddc_decompress(const void *src, unsigned src_len, void *dst)
{
	const uint8_t *in = src;
	struct ref *r;
	uint32_t id;
	int ret;

	if (in[0] == KIND_ORD) {
		ret = lz4kd_decode(in + HDR_ORD, dst, src_len - HDR_ORD, BLOCK);
		return ret == BLOCK ? 0 : -1;
	}
	if (in[0] != KIND_ALIAS && in[0] != KIND_DELTA)
		return -1;
	if (src_len < HDR_REF)
		return -1;

	memcpy(&id, in + 1, sizeof(id));
	r = ref_by_id(id);
	if (!r)
		return -1;

	if (in[0] == KIND_ALIAS) {
		memcpy(dst, r->page, BLOCK);
		return 0;
	}

	memcpy(window, r->page, BLOCK);
	memset(window + BLOCK, 0, BLOCK);
	ret = lz4kd_decode_delta(in + HDR_REF, window, window + BLOCK,
				 src_len - HDR_REF, BLOCK);
	if (ret != BLOCK)
		return -1;
	memcpy(dst, window + BLOCK, BLOCK);
	return 0;
}

/* Mirrors lz4kds_sddc_put_payload(). */
static void sddc_put_payload(const void *src, unsigned src_len)
{
	const uint8_t *in = src;
	uint32_t id;

	if (src_len < HDR_REF)
		return;
	if (in[0] != KIND_ALIAS && in[0] != KIND_DELTA)
		return;
	memcpy(&id, in + 1, sizeof(id));
	ref_put(ref_by_id(id));
}

static void refs_reset(void)
{
	for (int i = 0; i < REF_MAX; i++) {
		free(refs[i].page);
		refs[i].page = NULL;
		refs[i].used = 0;
	}
	nr_refs = 0;
	next_id = 1;
}

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return rng_state >> 8;
}

int main(void)
{
	void *state = calloc(1, STATE_BYTES);
	uint8_t *page = malloc(BLOCK), *page2 = malloc(BLOCK);
	uint8_t *out = malloc(2 * BLOCK), *back = malloc(BLOCK);
	int fail = 0, n_alias = 0, n_delta = 0, n_ord = 0, n_raw = 0;
	const int N = 2000;
	unsigned len;

	if (!state || !page || !page2 || !out || !back)
		return 1;

	printf("state_bytes=%u (delta needs %u)\n", STATE_BYTES,
	       (unsigned)((8192u + 32u) * 2u));

	/* Phase 1: first write of a compressible page -> ordinary. */
	refs_reset();
	for (int i = 0; i < BLOCK; i++)
		page[i] = (uint8_t)('A' + (i % 26));

	len = 2 * BLOCK;
	if (sddc_compress(state, page, out, &len) ||
	    sddc_decompress(out, len, back) || memcmp(page, back, BLOCK)) {
		printf("FAIL phase1 kind=%02x len=%u\n", out[0], len);
		return 1;
	}
	printf("phase1 kind=%02x size=%u (ok)\n", out[0], len);

	/* Phase 2: identical page -> alias. */
	len = 2 * BLOCK;
	if (sddc_compress(state, page, out, &len) ||
	    sddc_decompress(out, len, back) || memcmp(page, back, BLOCK)) {
		printf("FAIL phase2 roundtrip kind=%02x\n", out[0]);
		return 1;
	}
	if (out[0] != KIND_ALIAS) {
		printf("FAIL phase2 expected alias, got %02x\n", out[0]);
		return 1;
	}
	printf("phase2 alias ok size=%u\n", len);

	/* Phase 3: near-identical page -> delta. */
	memcpy(page2, page, BLOCK);
	for (int i = 0; i < 40; i++)
		page2[rnd() % BLOCK] ^= 0x5a;

	len = 2 * BLOCK;
	if (sddc_compress(state, page2, out, &len) ||
	    sddc_decompress(out, len, back) || memcmp(page2, back, BLOCK)) {
		printf("FAIL phase3 roundtrip kind=%02x\n", out[0]);
		return 1;
	}
	if (out[0] != KIND_DELTA) {
		printf("FAIL phase3 expected delta, got %02x\n", out[0]);
		return 1;
	}
	printf("phase3 delta ok size=%u\n", len);

	/*
	 * Phase 4: randomised round-trip stress with a payload table, so
	 * references are released exactly like zram releases slots.
	 */
	refs_reset();
	{
		struct { uint8_t buf[2 * BLOCK]; unsigned len; } *slots;
		const int NSLOT = 64;
		int s;

		slots = calloc(NSLOT, sizeof(*slots));
		if (!slots)
			return 1;

		for (int t = 0; t < N; t++) {
			int mode = t % 5;

			for (int i = 0; i < BLOCK; i++) {
				if (mode == 0)
					page[i] = (uint8_t)rnd();
				else if (mode == 1)
					page[i] = 0;
				else if (mode == 2)
					page[i] = (uint8_t)(i & 0xff);
				else if (mode == 3)
					page[i] = (uint8_t)((i * 7 + t) & 0xff);
				else
					page[i] = page2[i];
			}

			s = t % NSLOT;
			/*
			 * Overwrite: release the reference held by that slot.
			 * A raw page (len == BLOCK) is stored verbatim by zram
			 * and carries no marker, so it must never be handed to
			 * put_payload() -- its first byte is arbitrary data.
			 */
			if (slots[s].len && slots[s].len != BLOCK)
				sddc_put_payload(slots[s].buf, slots[s].len);

			len = 2 * BLOCK;
			if (sddc_compress(state, page, out, &len)) {
				printf("FAIL stress compress t=%d\n", t);
				fail++;
				break;
			}

			memcpy(slots[s].buf, out, len);
			slots[s].len = len;

			if (len == BLOCK) {
				n_raw++;
				continue;
			}
			if (sddc_decompress(out, len, back)) {
				printf("FAIL stress decompress t=%d kind=%02x len=%u\n",
				       t, out[0], len);
				fail++;
				break;
			}
			if (memcmp(page, back, BLOCK)) {
				printf("FAIL stress roundtrip t=%d kind=%02x\n",
				       t, out[0]);
				fail++;
				break;
			}
			if (out[0] == KIND_ALIAS)
				n_alias++;
			else if (out[0] == KIND_DELTA)
				n_delta++;
			else
				n_ord++;
		}

		/* Final integrity pass over every live payload. */
		if (!fail) {
			for (s = 0; s < NSLOT && !fail; s++) {
				if (!slots[s].len || slots[s].len == BLOCK)
					continue;
				if (sddc_decompress(slots[s].buf, slots[s].len,
						    back)) {
					printf("FAIL final decode slot=%d kind=%02x\n",
					       s, slots[s].buf[0]);
					fail++;
				}
			}
		}
		free(slots);
	}

	printf("stress: alias=%d delta=%d ord=%d raw=%d refs=%d\n",
	       n_alias, n_delta, n_ord, n_raw, nr_refs);
	printf("%s\n", fail ? "RESULT: FAIL" : "RESULT: PASS");

	refs_reset();
	return fail ? 1 : 0;
}
