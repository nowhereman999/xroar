/* Host test: CRC list assign/match for documented NTSC Super ECB 0xb4c88d6c.
 * Compiles against src/crclist.c + portalib; no SDL or ROMs. */

#include "top-config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdsx.h"

#include "crc32.h"
#include "crclist.h"

static int fails;

static void expect(int cond, const char *msg) {
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		fails++;
	}
}

int main(void) {
	/* Table constant and python zlib.crc32 / strtoul agree (not endian-swapped). */
	expect(strtoul("0xb4c88d6c", NULL, 16) == 0xb4c88d6c, "strtoul 0xb4c88d6c");
	expect(strtoul("b4c88d6c", NULL, 16) == 0xb4c88d6c, "strtoul b4c88d6c");

	/* File-style parse=1 split used by xconfig for crclist coco3=... */
	struct sdsx_list *values = sdsx_split_str("0xb4c88d6c,0xff050d80", "[ \t]*,[ \t]*", 1);
	expect(values && values->len == 2, "split two CRC strings");
	expect(values && strcmp(values->elem[0], "0xb4c88d6c") == 0, "first CRC token unchanged");
	expect(values && strcmp(values->elem[1], "0xff050d80") == 0, "second CRC token unchanged");
	crclist_assign("coco3", values);
	sdsx_list_free(values);

	expect(crclist_match("@coco3", 0xb4c88d6c) == 1, "match NTSC Super ECB");
	expect(crclist_match("@coco3", 0xff050d80) == 1, "match PAL Super ECB");
	expect(crclist_match("@coco3", 0xdeadbeef) == 0, "reject unknown CRC");
	expect(crclist_match("0xb4c88d6c", 0xb4c88d6c) == 1, "bare hex match");

	char listbuf[128];
	crclist_snprintf(listbuf, sizeof(listbuf), "@coco3");
	expect(strstr(listbuf, "0xb4c88d6c") != NULL, "snprintf contains NTSC CRC");

	/* Empty assignment must not wipe the builtin-equivalent list. */
	struct sdsx_list *empty = sdsx_split_str("", ",", 1);
	crclist_assign("coco3", empty);
	sdsx_list_free(empty);
	expect(crclist_match("@coco3", 0xb4c88d6c) == 1, "empty assign keeps NTSC CRC");

	/* Missing list still accepts documented Super ECB CRCs. */
	expect(crclist_match("@coco3_missing_xyz", 0xb4c88d6c) == 0, "unknown list no builtin");
	crclist_shutdown();
	expect(crclist_match("@coco3", 0xb4c88d6c) == 1, "builtin fallback after shutdown");
	expect(crclist_match("@coco3", 0xff050d80) == 1, "builtin fallback PAL");

	/* crc32_block of a 32K slot (same length as coco3.rom) is stable. */
	uint8_t *buf = malloc(32768);
	expect(buf != NULL, "malloc 32K");
	if (buf) {
		memset(buf, 0xa5, 32768);
		uint32_t a = crc32_block(CRC32_RESET, buf, 32768);
		uint32_t b = crc32_block(CRC32_RESET, buf, 32768);
		expect(a == b && a != 0, "crc32_block 32K stable");
		free(buf);
	}

	crclist_shutdown();
	if (fails) {
		fprintf(stderr, "crclist_match_test: %d failure(s)\n", fails);
		return 1;
	}
	printf("crclist_match_test: ok\n");
	return 0;
}
