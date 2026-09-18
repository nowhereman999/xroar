/** \file
 *
 *  \brief Parse CoCo RS-DOS (DECB) and DragonDOS binary images.
 */

#include "top-config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decb_bin.h"

#define DECB_BIN_MAX_BLOCKS 4096u

static void set_err(char *errbuf, size_t errbuf_size, const char *msg) {
	if (!errbuf || errbuf_size == 0) {
		return;
	}
	snprintf(errbuf, errbuf_size, "%s", msg);
}

static int read_u8(FILE *fp) {
	int c = fgetc(fp);
	return c;
}

static int read_u16be(FILE *fp) {
	int hi = fgetc(fp);
	int lo = fgetc(fp);
	if (hi < 0 || lo < 0) {
		return -1;
	}
	return (hi << 8) | lo;
}

static int add_block(struct decb_bin *out, uint16_t load, uint16_t length,
                     uint8_t *data, char *errbuf, size_t errbuf_size) {
	if (out->nblocks >= DECB_BIN_MAX_BLOCKS) {
		free(data);
		set_err(errbuf, errbuf_size, "too many preamble blocks");
		return -1;
	}
	struct decb_bin_block *nblocks = realloc(out->blocks,
	                                         (out->nblocks + 1) * sizeof(*nblocks));
	if (!nblocks) {
		free(data);
		set_err(errbuf, errbuf_size, "out of memory");
		return -1;
	}
	out->blocks = nblocks;
	out->blocks[out->nblocks].load = load;
	out->blocks[out->nblocks].length = length;
	out->blocks[out->nblocks].data = data;
	out->nblocks++;
	return 0;
}

static int read_payload(FILE *fp, uint16_t length, uint8_t **out_data,
                        char *errbuf, size_t errbuf_size) {
	if (length == 0) {
		*out_data = NULL;
		return 0;
	}
	uint8_t *data = malloc(length);
	if (!data) {
		set_err(errbuf, errbuf_size, "out of memory");
		return -1;
	}
	if (fread(data, 1, length, fp) != (size_t)length) {
		free(data);
		set_err(errbuf, errbuf_size, "short read in data chunk");
		return -1;
	}
	*out_data = data;
	return 0;
}

static int parse_decb(FILE *fp, struct decb_bin *out, char *errbuf, size_t errbuf_size) {
	out->kind = DECB_BIN_DECB;
	for (;;) {
		int chunk = read_u8(fp);
		if (chunk < 0) {
			if (out->nblocks == 0) {
				set_err(errbuf, errbuf_size, "empty or truncated file");
			} else {
				set_err(errbuf, errbuf_size, "missing postamble");
			}
			return -1;
		}
		if (chunk == 0x00) {
			int length = read_u16be(fp);
			int load = read_u16be(fp);
			if (length < 0 || load < 0) {
				set_err(errbuf, errbuf_size, "truncated preamble header");
				return -1;
			}
			uint8_t *data = NULL;
			if (read_payload(fp, (uint16_t)length, &data, errbuf, errbuf_size) != 0) {
				return -1;
			}
			if (add_block(out, (uint16_t)load, (uint16_t)length, data, errbuf, errbuf_size) != 0) {
				return -1;
			}
			continue;
		}
		if (chunk == 0xff) {
			int length = read_u16be(fp);
			int exec = read_u16be(fp);
			(void)length; /* DECB postamble length is unused (usually 0) */
			if (exec < 0) {
				set_err(errbuf, errbuf_size, "truncated postamble");
				return -1;
			}
			if (out->nblocks == 0) {
				set_err(errbuf, errbuf_size, "postamble with no preamble blocks");
				return -1;
			}
			out->has_exec = 1;
			out->exec = (uint16_t)exec;
			return 0;
		}
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "unknown chunk type 0x%02x", chunk);
			set_err(errbuf, errbuf_size, buf);
			return -1;
		}
	}
}

static int parse_dragondos(FILE *fp, struct decb_bin *out, char *errbuf, size_t errbuf_size) {
	out->kind = DECB_BIN_DRAGONDOS;
	int filetype = read_u8(fp);
	int load = read_u16be(fp);
	int length = read_u16be(fp);
	int exec = read_u16be(fp);
	int trailer = read_u8(fp);
	(void)filetype;
	(void)trailer;
	if (load < 0 || length < 0 || exec < 0) {
		set_err(errbuf, errbuf_size, "truncated DragonDOS header");
		return -1;
	}
	uint8_t *data = NULL;
	if (read_payload(fp, (uint16_t)length, &data, errbuf, errbuf_size) != 0) {
		return -1;
	}
	if (add_block(out, (uint16_t)load, (uint16_t)length, data, errbuf, errbuf_size) != 0) {
		return -1;
	}
	out->has_exec = 1;
	out->exec = (uint16_t)exec;
	return 0;
}

void decb_bin_free(struct decb_bin *bin) {
	if (!bin) {
		return;
	}
	for (size_t i = 0; i < bin->nblocks; i++) {
		free(bin->blocks[i].data);
	}
	free(bin->blocks);
	memset(bin, 0, sizeof(*bin));
}

int decb_bin_load_file(const char *filename, struct decb_bin *out,
                       char *errbuf, size_t errbuf_size) {
	if (out) {
		memset(out, 0, sizeof(*out));
	}
	if (!filename || !*filename) {
		set_err(errbuf, errbuf_size, "missing file path");
		return -1;
	}
	if (!out) {
		set_err(errbuf, errbuf_size, "internal error");
		return -1;
	}

	FILE *fp = fopen(filename, "rb");
	if (!fp) {
		set_err(errbuf, errbuf_size, strerror(errno));
		return -1;
	}

	int type = read_u8(fp);
	int rc = -1;
	if (type < 0) {
		set_err(errbuf, errbuf_size, "empty file");
	} else if (type == 0x00) {
		/* First preamble byte already consumed; parse_decb expects it.
		 * Rewind so the DECB loop sees the 0x00 chunk. */
		if (fseek(fp, 0, SEEK_SET) != 0) {
			set_err(errbuf, errbuf_size, "failed to rewind file");
		} else {
			rc = parse_decb(fp, out, errbuf, errbuf_size);
		}
	} else if (type == 0x55) {
		rc = parse_dragondos(fp, out, errbuf, errbuf_size);
	} else {
		char buf[80];
		snprintf(buf, sizeof(buf),
		         "not a DECB or DragonDOS binary (first byte 0x%02x)", type);
		set_err(errbuf, errbuf_size, buf);
	}

	fclose(fp);
	if (rc != 0) {
		decb_bin_free(out);
		return -1;
	}
	return 0;
}

void decb_bin_poke(const struct decb_bin *bin, decb_bin_write_fn write, void *ctx) {
	if (!bin || !write) {
		return;
	}
	for (size_t b = 0; b < bin->nblocks; b++) {
		const struct decb_bin_block *bl = &bin->blocks[b];
		for (unsigned i = 0; i < bl->length; i++) {
			write(ctx, (uint16_t)((bl->load + i) & 0xffff), bl->data[i]);
		}
	}
}
