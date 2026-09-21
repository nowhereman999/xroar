/** \file
 *
 *  \brief CoCoSDC cartridge (Phase D).
 *
 *  Maps the SCS register contract used by Studio's CommSDC client onto a
 *  host directory (-sdc-root).  Mount/eject, directory/info/CWD, and
 *  256-byte logical sector R/W follow SDC_FileAccess.asm.  Stream $90/$91
 *  (512-byte sectors) follows SDC_StreamFile_Library.asm / SDC_BigLoadm.asm
 *  / SDC_Play.asm (open/stream/abort; DAC timing is not in this layer).
 *  When $FF40 is not $43, WD1773-ish FDC registers serve SDC-DOS Disk BASIC
 *  LOAD/RUN/LOADM on an M: mounted DSK.  STARTUP.CFG auto-mounts at attach.
 *  -cart-becker (the existing cart flag) opens the Becker port on P2
 *  $FF41 (status) and $FF42 (data) so FujiNet-PC can sit beside -sdc-root.
 *  $FF42 is also the flash data helper; Becker wins while the port is
 *  open.  $FF43 flash bank and $FF40/$FF48–$FF4B SDC/FDC are unchanged.
 *  The built-in profile defaults cart-rom @sdcdos (sdcdos.rom on the ROM
 *  path) so Hardware → Cartridge → CoCoSDC and CLI -cart cocosdc both boot
 *  SDC-DOS when that image is present.  -cart-rom still overrides.
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

#include "becker.h"
#include "cart.h"
#include "cocosdc_fdc.h"
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
	struct sdc_fdc fdc;
	struct sdc_fs fs;
	char *root;
	uint8_t flash_data;
	uint8_t flash_bank;
	struct becker *becker;
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

static void cocosdc_config_complete(struct cart_config *);
static void cocosdc_apply_root(struct cocosdc *sdc);
static void cocosdc_log_startup(struct cocosdc *sdc, int err, const char *when);
static void cocosdc_log_fdc_dir(struct cocosdc *sdc);
static int cocosdc_apply_startup(struct cocosdc *sdc, const char *when);
static void cocosdc_log_completed(struct cocosdc *sdc);
static void cocosdc_update_lines(struct cocosdc *sdc);
static void cocosdc_log_rom(struct cart *c);
static int cocosdc_rom_loaded(struct cart *c);
static void strip_root_quotes(char *s);

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
		.description = "Darren Atkinson | CoCoSDC (SDC-DOS floppy)",
		.funcs = &cocosdc_funcs
	},
	.config_complete = cocosdc_config_complete
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
	sdc_fdc_reset(&sdc->fdc);
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
	struct cart *c = &sdc->cart;

	if (!cart_rom_finish(p)) {
		return 0;
	}

	cocosdc_log_rom(c);

	if (!sdc->root && xroar.cfg.sdc.root) {
		sdc->root = xstrdup(xroar.cfg.sdc.root);
	}
	if (sdc->root) {
		strip_root_quotes(sdc->root);
	}
	cocosdc_apply_root(sdc);

	/* Same flag as rsdos/mooh/idecart.  becker_open() uses -becker-ip /
	 * -becker-port (default 127.0.0.1:65504).  A failed connect leaves
	 * the pointer NULL so $FF42 stays the flash data helper. */
	if (c->config && c->config->becker_port) {
		sdc->becker = becker_open();
		if (sdc->becker) {
			LOG_MOD_DEBUG(1, "cocosdc",
				      "Becker port open ($FF41 status, $FF42 data; "
				      "flash data at $FF42 yields to Becker)\n");
		}
#ifndef WANT_BECKER
		else {
			LOG_MOD_WARN("cocosdc",
				     "-cart-becker set but this build has no Becker support\n");
		}
#endif
	}

	return 1;
}

static void cocosdc_free(struct part *p) {
	struct cocosdc *sdc = (struct cocosdc *)p;
	becker_close(sdc->becker);
	sdc->becker = NULL;
	sdc_fs_flush(&sdc->fs);
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

static void strip_root_quotes(char *s) {
	size_t n;
	if (!s || s[0] == 0) {
		return;
	}
	n = strlen(s);
	if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
		memmove(s, s + 1, n - 2);
		s[n - 2] = 0;
	}
}

static void cocosdc_config_complete(struct cart_config *cc) {
	/* Same pattern as rsdos/ide: menu Hardware → Cartridge and -cart
	 * cocosdc share this profile.  Without a default ROM, $C000 is empty
	 * and Hard Reset falls through to ECB / Super ECB. */
	if (!cc->rom_dfn && !cc->rom) {
		cc->rom = xstrdup("@sdcdos");
	}
}

static int cocosdc_rom_loaded(struct cart *c) {
	unsigned i;

	if (!c || !c->ROM) {
		return 0;
	}
	for (i = 0; i < c->ROM->nslots; i++) {
		if (c->ROM->d[i]) {
			return 1;
		}
	}
	return 0;
}

static void cocosdc_log_rom(struct cart *c) {
	const char *want;
	const char *rompath;

	want = (c && c->config && c->config->rom) ? c->config->rom : "(none)";
	rompath = xroar.cfg.file.rompath ? xroar.cfg.file.rompath : "";

	if (cocosdc_rom_loaded(c)) {
		const char *file = c->ROM->slot[0].filename;
		LOG_MOD_DEBUG(1, "cocosdc", "SDC-DOS ROM loaded: %s\n",
			      file ? file : want);
		return;
	}

	/* Default log level is 1, but CRC32 INVALID is DEBUG-only.  Menu
	 * CoCoSDC after Floppy otherwise looks like a working cart (checkmark)
	 * sitting on a green ECB OK prompt. */
	LOG_MOD_ERROR("cocosdc",
		     "SDC-DOS ROM not found (cart-rom %s, rompath %s). "
		     "Cartridge stays selected but $C000 is empty, so Hard Reset "
		     "boots ECB / Super ECB OK. Place sdcdos.rom in the ROM path "
		     "(macOS: ~/Library/XRoar/roms/) or pass -cart-rom FILE.\n",
		     want, rompath[0] ? rompath : "(default)");
}

static void cocosdc_log_startup(struct cocosdc *sdc, int err, const char *when) {
	int any = 0;
	int i;

	for (i = 0; i < SDC_SLOTS; i++) {
		if (sdc->fs.slot[i].host_path) {
			any = 1;
			LOG_MOD_DEBUG(1, "cocosdc", "STARTUP.CFG %s drive %d: %s%s\n",
				      when, i, sdc->fs.slot[i].host_path,
				      sdc->fs.slot[i].fdc_ok ? " (FDC)" : " (not FDC)");
			LOG_MOD_DEBUG_FDC(LOG_FDC_EVENTS, "cocosdc",
					  "STARTUP.CFG %s drive %d: %s fdc_ok=%d\n",
					  when, i, sdc->fs.slot[i].host_path,
					  sdc->fs.slot[i].fdc_ok);
			if (!sdc->fs.slot[i].fdc_ok) {
				LOG_MOD_WARN("cocosdc", "STARTUP.CFG %s drive %d mounted '%s' but FDC is not ready (raw m: image?)\n",
					     when, i, sdc->fs.slot[i].host_path);
			}
		}
	}
	if (!any) {
		if (err == 1) {
			LOG_MOD_DEBUG(1, "cocosdc", "STARTUP.CFG %s: no STARTUP.CFG in %s\n",
				      when, sdc->fs.root ? sdc->fs.root : "(no root)");
		} else if (err) {
			LOG_MOD_WARN("cocosdc", "STARTUP.CFG %s: 0=/1= mount failed (%s, $%02X)\n",
				     when, sdc_fs_err_name(err), err & 0xff);
		} else {
			LOG_MOD_DEBUG(1, "cocosdc", "STARTUP.CFG %s: file present but no 0=/1= disk mounted\n",
				      when);
		}
	}
	fflush(stdout);
	fflush(stderr);
}

static int fdc_printable_name(const uint8_t *b, char out[12]) {
	unsigned i;
	for (i = 0; i < 11; i++) {
		if (b[i] < 32 || b[i] > 126) {
			return 0;
		}
		out[i] = (char)b[i];
	}
	out[11] = 0;
	return 1;
}

/* DECB DIR starts at T17 S3.  A catalog parked on S2 (the FAT) looks fine to a
 * host hex dump of track 17 but DIR prints nothing. */
static void cocosdc_log_fdc_dir(struct cocosdc *sdc) {
	char name[12];
	uint8_t fat[SDC_BLOCK_SIZE];
	uint8_t first;

	if (!sdc->fdc.drq || sdc->fdc.track != 17 || sdc->fdc.sector != 3) {
		return;
	}
	if (fdc_printable_name(sdc->fdc.buf, name)) {
		LOG_MOD_DEBUG(1, "cocosdc", "FDC DECB DIR T17 S3: \"%s\"\n", name);
		return;
	}
	first = sdc->fdc.buf[0];
	if (sdc_fs_fdc_read(&sdc->fs, sdc->fdc.drive, 17, 2, 0, fat) == 0 &&
	    fdc_printable_name(fat, name)) {
		LOG_MOD_WARN("cocosdc",
			     "DECB DIR T17 S3 is empty ($%02X); T17 S2 looks like a catalog \"%s\". "
			     "Disk BASIC lists sectors 3-11; the FAT belongs on sector 2.\n",
			     first, name);
	} else {
		LOG_MOD_DEBUG(1, "cocosdc", "FDC DECB DIR T17 S3 first byte $%02X (empty catalog)\n",
			      first);
	}
}

static int cocosdc_apply_startup(struct cocosdc *sdc, const char *when) {
	int err = sdc_fs_apply_startup(&sdc->fs);
	cocosdc_log_startup(sdc, err, when);
	return err;
}

static void cocosdc_apply_root(struct cocosdc *sdc) {
	int err;

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
	strip_root_quotes(sdc->root);

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
	if (sdc->fs.slot[0].host_path || sdc->fs.slot[1].host_path) {
		err = 0;
	} else {
		/* Distinguish missing CFG from a failed 0= mount. */
		err = sdc_fs_apply_startup(&sdc->fs);
	}
	cocosdc_log_startup(sdc, err, "attach");
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
	case 0xa2:
		LOG_MOD_DEBUG(2, "cocosdc", "write LSN $%02X%s\n", sdc->hw.cmd,
			      (sdc->hw.status & SDC_FAILED) ? " FAILED" : "");
		break;
	case 0x80:
	case 0x82:
	case 0x84:
	case 0x86:
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

static void cocosdc_update_lines(struct cocosdc *sdc) {
	struct cart *c = &sdc->cart;
	if (sdc->hw.cmd_mode) {
		DELEGATE_CALL(c->signal_halt, 0);
		DELEGATE_CALL(c->signal_nmi, 0);
		return;
	}
	DELEGATE_CALL(c->signal_halt, sdc_fdc_want_halt(&sdc->fdc) ? 1 : 0);
	DELEGATE_CALL(c->signal_nmi, sdc_fdc_want_nmi(&sdc->fdc) ? 1 : 0);
}

static uint8_t cocosdc_flash_read(struct cocosdc *sdc, int reg) {
	if (reg == 0x02) {
		return sdc->flash_data;
	}
	/* SDC-DOS probes $FF43: bank in bits 0-2, last data in 3-7. */
	return (uint8_t)((sdc->flash_bank & 0x07) | (sdc->flash_data & 0xf8));
}

static void cocosdc_flash_write(struct cocosdc *sdc, int reg, uint8_t D) {
	if (reg == 0x02) {
		sdc->flash_data = D;
	} else {
		sdc->flash_bank = (uint8_t)(D & 0x07);
	}
}

static void cocosdc_reset(struct cart *c, bool hard) {
	struct cocosdc *sdc = (struct cocosdc *)c;
	cart_rom_reset(c, hard);
	sdc_hw_reset(&sdc->hw);
	sdc_fdc_reset(&sdc->fdc);
	if (hard) {
		sdc_fs_reset(&sdc->fs);
		(void)cocosdc_apply_startup(sdc, "hard reset");
	}
	if (sdc->becker) {
		becker_reset(sdc->becker);
	}
	cocosdc_update_lines(sdc);
}

static void cocosdc_attach(struct cart *c) {
	struct cocosdc *sdc = (struct cocosdc *)c;
	cart_rom_attach(c);
	/* Finish already applied the root.  Re-apply STARTUP.CFG if a hard
	 * reset later unmounted, or if attach races ahead of finish on some
	 * hosts: only mount when drive 0 is empty and a root exists. */
	if (sdc->fs.root && !sdc->fs.slot[0].host_path) {
		(void)cocosdc_apply_startup(sdc, "cart attach");
	}
}

static void cocosdc_detach(struct cart *c) {
	struct cocosdc *sdc = (struct cocosdc *)c;
	sdc_fs_flush(&sdc->fs);
	if (sdc->becker) {
		becker_reset(sdc->becker);
	}
	cart_rom_detach(c);
}

static uint8_t cocosdc_read(struct cart *c, uint16_t A, bool P2, bool R2, uint8_t D) {
	struct cocosdc *sdc = (struct cocosdc *)c;
	int cls;
	int reg;

	if (R2) {
		rombank_d8(c->ROM, A, &D);
		return D;
	}
	if (!P2) {
		return D;
	}

	/* Becker before flash so $FF42 talks to FujiNet when -cart-becker
	 * connected.  SDC registers are classified separately and fall
	 * through below. */
	cls = cocosdc_p2_class(A, sdc->becker != NULL);
	if (cls == COCOSDC_P2_BECKER_STATUS) {
		return becker_read_status(sdc->becker);
	}
	if (cls == COCOSDC_P2_BECKER_DATA) {
		return becker_read_data(sdc->becker);
	}
	if (cls == COCOSDC_P2_FLASH) {
		return cocosdc_flash_read(sdc, (int)(A & 0x0f));
	}

	reg = sdc_hw_reg(A);
	if (reg < 0) {
		return D;
	}
	if (sdc->hw.cmd_mode) {
		D = sdc_hw_read(&sdc->hw, reg);
		if (sdc->hw.cmd_ready) {
			sdc_fs_execute(&sdc->fs, &sdc->hw);
		}
		cocosdc_log_completed(sdc);
		return D;
	}
	D = sdc_fdc_read(&sdc->fdc, &sdc->fs, reg);
	cocosdc_update_lines(sdc);
	return D;
}

static uint8_t cocosdc_write(struct cart *c, uint16_t A, bool P2, bool R2, uint8_t D) {
	struct cocosdc *sdc = (struct cocosdc *)c;
	int cls;
	int reg;

	if (R2) {
		rombank_d8(c->ROM, A, &D);
		return D;
	}
	if (!P2) {
		return D;
	}

	cls = cocosdc_p2_class(A, sdc->becker != NULL);
	if (cls == COCOSDC_P2_BECKER_DATA) {
		becker_write_data(sdc->becker, D);
		return D;
	}
	if (cls == COCOSDC_P2_BECKER_STATUS) {
		/* Status is read-only, same as rsdos. */
		return D;
	}
	if (cls == COCOSDC_P2_FLASH) {
		cocosdc_flash_write(sdc, (int)(A & 0x0f), D);
		return D;
	}

	reg = sdc_hw_reg(A);
	if (reg < 0) {
		return D;
	}

	if (reg == 0x00) {
		sdc_hw_write(&sdc->hw, reg, D);
		if (D == SDC_CMDMODE) {
			sdc->fdc.halt_enable = 0;
			sdc->fdc.drq = 0;
			sdc_fdc_set_intrq(&sdc->fdc, 0);
			sdc->fdc.status = 0;
		} else {
			sdc_fdc_write(&sdc->fdc, &sdc->fs, 0x00, D);
		}
		cocosdc_update_lines(sdc);
		return D;
	}

	if (sdc->hw.cmd_mode) {
		sdc_hw_write(&sdc->hw, reg, D);
		if (sdc->hw.cmd_ready) {
			sdc_fs_execute(&sdc->fs, &sdc->hw);
		}
		cocosdc_log_completed(sdc);
		return D;
	}

	if (logging.level >= 2 && reg == 0x08) {
		LOG_MOD_DEBUG(2, "cocosdc", "FDC cmd $%02X drv=%u tr=%u se=%u side=%u nmi=%d\n",
			      D, sdc->fdc.drive, sdc->fdc.track, sdc->fdc.sector, sdc->fdc.side,
			      sdc->fdc.nmi_enable);
	}
	sdc_fdc_write(&sdc->fdc, &sdc->fs, reg, D);
	if (reg == 0x08 && (D & 0xe0) == 0x80 && sdc->fdc.drq) {
		char peek[12];
		if (fdc_printable_name(sdc->fdc.buf, peek)) {
			LOG_MOD_DEBUG(2, "cocosdc", "FDC read T%u S%u \"%s\"\n",
				      sdc->fdc.track, sdc->fdc.sector, peek);
		} else {
			LOG_MOD_DEBUG(2, "cocosdc", "FDC read T%u S%u first=$%02X\n",
				      sdc->fdc.track, sdc->fdc.sector, sdc->fdc.buf[0]);
		}
		cocosdc_log_fdc_dir(sdc);
	}
	if ((logging.level >= 2 || (logging.debug_fdc & LOG_FDC_EVENTS)) && reg == 0x08) {
		LOG_MOD_DEBUG(2, "cocosdc", "FDC status=$%02X%s%s%s ready=%d\n", sdc->fdc.status,
			      sdc->fdc.drq ? " DRQ" : "", sdc->fdc.intrq ? " INTRQ" : "",
			      sdc_fdc_want_nmi(&sdc->fdc) ? " NMI" : "",
			      sdc_fs_fdc_ready(&sdc->fs, sdc->fdc.drive));
		LOG_MOD_DEBUG_FDC(LOG_FDC_EVENTS, "cocosdc",
				  "FDC cmd $%02X status=$%02X fdc_ok=%d\n",
				  D, sdc->fdc.status,
				  sdc_fs_fdc_ready(&sdc->fs, sdc->fdc.drive));
	}
	cocosdc_update_lines(sdc);
	return D;
}
