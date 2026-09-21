/** \file
 *
 *  \brief Support for various binary representations.
 *
 *  \copyright Copyright 2003-2026 Ciaran Anscomb
 *
 *  \licenseblock This file is part of XRoar, a Dragon/Tandy CoCo emulator.
 *
 *  XRoar is free software; you can redistribute it and/or modify it under the
 *  terms of the GNU General Public License as published by the Free Software
 *  Foundation, either version 3 of the License, or (at your option) any later
 *  version.
 *
 *  See COPYING.GPL for redistribution conditions.
 *
 *  \endlicenseblock
 *
 *  Supports:
 *
 *  - Intel HEX
 *
 *  - DragonDOS binary
 *
 *  - CoCo RS-DOS ("DECB") binary
 */

#include "top-config.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "delegate.h"

#include "debug.h"
#include "decb_bin.h"
#include "fs.h"
#include "hexs19.h"
#include "logging.h"
#include "machine.h"
#include "xroar.h"

static int dragon_bin_load(const char *filename, int autorun);
static int coco_bin_load(const char *filename, int autorun);

static uint8_t read_nibble(FILE *fd) {
	int in;
	in = fs_read_uint8(fd);
	if (in >= '0' && in <= '9')
		return (in-'0');
	in |= 0x20;
	if (in >= 'a' && in <= 'f')
		return (in-'a')+10;
	return 0xff;
}

static uint8_t read_byte(FILE *fd) {
	return read_nibble(fd) << 4 | read_nibble(fd);
}

static uint16_t read_word(FILE *fd) {
	return read_byte(fd) << 8 | read_byte(fd);
}

static int skip_eol(FILE *fd) {
	int d;
	do {
		d = fs_read_uint8(fd);
	} while (d >= 0 && d != 10);
	if (d >= 0)
		return 1;
	return 0;
}

int intel_hex_read(const char *filename, int autorun) {
	FILE *fd;
	int data;
	uint16_t exec = 0;
	struct log_handle *log_hex = NULL;
	if (filename == NULL) {
		return -1;
	}
	if (!(fd = fopen(filename, "rb"))) {
		LOG_MOD_SUB_WARN("binary", "hex", "%s: %s\n", filename, strerror(errno));
		return -1;
	}
	LOG_MOD_SUB_DEBUG(1, "binary", "hex", "%s: reading Intel HEX record\n", filename);
	if (logging.debug_file & LOG_FILE_BIN_DATA)
		log_open_hexdump(&log_hex, "[binary/hex]");
	while ((data = fs_read_uint8(fd)) >= 0) {
		if (data != ':') {
			fclose(fd);
			if (logging.debug_file & LOG_FILE_BIN_DATA) {
				log_hexdump_flag(log_hex);
				log_close(&log_hex);
			}
			return -1;
		}
		int length = read_byte(fd);
		int addr = read_word(fd);
		int type = read_byte(fd);
		if (type == 0 && (logging.debug_file & LOG_FILE_BIN_DATA))
			log_hexdump_set_addr(log_hex, addr);
		uint8_t rsum = length + (length >> 8) + addr + (addr >> 8) + type;
		for (int i = 0; i < length; i++) {
			data = read_byte(fd);
			rsum += data;
			if (type == 0) {
				if (logging.debug_file & LOG_FILE_BIN_DATA)
					log_hexdump_byte(log_hex, data);
				xroar.machine->write_byte(xroar.machine, addr & 0xffff, data);
				addr++;
			}
		}
		int sum = read_byte(fd);
		rsum = ~rsum + 1;
		if (sum != rsum) {
			if (logging.debug_file & LOG_FILE_BIN_DATA)
				log_hexdump_flag(log_hex);
		}
		if (skip_eol(fd) == 0)
			break;
		if (type == 1) {
			exec = addr;
			break;
		}
	}

	if (logging.debug_file & LOG_FILE_BIN_DATA)
		log_close(&log_hex);
	if (exec != 0) {
		if (autorun) {
			LOG_MOD_SUB_DEBUG_FILE(LOG_FILE_BIN, "binary", "hex", "EXEC $%04x - autorunning\n", exec);
			debug_set_register(xroar.machine->debug.target, xroar.machine->debug.cpu.register_pc, exec);
		} else {
			LOG_MOD_SUB_DEBUG_FILE(LOG_FILE_BIN, "binary", "hex", "EXEC $%04x - not autorunning\n", exec);
		}
	}

	fclose(fd);
	return 0;
}

int motorola_s19_read(const char *filename, int autorun) {
	FILE *fd;
	int data;
	uint16_t exec = 0;
	struct log_handle *log_s19 = NULL;
	if (filename == NULL) {
		return -1;
	}
	if (!(fd = fopen(filename, "rb"))) {
		LOG_MOD_SUB_WARN("binary", "s19", "%s: %s\n", filename, strerror(errno));
		return -1;
	}
	LOG_MOD_SUB_DEBUG(1, "binary", "s19", "%s: reading Motorola S19 record\n", filename);
	if (logging.debug_file & LOG_FILE_BIN_DATA)
		log_open_hexdump(&log_s19, "[binary/s19]");
	while ((data = fs_read_uint8(fd)) >= 0) {
		if (data != 'S') {
			fclose(fd);
			if (logging.debug_file & LOG_FILE_BIN_DATA) {
				log_hexdump_flag(log_s19);
				log_close(&log_s19);
			}
			return -1;
		}
		int type = read_nibble(fd);
		int length = read_byte(fd);
		int addr = read_word(fd);
		if (type == 1 && (logging.debug_file & LOG_FILE_BIN_DATA))
			log_hexdump_set_addr(log_s19, addr);
		uint8_t rsum = length + addr + (addr >> 8);
		length -= 3;  // don't include address or sum in byte count
		for (int i = 0; i < length; i++) {
			data = read_byte(fd);
			rsum += data;
			if (type == 1) {
				if (logging.debug_file & LOG_FILE_BIN_DATA)
					log_hexdump_byte(log_s19, data);
				xroar.machine->write_byte(xroar.machine, addr & 0xffff, data);
				addr++;
			}
		}
		int sum = read_byte(fd);
		rsum = ~rsum;
		if (sum != rsum) {
			fprintf(stderr, "%02x != %02x\n", sum, rsum);
			if (logging.debug_file & LOG_FILE_BIN_DATA)
				log_hexdump_flag(log_s19);
		}
		if (skip_eol(fd) == 0)
			break;
		if (type == 9) {
			exec = addr;
			break;
		}
	}

	if (logging.debug_file & LOG_FILE_BIN_DATA)
		log_close(&log_s19);
	if (exec != 0) {
		if (autorun) {
			LOG_MOD_SUB_DEBUG_FILE(LOG_FILE_BIN, "binary", "s19", "EXEC $%04x - autorunning\n", exec);
			debug_set_register(xroar.machine->debug.target, xroar.machine->debug.cpu.register_pc, exec);
		} else {
			LOG_MOD_SUB_DEBUG_FILE(LOG_FILE_BIN, "binary", "s19", "EXEC $%04x - not autorunning\n", exec);
		}
	}

	fclose(fd);
	return 0;
}

int bin_load(const char *filename, int autorun) {
	if (filename == NULL) {
		return -1;
	}
	FILE *fd = fopen(filename, "rb");
	if (!fd) {
		LOG_MOD_WARN("binary", "%s: %s\n", filename, strerror(errno));
		return -1;
	}
	int type = fs_read_uint8(fd);
	fclose(fd);

	switch (type) {
	case 0x55:
		return dragon_bin_load(filename, autorun);
	case 0x00:
		return coco_bin_load(filename, autorun);
	default:
		break;
	}
	LOG_MOD_ERROR("binary", "unknown file type\n");
	return -1;
}

static void poke_machine_byte(void *ctx, uint16_t addr, uint8_t data) {
	struct machine *m = ctx;
	m->write_byte(m, addr, data);
}

int bin_apply_image(const struct decb_bin *bin, int autorun) {
	if (!bin || !xroar.machine || !xroar.machine->write_byte) {
		return -1;
	}
	const char *sub = (bin->kind == DECB_BIN_DRAGONDOS) ? "ddos" : "rsdos";
	for (size_t b = 0; b < bin->nblocks; b++) {
		const struct decb_bin_block *bl = &bin->blocks[b];
		LOG_MOD_SUB_DEBUG_FILE(LOG_FILE_BIN, "binary", sub,
		                       "LOAD $%04x bytes to $%04x\n",
		                       (unsigned)bl->length, (unsigned)bl->load);
		struct log_handle *log_bin = NULL;
		if (logging.debug_file & LOG_FILE_BIN_DATA) {
			log_open_hexdump(&log_bin, (bin->kind == DECB_BIN_DRAGONDOS)
			                           ? "[binary/ddos]" : "[binary/rsdos]");
			log_hexdump_set_addr(log_bin, bl->load);
		}
		for (unsigned i = 0; i < bl->length; i++) {
			log_hexdump_byte(log_bin, bl->data[i]);
		}
		log_close(&log_bin);
	}
	decb_bin_poke(bin, poke_machine_byte, xroar.machine);
	if (bin->has_exec) {
		if (autorun && bin->exec != 0) {
			LOG_MOD_SUB_DEBUG_FILE(LOG_FILE_BIN, "binary", sub,
			                       "EXEC $%04x - autorunning\n", (unsigned)bin->exec);
			debug_set_register(xroar.machine->debug.target,
			                   xroar.machine->debug.cpu.register_pc, bin->exec);
		} else {
			LOG_MOD_SUB_DEBUG_FILE(LOG_FILE_BIN, "binary", sub,
			                       "EXEC $%04x - not autorunning\n", (unsigned)bin->exec);
		}
	}
	return 0;
}

static int load_parsed_bin(const char *filename, int autorun, const char *sub) {
	struct decb_bin bin;
	char err[256];
	if (decb_bin_load_file(filename, &bin, err, sizeof err) != 0) {
		LOG_MOD_SUB_ERROR("binary", sub, "%s: %s\n", filename, err);
		return -1;
	}
	LOG_MOD_SUB_DEBUG(1, "binary", sub, "%s: reading %s BIN\n", filename,
	                  (bin.kind == DECB_BIN_DRAGONDOS) ? "DragonDOS" : "RS-DOS");
	int rc = bin_apply_image(&bin, autorun);
	decb_bin_free(&bin);
	return rc;
}

static int dragon_bin_load(const char *filename, int autorun) {
	return load_parsed_bin(filename, autorun, "ddos");
}

static int coco_bin_load(const char *filename, int autorun) {
	return load_parsed_bin(filename, autorun, "rsdos");
}
