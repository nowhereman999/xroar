/* Host-side CommSDC wait-loop against src/cocosdc_hw.h.
 * Compile (no XRoar deps):
 *   cc -std=c11 -Wall -Werror -I src -o /tmp/cocosdc_hw_test tools/cocosdc_hw_test.c
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "cocosdc_hw.h"

#define BUSY   SDC_BUSY
#define READY  SDC_READY
#define FAILED SDC_FAILED

/* Mirror of waitForIt in SDC_Comm.asm, without the timeout spin. */
static int wait_for_it(struct sdc_hw *h) {
	uint8_t b = sdc_hw_read(h, 0x08);
	if (b & FAILED) {
		return -1;
	}
	if (!(b & BUSY)) {
		return 0; /* Not Busy: Z set */
	}
	if (b & READY) {
		return 1; /* Ready: C clear, Z clear */
	}
	return -1; /* would time out */
}

/* Cart layer: VERSION/$1C, otherwise succeed (no payload). */
static void pump(struct sdc_hw *h) {
	if (!sdc_hw_take_builtin(h) && h->cmd_ready) {
		sdc_hw_succeed(h);
	}
}

/* Minimal CommSDC: A=cmd, B=param1, X=param2/3, optional 256-byte buffer. */
static int comm_sdc(struct sdc_hw *h, uint8_t cmd, uint8_t b, uint16_t x,
		    uint8_t *buf, int have_buf) {
	sdc_hw_write(h, 0x00, SDC_CMDMODE);
	sdc_hw_write(h, 0x09, b);
	sdc_hw_write(h, 0x0a, (uint8_t)(x >> 8));
	sdc_hw_write(h, 0x0b, (uint8_t)(x & 0xff));
	if (wait_for_it(h) < 0) {
		sdc_hw_write(h, 0x00, 0);
		return -1;
	}
	sdc_hw_write(h, 0x08, cmd);
	pump(h);
	if (cmd & 0x20) {
		if (wait_for_it(h) != 1) {
			sdc_hw_write(h, 0x00, 0);
			return -1;
		}
		for (int i = 0; i < SDC_BLOCK_SIZE; i++) {
			uint8_t v = buf ? buf[i] : 0;
			sdc_hw_write(h, (i & 1) ? 0x0b : 0x0a, v);
		}
		pump(h);
		if (wait_for_it(h) < 0) {
			sdc_hw_write(h, 0x00, 0);
			return -1;
		}
		if (sdc_hw_read(h, 0x08) & BUSY) {
			sdc_hw_write(h, 0x00, 0);
			return -1;
		}
	} else {
		int w = wait_for_it(h);
		if (w < 0) {
			sdc_hw_write(h, 0x00, 0);
			return -1;
		}
		if (w == 1 && have_buf && buf) {
			for (int i = 0; i < SDC_BLOCK_SIZE; i++) {
				buf[i] = sdc_hw_read(h, (i & 1) ? 0x0b : 0x0a);
			}
		}
	}
	sdc_hw_write(h, 0x00, 0);
	return 0;
}

static int fail(const char *msg) {
	fprintf(stderr, "FAIL: %s\n", msg);
	return 1;
}

int main(void) {
	struct sdc_hw h;
	uint8_t block[SDC_BLOCK_SIZE];
	int nfail = 0;

	sdc_hw_reset(&h);

	/* Probe: command mode, not busy, not failed. */
	sdc_hw_write(&h, 0x00, SDC_CMDMODE);
	if (sdc_hw_read(&h, 0x08) & (BUSY | FAILED)) {
		nfail += fail("probe status busy/failed after $43");
	}

	/* VERSION: $C0, 'V' — result in $FF4A/$FF4B after leaving command mode. */
	if (comm_sdc(&h, 0xc0, 'V', 0, NULL, 0) != 0) {
		nfail += fail("VERSION CommSDC hung or failed");
	}
	{
		uint8_t hi = sdc_hw_read(&h, 0x0a);
		uint8_t lo = sdc_hw_read(&h, 0x0b);
		if (hi != 0x01 || lo != 0x27) {
			fprintf(stderr, "FAIL: VERSION %02x%02x, expected 0127\n", hi, lo);
			nfail++;
		}
		if (sdc_hw_read(&h, 0x08) & BUSY) {
			nfail += fail("VERSION left BUSY set");
		}
	}

	/* Mount $E0: bit 5 set, 256-byte name, then not busy. */
	memset(block, 0, sizeof(block));
	memcpy(block, "m:HELLO.TXT", 12);
	if (comm_sdc(&h, 0xe0, 0, 0, block, 1) != 0) {
		nfail += fail("MOUNT CommSDC hung or failed");
	}
	if (sdc_hw_read(&h, 0x08) & BUSY) {
		nfail += fail("MOUNT left BUSY set");
	}
	if (memcmp(h.block, "m:HELLO.TXT", 12) != 0) {
		nfail += fail("MOUNT payload not latched");
	}

	/* Reset $1C: 'PM' in param 1/2. */
	if (comm_sdc(&h, 0x1c, 0xaa, 0x5500, NULL, 0) != 0) {
		nfail += fail("RESET CommSDC hung or failed");
	}
	if (sdc_hw_read(&h, 0x09) != 'P' || sdc_hw_read(&h, 0x0a) != 'M') {
		nfail += fail("RESET did not return PM");
	}

	/* $FF40 is not an SDC data register decoder miss. */
	if (sdc_hw_reg(0xff40) != 0x00 || sdc_hw_reg(0xff48) != 0x08 ||
	    sdc_hw_reg(0xff4b) != 0x0b || sdc_hw_reg(0xff00) != -1) {
		nfail += fail("address decode");
	}

	if (nfail) {
		fprintf(stderr, "%d test(s) failed\n", nfail);
		return 1;
	}
	puts("cocosdc_hw: CommSDC probe/VERSION/MOUNT/RESET ok");
	return 0;
}
