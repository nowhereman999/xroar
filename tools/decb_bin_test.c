/* Host-side DECB / DragonDOS binary parser tests.  No emulator or ROMs.
 *   cc -std=c11 -Wall -Werror -I. -Isrc -o /tmp/decb_bin_test \
 *      tools/decb_bin_test.c src/decb_bin.c
 */
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "decb_bin.h"

static int ncheck;
static int nfail;

static void check(int ok, const char *msg) {
	ncheck++;
	if (!ok) {
		nfail++;
		fprintf(stderr, "FAIL: %s\n", msg);
	}
}

static int write_file(const char *path, const uint8_t *data, size_t n) {
	FILE *fp = fopen(path, "wb");
	if (!fp) {
		return -1;
	}
	size_t w = n ? fwrite(data, 1, n, fp) : 0;
	int rc = fclose(fp);
	return (rc == 0 && w == n) ? 0 : -1;
}

static uint8_t ram[65536];

static void poke_ram(void *ctx, uint16_t addr, uint8_t data) {
	uint8_t *mem = ctx;
	mem[addr] = data;
}

int main(void) {
	char dir[] = "/tmp/decb_bin_test_XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}

	char path[256];
	char err[128];
	struct decb_bin bin;

	/* Missing file */
	snprintf(path, sizeof(path), "%s/nope.bin", dir);
	check(decb_bin_load_file(path, &bin, err, sizeof err) != 0, "missing file fails");
	check(strstr(err, "No such file") != NULL || strstr(err, "not found") != NULL
	      || strstr(err, "No such") != NULL, "missing file names the error");

	/* Empty file */
	snprintf(path, sizeof(path), "%s/empty.bin", dir);
	check(write_file(path, (const uint8_t *)"", 0) == 0, "write empty");
	check(decb_bin_load_file(path, &bin, err, sizeof err) != 0, "empty file fails");
	check(strstr(err, "empty") != NULL, "empty file message");

	/* Not a binary */
	{
		uint8_t raw[] = { 0x01, 0x02, 0x03 };
		snprintf(path, sizeof(path), "%s/raw.bin", dir);
		check(write_file(path, raw, sizeof raw) == 0, "write raw");
		check(decb_bin_load_file(path, &bin, err, sizeof err) != 0, "raw fails");
		check(strstr(err, "not a DECB") != NULL, "raw file message");
	}

	/* Truncated preamble */
	{
		uint8_t raw[] = { 0x00, 0x00, 0x04, 0x0e };
		snprintf(path, sizeof(path), "%s/trunc_hdr.bin", dir);
		check(write_file(path, raw, sizeof raw) == 0, "write trunc header");
		check(decb_bin_load_file(path, &bin, err, sizeof err) != 0, "trunc header fails");
		check(strstr(err, "truncated preamble") != NULL, "trunc header message");
	}

	/* Short data chunk */
	{
		uint8_t raw[] = { 0x00, 0x00, 0x04, 0x0e, 0x00, 0xaa, 0xbb };
		snprintf(path, sizeof(path), "%s/short.bin", dir);
		check(write_file(path, raw, sizeof raw) == 0, "write short data");
		check(decb_bin_load_file(path, &bin, err, sizeof err) != 0, "short data fails");
		check(strstr(err, "short read") != NULL, "short data message");
	}

	/* Missing postamble */
	{
		uint8_t raw[] = { 0x00, 0x00, 0x02, 0x0e, 0x00, 0xaa, 0xbb };
		snprintf(path, sizeof(path), "%s/nopost.bin", dir);
		check(write_file(path, raw, sizeof raw) == 0, "write no postamble");
		check(decb_bin_load_file(path, &bin, err, sizeof err) != 0, "no postamble fails");
		check(strstr(err, "missing postamble") != NULL, "no postamble message");
	}

	/* Unknown chunk after a valid preamble */
	{
		uint8_t raw[] = {
			0x00, 0x00, 0x01, 0x0e, 0x00, 0xaa,
			0x02
		};
		snprintf(path, sizeof(path), "%s/badchunk.bin", dir);
		check(write_file(path, raw, sizeof raw) == 0, "write bad chunk");
		check(decb_bin_load_file(path, &bin, err, sizeof err) != 0, "bad chunk fails");
		check(strstr(err, "unknown chunk type 0x02") != NULL, "bad chunk message");
	}

	/* Valid multi-block DECB + poke + EXEC */
	{
		uint8_t raw[] = {
			0x00, 0x00, 0x04, 0x0e, 0x00, 0xaa, 0xbb, 0xcc, 0xdd,
			0x00, 0x00, 0x02, 0x26, 0x00, 0x11, 0x22,
			0xff, 0x00, 0x00, 0x0e, 0x00
		};
		snprintf(path, sizeof(path), "%s/ok.bin", dir);
		check(write_file(path, raw, sizeof raw) == 0, "write ok.bin");
		check(decb_bin_load_file(path, &bin, err, sizeof err) == 0, "ok.bin parses");
		check(bin.kind == DECB_BIN_DECB, "ok.bin is DECB");
		check(bin.nblocks == 2, "ok.bin has 2 blocks");
		check(bin.blocks[0].load == 0x0e00 && bin.blocks[0].length == 4, "block0 header");
		check(bin.blocks[1].load == 0x2600 && bin.blocks[1].length == 2, "block1 header");
		check(bin.has_exec && bin.exec == 0x0e00, "EXEC $0E00");
		memset(ram, 0, sizeof ram);
		decb_bin_poke(&bin, poke_ram, ram);
		check(ram[0x0e00] == 0xaa && ram[0x0e03] == 0xdd, "poked block 0");
		check(ram[0x2600] == 0x11 && ram[0x2601] == 0x22, "poked block 1");
		decb_bin_free(&bin);
		check(bin.nblocks == 0 && bin.blocks == NULL, "free clears");
	}

	/* Address wrap */
	{
		uint8_t raw[] = {
			0x00, 0x00, 0x03, 0xff, 0xfe, 0x01, 0x02, 0x03,
			0xff, 0x00, 0x00, 0xff, 0xfe
		};
		snprintf(path, sizeof(path), "%s/wrap.bin", dir);
		check(write_file(path, raw, sizeof raw) == 0, "write wrap.bin");
		check(decb_bin_load_file(path, &bin, err, sizeof err) == 0, "wrap.bin parses");
		memset(ram, 0, sizeof ram);
		decb_bin_poke(&bin, poke_ram, ram);
		check(ram[0xfffe] == 0x01 && ram[0xffff] == 0x02 && ram[0x0000] == 0x03,
		      "wraps 16-bit CPU address");
		decb_bin_free(&bin);
	}

	/* DragonDOS */
	{
		uint8_t raw[] = {
			0x55, 0x02,
			0x20, 0x00,       /* load */
			0x00, 0x03,       /* length */
			0x20, 0x00,       /* exec */
			0xaa,
			0x10, 0x20, 0x30
		};
		snprintf(path, sizeof(path), "%s/ddos.bin", dir);
		check(write_file(path, raw, sizeof raw) == 0, "write ddos.bin");
		check(decb_bin_load_file(path, &bin, err, sizeof err) == 0, "ddos.bin parses");
		check(bin.kind == DECB_BIN_DRAGONDOS, "ddos kind");
		check(bin.nblocks == 1 && bin.blocks[0].load == 0x2000, "ddos load");
		check(bin.has_exec && bin.exec == 0x2000, "ddos exec");
		memset(ram, 0, sizeof ram);
		decb_bin_poke(&bin, poke_ram, ram);
		check(ram[0x2000] == 0x10 && ram[0x2002] == 0x30, "ddos poked");
		decb_bin_free(&bin);
	}

	/* unlink temps; ignore errors */
	{
		const char *names[] = {
			"empty.bin", "raw.bin", "trunc_hdr.bin", "short.bin",
			"nopost.bin", "badchunk.bin", "ok.bin", "wrap.bin", "ddos.bin",
			NULL
		};
		for (int i = 0; names[i]; i++) {
			snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
			unlink(path);
		}
		rmdir(dir);
	}

	printf("decb_bin: %d checks ok\n", ncheck - nfail);
	if (nfail) {
		fprintf(stderr, "decb_bin: %d failed of %d\n", nfail, ncheck);
		return 1;
	}
	return 0;
}
