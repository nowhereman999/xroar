/* Host-side CommSDC + sdc_fs against a temporary -sdc-root.
 * Compile:
 *   cc -std=c11 -Wall -Werror -I. -Isrc \
 *      -o /tmp/cocosdc_fs_test tools/cocosdc_fs_test.c src/cocosdc_fs.c
 */
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cocosdc_fs.h"
#include "cocosdc_hw.h"

#define BUSY   SDC_BUSY
#define READY  SDC_READY
#define FAILED SDC_FAILED

static struct sdc_fs fs;
static struct sdc_hw hw;

static int wait_for_it(struct sdc_hw *h) {
	uint8_t b = sdc_hw_read(h, 0x08);
	if (b & FAILED) {
		return -1;
	}
	if (!(b & BUSY)) {
		return 0;
	}
	if (b & READY) {
		return 1;
	}
	return -1;
}

static void pump(struct sdc_hw *h) {
	sdc_fs_execute(&fs, h);
}

static int comm_sdc(uint8_t cmd, uint8_t b, uint16_t x, uint8_t *buf, int have_buf) {
	sdc_hw_write(&hw, 0x00, SDC_CMDMODE);
	sdc_hw_write(&hw, 0x09, b);
	sdc_hw_write(&hw, 0x0a, (uint8_t)(x >> 8));
	sdc_hw_write(&hw, 0x0b, (uint8_t)(x & 0xff));
	if (wait_for_it(&hw) < 0) {
		sdc_hw_write(&hw, 0x00, 0);
		return -1;
	}
	sdc_hw_write(&hw, 0x08, cmd);
	pump(&hw);
	if (cmd & 0x20) {
		if (wait_for_it(&hw) != 1) {
			sdc_hw_write(&hw, 0x00, 0);
			return -1;
		}
		for (int i = 0; i < SDC_BLOCK_SIZE; i++) {
			uint8_t v = buf ? buf[i] : 0;
			sdc_hw_write(&hw, (i & 1) ? 0x0b : 0x0a, v);
		}
		pump(&hw);
		if (wait_for_it(&hw) < 0) {
			uint8_t st = sdc_hw_read(&hw, 0x08);
			sdc_hw_write(&hw, 0x00, 0);
			return -st;
		}
	} else {
		int w = wait_for_it(&hw);
		if (w < 0) {
			uint8_t st = sdc_hw_read(&hw, 0x08);
			sdc_hw_write(&hw, 0x00, 0);
			return -st;
		}
		if (w == 1 && have_buf && buf) {
			for (int i = 0; i < SDC_BLOCK_SIZE; i++) {
				buf[i] = sdc_hw_read(&hw, (i & 1) ? 0x0b : 0x0a);
			}
		}
	}
	sdc_hw_write(&hw, 0x00, 0);
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

static int fail(const char *msg) {
	fprintf(stderr, "FAIL: %s\n", msg);
	return 1;
}

static int write_file(const char *path, const char *data, size_t n) {
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

static int rm_tree(const char *path) {
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
	return system(cmd);
}

int main(void) {
	char tmpl[] = "/tmp/cocosdc-fs-XXXXXX";
	char *root;
	char path[512];
	uint8_t block[SDC_BLOCK_SIZE];
	uint8_t block2[SDC_BLOCK_SIZE];
	int nfail = 0;
	char hello[] = "Hello from sdc-root via m: raw blocks.";
	uint32_t info_size;

	root = mkdtemp(tmpl);
	if (!root) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(path, sizeof(path), "%s/HELLO.TXT", root);
	if (write_file(path, hello, sizeof(hello)) != 0) {
		nfail += fail("write HELLO.TXT");
		rm_tree(root);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/GAMES", root);
	if (mkdir(path, 0755) != 0) {
		nfail += fail("mkdir GAMES");
		rm_tree(root);
		return 1;
	}
	snprintf(path, sizeof(path), "%s/GAMES/FOO.BIN", root);
	{
		char foo[300];
		memset(foo, 0x5a, sizeof(foo));
		foo[0] = 'F';
		foo[1] = 'O';
		foo[2] = 'O';
		if (write_file(path, foo, sizeof(foo)) != 0) {
			nfail += fail("write FOO.BIN");
			rm_tree(root);
			return 1;
		}
	}

	sdc_hw_reset(&hw);
	sdc_fs_init(&fs);
	if (sdc_fs_set_root(&fs, root) != 0) {
		nfail += fail("set_root");
		goto done;
	}

	if (comm_sdc(0xc0, 'V', 0, NULL, 0) != 0) {
		nfail += fail("VERSION");
	} else if (sdc_hw_read(&hw, 0x0a) != 0x01 || sdc_hw_read(&hw, 0x0b) != 0x27) {
		nfail += fail("VERSION payload");
	}

	put_cmd(block, "m:MISSING.BIN");
	if (comm_sdc(0xe0, 0, 0, block, 1) == 0) {
		nfail += fail("missing mount should FAILED");
	} else {
		/* status was FAILED|NOTFOUND; command mode already left. */
	}

	put_cmd(block, "m:HELLO.TXT");
	if (comm_sdc(0xe0, 0, 0, block, 1) != 0) {
		nfail += fail("mount m:HELLO.TXT");
		goto done;
	}

	memset(block, 0, sizeof(block));
	if (comm_sdc(0x80, 0, 0, block, 1) != 0) {
		nfail += fail("read LSN 0");
	} else if (memcmp(block, hello, sizeof(hello)) != 0) {
		nfail += fail("read LSN 0 data");
	}

	memset(block, 0, sizeof(block));
	if (comm_sdc(0xc0, 'I', 0, block, 1) != 0) {
		nfail += fail("INFO");
	} else {
		if (memcmp(block, "HELLO   TXT", 11) != 0) {
			nfail += fail("INFO 8.3 name");
		}
		info_size = (uint32_t)block[28] | ((uint32_t)block[29] << 8) |
			    ((uint32_t)block[30] << 16) | ((uint32_t)block[31] << 24);
		if (info_size != sizeof(hello)) {
			fprintf(stderr, "FAIL: INFO size %u expected %zu\n",
				info_size, sizeof(hello));
			nfail++;
		}
	}

	put_cmd(block, "M:");
	if (comm_sdc(0xe0, 0, 0, block, 1) != 0) {
		nfail += fail("eject M:");
	}
	memset(block, 0, sizeof(block));
	if (comm_sdc(0x80, 0, 0, block, 1) == 0) {
		nfail += fail("read after eject should fail");
	}

	put_cmd(block, "m:games/foo.bin");
	if (comm_sdc(0xe0, 0, 0, block, 1) != 0) {
		nfail += fail("mount m:games/foo.bin (case)");
	} else {
		memset(block, 0, sizeof(block));
		if (comm_sdc(0x80, 0, 0, block, 1) != 0 || block[0] != 'F' || block[2] != 'O') {
			nfail += fail("read FOO.BIN LSN 0");
		}
		memset(block, 0xaa, sizeof(block));
		if (comm_sdc(0x80, 0, 1, block, 1) != 0) {
			nfail += fail("read FOO.BIN LSN 1 (partial)");
		} else if (block[0] != 0x5a || block[43] != 0x5a || block[44] != 0) {
			nfail += fail("partial sector pad");
		}
		if (comm_sdc(0x80, 0, 2, block, 1) == 0) {
			nfail += fail("LSN past EOF should fail");
		}
		put_cmd(block, "M:");
		(void)comm_sdc(0xe0, 0, 0, block, 1);
	}

	put_cmd(block, "n:NEWFILE.BIN");
	if (comm_sdc(0xe0, 0, 0, block, 1) != 0) {
		nfail += fail("mount n:NEWFILE.BIN");
	} else {
		memset(block2, 0x3c, sizeof(block2));
		memcpy(block2, "WRITTEN", 7);
		if (comm_sdc(0xa0, 0, 0, block2, 1) != 0) {
			nfail += fail("write LSN 0");
		}
		put_cmd(block, "M:");
		if (comm_sdc(0xe0, 0, 0, block, 1) != 0) {
			nfail += fail("eject after write");
		}
		put_cmd(block, "m:NEWFILE.BIN");
		if (comm_sdc(0xe0, 0, 0, block, 1) != 0) {
			nfail += fail("reopen NEWFILE.BIN");
		} else {
			memset(block, 0, sizeof(block));
			if (comm_sdc(0x80, 0, 0, block, 1) != 0 || memcmp(block, "WRITTEN", 7) != 0) {
				nfail += fail("read back NEWFILE.BIN");
			}
			put_cmd(block, "M:");
			(void)comm_sdc(0xe0, 0, 0, block, 1);
		}
	}

	put_cmd(block, "L:*.*");
	if (comm_sdc(0xe0, 0, 0, block, 1) != 0) {
		nfail += fail("L:*.*");
	} else {
		memset(block, 0, sizeof(block));
		if (comm_sdc(0xc0, '>', 0, block, 1) != 0) {
			nfail += fail("DIR page");
		} else {
			int saw_hello = 0, saw_games = 0, saw_new = 0;
			for (int i = 0; i < 16; i++) {
				uint8_t *r = block + i * 16;
				if (r[0] == 0) {
					break;
				}
				if (memcmp(r, "HELLO   TXT", 11) == 0) {
					saw_hello = 1;
				}
				if (memcmp(r, "GAMES      ", 11) == 0 && (r[11] & SDC_ATTR_DIR)) {
					saw_games = 1;
				}
				if (memcmp(r, "NEWFILE BIN", 11) == 0) {
					saw_new = 1;
				}
			}
			if (!saw_hello || !saw_games || !saw_new) {
				nfail += fail("DIR page contents");
			}
		}
	}

	put_cmd(block, "D:GAMES");
	if (comm_sdc(0xe0, 0, 0, block, 1) != 0) {
		nfail += fail("D:GAMES");
	} else {
		memset(block, 0, sizeof(block));
		if (comm_sdc(0xc0, 'C', 0, block, 1) != 0) {
			nfail += fail("CWD leaf");
		} else if (memcmp(block, "GAMES      ", 11) != 0) {
			nfail += fail("CWD name");
		}
		put_cmd(block, "m:FOO.BIN");
		if (comm_sdc(0xe0, 0, 0, block, 1) != 0) {
			nfail += fail("mount relative to CWD");
		}
		put_cmd(block, "M:");
		(void)comm_sdc(0xe0, 0, 0, block, 1);
	}

	if (comm_sdc(0x1c, 0xaa, 0x5500, NULL, 0) != 0 ||
	    sdc_hw_read(&hw, 0x09) != 'P' || sdc_hw_read(&hw, 0x0a) != 'M') {
		nfail += fail("RESET PM");
	}

done:
	sdc_fs_free(&fs);
	rm_tree(root);
	if (nfail) {
		fprintf(stderr, "%d test(s) failed\n", nfail);
		return 1;
	}
	puts("cocosdc_fs: mount/eject/info/dir/cwd/LSN R/W ok");
	return 0;
}
