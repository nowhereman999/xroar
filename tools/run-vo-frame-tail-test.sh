#!/bin/sh
# Run after configuring/building XRoar (uses portalib/libporta.a).
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -DHAVE_CONFIG_H -Wall -Wextra \
    -I"$root" -I"$root/src" -I"$root/portalib" \
    "$root/tools/vo_frame_tail_test.c" "$root/src/vo_render.c" \
    "$root/src/colourspace.c" "$root/src/filter.c" "$root/src/ntsc.c" \
    "$root/src/messenger.c" "$root/portalib/libporta.a" -lm \
    -o "$tmp/vo_frame_tail_test"
"$tmp/vo_frame_tail_test"
