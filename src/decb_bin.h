/** \file
 *
 *  \brief Parse CoCo RS-DOS (DECB) and DragonDOS binary images.
 *
 *  Independent of the emulator so host tests can check preamble/postamble
 *  handling without a machine or ROMs.  Poking into RAM and setting PC is
 *  left to the caller (hexs19 / -inject-bin).
 */

#ifndef XROAR_DECB_BIN_H_
#define XROAR_DECB_BIN_H_

#include <stddef.h>
#include <stdint.h>

enum decb_bin_kind {
	DECB_BIN_DECB = 0,      /* Color BASIC / RS-DOS: 00-preamble, FF-postamble */
	DECB_BIN_DRAGONDOS = 1, /* 0x55 header + payload */
};

struct decb_bin_block {
	uint16_t load;
	uint16_t length;
	uint8_t *data;
};

struct decb_bin {
	enum decb_bin_kind kind;
	struct decb_bin_block *blocks;
	size_t nblocks;
	int has_exec;
	uint16_t exec;
};

typedef void (*decb_bin_write_fn)(void *ctx, uint16_t addr, uint8_t data);

/* Parse filename into *out.  Returns 0 on success.  On failure returns -1,
 * writes a short reason to errbuf (if non-NULL), and leaves *out zeroed. */
int decb_bin_load_file(const char *filename, struct decb_bin *out,
                       char *errbuf, size_t errbuf_size);

void decb_bin_free(struct decb_bin *bin);

/* Poke each block through write(), wrapping 16-bit CPU addresses. */
void decb_bin_poke(const struct decb_bin *bin, decb_bin_write_fn write, void *ctx);

#endif
