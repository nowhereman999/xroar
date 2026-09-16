/** \file
 *
 *  \brief CoCoSDC host-folder filesystem (Phase D).
 *
 *  Maps Studio SDC_FileAccess.asm commands onto a directory given as
 *  -sdc-root / sdc-root=:
 *
 *  - $E0/$E1  M:/m: mount, N:/n: create+mount, M: eject; also D:/L:/K:/X:
 *  - $C0/$C1  'I' info, '>' dir page, 'C' CWD, 'V' version (builtin)
 *  - $80/$81  read logical sector (256 bytes at LSN*256)
 *  - $A0/$A1  write logical sector
 *  - $90/$91  stream 512-byte sectors from LSN*512 until EOF or $D0
 *             (Play / StreamFile / BIGLOADM; $D0 aborts in the register layer)
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

struct sdc_slot {
	char *host_path;
	char name[8];
	char ext[3];
	uint8_t attr;
	int writable;
	FILE *fp;
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

#endif
