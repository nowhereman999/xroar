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

#define SDC_CMDMODE    (0x43)
#define SDC_BLOCK_SIZE (256)

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
	uint8_t block[SDC_BLOCK_SIZE];
	unsigned xfer_index;
	unsigned xfer;  /* enum sdc_xfer */
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
	h->status = 0;
	h->cmd_ready = false;
	h->completed = true;
}

static inline void sdc_hw_fail(struct sdc_hw *h, uint8_t bits) {
	h->xfer = SDC_XFER_NONE;
	h->xfer_index = 0;
	h->status = (uint8_t)(SDC_FAILED | bits);
	h->cmd_ready = false;
	h->completed = true;
}

/* Present h->block as a 256-byte response.  CommSDC waits for READY. */
static inline void sdc_hw_start_rx(struct sdc_hw *h) {
	h->xfer = SDC_XFER_RX;
	h->xfer_index = 0;
	h->status = (uint8_t)(SDC_BUSY | SDC_READY);
	h->cmd_ready = false;
	h->completed = false;
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
		if (h->xfer == SDC_XFER_RX && h->xfer_index < SDC_BLOCK_SIZE) {
			uint8_t v = h->block[h->xfer_index++];
			h->preg[1] = v;
			if (h->xfer_index >= SDC_BLOCK_SIZE) {
				sdc_hw_succeed(h);
			}
			return v;
		}
		return h->preg[1];
	case 0x0b:
		if (h->xfer == SDC_XFER_RX && h->xfer_index < SDC_BLOCK_SIZE) {
			uint8_t v = h->block[h->xfer_index++];
			h->preg[2] = v;
			if (h->xfer_index >= SDC_BLOCK_SIZE) {
				sdc_hw_succeed(h);
			}
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
			 * but keeps param registers (VERSION is read after
			 * CommSDC clears $FF40). */
			h->cmd_mode = false;
			h->xfer = SDC_XFER_NONE;
			h->xfer_index = 0;
			h->cmd_ready = false;
			h->status = 0;
		}
		break;

	case 0x08:
		if (h->cmd_mode) {
			sdc_hw_start_command(h, D);
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

#endif
