# XRoar: stale pixels below the end of an NTSC field

## Source and scope

Tested on Glen's macOS fork, `nowhereman999/xroar`, baseline commit
`483ccb24` (full baseline from `git rev-parse HEAD`), reporting **XRoar 1.13.0.dev**.
The patch changes generic video rendering, not CoCoSDC, CPU/GIME timing or
compiler-generated code. Existing Cocoa menus and Inject support are preserved.

## Reported symptom

On a CoCo 3 with 128K, play `20100810.NTM`, then `KM.NTM` with the compiler's
`SDC_PLAYMOVIE` command, followed by `CLS` and `SCREEN 0,0`. The latter movie
uses GMODE 259 (640 x 113 logical pixels, 225 physical active scanlines).
Glen reports correct output on hardware but a thin strip of old pixels below
XRoar's restored text display. The supplied screenshot is consistent with
unwritten rows at the bottom of the host framebuffer.

## Cause

Video renderers advance `vr->pixel` only for scanlines written inside the
viewport. Action (270-row) and underscan (276-row) viewports can exceed an NTSC
field's 263 scanlines. A prior field, changed viewport or initial allocation
can leave old contents in the remaining rows. `vo_vsync()` previously presented
the entire framebuffer without defining those rows. Resetting `vr->pixel` for
the next frame did not erase them. The defect is in host framebuffer presentation;
changing the emulated GIME field length would be the wrong repair.

## Code changes

- `src/vo_render.h`: add the internal `finish_frame` callback.
- `src/vo_render_tmpl.c`: implement it for both 16-bit and 32-bit renderers.
  Starting at the next unwritten row (`vr->pixel`), fill remaining visible rows
  with mapped RGB black. Respect buffer pitch and leave padding unchanged.
  The pixel-format mapper preserves opaque alpha, unlike a blanket zero fill.
- `src/vo.h`: call this before the draw delegate, and only for a presented frame.
  Clearing after drawing would still expose the damaged frame once. Fully
  populated frames require no pixel writes; skipped frames retain their buffer.
- `tools/vo_frame_tail_test.c` and `tools/run-vo-frame-tail-test.sh`: reproducible
  regression tests requiring a configured/buildable XRoar tree, no ROMs or SDL
  window. The test supplies complete prior contents before a shorter field and
  checks the buffer *inside the draw callback*, plus alpha, pitch and frame skip.

## Validation

- 72 renderer cases: six pixel formats x three viewport heights x two field
  lengths x RGB/composite palette rendering. **0 failures after the fix.**
- The same tests using the original `vo_vsync` implementation fail **24 cases**,
  each beginning at row 263 in the larger NTSC viewports.
- `make -j4` successfully rebuilt the macOS executable.
- `make -j4 check`: **4 passed, 0 failed**.
- Rebuilt executable boots a CoCo 3 with 128K in a headless smoke test.
- The complete interactive two-movie sequence has not been replayed visually;
  validation here isolates the renderer defect without relying on movie timing.

## Applying and testing

From a compatible XRoar source tree:

```sh
git apply --check /path/to/xroar-frame-tail.patch
git apply /path/to/xroar-frame-tail.patch
make -j4
sh tools/run-vo-frame-tail-test.sh
make check
```

The patch contains only this fix and its regression test. It does not include
Glen's other fork-specific changes. The author's tree may require normal context
adjustments if its renderer has diverged.

On Glen's Mac the rebuilt binary is `/Users/glenhewlett/xroar/src/xroar`, Studio's
default XRoar path. Close the old emulator window and launch again from Studio;
an already-running process retains the old executable.

This fix is maintained in Glen's fork at https://github.com/nowhereman999/xroar
on `main`. The patch is also provided here for sharing with the original author;
no upstream submission has been made.
