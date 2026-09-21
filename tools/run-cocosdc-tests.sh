#!/bin/sh
# Compile and run CoCoSDC host-side tests (register engine + temp sdc-root + CRC lists).
# No XRoar, SDL, autotools, or CoCo ROMs required.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
cflags="-std=c11 -Wall -Werror"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "==> cocosdc_hw_test (CommSDC wait-loop)"
$cc $cflags -I"$root/src" -o "$tmp/cocosdc_hw_test" \
	"$root/tools/cocosdc_hw_test.c"
"$tmp/cocosdc_hw_test"

echo "==> decb_bin_test (DECB/DragonDOS preamble parse + RAM poke)"
$cc $cflags -I"$root" -I"$root/src" -o "$tmp/decb_bin_test" \
	"$root/tools/decb_bin_test.c" "$root/src/decb_bin.c"
"$tmp/decb_bin_test"

echo "==> cocosdc_fs_test (mount/dir/LSN/stream/Play/FDC/startup.cfg against temp sdc-root)"
$cc $cflags -I"$root" -I"$root/src" -o "$tmp/cocosdc_fs_test" \
	"$root/tools/cocosdc_fs_test.c" "$root/src/cocosdc_fs.c" "$root/src/cocosdc_fdc.c"
"$tmp/cocosdc_fs_test"

echo "==> cocosdc_io_error_test (MCU/FDC flush and sync failure reporting)"
$cc $cflags -I"$root" -I"$root/src" -o "$tmp/cocosdc_io_error_test" \
	"$root/tools/cocosdc_io_error_test.c"
"$tmp/cocosdc_io_error_test"

echo "==> crclist_match_test (NTSC Super ECB 0xb4c88d6c vs @coco3)"
$cc $cflags -DHAVE_REGEX_H -I"$root" -I"$root/src" -I"$root/portalib" -o "$tmp/crclist_match_test" \
	"$root/tools/crclist_match_test.c" \
	"$root/src/crclist.c" "$root/src/crc32.c" \
	"$root/portalib/sds.c" "$root/portalib/sdsx.c" \
	"$root/portalib/slist.c" "$root/portalib/xmalloc.c"
"$tmp/crclist_match_test"

echo "cocosdc host tests: ok"
