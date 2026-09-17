/** \file
 *
 *  \brief CoCoSDC host-folder filesystem (Phase D + SDC-DOS floppy).
 *
 *  Maps Studio SDC_FileAccess.asm commands onto a directory given as
 *  -sdc-root / sdc-root=:
 *
 *  - $E0/$E1  M:/m: mount, N:/n: create+mount, M: eject; also D:/L:/K:/X:
 *  - $C0/$C1  'I' info, '>' dir page, 'C' CWD, 'V' version (builtin)
 *  - $80-$87  read logical sector (bit 1: single-sided LSN; bit 2: byte port)
 *  - $A0-$A3  write logical sector (bit 1: single-sided LSN)
 *  - $90/$91  stream 512-byte sectors from LSN*512 until EOF or $D0
 *             (Play / StreamFile / BIGLOADM; $D0 aborts in the register layer)
 *
 *  Uppercase M:/N: mount a floppy/hard-disk image (JVC/VDK header, FDC CHS).
 *  Lowercase m:/n: are raw 256-byte blocks (FileAccess / stream; no FDC).
 *
 *  STARTUP.CFG in the volume root (0=FILE.DSK / 1=... / D=dir) is applied
 *  when the root is set and on hard reset, matching the MCU auto-mount.
 *
 *  Paths are 8.3-ish, case-insensitive, relative to the SD CWD unless they
 *  begin with '/'.  This is not a VCC SDC.dll port.
 */

#ifndef XROAR_COCOSDC_FS_H_
#define XROAR_COCOSDC_FS_H_

#include <stdio.h>
#include <stdint.h>

#include "cocosdc_hw.h"

#define SDC_SLOTS 2

#define SDC_ATTR_LOCKED (0x01)
#define SDC_ATTR_HIDDEN (0x02)
#define SDC_ATTR_SDF    (0x04)
#define SDC_ATTR_DIR    (0x10)

/* DSK images smaller than 18 tracks cannot hold the DECB directory track. */
#define SDC_DSK_MIN_BYTES 82944u
#define SDC_FLOPPY_SPT    18u
#define SDC_FLOPPY_MAX_SEC 2880u

#define SDC_FDC_OK       0
#define SDC_FDC_NOTREADY 1
#define SDC_FDC_RNF      2
#define SDC_FDC_WP       3
#define SDC_FDC_IO       4

enum sdc_disk_type {
	SDC_DTYPE_RAW = 0,
	SDC_DTYPE_DSK,
	SDC_DTYPE_JVC,
	SDC_DTYPE_VDK
};

struct sdc_slot {
	char *host_path;
	char name[8];
	char ext[3];
	uint8_t attr;
	int writable;
	FILE *fp;
	int fdc_ok;           /* M:/N: disk image; FDC CHS allowed */
	enum sdc_disk_type type;
	uint32_t header;      /* JVC/VDK header bytes before sector 0 */
	uint16_t spt;         /* sectors per track (floppy = 18) */
	uint8_t sides;        /* 1 or 2 */
	uint32_t fdc_sectors; /* sectors visible to FDC (HDD capped at 1440) */
};

struct sdc_dir_rec {
	uint8_t rec[16];
};

struct sdc_fs {
	char *root;
	char *cwd_rel; /* relative to root; "" = SD volume root */
	struct sdc_slot slot[SDC_SLOTS];
	struct sdc_dir_rec *dirents;
	unsigned ndirents;
	unsigned dir_cap;
	unsigned dir_index;
	int listing_active;
	/* Stream ($90/$91): byte offset in the mounted file. */
	uint32_t stream_off;
	uint32_t stream_end;
};

void sdc_fs_init(struct sdc_fs *fs);
void sdc_fs_free(struct sdc_fs *fs);
/* Keep root; unmount, CWD back to volume root, drop listing. */
void sdc_fs_reset(struct sdc_fs *fs);
/* Copy and use host_root as the SD volume.  Returns 0 if it exists as a dir. */
int sdc_fs_set_root(struct sdc_fs *fs, const char *host_root);

/* Execute a command whose params (and optional TX block) are ready. */
void sdc_fs_execute(struct sdc_fs *fs, struct sdc_hw *hw);

/* MCU STARTUP.CFG: 0=/1= image path, D= current directory.
 * Returns 0 if applied (or CFG had no 0=/1=), 1 if STARTUP.CFG is missing,
 * or SDC_ERR_* if a 0=/1= disk-image mount failed. */
int sdc_fs_apply_startup(struct sdc_fs *fs);

/* Human-readable SDC_ERR_* / apply_startup result for logs. */
const char *sdc_fs_err_name(int err);

/* FDC path: only M:/N: disk images, not m: raw mounts.
 * CHS is 1-based sector (WD1773).  DECB DIR is track 17 sectors 3–11;
 * the FAT is track 17 sector 2. */
int sdc_fs_fdc_ready(const struct sdc_fs *fs, unsigned drive);
int sdc_fs_fdc_wp(const struct sdc_fs *fs, unsigned drive);
int sdc_fs_fdc_read(struct sdc_fs *fs, unsigned drive, unsigned track,
		    unsigned sector, unsigned side, uint8_t *buf);
int sdc_fs_fdc_write(struct sdc_fs *fs, unsigned drive, unsigned track,
		     unsigned sector, unsigned side, const uint8_t *buf);
/* fflush+fsync mounted M: FILE*s so DSKINI/SAVE are visible after exit. */
void sdc_fs_flush(struct sdc_fs *fs);

#endif
