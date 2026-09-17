/* Host-side CommSDC + sdc_fs against a temporary -sdc-root.
 *
 * Mirrors SDC_Comm.asm waitForIt / 256-byte $FF4A/$FF4B transfers and the
 * FileAccess commands Phase B implements (mount, dir/info/CWD, LSN R/W)
 * plus Phase C stream ($90/$91 512-byte sectors, abort $D0) as used by
 * SDC_StreamFile_Library.asm / SDC_BigLoadm.asm, and Phase D Play
 * (SDC_Play.asm open/stream/interleaved 512 / $D0 / CLR $FF40).
 *
 * Compile / run (no XRoar, no SDL):
 *   ./tools/run-cocosdc-tests.sh
 */
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cocosdc_fdc.h"
#include "cocosdc_fs.h"
#include "cocosdc_hw.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static struct sdc_fs fs;
static struct sdc_hw hw;
static uint8_t last_status;
static int last_wait;
static int nfail;
static int ncheck;
static char root_path[PATH_MAX];

static void put_cmd(uint8_t *block, const char *s);

static int check(int cond, const char *msg) {
	ncheck++;
	if (!cond) {
		fprintf(stderr, "FAIL: %s (status=$%02X wait=%d)\n",
			msg, last_status, last_wait);
		nfail++;
		return 0;
	}
	return 1;
}

static int wait_for_it(void) {
	last_status = sdc_hw_read(&hw, 0x08);
	if (last_status & SDC_FAILED) {
		return -1;
	}
	if (!(last_status & SDC_BUSY)) {
		return 0;
	}
	if (last_status & SDC_READY) {
		return 1;
	}
	return -1;
}

static void pump(void) {
	sdc_fs_execute(&fs, &hw);
}

static void leave_cmd(void) {
	sdc_hw_write(&hw, 0x00, 0);
}

/* StreamFile / BIGLOADM: not CommSDC.  $90 is READY per 512-byte sector. */
static int stream_start(uint8_t cmd, uint32_t lsn) {
	sdc_hw_write(&hw, 0x00, SDC_CMDMODE);
	sdc_hw_write(&hw, 0x09, (uint8_t)(lsn >> 16));
	sdc_hw_write(&hw, 0x0a, (uint8_t)(lsn >> 8));
	sdc_hw_write(&hw, 0x0b, (uint8_t)lsn);
	last_wait = wait_for_it();
	if (last_wait < 0) {
		leave_cmd();
		return -1;
	}
	sdc_hw_write(&hw, 0x08, cmd);
	pump();
	last_wait = wait_for_it();
	return last_wait;
}

/* Read one 512-byte stream sector (LDD $FF4A / LDU ,Y style).  pump() after
 * each DATREG read so the last byte can refill or clear BUSY (cart path). */
static int stream_read_512(uint8_t *buf) {
	int i;

	for (i = 0; i < (int)SDC_STREAM_SIZE; i++) {
		buf[i] = sdc_hw_read(&hw, (i & 1) ? 0x0b : 0x0a);
		pump();
	}
	last_wait = wait_for_it();
	return last_wait;
}

static void stream_abort(void) {
	sdc_hw_write(&hw, 0x08, 0xd0);
	pump();
	last_wait = wait_for_it();
}

/* Play.asm: LDA $FF48 / ASRA / LBCC eof / BEQ wait.
 *  0 = not busy (EOF), 1 = BUSY|READY (proceed), -1 = wait / fail. */
static int play_asra(void) {
	last_status = sdc_hw_read(&hw, 0x08);
	if (last_status & SDC_FAILED) {
		return -1;
	}
	if (!(last_status & SDC_BUSY)) {
		return 0;
	}
	if (last_status & SDC_READY) {
		return 1;
	}
	return -1;
}

/* LDU <$4A / LDD $FF4A: high from $FF4A, low from $FF4B.  pump() after each
 * DATREG read so the last byte of a sector refills like the cart. */
static uint16_t play_ldd_ff4a(void) {
	uint8_t hi = sdc_hw_read(&hw, 0x0a);
	pump();
	uint8_t lo = sdc_hw_read(&hw, 0x0b);
	pump();
	return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static void play_read_512_words(uint8_t *buf) {
	int i;

	for (i = 0; i < (int)(SDC_STREAM_SIZE / 2); i++) {
		uint16_t w = play_ldd_ff4a();
		buf[i * 2] = (uint8_t)(w >> 8);
		buf[i * 2 + 1] = (uint8_t)w;
	}
	last_wait = wait_for_it();
}

/* OpenSDC_File_X_At_Start (StreamFile / Play): stay in command mode between
 * $E0/$E1 mount and $90/$91.  Play defaults SDC_DriveNumber to 1. */
static int open_sdc_file_x_at_start(uint8_t drive, const char *m_path) {
	uint8_t block[SDC_BLOCK_SIZE];
	int i;

	sdc_hw_write(&hw, 0x00, SDC_CMDMODE);
	last_wait = wait_for_it();
	if (last_wait < 0 || (last_status & SDC_BUSY)) {
		leave_cmd();
		return -1;
	}
	sdc_hw_write(&hw, 0x08, (uint8_t)(0xe0 + (drive & 1)));
	pump();
	last_wait = wait_for_it();
	if (last_wait != 1) {
		leave_cmd();
		return -1;
	}
	put_cmd(block, m_path);
	for (i = 0; i < (int)SDC_BLOCK_SIZE; i++) {
		sdc_hw_write(&hw, (i & 1) ? 0x0b : 0x0a, block[i]);
	}
	pump();
	last_wait = wait_for_it();
	if (last_wait < 0 || (last_status & SDC_BUSY)) {
		leave_cmd();
		return -1;
	}
	/* 24-bit LSN 0, then $43 again, then stream (OpenSDC_File_X). */
	sdc_hw_write(&hw, 0x09, 0);
	sdc_hw_write(&hw, 0x0a, 0);
	sdc_hw_write(&hw, 0x0b, 0);
	sdc_hw_write(&hw, 0x00, SDC_CMDMODE);
	last_wait = wait_for_it();
	if (last_wait < 0 || (last_status & SDC_BUSY)) {
		leave_cmd();
		return -1;
	}
	sdc_hw_write(&hw, 0x08, (uint8_t)(0x90 + (drive & 1)));
	pump();
	last_wait = wait_for_it();
	return last_wait;
}

/* CommSDC: A=cmd, B=param1, X=param2/3, optional 256-byte buffer. */
static int comm_sdc(uint8_t cmd, uint8_t b, uint16_t x, uint8_t *buf, int have_buf) {
	sdc_hw_write(&hw, 0x00, SDC_CMDMODE);
	sdc_hw_write(&hw, 0x09, b);
	sdc_hw_write(&hw, 0x0a, (uint8_t)(x >> 8));
	sdc_hw_write(&hw, 0x0b, (uint8_t)(x & 0xff));
	last_wait = wait_for_it();
	if (last_wait < 0) {
		leave_cmd();
		return -1;
	}
	sdc_hw_write(&hw, 0x08, cmd);
	pump();
	if (cmd & 0x20) {
		last_wait = wait_for_it();
		if (last_wait != 1) {
			leave_cmd();
			return -1;
		}
		for (int i = 0; i < SDC_BLOCK_SIZE; i++) {
			uint8_t v = buf ? buf[i] : 0;
			sdc_hw_write(&hw, (i & 1) ? 0x0b : 0x0a, v);
		}
		pump();
		last_wait = wait_for_it();
		if (last_wait < 0 || (last_status & SDC_BUSY)) {
			leave_cmd();
			return -1;
		}
	} else {
		last_wait = wait_for_it();
		if (last_wait < 0) {
			leave_cmd();
			return -1;
		}
		if (last_wait == 1 && have_buf && buf) {
			for (int i = 0; i < SDC_BLOCK_SIZE; i++) {
				buf[i] = sdc_hw_read(&hw, (i & 1) ? 0x0b : 0x0a);
			}
			last_status = sdc_hw_read(&hw, 0x08);
		}
	}
	leave_cmd();
	return 0;
}

static void put_cmd(uint8_t *block, const char *s) {
	size_t n = strlen(s);
	memset(block, 0, SDC_BLOCK_SIZE);
	if (n >= SDC_BLOCK_SIZE) {
		n = SDC_BLOCK_SIZE - 1;
	}
	memcpy(block, s, n);
}

static int write_file(const char *path, const void *data, size_t n) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		return -1;
	}
	if (n && fwrite(data, 1, n, f) != n) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int rm_path(const char *path) {
	DIR *d = opendir(path);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			char child[PATH_MAX];
			if (de->d_name[0] == '.' &&
			    (de->d_name[1] == 0 ||
			     (de->d_name[1] == '.' && de->d_name[2] == 0))) {
				continue;
			}
			if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >=
			    (int)sizeof(child)) {
				closedir(d);
				return -1;
			}
			rm_path(child);
		}
		closedir(d);
		return rmdir(path);
	}
	return unlink(path);
}

static int host_size(const char *rel, long *out) {
	char path[PATH_MAX];
	struct stat st;
	if (snprintf(path, sizeof(path), "%s/%s", root_path, rel) >= (int)sizeof(path)) {
		return -1;
	}
	if (stat(path, &st) != 0) {
		return -1;
	}
	*out = (long)st.st_size;
	return 0;
}

static int rec_named(const uint8_t *page, const char *fat11) {
	for (int i = 0; i < 16; i++) {
		const uint8_t *r = page + i * 16;
		if (r[0] == 0) {
			break;
		}
		if (memcmp(r, fat11, 11) == 0) {
			return i;
		}
	}
	return -1;
}

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int failed_bits(uint8_t bits) {
	return (last_status & SDC_FAILED) && ((last_status & bits) == bits);
}

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void test_version_reset(void) {
	check(comm_sdc(0xc0, 'V', 0, NULL, 0) == 0, "VERSION");
	check(sdc_hw_read(&hw, 0x0a) == 0x01 && sdc_hw_read(&hw, 0x0b) == 0x27,
	      "VERSION BCD 1.27 after leaving command mode");
	check(comm_sdc(0x1c, 0xaa, 0x5500, NULL, 0) == 0, "RESET $1C");
	check(sdc_hw_read(&hw, 0x09) == 'P' && sdc_hw_read(&hw, 0x0a) == 'M',
	      "RESET returns PM");
}

static void test_missing_and_eject(void) {
	uint8_t block[SDC_BLOCK_SIZE];
	put_cmd(block, "m:MISSING.BIN");
	check(comm_sdc(0xe0, 0, 0, block, 1) != 0, "missing mount fails");
	check(failed_bits(SDC_ERR_NOTFOUND), "missing mount FAILED|$10");

	put_cmd(block, "M:");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "eject empty slot 0");
	check(comm_sdc(0x80, 0, 0, block, 1) != 0, "read with nothing mounted fails");
	check(last_status & SDC_FAILED, "read unmounted FAILED");
}

static void test_mount_read_info(const char *hello, size_t hello_n) {
	uint8_t block[SDC_BLOCK_SIZE];
	uint32_t sz;

	put_cmd(block, "m:HELLO.TXT");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "mount m:HELLO.TXT");

	memset(block, 0, sizeof(block));
	check(comm_sdc(0x80, 0, 0, block, 1) == 0, "read LSN 0");
	check(last_wait == 1, "read LSN offered READY payload");
	check(memcmp(block, hello, hello_n) == 0, "LSN 0 matches host file");
	check(block[hello_n] == 0, "short file zero-padded in sector");

	memset(block, 0, sizeof(block));
	check(comm_sdc(0xc0, 'I', 0, block, 1) == 0, "INFO");
	check(memcmp(block, "HELLO   TXT", 11) == 0, "INFO 8.3 name");
	sz = le32(block + 28);
	check(sz == (uint32_t)hello_n, "INFO size LSB-first at 28-31");

	check(comm_sdc(0xc0, 'Q', 0, NULL, 0) == 0, "QUERY size");
	check(sdc_hw_read(&hw, 0x09) == 0 && sdc_hw_read(&hw, 0x0a) == 0 &&
	      sdc_hw_read(&hw, 0x0b) == 1, "QUERY sector count 1");

	/* FileExists sequence: mount then M: eject. */
	put_cmd(block, "M:");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "eject after info");
	check(comm_sdc(0xc0, 'I', 0, block, 1) != 0, "INFO after eject fails");
	check(failed_bits(SDC_ERR_NOTFOUND), "INFO unmounted FAILED|$10");
}

static void test_case_partial_and_cwd(void) {
	uint8_t block[SDC_BLOCK_SIZE];

	put_cmd(block, "m:games/foo.bin");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "mount m:games/foo.bin (case)");
	memset(block, 0, sizeof(block));
	check(comm_sdc(0x80, 0, 0, block, 1) == 0 && block[0] == 'F' && block[2] == 'O',
	      "FOO.BIN LSN 0");
	memset(block, 0xaa, sizeof(block));
	check(comm_sdc(0x80, 0, 1, block, 1) == 0, "FOO.BIN LSN 1 partial");
	check(block[0] == 0x5a && block[43] == 0x5a && block[44] == 0,
	      "partial sector padded with zeros");
	check(comm_sdc(0x80, 0, 2, block, 1) != 0, "LSN past EOF fails");
	put_cmd(block, "M:");
	(void)comm_sdc(0xe0, 0, 0, block, 1);

	put_cmd(block, "m:/GAMES/FOO.BIN");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "absolute /GAMES/FOO.BIN");
	put_cmd(block, "M:");
	(void)comm_sdc(0xe0, 0, 0, block, 1);

	/* CWD at volume root: bits 4+7, no payload (CommSDC BMI on FAILED). */
	check(comm_sdc(0xc0, 'C', 0, block, 1) != 0, "CWD at root fails");
	check(failed_bits(SDC_ERR_NOTFOUND), "CWD root FAILED|$10");

	put_cmd(block, "D:GAMES");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "D:GAMES");
	memset(block, 0, sizeof(block));
	check(comm_sdc(0xc0, 'C', 0, block, 1) == 0, "CWD leaf");
	check(memcmp(block, "GAMES      ", 11) == 0, "CWD 8.3 name");
	put_cmd(block, "m:FOO.BIN");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "mount relative to CWD");
	put_cmd(block, "M:");
	(void)comm_sdc(0xe0, 0, 0, block, 1);
	put_cmd(block, "D:/");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "D:/ back to root");
}

static void test_create_write_lsn_latch(void) {
	uint8_t block[SDC_BLOCK_SIZE];
	uint8_t a[SDC_BLOCK_SIZE];
	uint8_t b[SDC_BLOCK_SIZE];
	long sz = -1;

	put_cmd(block, "n:NEWFILE.BIN");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "n:NEWFILE.BIN");

	/* Payload of 0x3c would look like LSN 0x003C3C if params were not latched. */
	memset(a, 0x3c, sizeof(a));
	memcpy(a, "WRITTEN", 7);
	check(comm_sdc(0xa0, 0, 0, a, 1) == 0, "write LSN 0");
	check(host_size("NEWFILE.BIN", &sz) == 0 && sz == SDC_BLOCK_SIZE,
	      "write LSN 0 size is 256 (LSN latched, not clobbered by data port)");

	memset(b, 'B', sizeof(b));
	check(comm_sdc(0xa0, 0, 1, b, 1) == 0, "write LSN 1");
	check(host_size("NEWFILE.BIN", &sz) == 0 && sz == 2 * SDC_BLOCK_SIZE,
	      "two sequential FileAccess-style LBNs");

	put_cmd(block, "M:");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "eject after writes");
	put_cmd(block, "m:NEWFILE.BIN");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "reopen NEWFILE.BIN");
	memset(block, 0, sizeof(block));
	check(comm_sdc(0x80, 0, 0, block, 1) == 0 && memcmp(block, "WRITTEN", 7) == 0,
	      "read back LSN 0");
	memset(block, 0, sizeof(block));
	check(comm_sdc(0x80, 0, 1, block, 1) == 0 && block[0] == 'B' && block[255] == 'B',
	      "read back LSN 1");

	memset(block, 0, sizeof(block));
	check(comm_sdc(0xc0, 'I', 0, block, 1) == 0, "INFO after write");
	check(le32(block + 28) == 2u * SDC_BLOCK_SIZE, "INFO size 512");
	put_cmd(block, "M:");
	(void)comm_sdc(0xe0, 0, 0, block, 1);
}

static void test_slots_inuse(void) {
	uint8_t block[SDC_BLOCK_SIZE];
	uint8_t data[SDC_BLOCK_SIZE];

	put_cmd(block, "m:HELLO.TXT");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "slot 0 HELLO");
	put_cmd(block, "m:HELLO.TXT");
	check(comm_sdc(0xe1, 0, 0, block, 1) != 0, "same file on slot 1 is in use");
	check(failed_bits(SDC_ERR_INUSE), "in use FAILED|$20");

	put_cmd(block, "n:SLOT1.BIN");
	check(comm_sdc(0xe1, 0, 0, block, 1) == 0, "n: on slot 1");
	memset(data, 0x11, sizeof(data));
	data[0] = 'S';
	check(comm_sdc(0xa1, 0, 0, data, 1) == 0, "write slot 1 LSN 0");
	memset(block, 0, sizeof(block));
	check(comm_sdc(0x81, 0, 0, block, 1) == 0 && block[0] == 'S' && block[1] == 0x11,
	      "read slot 1");

	put_cmd(block, "M:");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "eject slot 0");
	check(comm_sdc(0xe1, 0, 0, block, 1) == 0, "eject slot 1");
}

static void test_dir_listing(const char *hello, size_t hello_n) {
	uint8_t block[SDC_BLOCK_SIZE];
	int i, nrec, idx;
	char path[PATH_MAX];

	put_cmd(block, "L:*.*");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "L:*.*");
	memset(block, 0, sizeof(block));
	check(comm_sdc(0xc0, '>', 0, block, 1) == 0, "DIR page");
	check(last_wait == 1, "DIR page READY");
	check(rec_named(block, "HELLO   TXT") >= 0, "DIR has HELLO.TXT");
	check(rec_named(block, "GAMES      ") >= 0, "DIR has GAMES");
	idx = rec_named(block, "HELLO   TXT");
	if (idx >= 0) {
		const uint8_t *r = block + idx * 16;
		check(be32(r + 12) == (uint32_t)hello_n, "DIR size MSB-first at 12-15");
		check((r[11] & SDC_ATTR_DIR) == 0, "HELLO is not a directory");
	}
	idx = rec_named(block, "GAMES      ");
	if (idx >= 0) {
		check((block[idx * 16 + 11] & SDC_ATTR_DIR) != 0, "GAMES attr $10");
	}

	put_cmd(block, "L:GAMES/*.BIN");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "L:GAMES/*.BIN");
	memset(block, 0, sizeof(block));
	check(comm_sdc(0xc0, '>', 0, block, 1) == 0, "wildcard page");
	check(rec_named(block, "FOO     BIN") >= 0, "wildcard sees FOO.BIN");
	check(rec_named(block, "HELLO   TXT") < 0, "wildcard does not list root HELLO");

	/* Two pages: 20 files → 16 + 4, unused records zero. */
	if (snprintf(path, sizeof(path), "%s/PAGES", root_path) < (int)sizeof(path) &&
	    mkdir(path, 0755) == 0) {
		for (i = 0; i < 20; i++) {
			char f[PATH_MAX];
			char name[16];
			snprintf(name, sizeof(name), "F%02d.DAT", i);
			snprintf(f, sizeof(f), "%s/%s", path, name);
			(void)write_file(f, name, strlen(name));
		}
		put_cmd(block, "L:PAGES/*.*");
		check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "L:PAGES/*.*");
		memset(block, 0, sizeof(block));
		check(comm_sdc(0xc0, '>', 0, block, 1) == 0, "DIR page 1");
		nrec = 0;
		for (i = 0; i < 16; i++) {
			if (block[i * 16] != 0) {
				nrec++;
			}
		}
		check(nrec == 16, "first dir page has 16 records");
		memset(block, 0, sizeof(block));
		check(comm_sdc(0xc0, '>', 0, block, 1) == 0, "DIR page 2");
		nrec = 0;
		for (i = 0; i < 16; i++) {
			if (block[i * 16] != 0) {
				nrec++;
			}
		}
		check(nrec == 4, "second dir page has 4 records");
		check(block[4 * 16] == 0, "unused dir records are zero");
	} else {
		check(0, "mkdir PAGES");
	}
}

static void test_mkdir_delete(void) {
	uint8_t block[SDC_BLOCK_SIZE];
	char path[PATH_MAX];
	struct stat st;

	put_cmd(block, "K:SUBDIR");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "K:SUBDIR");
	snprintf(path, sizeof(path), "%s/SUBDIR", root_path);
	check(stat(path, &st) == 0 && S_ISDIR(st.st_mode), "SUBDIR exists on host");

	put_cmd(block, "n:SUBDIR/TMP.BIN");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "create file in SUBDIR");
	put_cmd(block, "M:");
	(void)comm_sdc(0xe0, 0, 0, block, 1);

	put_cmd(block, "X:SUBDIR");
	check(comm_sdc(0xe0, 0, 0, block, 1) != 0, "X: non-empty dir fails");

	put_cmd(block, "X:SUBDIR/TMP.BIN");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "X: file");
	put_cmd(block, "X:SUBDIR");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "X: empty dir");
	check(stat(path, &st) != 0, "SUBDIR removed");
}

static void test_escape(void) {
	uint8_t block[SDC_BLOCK_SIZE];

	put_cmd(block, "m:../../etc/passwd");
	check(comm_sdc(0xe0, 0, 0, block, 1) != 0, "path escape fails");
	check((last_status & SDC_FAILED) != 0, "path escape FAILED");
}

static void test_stream(void) {
	uint8_t name[SDC_BLOCK_SIZE];
	uint8_t sec[SDC_STREAM_SIZE];
	uint8_t file[SDC_STREAM_SIZE * 2 + 40];
	char path[PATH_MAX];
	size_t i;
	size_t nfile = sizeof(file);

	for (i = 0; i < nfile; i++) {
		file[i] = (uint8_t)(i & 0xff);
	}
	file[0] = 0xa0;
	file[511] = 0xa1;
	file[512] = 0xb0;
	file[1023] = 0xb1;
	file[1024] = 0xc0;
	file[nfile - 1] = 0xc1;
	if (snprintf(path, sizeof(path), "%s/STREAM.BIN", root_path) >= (int)sizeof(path) ||
	    write_file(path, file, nfile) != 0) {
		check(0, "write STREAM.BIN");
		return;
	}

	/* Unmounted $90 fails (POLLREADY would hang; waitForIt sees FAILED). */
	check(stream_start(0x90, 0) < 0, "stream with nothing mounted fails");
	check(last_status & SDC_FAILED, "unmounted stream FAILED");
	leave_cmd();

	put_cmd(name, "m:STREAM.BIN");
	check(comm_sdc(0xe0, 0, 0, name, 1) == 0, "mount m:STREAM.BIN");

	/* OpenSDC_File_X_At_Start: $90, POLLREADY, then 512-byte sectors. */
	check(stream_start(0x90, 0) == 1, "stream $90 READY for first sector");
	check((last_status & (SDC_BUSY | SDC_READY)) == (SDC_BUSY | SDC_READY),
	      "stream first sector BUSY|READY");
	check(stream_read_512(sec) == 1, "after sector 0 READY for sector 1");
	check(sec[0] == 0xa0 && sec[511] == 0xa1 && sec[1] == 0x01,
	      "stream sector 0 bytes");
	check(memcmp(sec, file, SDC_STREAM_SIZE) == 0, "stream sector 0 matches host");

	check(stream_read_512(sec) == 1, "after sector 1 READY for padded tail");
	check(sec[0] == 0xb0 && sec[511] == 0xb1, "stream sector 1 boundary");
	check(memcmp(sec, file + SDC_STREAM_SIZE, SDC_STREAM_SIZE) == 0,
	      "stream sector 1 matches host");

	check(stream_read_512(sec) == 0, "last stream sector then Not Busy (EOF)");
	check(sec[0] == 0xc0 && sec[39] == 0xc1, "stream tail payload");
	check(sec[40] == 0 && sec[511] == 0, "short last sector zero-padded to 512");
	check(!(last_status & SDC_BUSY), "EOF clears BUSY");
	leave_cmd();

	/* LSN is a 512-byte stream sector (not FileAccess's 256-byte LBN). */
	check(stream_start(0x90, 1) == 1, "stream from LSN 1");
	check(stream_read_512(sec) == 1, "LSN 1 first sector is file+512");
	check(sec[0] == 0xb0 && sec[511] == 0xb1, "LSN 1 skips first 512 bytes");
	leave_cmd();

	/* CLR $FF40 aborts an in-flight stream (Close_SD_File / BIGLOADM done). */
	check(stream_start(0x90, 0) == 1, "stream for leave-cmd abort");
	(void)sdc_hw_read(&hw, 0x0a);
	pump();
	leave_cmd();
	check(!(sdc_hw_read(&hw, 0x08) & SDC_BUSY), "leave command mode clears stream BUSY");

	/* $D0 abort (StreamTest.asm BREAK key) after the first sector. */
	check(stream_start(0x90, 0) == 1, "stream for $D0 abort");
	check(stream_read_512(sec) == 1, "sector 0 before abort");
	check(sec[0] == 0xa0, "abort path still delivered sector 0");
	stream_abort();
	check(last_wait == 0 && !(last_status & SDC_FAILED),
	      "abort $D0 is Not Busy");
	leave_cmd();

	put_cmd(name, "M:");
	check(comm_sdc(0xe0, 0, 0, name, 1) == 0, "eject STREAM.BIN");

	/* Idle $D0 still must not hang (Phase B). */
	check(comm_sdc(0xd0, 0, 0, NULL, 0) == 0, "idle abort $D0 does not hang");

	/* Slot 1: StreamFile defaults SDC_DriveNumber to 1 ($E1 / $91). */
	put_cmd(name, "m:GAMES/FOO.BIN");
	check(comm_sdc(0xe1, 0, 0, name, 1) == 0, "mount FOO.BIN slot 1");
	check(stream_start(0x91, 0) == 1, "stream $91 READY");
	check(stream_read_512(sec) == 0, "FOO.BIN one padded 512 then EOF");
	check(sec[0] == 'F' && sec[1] == 'O' && sec[2] == 'O' && sec[3] == 0x5a,
	      "slot 1 stream payload");
	check(sec[299] == 0x5a && sec[300] == 0, "FOO.BIN 300 bytes padded in stream sector");
	leave_cmd();
	put_cmd(name, "M:");
	check(comm_sdc(0xe1, 0, 0, name, 1) == 0, "eject slot 1");

	/* FileAccess 256-byte LSN still works on the stream file. */
	put_cmd(name, "m:STREAM.BIN");
	check(comm_sdc(0xe0, 0, 0, name, 1) == 0, "reopen STREAM.BIN for $80");
	memset(name, 0, sizeof(name));
	check(comm_sdc(0x80, 0, 0, name, 1) == 0 && name[0] == 0xa0 &&
	      name[255] == (uint8_t)255, "FileAccess LSN 0 is 256 bytes not 512");
	put_cmd(name, "M:");
	(void)comm_sdc(0xe0, 0, 0, name, 1);

	/* LSN past EOF */
	put_cmd(name, "m:HELLO.TXT");
	check(comm_sdc(0xe0, 0, 0, name, 1) == 0, "mount HELLO for past-EOF stream");
	check(stream_start(0x90, 1) < 0, "stream LSN past EOF fails");
	check(last_status & SDC_FAILED, "stream past EOF FAILED");
	leave_cmd();
	put_cmd(name, "M:");
	(void)comm_sdc(0xe0, 0, 0, name, 1);
}

static void test_play(void) {
	uint8_t sec[SDC_STREAM_SIZE];
	uint8_t got[SDC_STREAM_SIZE * 4];
	uint8_t pcm[SDC_STREAM_SIZE * 3 + 80];
	char path[PATH_MAX];
	size_t i;
	size_t npcm = sizeof(pcm);
	size_t nsec;
	size_t ngot;
	int asra;
	unsigned sector;

	for (i = 0; i < npcm; i++) {
		pcm[i] = (uint8_t)(0x40 + (i & 0x3f));
	}
	pcm[0] = 0x11;
	pcm[1] = 0x22;
	pcm[511] = 0x33;
	pcm[512] = 0x44;
	pcm[1023] = 0x55;
	pcm[1024] = 0x66;
	pcm[npcm - 1] = 0x77;
	if (snprintf(path, sizeof(path), "%s/PLAY.RAW", root_path) >= (int)sizeof(path) ||
	    write_file(path, pcm, npcm) != 0) {
		check(0, "write PLAY.RAW");
		return;
	}

	/* Play defaults SDC_DriveNumber to 1: $E1 mount, $91 stream, stay in
	 * command mode (no CommSDC $FF40=0 between mount and $90). */
	check(open_sdc_file_x_at_start(1, "m:PLAY.RAW") == 1,
	      "Play OpenSDC_File_X_At_Start $E1/$91 READY");
	check(play_asra() == 1, "Play ASRA first sector BUSY|READY");

	/* First 512 into buffer 1, then ASRA wait for the next sector (Play). */
	play_read_512_words(sec);
	check(sec[0] == 0x11 && sec[1] == 0x22 && sec[511] == 0x33,
	      "Play first sector 16-bit $FF4A/$FF4B order");
	check(memcmp(sec, pcm, SDC_STREAM_SIZE) == 0, "Play sector 0 matches host");
	check(play_asra() == 1, "Play ASRA after sector 0 waits as READY (sector 1)");

	memcpy(got, sec, SDC_STREAM_SIZE);
	ngot = SDC_STREAM_SIZE;

	/* Interleaved Play pacing: after ~166 dummy samples, blast-read 512
	 * without a READY wait loop; one ASRA poll afterwards for EOF. */
	nsec = (npcm + SDC_STREAM_SIZE - 1) / SDC_STREAM_SIZE;
	for (sector = 1; sector < nsec; sector++) {
		play_read_512_words(sec);
		memcpy(got + ngot, sec, SDC_STREAM_SIZE);
		ngot += SDC_STREAM_SIZE;
		asra = play_asra();
		if (sector + 1 < nsec) {
			check(asra == 1, "Play interleaved poll still BUSY (more sectors)");
		} else {
			check(asra == 0, "Play interleaved poll Not Busy after last sector");
		}
	}
	check(ngot >= npcm, "Play consumed at least the file length");
	check(memcmp(got, pcm, npcm) == 0, "Play concatenated sectors match PLAY.RAW");
	check(got[npcm] == 0 && got[ngot - 1] == 0, "Play short last sector zero-padded");
	check(!(sdc_hw_read(&hw, 0x08) & SDC_BUSY), "Play EOF CLR path sees Not Busy");
	leave_cmd();

	/* BREAK: $D0 then CLR $FF40 (SDCBreak / SDCAudioPlayDone).  $D0 must
	 * clear BUSY in the command write — Play does not waitForIt. */
	check(open_sdc_file_x_at_start(1, "m:PLAY.RAW") == 1, "Play reopen for BREAK");
	play_read_512_words(sec);
	check(play_asra() == 1, "Play has another sector before BREAK");
	play_read_512_words(sec);
	sdc_hw_write(&hw, 0x08, 0xd0);
	last_status = sdc_hw_read(&hw, 0x08);
	check(!(last_status & (SDC_BUSY | SDC_FAILED)),
	      "Play $D0 BREAK is Not Busy without pump");
	leave_cmd();
	check(!(sdc_hw_read(&hw, 0x08) & SDC_BUSY), "Play CLR $FF40 after $D0");

	/* Same host file cannot occupy both slots; eject slot 1 first. */
	{
		uint8_t name[SDC_BLOCK_SIZE];
		put_cmd(name, "M:");
		check(comm_sdc(0xe1, 0, 0, name, 1) == 0, "eject Play slot 1 before slot 0");
	}

	/* Drive 0 Play path ($E0/$90) — same contract, other slot. */
	check(open_sdc_file_x_at_start(0, "m:PLAY.RAW") == 1, "Play $E0/$90 READY");
	play_read_512_words(sec);
	check(sec[0] == 0x11, "Play slot 0 first sample");
	sdc_hw_write(&hw, 0x08, 0xd0);
	leave_cmd();

	/* Eject both slots (Play does not M: eject; FileAccess might next). */
	{
		uint8_t name[SDC_BLOCK_SIZE];
		put_cmd(name, "M:");
		check(comm_sdc(0xe1, 0, 0, name, 1) == 0, "eject Play slot 1");
		check(comm_sdc(0xe0, 0, 0, name, 1) == 0, "eject Play slot 0");
	}
}

static void test_no_root(void) {
	uint8_t block[SDC_BLOCK_SIZE];
	sdc_fs_free(&fs);
	sdc_fs_init(&fs);
	check(comm_sdc(0xc0, 'V', 0, NULL, 0) == 0, "VERSION without sdc-root");
	put_cmd(block, "m:HELLO.TXT");
	check(comm_sdc(0xe0, 0, 0, block, 1) != 0, "mount without sdc-root fails");
	check(last_status & SDC_FAILED, "mount without root FAILED");
	sdc_fs_free(&fs);
	sdc_fs_init(&fs);
	check(sdc_fs_set_root(&fs, root_path) == 0, "restore sdc-root");
}

/* 35-track SS DECB DSK: GAT T17 S1, directory T17 S2, START.BAS in granule 0. */
#define DECB_DSK_BYTES (35 * 18 * 256)
#define DECB_GAT_OFF   ((17 * 18 + 0) * 256)
#define DECB_DIR_OFF   ((17 * 18 + 1) * 256)

static int write_decb_dsk(const char *path, const char *payload, size_t pay_n) {
	uint8_t *img;
	uint8_t *dir;
	size_t n = pay_n > 255 ? 255 : pay_n;
	int rc = -1;

	img = calloc(1, DECB_DSK_BYTES);
	if (!img) {
		return -1;
	}
	memset(img + DECB_GAT_OFF, 0xff, 256);
	img[DECB_GAT_OFF + 0] = (uint8_t)(0xc0 | 1); /* granule 0 last, 1 sector */
	dir = img + DECB_DIR_OFF;
	memcpy(dir, "START   BAS", 11);
	dir[11] = 0;    /* BASIC */
	dir[12] = 0xff; /* ASCII */
	dir[13] = 0;    /* first granule */
	dir[14] = 0;
	dir[15] = (uint8_t)n;
	memcpy(img, payload, n);
	if (write_file(path, img, DECB_DSK_BYTES) == 0) {
		rc = 0;
	}
	free(img);
	return rc;
}

static void fdc_latch_drive0(struct sdc_fdc *f) {
	sdc_fdc_write(f, &fs, 0x00, 0xa9); /* halt + density + motor + drv0 */
}

static void fdc_restore(struct sdc_fdc *f) {
	sdc_fdc_write(f, &fs, 0x08, 0x03);
}

static int fdc_read_sector(struct sdc_fdc *f, uint8_t track, uint8_t sector, uint8_t *buf) {
	unsigned i;
	uint8_t st;

	sdc_fdc_write(f, &fs, 0x09, track);
	sdc_fdc_write(f, &fs, 0x0a, sector);
	sdc_fdc_write(f, &fs, 0x08, 0x80);
	st = sdc_fdc_read(f, &fs, 0x08);
	if (st & SDC_FLP_NOTREADY) {
		last_status = st;
		return -1;
	}
	if (!(st & SDC_FLP_DRQ)) {
		last_status = st;
		return -1;
	}
	for (i = 0; i < SDC_BLOCK_SIZE; i++) {
		buf[i] = sdc_fdc_read(f, &fs, 0x0b);
	}
	last_status = sdc_fdc_read(f, &fs, 0x08);
	return 0;
}

static void test_dsk_mount_and_fdc(void) {
	char path[PATH_MAX];
	const char *bas = "10 PRINT \"OK\"\r";
	uint8_t block[SDC_BLOCK_SIZE];
	uint8_t sec[SDC_BLOCK_SIZE];
	struct sdc_fdc fdc;
	uint8_t wr[SDC_BLOCK_SIZE];

	if (snprintf(path, sizeof(path), "%s/START.DSK", root_path) >= (int)sizeof(path) ||
	    write_decb_dsk(path, bas, strlen(bas)) != 0) {
		check(0, "write START.DSK");
		return;
	}

	put_cmd(block, "M:HELLO.TXT");
	check(comm_sdc(0xe0, 0, 0, block, 1) != 0, "M: of tiny non-DSK fails");
	check(failed_bits(SDC_ERR_INVALID), "M: HELLO.TXT FAILED|$04 invalid image");

	sdc_fdc_reset(&fdc);
	fdc_latch_drive0(&fdc);
	fdc_restore(&fdc);
	check(sdc_fdc_read(&fdc, &fs, 0x08) & SDC_FLP_NOTREADY,
	      "FDC restore with nothing mounted is NOTREADY");

	put_cmd(block, "m:START.DSK");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "m:START.DSK raw mount");
	sdc_fdc_reset(&fdc);
	fdc_latch_drive0(&fdc);
	check(fdc_read_sector(&fdc, 17, 2, sec) != 0, "FDC read of m: raw is not ready");
	check(last_status & SDC_FLP_NOTREADY, "m: DSK does not enable FDC");
	put_cmd(block, "M:");
	(void)comm_sdc(0xe0, 0, 0, block, 1);

	put_cmd(block, "M:START.DSK");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "M:START.DSK disk-image mount");
	check(sdc_fs_fdc_ready(&fs, 0), "M: sets fdc_ok on slot 0");

	memset(block, 0, sizeof(block));
	check(comm_sdc(0x80, 0, 0, block, 1) == 0, "LSN 0 after M: is first sector");
	check(memcmp(block, bas, strlen(bas)) == 0, "LSN 0 is START.BAS payload");

	sdc_fdc_reset(&fdc);
	fdc_latch_drive0(&fdc);
	fdc_restore(&fdc);
	check(fdc.track == 0, "restore sets track 0");
	check(fdc.intrq, "restore raises INTRQ");
	(void)sdc_fdc_read(&fdc, &fs, 0x08);
	check(!fdc.intrq, "status read clears INTRQ");
	check(!(last_status & SDC_FLP_NOTREADY), "restore with mounted DSK is ready");

	check(fdc_read_sector(&fdc, 17, 2, sec) == 0, "DSKCON read directory T17 S2");
	check(memcmp(sec, "START   BAS", 11) == 0, "directory entry START.BAS");
	check(sec[12] == 0xff && sec[13] == 0, "ASCII BASIC granule 0");
	check(sdc_fdc_want_nmi(&fdc) || !fdc.drq, "sector complete drops DRQ");

	check(fdc_read_sector(&fdc, 0, 1, sec) == 0, "DSKCON read T0 S1 (granule 0)");
	check(memcmp(sec, bas, strlen(bas)) == 0, "START.BAS payload via FDC CHS");

	check(fdc_read_sector(&fdc, 17, 1, sec) == 0, "read GAT T17 S1");
	check((sec[0] & 0xc0) == 0xc0, "GAT granule 0 is last granule");

	memset(wr, 0x5a, sizeof(wr));
	sdc_fdc_write(&fdc, &fs, 0x09, 0);
	sdc_fdc_write(&fdc, &fs, 0x0a, 2);
	sdc_fdc_write(&fdc, &fs, 0x08, 0xa0);
	check(fdc.drq, "write sector presents DRQ");
	{
		unsigned i;
		for (i = 0; i < SDC_BLOCK_SIZE; i++) {
			sdc_fdc_write(&fdc, &fs, 0x0b, wr[i]);
		}
	}
	check(fdc_read_sector(&fdc, 0, 2, sec) == 0 && sec[0] == 0x5a && sec[255] == 0x5a,
	      "FDC write sector round-trip");

	put_cmd(block, "M:");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "eject START.DSK");
}

static void test_startup_cfg_and_jvc(void) {
	char path[PATH_MAX];
	char cfg[PATH_MAX];
	const char *bas = "10 PRINT \"CFG\"\r";
	uint8_t *jvc;
	uint8_t sec[SDC_BLOCK_SIZE];
	uint8_t block[SDC_BLOCK_SIZE];
	struct sdc_fdc fdc;
	struct sdc_fs fs2;

	if (snprintf(path, sizeof(path), "%s/AUTO.DSK", root_path) >= (int)sizeof(path) ||
	    write_decb_dsk(path, bas, strlen(bas)) != 0) {
		check(0, "write AUTO.DSK");
		return;
	}
	if (snprintf(cfg, sizeof(cfg), "%s/startup.cfg", root_path) >= (int)sizeof(cfg) ||
	    write_file(cfg, "D=/\n0=AUTO.DSK\n", strlen("D=/\n0=AUTO.DSK\n")) != 0) {
		check(0, "write startup.cfg");
		return;
	}

	sdc_fs_reset(&fs);
	check(sdc_fs_apply_startup(&fs) == 0, "apply STARTUP.CFG");
	check(sdc_fs_fdc_ready(&fs, 0), "startup.cfg 0=AUTO.DSK auto-mounted");

	sdc_fdc_reset(&fdc);
	fdc_latch_drive0(&fdc);
	check(fdc_read_sector(&fdc, 17, 2, sec) == 0, "auto-mount directory readable");
	check(memcmp(sec, "START   BAS", 11) == 0, "auto-mounted DSK has START.BAS");
	check(fdc_read_sector(&fdc, 0, 1, sec) == 0 && memcmp(sec, bas, strlen(bas)) == 0,
	      "auto-mounted START.BAS payload");

	put_cmd(block, "M:");
	(void)comm_sdc(0xe0, 0, 0, block, 1);

	jvc = malloc(2 + DECB_DSK_BYTES);
	if (!jvc) {
		check(0, "alloc JVC");
		return;
	}
	jvc[0] = 18;
	jvc[1] = 1;
	if (snprintf(path, sizeof(path), "%s/AUTO.DSK", root_path) >= (int)sizeof(path)) {
		free(jvc);
		check(0, "AUTO.DSK path");
		return;
	}
	{
		FILE *fp = fopen(path, "rb");
		if (!fp || fread(jvc + 2, 1, DECB_DSK_BYTES, fp) != DECB_DSK_BYTES) {
			if (fp) {
				fclose(fp);
			}
			free(jvc);
			check(0, "read AUTO.DSK for JVC wrap");
			return;
		}
		fclose(fp);
	}
	if (snprintf(path, sizeof(path), "%s/JVC.DSK", root_path) >= (int)sizeof(path) ||
	    write_file(path, jvc, 2 + DECB_DSK_BYTES) != 0) {
		free(jvc);
		check(0, "write JVC.DSK");
		return;
	}
	free(jvc);

	put_cmd(block, "M:JVC.DSK");
	check(comm_sdc(0xe0, 0, 0, block, 1) == 0, "M: JVC 2-byte header");
	memset(block, 0, sizeof(block));
	check(comm_sdc(0x80, 0, 0, block, 1) == 0, "LSN 0 skips JVC header");
	check(memcmp(block, "10 PRINT", 8) == 0, "JVC LSN 0 is sector data not header");
	sdc_fdc_reset(&fdc);
	fdc_latch_drive0(&fdc);
	check(fdc_read_sector(&fdc, 0, 1, sec) == 0 && memcmp(sec, "10 PRINT", 8) == 0,
	      "FDC CHS skips JVC header");
	put_cmd(block, "M:");
	(void)comm_sdc(0xe0, 0, 0, block, 1);

	/* Hard-reset style: set_root reapplies startup.cfg */
	sdc_fs_init(&fs2);
	check(sdc_fs_set_root(&fs2, root_path) == 0, "set_root applies startup.cfg");
	check(sdc_fs_fdc_ready(&fs2, 0), "set_root auto-mounts AUTO.DSK");
	sdc_fs_free(&fs2);
}

int main(void) {
	char tmpl[] = "/tmp/cocosdc-fs-XXXXXX";
	char path[PATH_MAX];
	char hello[] = "Hello from sdc-root via m: raw blocks.";
	char *root;
	char foo[300];

	root = mkdtemp(tmpl);
	if (!root) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(root_path, sizeof(root_path), "%s", root);

	snprintf(path, sizeof(path), "%s/HELLO.TXT", root);
	if (write_file(path, hello, sizeof(hello)) != 0) {
		fprintf(stderr, "FAIL: write HELLO.TXT\n");
		rm_path(root);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/GAMES", root);
	if (mkdir(path, 0755) != 0) {
		fprintf(stderr, "FAIL: mkdir GAMES\n");
		rm_path(root);
		return 1;
	}
	memset(foo, 0x5a, sizeof(foo));
	foo[0] = 'F';
	foo[1] = 'O';
	foo[2] = 'O';
	snprintf(path, sizeof(path), "%s/GAMES/FOO.BIN", root);
	if (write_file(path, foo, sizeof(foo)) != 0) {
		fprintf(stderr, "FAIL: write FOO.BIN\n");
		rm_path(root);
		return 1;
	}

	sdc_hw_reset(&hw);
	sdc_fs_init(&fs);
	if (sdc_fs_set_root(&fs, root) != 0) {
		fprintf(stderr, "FAIL: set_root\n");
		sdc_fs_free(&fs);
		rm_path(root);
		return 1;
	}

	test_version_reset();
	test_missing_and_eject();
	test_mount_read_info(hello, sizeof(hello));
	test_case_partial_and_cwd();
	test_create_write_lsn_latch();
	test_slots_inuse();
	test_dir_listing(hello, sizeof(hello));
	test_mkdir_delete();
	test_escape();
	test_stream();
	test_play();
	test_dsk_mount_and_fdc();
	test_startup_cfg_and_jvc();
	test_no_root();

	sdc_fs_free(&fs);
	rm_path(root);

	if (nfail) {
		fprintf(stderr, "%d/%d check(s) failed\n", nfail, ncheck);
		return 1;
	}
	printf("cocosdc_fs: %d checks ok (mount/dir/LSN R/W/stream 512/Play/FDC DSK/startup.cfg)\n", ncheck);
	return 0;
}
