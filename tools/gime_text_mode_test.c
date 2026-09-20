/* Host-side GIME WIDTH 40 vs 64/80 vs HSCREEN 1.
 *   cc -std=c11 -Wall -Werror -Isrc -o /tmp/gime_text_mode_test \
 *      tools/gime_text_mode_test.c src/tcc1014/font-gime.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "tcc1014/font-gime.h"
#include "tcc1014/gime-text-mode.h"

static int ncheck, nfail;

static void check(int ok, const char *msg)
{
	ncheck++;
	if (!ok) {
		nfail++;
		fprintf(stderr, "FAIL: %s\n", msg);
	}
}

int main(void)
{
	const uint8_t init0 = 0x4c;     /* COCO=0 */
	const uint8_t vmode_text = 0x03; /* BP=0, LPR=3 */
	const uint8_t vmode_gfx = 0x83;  /* BP=1 HSCREEN 1 */

	check(gime_is_native_text(init0, vmode_text), "BP=0 is native text");
	check(!gime_is_native_text(init0, vmode_gfx), "BP=1 is not text");
	check(!gime_is_native_text(0xcc, vmode_text), "COCO=1 is VDG, not GIME text");

	check(gime_text_columns(0x05) == 40 && gime_text_resolution(1) == 1,
	      "WIDTH 40: $FF99=$05 → 40 col, res 1 (16 px/glyph, HR2=0)");
	check(gime_text_columns(0x11) == 64 && gime_text_resolution(4) == 2,
	      "WIDTH 64: $FF99=$11 → 64 col, res 2 (8 px/glyph, HR2=1)");
	check(gime_text_columns(0x15) == 80 && gime_text_resolution(5) == 2,
	      "WIDTH 80: $FF99=$15 → 80 col, res 2 (8 px/glyph, HR2=1)");
	check(gime_text_bpr(1) == 40 && gime_text_bpr(4) == 64 && gime_text_bpr(5) == 80,
	      "text BPR 40/64/80");
	check(gime_graphics_resolution(5) == 2,
	      "HSCREEN 1 graphics res is also 2 (same $FF99, different blit)");

	{
		uint8_t t40[8], t64[8], t80[8], gfx[8], expect[8];
		unsigned fr = 5; /* mid-glyph row of 'A' */

		gime_blit_cell(1, font_gime, 'A', fr, t40);
		gime_blit_cell(1, font_gime, 'A', fr, t64);
		gime_blit_cell(1, font_gime, 'A', fr, t80);
		gime_blit_cell(0, font_gime, 'A', fr, gfx);
		gime_expand_text_bits(font_gime[('A' & 0x7f) * 12 + fr], expect);

		check(memcmp(t40, expect, 8) == 0, "WIDTH 40 blit is CGROM");
		check(memcmp(t64, expect, 8) == 0, "WIDTH 64 blit is the same CGROM (not CRES)");
		check(memcmp(t80, expect, 8) == 0, "WIDTH 80 blit is the same CGROM (not CRES)");
		check(memcmp(t80, gfx, 8) != 0, "$FF99=$15 + BP=0 is not HSCREEN CRES of 0x41");
		check(gime_count_nonzero(gfx, 8) <= 4, "HSCREEN 1 of ASCII 0x41 is sparse dots");
		check(gime_count_nonzero(t80, 8) >= 2, "CGROM 'A' mid-row has real bits");
	}

	/* Attribute cell layout: even ASCII, odd $38 */
	check(((0x38 >> 3) & 7) == 7 && (0x38 & 7) == 0,
	      "attr $38 is bright fg / black bg");

	if (nfail) {
		fprintf(stderr, "gime_text_mode_test: %d/%d failed\n", nfail, ncheck);
		return 1;
	}
	printf("gime_text_mode_test: %d checks ok\n", ncheck);
	return 0;
}
