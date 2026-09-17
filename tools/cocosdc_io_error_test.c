/* Sector-write completion must include the buffered host I/O result.
 * Include the implementation with two portable test-only wrappers; no
 * platform-specific FILE callbacks or production fault hooks are needed.
 */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int test_fflush(FILE *fp);
static int test_fsync(int fd);

#define fflush test_fflush
#define fsync test_fsync
#include "../src/cocosdc_fs.c"
#undef fflush
#undef fsync

enum fault_kind { NO_FAULT, FLUSH_FAILURE, SYNC_FAILURE };
static enum fault_kind fault;
static unsigned flush_calls;
static unsigned sync_calls;
static unsigned checks;
static unsigned failures;

static int test_fflush(FILE *fp) {
	flush_calls++;
	if (fault == FLUSH_FAILURE) {
		errno = ENOSPC;
		return EOF;
	}
	return fflush(fp);
}

static int test_fsync(int fd) {
	sync_calls++;
	if (fault == SYNC_FAILURE) {
		errno = EIO;
		return -1;
	}
	return fsync(fd);
}

static void check(int condition, const char *path, enum fault_kind injected,
		  const char *message) {
	checks++;
	if (!condition) {
		fprintf(stderr, "FAIL: %s fault=%d: %s\n", path, injected, message);
		failures++;
	}
}

static void run_case(int fdc, enum fault_kind injected) {
	const char *path = fdc ? "FDC write" : "MCU $A0 write";
	struct sdc_fs fs = {0};
	struct sdc_hw hw;
	uint8_t before[SDC_BLOCK_SIZE] = {0};
	uint8_t written[SDC_BLOCK_SIZE];
	uint8_t readback[SDC_BLOCK_SIZE];
	FILE *fp = tmpfile();
	if (!fp) {
		perror("tmpfile");
		failures++;
		return;
	}
	if (fwrite(before, 1, sizeof(before), fp) != sizeof(before) || fflush(fp)) {
		perror("initialize test sector");
		fclose(fp);
		failures++;
		return;
	}
	fs.slot[0].fp = fp;
	fs.slot[0].writable = 1;
	fs.slot[0].fdc_ok = 1;
	fs.slot[0].spt = SDC_FLOPPY_SPT;
	fs.slot[0].sides = 1;
	fs.slot[0].fdc_sectors = 1;
	memset(written, 0xA5, sizeof(written));
	fault = injected;
	flush_calls = 0;
	sync_calls = 0;
	if (fdc) {
		int rc = sdc_fs_fdc_write(&fs, 0, 0, 1, 0, written);
		check(rc == (injected == NO_FAULT ? SDC_FDC_OK : SDC_FDC_IO),
		      path, injected, "completion reports the host I/O failure");
	} else {
		sdc_hw_reset(&hw);
		hw.cmd = 0xA0;
		hw.cmd_ready = true;
		hw.status = SDC_BUSY;
		memcpy(hw.block, written, sizeof(written));
		sdc_fs_execute(&fs, &hw);
		check(hw.status == (injected == NO_FAULT ? 0 : SDC_FAILED | SDC_ERR_MISC),
		      path, injected, "completion reports the host I/O failure");
		check(hw.completed && !hw.cmd_ready && hw.xfer == SDC_XFER_NONE,
		      path, injected, "failure terminates the command without pending transfer");
	}
	check(flush_calls == 1, path, injected, "write reaches the flush boundary");
	check(sync_calls == (injected == FLUSH_FAILURE ? 0u : 1u),
	      path, injected, "failed flush stops before sync; other writes reach sync");
	if (injected == NO_FAULT) {
		check(fseek(fp, 0, SEEK_SET) == 0 &&
		      fread(readback, 1, sizeof(readback), fp) == sizeof(readback) &&
		      memcmp(readback, written, sizeof(written)) == 0,
		      path, injected, "successful control persists all 256 bytes");
	}
	fault = NO_FAULT;
	fclose(fp);
}

int main(void) {
	for (int fdc = 0; fdc <= 1; fdc++) {
		run_case(fdc, NO_FAULT);
		run_case(fdc, FLUSH_FAILURE);
		run_case(fdc, SYNC_FAILURE);
	}
	if (failures) {
		fprintf(stderr, "cocosdc_io_error: %u/%u checks failed\n", failures, checks);
		return 1;
	}
	printf("cocosdc_io_error: %u checks ok (MCU/FDC success, fflush and fsync failures)\n", checks);
	return 0;
}
