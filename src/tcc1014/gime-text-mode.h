/** \file
 *
 *  \brief GIME native-text geometry (WIDTH 32/40/64/80 vs HSCREEN).
 *
 *  Color BASIC WIDTH 40/64/80 all use $FF98 BP=0.  Only $FF99 HRES differs:
 *  HRES 1 → 40 col, resolution 1 (16 px/glyph); HRES 4/5 → 64/80 col,
 *  resolution 2 (8 px/glyph).  $FF99=$15 with BP=1 is HSCREEN 1 graphics.
 */

#ifndef XROAR_TCC1014_GIME_TEXT_MODE_H
#define XROAR_TCC1014_GIME_TEXT_MODE_H

#include <stdint.h>

#define GIME_INIT0_COCO  (0x80)
#define GIME_VMODE_BP    (0x80)

static inline int gime_is_native_text(uint8_t init0, uint8_t vmode)
{
	return !(init0 & GIME_INIT0_COCO) && !(vmode & GIME_VMODE_BP);
}

/* Text columns: $FF99 bits 4+2 (MAME vres & 0x15). */
static inline unsigned gime_text_columns(uint8_t vres)
{
	switch (vres & 0x15) {
	case 0x04:
	case 0x05:
		return 40;
	case 0x10:
	case 0x11:
		return 64;
	case 0x14:
	case 0x15:
		return 80;
	default:
		return 32;
	}
}

/* Sock / XRoar: HRES&4 selects 8 px/glyph (64/80) vs 16 px/glyph (32/40). */
static inline unsigned gime_text_resolution(unsigned hres)
{
	return (hres & 4u) ? 2u : 1u;
}

static inline unsigned gime_text_bpr(unsigned hres)
{
	static const unsigned bpr[8] = { 32, 40, 32, 40, 64, 80, 64, 80 };
	return bpr[hres & 7u];
}

/* Graphics HRES 5 + CRES 1 is HSCREEN 1 (same $FF99=$15 as WIDTH 80). */
static inline unsigned gime_graphics_resolution(unsigned hres)
{
	return (hres & 7u) >> 1;
}

static inline void gime_expand_text_bits(uint8_t gdata, uint8_t pixels[8])
{
	unsigned i;
	for (i = 0; i < 8; i++)
		pixels[i] = (gdata & (uint8_t)(0x80u >> i)) ? 1u : 0u;
}

/* CRES=1: ASCII 0x41 → two colour-1 dots (the WIDTH 80 “graphics” fail). */
static inline void gime_expand_graphics_cres1(uint8_t gdata, uint8_t pixels[8])
{
	unsigned i;
	for (i = 0; i < 4; i++) {
		uint8_t c = (uint8_t)((gdata >> (6 - 2 * (int)i)) & 3);
		pixels[i * 2u] = c;
		pixels[i * 2u + 1u] = c;
	}
}

static inline unsigned gime_count_nonzero(const uint8_t *pixels, unsigned n)
{
	unsigned i, c = 0;
	for (i = 0; i < n; i++) {
		if (pixels[i])
			c++;
	}
	return c;
}

/*
 * One cell, one scanline.  native_text uses CGROM for any HRES including
 * 4/5 ($FF99 $11/$15).  Graphics CRES=1 blits the raw ASCII byte.
 */
static inline void gime_blit_cell(int native_text, const uint8_t *cgrom,
				  uint8_t byte0, unsigned font_row,
				  uint8_t pixels[8])
{
	if (native_text) {
		unsigned fr = font_row;
		if (fr > 11u)
			fr = 0;
		gime_expand_text_bits(cgrom[(byte0 & 0x7fu) * 12u + fr], pixels);
	} else {
		gime_expand_graphics_cres1(byte0, pixels);
	}
}

#endif
