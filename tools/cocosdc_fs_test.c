/* Host-side CommSDC + sdc_fs against a temporary -sdc-root.
 *
 * Mirrors SDC_Comm.asm waitForIt / 256-byte $FF4A/$FF4B transfers and the
 * FileAccess commands Phase B implements (mount, dir/info/CWD, LSN R/W).
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

static void test_escape_and_stream_stub(void) {
	uint8_t block[SDC_BLOCK_SIZE];

	put_cmd(block, "m:../../etc/passwd");
	check(comm_sdc(0xe0, 0, 0, block, 1) != 0, "path escape fails");
	check((last_status & SDC_FAILED) != 0, "path escape FAILED");

	/* Phase B: $90 stream must not hang (Not Busy, no 256-byte payload). */
	check(comm_sdc(0x90, 0, 0, block, 1) == 0, "stream $90 does not hang");
	check(last_wait == 0, "stream $90 is Not Busy (no READY block in Phase B)");
	check(comm_sdc(0xd0, 0, 0, NULL, 0) == 0, "abort $D0 does not hang");
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
	test_escape_and_stream_stub();
	test_no_root();

	sdc_fs_free(&fs);
	rm_path(root);

	if (nfail) {
		fprintf(stderr, "%d/%d check(s) failed\n", nfail, ncheck);
		return 1;
	}
	printf("cocosdc_fs: %d checks ok (mount/dir/LSN R/W/slots/CWD)\n", ncheck);
	return 0;
}
