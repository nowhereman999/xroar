/** \file
 *
 *  \brief CoCoSDC SCS register protocol (CommSDC contract).
 *
 *  Address map from Studio's SDC_Comm.asm:
 *
 *  - $FF40 control latch (write $43 = command mode, $00 = leave)
 *  - $FF48 command (write) / status (read)
 *  - $FF49 param 1
 *  - $FF4A param 2 / data A
 *  - $FF4B param 3 / data B
 *
 *  Status bits: BUSY %00000001, READY %00000010, FAILED %10000000.
 *
 *  Any command with bit 5 set transfers a 256-byte block TO the controller
 *  (READY then 256 writes to $FF4A/$FF4B).  The cart then succeeds, fails,
 *  or (rarely) offers a response block.
 *
 *  Other commands set BUSY and cmd_ready so the cart can succeed with no
 *  payload, fail, or offer a 256-byte response (READY, then 256 reads from
 *  $FF4A/$FF4B) — Get Info / dir page / CWD / read LSN.
 *
 *  Stream ($90/$91, User Guide $9X) is not a CommSDC 256-byte exchange.
 *  After the command, BUSY stays set and READY marks each 512-byte sector
 *  on $FF4A/$FF4B until EOF (BUSY cleared) or abort ($D0).  BIGLOADM /
 *  StreamFile poll READY for the first byte of every sector.
 *
 *  Play (SDC_Play.asm) uses the same stream: OpenSDC_File_X_At_Start
 *  (m: mount + $90/$91, default drive 1), 16-bit LDU/LDX $FF4A, then
 *  interleaved 512-byte loads while the previous buffer is "played".
 *  After the first two READY waits, Play does *not* loop on READY before
 *  the next 512 bytes — the last DATREG read of a sector must already
 *  have presented the next sector (or cleared BUSY at EOF).  BREAK
 *  writes $D0 to $FF48 and does not poll BUSY; CLR $FF40 at exit.
 *
 *  Extra FAILED bits used by SDC_FileAccess.asm:
 *    $04 invalid path, $08 miscellaneous, $10 not found, $20 in use.
 *
 *  This header is intentionally free of XRoar types so the wait-loop
 *  behaviour can be exercised from a tiny host-side test.
 */

#ifndef XROAR_COCOSDC_HW_H_
#define XROAR_COCOSDC_HW_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SDC_BUSY   (0x01)
#define SDC_READY  (0x02)
#define SDC_FAILED (0x80)

#define SDC_ERR_INVALID  (0x04)
#define SDC_ERR_MISC     (0x08)
#define SDC_ERR_NOTFOUND (0x10)
#define SDC_ERR_INUSE    (0x20)

#define SDC_CMDMODE     (0x43)
#define SDC_BLOCK_SIZE  (256)
#define SDC_STREAM_SIZE (512)

/* BCD 1.27 — Studio's CheckSDCFirmwareVersion requires >= $0127. */
#define SDC_FW_VERSION_BCD (0x0127)

enum sdc_xfer {
	SDC_XFER_NONE = 0,
	SDC_XFER_TX = 1,
	SDC_XFER_RX = 2
};

struct sdc_hw {
	bool cmd_mode;
	uint8_t latch;
	uint8_t status;
	uint8_t cmd;
	uint8_t preg[3];
	/* LSN / extra params are sampled when the command is written.
	 * CommSDC then reuses $FF4A/$FF4B as the 256-byte data port. */
	uint8_t latched_preg[3];
	uint8_t block[SDC_STREAM_SIZE];
	unsigned xfer_index;
	unsigned xfer_limit; /* SDC_BLOCK_SIZE or SDC_STREAM_SIZE */
	unsigned xfer;  /* enum sdc_xfer */
	/* Stream ($90/$91): keep BUSY across 512-byte sectors; after each
	 * sector cmd_ready asks the cart to refill or finish (EOF). */
	bool streaming;
	bool stream_8bit; /* $9X bit 1 / $8X bit 2: 8-bit RX via $FF4B only */
	/* Set when a command finishes (BUSY cleared).  The cart logs then
	 * clears this. */
	bool completed;
	/* Params (and optional TX block) are ready for the cart to execute. */
	bool cmd_ready;
};

static inline void sdc_hw_reset(struct sdc_hw *h) {
	memset(h, 0, sizeof(*h));
}

/* Return the low-nibble register index, or -1 if A is not an SDC register. */
static inline int sdc_hw_reg(uint16_t A) {
	if ((A & 0xfff0) != 0xff40) {
		return -1;
	}
	switch (A & 0x0f) {
	case 0x00:
	case 0x08:
	case 0x09:
	case 0x0a:
	case 0x0b:
		return (int)(A & 0x0f);
	default:
		return -1;
	}
}

static inline void sdc_hw_succeed(struct sdc_hw *h) {
	h->xfer = SDC_XFER_NONE;
	h->xfer_index = 0;
	h->xfer_limit = 0;
	h->streaming = false;
	h->status = 0;
	h->cmd_ready = false;
	h->completed = true;
}

static inline void sdc_hw_fail(struct sdc_hw *h, uint8_t bits) {
	h->xfer = SDC_XFER_NONE;
	h->xfer_index = 0;
	h->xfer_limit = 0;
	h->streaming = false;
	h->status = (uint8_t)(SDC_FAILED | bits);
	h->cmd_ready = false;
	h->completed = true;
}

static inline void sdc_hw_start_rx_n(struct sdc_hw *h, unsigned n, int streaming) {
	h->xfer = SDC_XFER_RX;
	h->xfer_index = 0;
	h->xfer_limit = n;
	h->streaming = streaming ? true : false;
	h->status = (uint8_t)(SDC_BUSY | SDC_READY);
	h->cmd_ready = false;
	h->completed = false;
}

/* Present h->block as a 256-byte response.  CommSDC waits for READY. */
static inline void sdc_hw_start_rx(struct sdc_hw *h) {
	sdc_hw_start_rx_n(h, SDC_BLOCK_SIZE, 0);
}

/* 512-byte stream sector.  BUSY stays set after the sector so the cart can
 * refill (READY again) or clear BUSY at EOF. */
static inline void sdc_hw_start_stream_rx(struct sdc_hw *h) {
	sdc_hw_start_rx_n(h, SDC_STREAM_SIZE, 1);
}

static inline uint8_t sdc_hw_take_rx(struct sdc_hw *h) {
	uint8_t v;

	if (h->xfer != SDC_XFER_RX || h->xfer_index >= h->xfer_limit) {
		return 0;
	}
	v = h->block[h->xfer_index++];
	if (h->xfer_index >= h->xfer_limit) {
		if (h->streaming) {
			/* Drop READY between sectors; keep BUSY.  cmd_ready
			 * asks sdc_fs_execute to refill or finish.
			 * Play's interleaved load (after ~166 dummy samples)
			 * then blast-reads $FF4A without a READY wait loop,
			 * so the cart must refill on this last byte — not
			 * wait for a later $FF48 poll. */
			h->xfer = SDC_XFER_NONE;
			h->status = SDC_BUSY;
			h->cmd_ready = true;
			h->completed = false;
		} else {
			sdc_hw_succeed(h);
		}
	}
	return v;
}

/* VERSION and the $1C program-mode handshake have no payload and no
 * filesystem work.  Returns 1 if the command was handled. */
static inline int sdc_hw_take_builtin(struct sdc_hw *h) {
	if (!h->cmd_ready) {
		return 0;
	}
	if ((h->cmd & 0xfe) == 0xc0 && h->preg[0] == 'V') {
		h->preg[1] = (uint8_t)(SDC_FW_VERSION_BCD >> 8);
		h->preg[2] = (uint8_t)(SDC_FW_VERSION_BCD & 0xff);
		sdc_hw_succeed(h);
		return 1;
	}
	if (h->cmd == 0x1c) {
		/* Program-mode handshake used by SDCReset in SDC_FileAccess.asm */
		h->preg[0] = 'P';
		h->preg[1] = 'M';
		sdc_hw_succeed(h);
		return 1;
	}
	return 0;
}

static inline void sdc_hw_tx_done(struct sdc_hw *h) {
	h->xfer = SDC_XFER_NONE;
	h->status = SDC_BUSY;
	h->cmd_ready = true;
}

static inline void sdc_hw_start_command(struct sdc_hw *h, uint8_t cmd) {
	h->cmd = cmd;
	h->completed = false;
	h->cmd_ready = false;
	h->xfer_index = 0;
	h->xfer_limit = 0;
	h->streaming = false;
	h->stream_8bit = false;
	h->latched_preg[0] = h->preg[0];
	h->latched_preg[1] = h->preg[1];
	h->latched_preg[2] = h->preg[2];
	memset(h->block, 0, sizeof(h->block));

	if (cmd & 0x20) {
		/* Bit 5: host will send a 256-byte parameter/data block. */
		h->xfer = SDC_XFER_TX;
		h->status = (uint8_t)(SDC_BUSY | SDC_READY);
	} else {
		/* Keep BUSY until the cart succeeds, fails, or starts RX. */
		h->xfer = SDC_XFER_NONE;
		h->status = SDC_BUSY;
		h->cmd_ready = true;
	}
}

static inline uint8_t sdc_hw_read(struct sdc_hw *h, int reg) {
	switch (reg) {
	case 0x00:
		return h->latch;
	case 0x08:
		return h->status;
	case 0x09:
		return h->preg[0];
	case 0x0a:
		if (h->xfer == SDC_XFER_RX && !h->stream_8bit) {
			uint8_t v = sdc_hw_take_rx(h);
			h->preg[1] = v;
			return v;
		}
		return h->preg[1];
	case 0x0b:
		if (h->xfer == SDC_XFER_RX) {
			uint8_t v = sdc_hw_take_rx(h);
			h->preg[2] = v;
			return v;
		}
		return h->preg[2];
	default:
		return 0;
	}
}

static inline void sdc_hw_write(struct sdc_hw *h, int reg, uint8_t D) {
	switch (reg) {
	case 0x00:
		h->latch = D;
		if (D == SDC_CMDMODE) {
			h->cmd_mode = true;
			if (h->xfer == SDC_XFER_NONE) {
				h->status = 0;
			}
		} else {
			/* Leaving command mode aborts an in-flight transfer
			 * (including stream) but keeps param registers
			 * (VERSION is read after CommSDC clears $FF40). */
			h->cmd_mode = false;
			h->xfer = SDC_XFER_NONE;
			h->xfer_index = 0;
			h->xfer_limit = 0;
			h->streaming = false;
			h->cmd_ready = false;
			h->status = 0;
		}
		break;

	case 0x08:
		if (h->cmd_mode) {
			if (D == 0xd0) {
				/* Abort stream.  Play BREAK writes $D0 and
				 * does not waitForIt; complete in this write
				 * so BUSY is already clear. */
				h->cmd = D;
				sdc_hw_succeed(h);
			} else {
				sdc_hw_start_command(h, D);
			}
		}
		break;

	case 0x09:
		h->preg[0] = D;
		break;

	case 0x0a:
		h->preg[1] = D;
		if (h->xfer == SDC_XFER_TX && h->xfer_index < SDC_BLOCK_SIZE) {
			h->block[h->xfer_index++] = D;
			if (h->xfer_index >= SDC_BLOCK_SIZE) {
				sdc_hw_tx_done(h);
			}
		}
		break;

	case 0x0b:
		h->preg[2] = D;
		if (h->xfer == SDC_XFER_TX && h->xfer_index < SDC_BLOCK_SIZE) {
			h->block[h->xfer_index++] = D;
			if (h->xfer_index >= SDC_BLOCK_SIZE) {
				sdc_hw_tx_done(h);
			}
		}
		break;

	default:
		break;
	}
}

/* Optional Becker / DriveWire / FujiNet port on the CoCoSDC cart.
 *
 * Same partial decode as rsdos.c once A3 is clear: (A&3)==1 is status,
 * (A&3)==2 is data.  Inside the $FF40 page that is $FF41/$FF45 and
 * $FF42/$FF46.  $FF48–$FF4F (A3 set) stay SDC/FDC, so $FF49/$FF4A are
 * never Becker even though their low bits match.
 *
 * $FF42 is also the flash data helper.  When -cart-becker is active,
 * Becker wins and flash data is not read or written.  $FF43 stays the
 * flash bank probe.  With Becker off, $FF41 is unused and $FF42/$FF43
 * stay flash.
 *
 * becker_active is true only when the port actually opened. */

enum {
	COCOSDC_P2_OTHER = 0,
	COCOSDC_P2_SDC = 1,
	COCOSDC_P2_FLASH = 2,
	COCOSDC_P2_BECKER_STATUS = 3,
	COCOSDC_P2_BECKER_DATA = 4
};

static inline int cocosdc_p2_class(uint16_t A, int becker_active) {
	if (sdc_hw_reg(A) >= 0) {
		return COCOSDC_P2_SDC;
	}
	/* $FF40–$FF47 only.  A3 selects the SDC/FDC block, as in rsdos. */
	if (becker_active && (A & 0xfff8) == 0xff40) {
		switch (A & 3) {
		case 1:
			return COCOSDC_P2_BECKER_STATUS;
		case 2:
			return COCOSDC_P2_BECKER_DATA;
		default:
			break;
		}
	}
	if ((A & 0xfff0) == 0xff40) {
		switch (A & 0x0f) {
		case 0x02:
		case 0x03:
			return COCOSDC_P2_FLASH;
		default:
			break;
		}
	}
	return COCOSDC_P2_OTHER;
}

#endif
