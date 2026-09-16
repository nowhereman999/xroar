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

	/* LSN is latched at command write; $FF4A/$FF4B then carry the data block. */
	{
		uint8_t fill[SDC_BLOCK_SIZE];
		memset(fill, 0x3c, sizeof(fill));
		if (comm_sdc(&h, 0xa0, 0x00, 0x0102, fill, 1) != 0) {
			nfail += fail("write-LSN latch transfer hung");
		}
		if (h.latched_preg[0] != 0x00 || h.latched_preg[1] != 0x01 ||
		    h.latched_preg[2] != 0x02) {
			nfail += fail("LSN not latched across 256-byte data port");
		}
		if (h.preg[1] == 0x01 && h.preg[2] == 0x02) {
			nfail += fail("data port should overwrite preg 2/3");
		}
	}

	/* 256-byte RX: READY, then not busy after the last DATREG read. */
	{
		uint8_t out[SDC_BLOCK_SIZE];
		memset(h.block, 0xa5, SDC_BLOCK_SIZE);
		h.block[0] = 0x11;
		h.block[255] = 0x22;
		sdc_hw_start_rx(&h);
		if (wait_for_it(&h) != 1) {
			nfail += fail("RX did not set READY");
		}
		for (int i = 0; i < SDC_BLOCK_SIZE; i++) {
			out[i] = sdc_hw_read(&h, (i & 1) ? 0x0b : 0x0a);
		}
		if (out[0] != 0x11 || out[255] != 0x22 || out[1] != 0xa5) {
			nfail += fail("RX payload");
		}
		if (sdc_hw_read(&h, 0x08) & BUSY) {
			nfail += fail("RX left BUSY set");
		}
	}

	/* 512-byte stream sector: READY for 512 DATREG reads, then BUSY
	 * without READY so the cart can refill (BIGLOADM / StreamFile). */
	{
		uint8_t out[SDC_STREAM_SIZE];
		memset(h.block, 0x5a, SDC_STREAM_SIZE);
		h.block[0] = 0x01;
		h.block[1] = 0x02;
		h.block[511] = 0xfe;
		sdc_hw_start_stream_rx(&h);
		if (wait_for_it(&h) != 1) {
			nfail += fail("stream RX did not set READY");
		}
		for (int i = 0; i < SDC_STREAM_SIZE; i++) {
			out[i] = sdc_hw_read(&h, (i & 1) ? 0x0b : 0x0a);
		}
		if (out[0] != 0x01 || out[1] != 0x02 || out[511] != 0xfe ||
		    out[2] != 0x5a) {
			nfail += fail("stream RX payload");
		}
		if (!h.cmd_ready || !h.streaming) {
			nfail += fail("stream sector did not request refill");
		}
		if ((sdc_hw_read(&h, 0x08) & (BUSY | READY)) != BUSY) {
			nfail += fail("stream sector end is BUSY without READY");
		}
		sdc_hw_succeed(&h);
		if (h.streaming || (sdc_hw_read(&h, 0x08) & BUSY)) {
			nfail += fail("stream succeed did not clear BUSY");
		}
	}

	/* Play.asm: LDA $FF48 / ASRA / LBCC eof / BEQ wait.
	 * BUSY only -> wait; BUSY|READY -> proceed; not busy -> EOF. */
	{
		uint8_t st;

		sdc_hw_write(&h, 0x00, SDC_CMDMODE);
		sdc_hw_start_stream_rx(&h);
		st = sdc_hw_read(&h, 0x08);
		if ((st & (BUSY | READY)) != (BUSY | READY)) {
			nfail += fail("Play first poll expected BUSY|READY");
		}
		/* ASRA of $03: carry set (BUSY), result $01 (not Z) -> proceed */
		if (!(st & BUSY) || !(st & READY)) {
			nfail += fail("Play ASRA would not proceed on first sector");
		}
		/* Drain one sector so READY drops (BUSY remains, cmd_ready). */
		for (int i = 0; i < SDC_STREAM_SIZE; i++) {
			(void)sdc_hw_read(&h, (i & 1) ? 0x0b : 0x0a);
		}
		st = sdc_hw_read(&h, 0x08);
		if ((st & (BUSY | READY)) != BUSY) {
			nfail += fail("Play ASRA wait: BUSY without READY between sectors");
		}
		/* $D0 completes in the $FF48 write (Play BREAK does not pump). */
		sdc_hw_write(&h, 0x08, 0xd0);
		if (h.cmd_ready || h.streaming || (sdc_hw_read(&h, 0x08) & BUSY)) {
			nfail += fail("Play $D0 abort left BUSY or cmd_ready");
		}
		if (h.cmd != 0xd0) {
			nfail += fail("Play $D0 did not latch command");
		}
		sdc_hw_write(&h, 0x00, 0);
	}

	/* FAILED is sticky for waitForIt (bmi). */
	sdc_hw_fail(&h, SDC_ERR_NOTFOUND);
	if (wait_for_it(&h) >= 0 || !(sdc_hw_read(&h, 0x08) & FAILED) ||
	    !(sdc_hw_read(&h, 0x08) & SDC_ERR_NOTFOUND)) {
		nfail += fail("FAILED|$10 waitForIt");
	}

	if (nfail) {
		fprintf(stderr, "%d test(s) failed\n", nfail);
		return 1;
	}
		puts("cocosdc_hw: CommSDC probe/VERSION/MOUNT/RESET/latch/RX/stream-sector/Play-abort ok");
	return 0;
}
