/** \file
 *
 *  \brief CoCoSDC floppy-emulation commands (WD1773-ish).
 *
 *  Restore/seek complete immediately.  Read/write sector maps CHS through
 *  sdc_fs_fdc_read/write onto a mounted M: image.  m: raw mounts report
 *  NOTREADY on the FDC path (User Guide: FDC is not supported for raw
 *  blocks).
 *
 *  Type II/III NOTREADY stays BUSY without DRQ or INTRQ so DECB's DRQ poll
 *  times out to ?IO ERROR.  Instant INTRQ is taken as NMI while NMIFLG is
 *  set; DECB then ANDA #$7C, hides NOTREADY, and DIR "succeeds" with zeros.
 *
 *  While DRQ is set, $FF4A and $FF4B both supply data so SDC-DOS can use
 *  8-bit LDA $FF4B or 16-bit LDU $FF4A.
 */

#include "top-config.h"

#include <string.h>

#include "cocosdc_fdc.h"

static uint8_t fdc_type2_ok(void) {
	/* Type II bit 2 is LOST DATA, not TRACK0.  Finishing a sector on
	 * track 0 with idle_status made DECB ANDA #$7C treat START.BAS as
	 * lost-data (?IO) after DIR (track 17) had already listed it. */
	return 0;
}

static uint8_t fdc_take_rx(struct sdc_fdc *f, struct sdc_fs *fs) {
	uint8_t v;

	(void)fs;
	if (!f->drq || f->writing || f->buf_i >= f->buf_n) {
		return f->data;
	}
	v = f->buf[f->buf_i++];
	f->data = v;
	if (f->buf_i >= f->buf_n) {
		sdc_fdc_finish(f, fdc_type2_ok());
	} else {
		f->status = (uint8_t)(SDC_FLP_BUSY | SDC_FLP_DRQ);
	}
	return v;
}

static void fdc_put_tx(struct sdc_fdc *f, struct sdc_fs *fs, uint8_t D) {
	int rc;

	f->data = D;
	if (!f->drq || !f->writing || f->buf_i >= f->buf_n) {
		return;
	}
	f->buf[f->buf_i++] = D;
	if (f->buf_i >= f->buf_n) {
		rc = sdc_fs_fdc_write(fs, f->drive, f->track, f->sector,
				      f->side, f->buf);
		if (rc == SDC_FDC_NOTREADY) {
			/* Same as a Type II read: do not NMI; stay busy. */
			f->writing = 0;
			f->buf_i = 0;
			f->buf_n = 0;
			f->drq = 0;
			f->intrq = 0;
			f->status = (uint8_t)(SDC_FLP_BUSY | SDC_FLP_NOTREADY);
		} else if (rc == SDC_FDC_WP) {
			sdc_fdc_finish(f, SDC_FLP_WP);
		} else if (rc != 0) {
			sdc_fdc_finish(f, SDC_FLP_RNF);
		} else {
			sdc_fdc_finish(f, fdc_type2_ok());
		}
	} else {
		f->status = (uint8_t)(SDC_FLP_BUSY | SDC_FLP_DRQ);
	}
}

static void fdc_type2_not_ready(struct sdc_fdc *f) {
	f->writing = 0;
	f->buf_i = 0;
	f->buf_n = 0;
	f->drq = 0;
	f->intrq = 0;
	f->status = (uint8_t)(SDC_FLP_BUSY | SDC_FLP_NOTREADY);
}

void sdc_fdc_command(struct sdc_fdc *f, struct sdc_fs *fs, uint8_t cmd) {
	unsigned family = cmd >> 4;
	int rc;

	f->cmd = cmd;
	f->writing = 0;
	f->buf_i = 0;
	f->buf_n = 0;
	f->drq = 0;

	switch (family) {
	case 0x0: /* Restore */
		f->track = 0;
		sdc_fdc_finish(f, sdc_fdc_idle_status(fs, f));
		break;
	case 0x1: /* Seek: track := data register */
		f->track = f->data;
		sdc_fdc_finish(f, sdc_fdc_idle_status(fs, f));
		break;
	case 0x2: /* Step */
	case 0x3:
		sdc_fdc_finish(f, sdc_fdc_idle_status(fs, f));
		break;
	case 0x4: /* Step in */
	case 0x5:
		if (f->track < 255) {
			f->track++;
		}
		sdc_fdc_finish(f, sdc_fdc_idle_status(fs, f));
		break;
	case 0x6: /* Step out */
	case 0x7:
		if (f->track > 0) {
			f->track--;
		}
		sdc_fdc_finish(f, sdc_fdc_idle_status(fs, f));
		break;
	case 0x8: /* Read sector */
	case 0x9:
		if (!sdc_fs_fdc_ready(fs, f->drive)) {
			fdc_type2_not_ready(f);
			break;
		}
		memset(f->buf, 0, sizeof(f->buf));
		rc = sdc_fs_fdc_read(fs, f->drive, f->track, f->sector, f->side, f->buf);
		if (rc == SDC_FDC_NOTREADY) {
			fdc_type2_not_ready(f);
		} else if (rc != 0) {
			sdc_fdc_finish(f, (uint8_t)(SDC_FLP_RNF | ((rc == SDC_FDC_WP) ? SDC_FLP_WP : 0)));
		} else {
			f->buf_n = SDC_BLOCK_SIZE;
			sdc_fdc_start_drq(f, 0);
		}
		break;
	case 0xa: /* Write sector */
	case 0xb:
		if (!sdc_fs_fdc_ready(fs, f->drive)) {
			fdc_type2_not_ready(f);
		} else if (sdc_fs_fdc_wp(fs, f->drive)) {
			sdc_fdc_finish(f, SDC_FLP_WP);
		} else {
			memset(f->buf, 0, sizeof(f->buf));
			f->buf_n = SDC_BLOCK_SIZE;
			sdc_fdc_start_drq(f, 1);
		}
		break;
	case 0xc: /* Read address — not needed for DECB DSKCON */
		sdc_fdc_finish(f, SDC_FLP_RNF | sdc_fdc_idle_status(fs, f));
		break;
	case 0xd: /* Force interrupt */
		f->status = sdc_fdc_idle_status(fs, f);
		f->drq = 0;
		if (cmd & 0x08) {
			sdc_fdc_set_intrq(f, 1);
		} else {
			f->intrq = 0;
		}
		break;
	default: /* Read/write track */
		sdc_fdc_finish(f, SDC_FLP_RNF | sdc_fdc_idle_status(fs, f));
		break;
	}
}

uint8_t sdc_fdc_read(struct sdc_fdc *f, struct sdc_fs *fs, int reg) {
	switch (reg) {
	case 0x08:
		/* Reading status clears INTRQ (WD1773). */
		if (f->intrq && !f->drq) {
			f->intrq = 0;
		}
		return f->status;
	case 0x09:
		return f->track;
	case 0x0a:
		/* During a sector transfer both DATREGA ($FF4A) and DATREGB
		 * ($FF4B) present data so LDU $FF4A works.  Otherwise this is
		 * the WD1773 sector register. */
		if (f->drq) {
			return fdc_take_rx(f, fs);
		}
		return f->sector;
	case 0x0b:
		return fdc_take_rx(f, fs);
	default:
		return 0;
	}
}

void sdc_fdc_write(struct sdc_fdc *f, struct sdc_fs *fs, int reg, uint8_t D) {
	switch (reg) {
	case 0x00: {
		unsigned new_drive = f->drive;
		if (D & 0x01) {
			new_drive = 0;
		} else if (D & 0x02) {
			new_drive = 1;
		}
		f->drive = new_drive;
		f->side = (D & 0x40) ? 1u : 0u;
		f->nmi_enable = (D & 0x20) ? 1 : 0;
		f->halt_enable = (D & 0x80) ? 1 : 0;
		if (f->intrq) {
			f->halt_enable = 0;
		}
		break;
	}
	case 0x08:
		sdc_fdc_command(f, fs, D);
		break;
	case 0x09:
		f->track = D;
		break;
	case 0x0a:
		if (f->drq && f->writing) {
			fdc_put_tx(f, fs, D);
		} else {
			f->sector = D;
		}
		break;
	case 0x0b:
		fdc_put_tx(f, fs, D);
		break;
	default:
		break;
	}
}
