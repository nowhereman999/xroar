/** \file
 *
 *  \brief CoCoSDC WD1773-style floppy-emulation registers.
 *
 *  When $FF40 is not $43, CoCoSDC looks like a Tandy FDC so SDC-DOS Disk
 *  BASIC DSKCON (LOAD/RUN/LOADM/DIR) can read a mounted DSK image:
 *
 *  - $FF40 drive control (drive 0/1, side, halt enable)
 *  - $FF48 command / status
 *  - $FF49 track, $FF4A sector, $FF4B data
 *
 *  Status bits match a WD1773 type II/III read: BUSY, DRQ, RNF, WP,
 *  NOTREADY.  Type I (restore/seek) completes immediately and raises INTRQ.
 *  This is not a cycle-accurate WD chip — sectors are supplied from the
 *  host file as soon as the command is written (VCC sdc.dll approach).
 */

#ifndef XROAR_COCOSDC_FDC_H_
#define XROAR_COCOSDC_FDC_H_

#include <stdint.h>
#include <string.h>

#include "cocosdc_fs.h"

#define SDC_FLP_BUSY     (0x01)
#define SDC_FLP_DRQ      (0x02)
#define SDC_FLP_LOST     (0x04)
#define SDC_FLP_CRC      (0x08)
#define SDC_FLP_RNF      (0x10)
#define SDC_FLP_SEEKERR  (0x10)
#define SDC_FLP_FAULT    (0x20)
#define SDC_FLP_TRACK0   (0x04)
#define SDC_FLP_WP       (0x40)
#define SDC_FLP_NOTREADY (0x80)

struct sdc_fdc {
	uint8_t track;
	uint8_t sector;
	uint8_t data;
	uint8_t status;
	uint8_t cmd;
	unsigned drive;
	unsigned side;
	int halt_enable;
	int drq;
	int intrq;
	int writing;
	unsigned buf_i;
	unsigned buf_n;
	uint8_t buf[SDC_BLOCK_SIZE];
};

static inline void sdc_fdc_reset(struct sdc_fdc *f) {
	memset(f, 0, sizeof(*f));
	f->sector = 1;
}

static inline int sdc_fdc_want_halt(const struct sdc_fdc *f) {
	return f->halt_enable && !f->intrq && !f->drq;
}

static inline int sdc_fdc_want_nmi(const struct sdc_fdc *f) {
	return f->intrq;
}

static inline void sdc_fdc_set_intrq(struct sdc_fdc *f, int on) {
	f->intrq = on ? 1 : 0;
	if (on) {
		f->halt_enable = 0;
		f->drq = 0;
	}
}

static inline uint8_t sdc_fdc_idle_status(struct sdc_fs *fs, struct sdc_fdc *f) {
	uint8_t st = 0;
	if (f->track == 0) {
		st |= SDC_FLP_TRACK0;
	}
	if (!sdc_fs_fdc_ready(fs, f->drive)) {
		st |= SDC_FLP_NOTREADY;
	} else if (sdc_fs_fdc_wp(fs, f->drive)) {
		st |= SDC_FLP_WP;
	}
	return st;
}

static inline void sdc_fdc_finish(struct sdc_fdc *f, uint8_t status) {
	f->writing = 0;
	f->buf_i = 0;
	f->buf_n = 0;
	f->drq = 0;
	f->status = status;
	sdc_fdc_set_intrq(f, 1);
}

static inline void sdc_fdc_start_drq(struct sdc_fdc *f, int writing) {
	f->writing = writing ? 1 : 0;
	f->buf_i = 0;
	f->drq = 1;
	f->intrq = 0;
	f->status = (uint8_t)(SDC_FLP_BUSY | SDC_FLP_DRQ);
}

void sdc_fdc_command(struct sdc_fdc *f, struct sdc_fs *fs, uint8_t cmd);
uint8_t sdc_fdc_read(struct sdc_fdc *f, struct sdc_fs *fs, int reg);
void sdc_fdc_write(struct sdc_fdc *f, struct sdc_fs *fs, int reg, uint8_t D);

#endif
