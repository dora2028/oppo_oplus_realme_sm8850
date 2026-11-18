/*
 * Host-side test for the REAL SDDC implementation.
 *
 * Unlike the previous test file, which re-implemented the compress/decompress
 * logic locally and therefore only proved that the copy worked, this test
 * links against src/lz4kds_sddc.c itself.  It exercises the shipped code path,
 * including the reference table, the alias bucket and the similarity sample
 * index.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lz4kd.h"
#include "lz4kds_sddc.h"

#define BLOCK 4096u
#define STATE_BYTES (1u << 15)

static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
	rnd_state = rnd_state * 1103515245u + 12345u;
	return (rnd_state >> 8) & 0xffffff;
}

struct payload {
	uint8_t buf[2 * BLOCK];
	unsigned len;
	int live;
};

int main(void)
{
	struct lz4kds_sddc *sddc;
	struct lz4kds_codec *codec;
	uint8_t *page, *page2, *out, *back;
	int fail = 0;

	page = malloc(BLOCK);
	page2 = malloc(BLOCK);
	out = malloc(2 * BLOCK);
	back = malloc(BLOCK);
	if (!page || !page2 || !out || !back) {
		printf("FAIL alloc\n");
		return 1;
	}

	sddc = lz4kds_sddc_create();
	if (!sddc || IS_ERR(sddc)) {
		printf("FAIL sddc_create\n");
		return 1;
	}
	codec = lz4kds_codec_create();
	if (!codec) {
		printf("FAIL codec_create\n");
		return 1;
	}

	printf("encode_state=%u delta_needs=%u\n", STATE_BYTES,
	       (unsigned)((8192u + 32u) * 2u));

	/* Phase 1: first write of a compressible page -> ordinary. */
	for (unsigned i = 0; i < BLOCK; i++)
		page[i] = (uint8_t)('A' + (i % 26));

	{
		unsigned len = 2 * BLOCK;

		if (lz4kds_sddc_compress(sddc, codec, page, out, &len) ||
		    lz4kds_sddc_decompress(sddc, codec, out, len, back) ||
		    memcmp(page, back, BLOCK)) {
			printf("FAIL phase1 kind=%02x len=%u\n", out[0], len);
			return 1;
		}
		printf("phase1 kind=%02x size=%u (ok)\n", out[0], len);
	}

	/* Phase 2: identical page -> alias. */
	{
		unsigned len = 2 * BLOCK;

		if (lz4kds_sddc_compress(sddc, codec, page, out, &len) ||
		    lz4kds_sddc_decompress(sddc, codec, out, len, back) ||
		    memcmp(page, back, BLOCK)) {
			printf("FAIL phase2 roundtrip kind=%02x\n", out[0]);
			return 1;
		}
		if (out[0] != 0x41) {
			printf("FAIL phase2 expected alias, got %02x\n", out[0]);
			return 1;
		}
		printf("phase2 alias ok size=%u\n", len);
	}

	/*
	 * Phase 3: near-identical page -> delta.  This is the case the old
	 * exact-hash index could never find, so it is the real regression
	 * guard for the similarity index.
	 */
	memcpy(page2, page, BLOCK);
	for (int i = 0; i < 40; i++)
		page2[rnd() % BLOCK] ^= 0x5a;

	{
		unsigned len = 2 * BLOCK;

		if (lz4kds_sddc_compress(sddc, codec, page2, out, &len) ||
		    lz4kds_sddc_decompress(sddc, codec, out, len, back) ||
		    memcmp(page2, back, BLOCK)) {
			printf("FAIL phase3 roundtrip kind=%02x\n", out[0]);
			return 1;
		}
		if (out[0] != 0x44) {
			printf("FAIL phase3 expected delta, got %02x\n", out[0]);
			return 1;
		}
		printf("phase3 delta ok size=%u\n", len);
	}

	/* Phase 4: randomised round-trip stress with slot overwrite. */
	{
		const int NSLOT = 64;
		struct payload *slots = calloc(NSLOT, sizeof(*slots));
		int n_alias = 0, n_delta = 0, n_ord = 0, n_raw = 0;

		if (!slots)
			return 1;

		for (int t = 0; t < 2000 && !fail; t++) {
			int mode = t % 5;
			int s = t % NSLOT;
			unsigned len = 2 * BLOCK;

			for (unsigned i = 0; i < BLOCK; i++) {
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

			if (lz4kds_sddc_compress(sddc, codec, page, out, &len)) {
				printf("FAIL stress compress t=%d\n", t);
				fail++;
				break;
			}

			memcpy(slots[s].buf, out, len);
			slots[s].len = len;
			slots[s].live = 1;

			if (len == BLOCK) {
				n_raw++;
				continue;
			}

			if (lz4kds_sddc_decompress(sddc, codec, out, len, back)) {
				printf("FAIL stress decompress t=%d kind=%02x len=%u\n",
				       t, out[0], len);
				fail++;
				break;
			}
			if (memcmp(page, back, BLOCK)) {
				printf("FAIL stress mismatch t=%d kind=%02x len=%u\n",
				       t, out[0], len);
				fail++;
				break;
			}

			if (out[0] == 0x41)
				n_alias++;
			else if (out[0] == 0x44)
				n_delta++;
			else
				n_ord++;
		}

		/*
		 * Phase 5: every stored payload must still decode after the
		 * stress run, i.e. references really are immortal.
		 */
		for (int s = 0; s < NSLOT && !fail; s++) {
			if (!slots[s].live || slots[s].len == BLOCK)
				continue;
			if (lz4kds_sddc_decompress(sddc, codec, slots[s].buf,
						   slots[s].len, back)) {
				printf("FAIL replay slot %d kind=%02x\n",
				       s, slots[s].buf[0]);
				fail++;
			}
		}

		printf("stress: alias=%d delta=%d ord=%d raw=%d\n",
		       n_alias, n_delta, n_ord, n_raw);

		if (n_delta == 0) {
			printf("FAIL similarity index produced no deltas\n");
			fail++;
		}

		free(slots);
	}

	lz4kds_codec_destroy(codec);
	lz4kds_sddc_destroy(sddc);
	free(page);
	free(page2);
	free(out);
	free(back);

	printf("%s\n", fail ? "RESULT: FAIL" : "RESULT: PASS");
	return fail ? 1 : 0;
}
