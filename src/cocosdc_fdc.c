/** \file
 *
 *  \brief CoCoSDC floppy-emulation commands (WD1773-ish).
 *
 *  Restore/seek complete immediately.  Read/write sector maps CHS through
 *  sdc_fs_fdc_read/write onto a mounted M: image.  m: raw mounts report
 *  NOTREADY on the FDC path (User Guide: FDC is not supported for raw
 *  blocks).
 */

#include "top-config.h"

#include "cocosdc_fdc.h"

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
		memset(f->buf, 0, sizeof(f->buf));
		rc = sdc_fs_fdc_read(fs, f->drive, f->track, f->sector, f->side, f->buf);
		if (rc == SDC_FDC_NOTREADY) {
			sdc_fdc_finish(f, SDC_FLP_NOTREADY);
		} else if (rc != 0) {
			sdc_fdc_finish(f, (uint8_t)(SDC_FLP_RNF | ((rc == SDC_FDC_WP) ? SDC_FLP_WP : 0)));
		} else {
			f->buf_n = SDC_BLOCK_SIZE;
			sdc_fdc_start_drq(f, 0);
		}
		break;
	case 0xa: /* Write sector */
	case 0xb:
		rc = sdc_fs_fdc_ready(fs, f->drive);
		if (!rc) {
			sdc_fdc_finish(f, SDC_FLP_NOTREADY);
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
		if (cmd & 0x08) {
			sdc_fdc_set_intrq(f, 1);
		} else {
			f->intrq = 0;
			f->drq = 0;
		}
		break;
	default: /* Read/write track */
		sdc_fdc_finish(f, SDC_FLP_RNF | sdc_fdc_idle_status(fs, f));
		break;
	}
}

uint8_t sdc_fdc_read(struct sdc_fdc *f, struct sdc_fs *fs, int reg) {
	uint8_t v;

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
		return f->sector;
	case 0x0b:
		if (f->drq && !f->writing && f->buf_i < f->buf_n) {
			v = f->buf[f->buf_i++];
			f->data = v;
			if (f->buf_i >= f->buf_n) {
				sdc_fdc_finish(f, sdc_fdc_idle_status(fs, f));
			} else {
				f->status = (uint8_t)(SDC_FLP_BUSY | SDC_FLP_DRQ);
			}
			return v;
		}
		return f->data;
	default:
		return 0;
	}
}

void sdc_fdc_write(struct sdc_fdc *f, struct sdc_fs *fs, int reg, uint8_t D) {
	int rc;

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
		f->sector = D;
		break;
	case 0x0b:
		f->data = D;
		if (f->drq && f->writing && f->buf_i < f->buf_n) {
			f->buf[f->buf_i++] = D;
			if (f->buf_i >= f->buf_n) {
				rc = sdc_fs_fdc_write(fs, f->drive, f->track, f->sector,
						      f->side, f->buf);
				if (rc == SDC_FDC_NOTREADY) {
					sdc_fdc_finish(f, SDC_FLP_NOTREADY);
				} else if (rc == SDC_FDC_WP) {
					sdc_fdc_finish(f, SDC_FLP_WP);
				} else if (rc != 0) {
					sdc_fdc_finish(f, SDC_FLP_RNF);
				} else {
					sdc_fdc_finish(f, sdc_fdc_idle_status(fs, f));
				}
			} else {
				f->status = (uint8_t)(SDC_FLP_BUSY | SDC_FLP_DRQ);
			}
		}
		break;
	default:
		break;
	}
}
