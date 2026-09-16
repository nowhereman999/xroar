/** \file
 *
 *  \brief CoCoSDC cartridge (Phase D).
 *
 *  Maps the SCS register contract used by Studio's CommSDC client onto a
 *  host directory (-sdc-root).  Mount/eject, directory/info/CWD, and
 *  256-byte logical sector R/W follow SDC_FileAccess.asm.  Stream $90/$91
 *  (512-byte sectors) follows SDC_StreamFile_Library.asm / SDC_BigLoadm.asm
 *  / SDC_Play.asm (open/stream/abort; DAC timing is not in this layer).
 *
 *  This is not a VCC SDC.dll port.
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
 */

#include "top-config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "array.h"
#include "sds.h"
#include "xalloc.h"

#include "cart.h"
#include "cocosdc_fs.h"
#include "cocosdc_hw.h"
#include "logging.h"
#include "part.h"
#include "path.h"
#include "rombank.h"
#include "serialise.h"
#include "xconfig.h"
#include "xroar.h"

struct cocosdc {
	struct cart cart;
	struct sdc_hw hw;
	struct sdc_fs fs;
	char *root;
};

#define COCOSDC_SER_HW_BLOCK (10)

static const struct ser_struct ser_struct_cocosdc[] = {
	SER_ID_STRUCT_NEST(1, &cart_ser_struct_data),
	SER_ID_STRUCT_ELEM(2, struct cocosdc, root),
	SER_ID_STRUCT_ELEM(3, struct cocosdc, hw.cmd_mode),
	SER_ID_STRUCT_ELEM(4, struct cocosdc, hw.latch),
	SER_ID_STRUCT_ELEM(5, struct cocosdc, hw.status),
	SER_ID_STRUCT_ELEM(6, struct cocosdc, hw.cmd),
	SER_ID_STRUCT_ELEM(7, struct cocosdc, hw.preg[0]),
	SER_ID_STRUCT_ELEM(8, struct cocosdc, hw.preg[1]),
	SER_ID_STRUCT_ELEM(9, struct cocosdc, hw.preg[2]),
	SER_ID_STRUCT_UNHANDLED(COCOSDC_SER_HW_BLOCK),
	SER_ID_STRUCT_ELEM(11, struct cocosdc, hw.xfer_index),
	SER_ID_STRUCT_ELEM(12, struct cocosdc, hw.xfer),
	SER_ID_STRUCT_ELEM(13, struct cocosdc, fs.cwd_rel),
	SER_ID_STRUCT_ELEM(14, struct cocosdc, fs.slot[0].host_path),
	SER_ID_STRUCT_ELEM(15, struct cocosdc, fs.slot[1].host_path),
	SER_ID_STRUCT_ELEM(16, struct cocosdc, hw.latched_preg[0]),
	SER_ID_STRUCT_ELEM(17, struct cocosdc, hw.latched_preg[1]),
	SER_ID_STRUCT_ELEM(18, struct cocosdc, hw.latched_preg[2]),
	SER_ID_STRUCT_ELEM(19, struct cocosdc, hw.xfer_limit),
	SER_ID_STRUCT_ELEM(20, struct cocosdc, hw.streaming),
	SER_ID_STRUCT_ELEM(21, struct cocosdc, hw.stream_8bit),
	SER_ID_STRUCT_ELEM(22, struct cocosdc, fs.stream_off),
	SER_ID_STRUCT_ELEM(23, struct cocosdc, fs.stream_end),
};

static bool cocosdc_read_elem(void *sptr, struct ser_handle *sh, int tag);
static bool cocosdc_write_elem(void *sptr, struct ser_handle *sh, int tag);

static const struct ser_struct_data cocosdc_ser_struct_data = {
	.elems = ser_struct_cocosdc,
	.num_elems = ARRAY_N_ELEMENTS(ser_struct_cocosdc),
	.read_elem = cocosdc_read_elem,
	.write_elem = cocosdc_write_elem,
};

static struct xconfig_option const cocosdc_options[] = {
	{ XCO_SET_STRING_NE("sdc-root", struct cocosdc, root) },
};

static const struct xconfig_set cocosdc_option_set = {
	ARRAY_N_ELEMENTS(cocosdc_options),
	cocosdc_options
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static uint8_t cocosdc_read(struct cart *c, uint16_t A, bool P2, bool R2, uint8_t D);
static uint8_t cocosdc_write(struct cart *c, uint16_t A, bool P2, bool R2, uint8_t D);
static void cocosdc_reset(struct cart *c, bool hard);
static void cocosdc_attach(struct cart *c);
static void cocosdc_detach(struct cart *c);

static void cocosdc_apply_root(struct cocosdc *sdc);
static void cocosdc_log_completed(struct cocosdc *sdc);

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static struct part *cocosdc_allocate(void);
static void cocosdc_initialise(struct part *p, void *options);
static bool cocosdc_finish(struct part *p);
static void cocosdc_free(struct part *p);

static const struct partdb_entry_funcs cocosdc_funcs = {
	.allocate = cocosdc_allocate,
	.initialise = cocosdc_initialise,
	.finish = cocosdc_finish,
	.free = cocosdc_free,

	.ser_struct_data = &cocosdc_ser_struct_data,

	.is_a = dragon_cart_is_a,
};

const struct cart_partdb_entry cocosdc_part = {
	.partdb_entry = {
		.name = "cocosdc",
		.description = "Darren Atkinson | CoCoSDC (Phase D)",
		.funcs = &cocosdc_funcs
	}
};

static struct part *cocosdc_allocate(void) {
	struct cocosdc *sdc = part_new(sizeof(*sdc));
	struct cart *c = &sdc->cart;
	struct part *p = &c->part;

	*sdc = (struct cocosdc){0};

	cart_rom_init(c);

	c->read = cocosdc_read;
	c->write = cocosdc_write;
	c->reset = cocosdc_reset;
	c->attach = cocosdc_attach;
	c->detach = cocosdc_detach;

	sdc_hw_reset(&sdc->hw);
	sdc_fs_init(&sdc->fs);

	return p;
}

static void cocosdc_initialise(struct part *p, void *options) {
	struct cart_config *cc = options;
	assert(cc != NULL);

	struct cocosdc *sdc = (struct cocosdc *)p;

	cart_rom_initialise(p, options);

	xconfig_parse_list_struct(&cocosdc_option_set, cc->opts, sdc);
}

static bool cocosdc_finish(struct part *p) {
	struct cocosdc *sdc = (struct cocosdc *)p;

	if (!cart_rom_finish(p)) {
		return 0;
	}

	if (!sdc->root && xroar.cfg.sdc.root) {
		sdc->root = xstrdup(xroar.cfg.sdc.root);
	}
	cocosdc_apply_root(sdc);

	return 1;
}

static void cocosdc_free(struct part *p) {
	struct cocosdc *sdc = (struct cocosdc *)p;
	sdc_fs_free(&sdc->fs);
	free(sdc->root);
	sdc->root = NULL;
	cart_rom_free(p);
}

static bool cocosdc_read_elem(void *sptr, struct ser_handle *sh, int tag) {
	struct cocosdc *sdc = sptr;
	switch (tag) {
	case COCOSDC_SER_HW_BLOCK:
		ser_read(sh, sdc->hw.block, SDC_STREAM_SIZE);
		return 1;
	default:
		return 0;
	}
}

static bool cocosdc_write_elem(void *sptr, struct ser_handle *sh, int tag) {
	struct cocosdc *sdc = sptr;
	switch (tag) {
	case COCOSDC_SER_HW_BLOCK:
		ser_write(sh, tag, sdc->hw.block, SDC_STREAM_SIZE);
		return 1;
	default:
		return 0;
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void cocosdc_apply_root(struct cocosdc *sdc) {
	if (!sdc->root || !sdc->root[0]) {
		LOG_MOD_DEBUG(1, "cocosdc", "no SD card root; use -sdc-root DIR or -cart-opt sdc-root=DIR\n");
		return;
	}

	sds expanded = path_interp(sdc->root);
	if (expanded && expanded[0]) {
		free(sdc->root);
		sdc->root = xstrdup(expanded);
	}
	sdsfree(expanded);

	struct stat st;
	if (stat(sdc->root, &st) != 0) {
		LOG_MOD_WARN("cocosdc", "SD card root '%s' not found\n", sdc->root);
		(void)sdc_fs_set_root(&sdc->fs, sdc->root);
		return;
	}
	if (!S_ISDIR(st.st_mode)) {
		LOG_MOD_WARN("cocosdc", "SD card root '%s' is not a directory\n", sdc->root);
		return;
	}
	if (sdc_fs_set_root(&sdc->fs, sdc->root) != 0) {
		LOG_MOD_WARN("cocosdc", "SD card root '%s' could not be opened\n", sdc->root);
		return;
	}
	LOG_MOD_DEBUG(1, "cocosdc", "SD card root: %s\n", sdc->root);
}

static void cocosdc_log_payload(const struct cocosdc *sdc) {
	char name[SDC_BLOCK_SIZE + 1];
	unsigned n = 0;
	while (n < SDC_BLOCK_SIZE && sdc->hw.block[n] != 0) {
		char ch = (char)sdc->hw.block[n];
		if (ch < 32 || ch > 126) {
			ch = '.';
		}
		name[n++] = ch;
	}
	name[n] = 0;
	LOG_MOD_DEBUG(2, "cocosdc", "payload \"%s\" (%u bytes)\n", name, sdc->hw.xfer_index ? SDC_BLOCK_SIZE : n);
}

static void cocosdc_log_completed(struct cocosdc *sdc) {
	if (!sdc->hw.completed) {
		return;
	}
	sdc->hw.completed = false;

	unsigned family = sdc->hw.cmd & 0xfe;
	unsigned drive = sdc->hw.cmd & 0x01;

	if (logging.level < 2) {
		return;
	}

	switch (family) {
	case 0xc0:
		if (sdc->hw.preg[0] == 'V') {
			LOG_MOD_DEBUG(2, "cocosdc", "VERSION -> %02x%02x (BCD)\n",
				      sdc->hw.preg[1], sdc->hw.preg[2]);
		} else if (sdc->hw.preg[0] == 'I') {
			LOG_MOD_DEBUG(2, "cocosdc", "INFO drive %u\n", drive);
		} else if (sdc->hw.preg[0] == '>') {
			LOG_MOD_DEBUG(2, "cocosdc", "DIR page\n");
		} else if (sdc->hw.preg[0] == 'C') {
			LOG_MOD_DEBUG(2, "cocosdc", "CWD\n");
		} else {
			LOG_MOD_DEBUG(2, "cocosdc", "ext $%02X param=$%02X (drive %u)\n",
				      sdc->hw.cmd, sdc->hw.preg[0], drive);
		}
		break;
	case 0xe0:
		if (sdc->hw.status & SDC_FAILED) {
			LOG_MOD_DEBUG(2, "cocosdc", "mount/ext-data $%02X FAILED status=$%02X\n",
				      sdc->hw.cmd, sdc->hw.status);
		} else {
			LOG_MOD_DEBUG(2, "cocosdc", "mount/ext-data $%02X\n", sdc->hw.cmd);
		}
		cocosdc_log_payload(sdc);
		break;
	case 0xa0:
		LOG_MOD_DEBUG(2, "cocosdc", "write LSN $%02X%s\n", sdc->hw.cmd,
			      (sdc->hw.status & SDC_FAILED) ? " FAILED" : "");
		break;
	case 0x80:
		LOG_MOD_DEBUG(2, "cocosdc", "read LSN $%02X%s\n", sdc->hw.cmd,
			      (sdc->hw.status & SDC_FAILED) ? " FAILED" : "");
		break;
	case 0x90:
	case 0x92:
		LOG_MOD_DEBUG(2, "cocosdc", "stream $%02X%s\n", sdc->hw.cmd,
			      (sdc->hw.status & SDC_FAILED) ? " FAILED" : "");
		break;
	default:
		if (sdc->hw.cmd == 0x1c) {
			LOG_MOD_DEBUG(2, "cocosdc", "reset/program-mode handshake (PM)\n");
		} else if (sdc->hw.cmd == 0xd0) {
			LOG_MOD_DEBUG(2, "cocosdc", "abort $D0\n");
		} else {
			LOG_MOD_DEBUG(2, "cocosdc", "cmd $%02X\n", sdc->hw.cmd);
		}
		break;
	}
}

static void cocosdc_reset(struct cart *c, bool hard) {
	struct cocosdc *sdc = (struct cocosdc *)c;
	cart_rom_reset(c, hard);
	sdc_hw_reset(&sdc->hw);
	if (hard) {
		sdc_fs_reset(&sdc->fs);
	}
}

static void cocosdc_attach(struct cart *c) {
	cart_rom_attach(c);
}

static void cocosdc_detach(struct cart *c) {
	cart_rom_detach(c);
}

static uint8_t cocosdc_read(struct cart *c, uint16_t A, bool P2, bool R2, uint8_t D) {
	struct cocosdc *sdc = (struct cocosdc *)c;

	if (R2) {
		rombank_d8(c->ROM, A, &D);
		return D;
	}
	if (!P2) {
		return D;
	}

	int reg = sdc_hw_reg(A);
	if (reg < 0) {
		return D;
	}
	D = sdc_hw_read(&sdc->hw, reg);
	if (sdc->hw.cmd_ready) {
		sdc_fs_execute(&sdc->fs, &sdc->hw);
	}
	cocosdc_log_completed(sdc);
	return D;
}

static uint8_t cocosdc_write(struct cart *c, uint16_t A, bool P2, bool R2, uint8_t D) {
	struct cocosdc *sdc = (struct cocosdc *)c;

	if (R2) {
		rombank_d8(c->ROM, A, &D);
		return D;
	}
	if (!P2) {
		return D;
	}

	int reg = sdc_hw_reg(A);
	if (reg < 0) {
		return D;
	}

	sdc_hw_write(&sdc->hw, reg, D);
	if (sdc->hw.cmd_ready) {
		sdc_fs_execute(&sdc->fs, &sdc->hw);
	}
	cocosdc_log_completed(sdc);
	return D;
}
