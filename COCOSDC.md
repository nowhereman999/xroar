# CoCoSDC in this XRoar fork (Phase D)

This fork adds a Dragon/CoCo cartridge type `cocosdc` so Studio’s CoCoSDC
client libraries can talk to a host folder on a Mac (or any Unix host)
without a physical CoCoSDC.

It is **not** a VCC `SDC.dll` port.  The register contract is the one used by
Studio’s `CommSDC` (`SDC_Comm.asm`): `$FF40` control latch, `$FF48`
command/status, `$FF49–$FF4B` parameter/data path.  Mount, directory, and
buffered 256-byte R/W follow `SDC_FileAccess.asm`.  Stream / BIGLOADM /
Play follow `SDC_StreamFile_Library.asm`, `SDC_BigLoadm.asm`, and
`SDC_Play.asm` (512-byte `$90`/`$91` sectors, abort `$D0`).  Firmware
opcodes and record layouts match Darren Atkinson’s CoCo SDC User Guide
where the client uses them.

Phase A was register plumbing only (VERSION, reset handshake, bit-5 command
*ack*).  Phase B maps FileAccess commands onto `-sdc-root`.  Phase C adds
the 512-byte stream path those loaders use.  Phase D is Play’s
open/stream/abort contract against that stream (not DAC audio, not Studio
Run).

## Enable the cartridge

Built-in profile (same name as the type):

```text
xroar -machine coco3 -cart cocosdc -sdc-root /path/to/sdcard
```

Equivalent using a cart-type option (useful in `xroar.conf`):

```text
cart cocosdc
  cart-type cocosdc
  cart-opt sdc-root=/path/to/sdcard
```

Or on the command line:

```text
xroar -machine coco3 -cart cocosdc -cart-opt sdc-root=/path/to/sdcard
```

`-sdc-root` may also appear in `~/Library/XRoar/xroar.conf` (macOS) or
`~/.xroar/xroar.conf` (Linux).  A leading `~/` is expanded.

MPI: insert the profile into a slot as with any other cart
(`-cart mpi -mpi-load-cart cocosdc`).  SCS (`$FF40–$FF5F`) follows the MPI
P2 routing like RS-DOS.

An optional `-cart-rom` can still be attached (SDC-DOS flash image).  It is
not required; load your CoCo program some other way (`-run`, cassette,
floppy, etc.).

Confirm the type is registered:

```text
xroar -cart-type help
xroar -cart help
```

You should see `cocosdc`.

## Example `-sdc-root` layout

Treat the directory as the SD volume root.  Names are matched
case-insensitively (8.3-style).  Relative paths use the SDC current
directory (`D:` / `'C'`).  A leading `/` is absolute from the volume root.

```text
~/sdc-root/
  HELLO.TXT          raw file for m: / n: (LSN N = byte offset N*256)
  NEWFILE.BIN
  GAMES/
    FOO.BIN
```

```text
mkdir -p ~/sdc-root/GAMES
printf 'hello from sdc\n' > ~/sdc-root/HELLO.TXT
src/xroar -machine coco3 -cart cocosdc -sdc-root ~/sdc-root -v 2
```

`-v 2` logs VERSION, mount, dir, LSN, and stream commands.

## What Phase D implements

Hardware, as used by `CommSDC`:

| Address | Role |
| --- | --- |
| `$FF40` write | `$43` enters command mode; `$00` leaves it (params are kept) |
| `$FF48` read | Status: `BUSY` bit 0, `READY` bit 1, `FAILED` bit 7 |
| `$FF48` write | Command (command mode only) |
| `$FF49` | Parameter 1 (LSN high / subcommand).  Latched when the command is written — `$FF4A/$FF4B` are then the 256-byte data port. |
| `$FF4A` | Parameter 2 / data A |
| `$FF4B` | Parameter 3 / data B |

`FAILED` extra bits (as tested by `SDCError` in `SDC_FileAccess.asm`):
`$04` invalid path, `$08` miscellaneous, `$10` not found, `$20` in use.

Command-mode behaviour:

- After `$43`, status is not busy so `waitForIt` / `POLLBUSY` return.
- Commands with **bit 5 set** (`$E0/$E1`, `$A0/$A1`, …) set `BUSY|READY`,
  accept 256 bytes on `$FF4A/$FF4B`, then succeed (not busy) or `FAILED`.
- Commands that return a block (`$C0` `'I'`/`'>'`/`'C'`, `$80/$81`) set
  `BUSY|READY` so CommSDC reads 256 bytes; then not busy.
- Stream `$90`/`$91` (bit 5 **clear**) is **not** a CommSDC 256-byte
  exchange.  `BUSY` stays set and `READY` marks each **512-byte** sector
  on `$FF4A`/`$FF4B` until EOF (`BUSY` cleared) or abort (`$D0` or
  `$FF40=0`).  BIGLOADM / StreamFile poll `READY` for the first byte of
  every sector (`ASRA` / `POLLREADY`).
- Play (`SDC_Play.asm`) uses the same stream via `OpenSDC_File_X_At_Start`
  (stays in command mode between `$E1`/`$E0` mount and `$91`/`$90`;
  default drive 1).  After the first two READY waits it blast-reads the
  next 512 bytes with 16-bit `LDU`/`LDX $FF4A` while the previous buffer
  is clocked to `$FF20` — it does **not** loop on READY before those
  interleaved loads.  The last DATREG read of a sector must already have
  presented the next sector (or cleared BUSY at EOF).  BREAK writes `$D0`
  to `$FF48` and does not poll BUSY; exit does `CLR $FF40`.
- `$D0` completes in the `$FF48` write (BUSY already clear).  Other
  commands without a payload clear `BUSY` when they finish (VERSION,
  `$1C`, …).

### CommSDC ops

| Client use | Command | Phase D |
| --- | --- | --- |
| Enter/leave command mode, poll status | `$FF40` / `$FF48` | **Implemented** |
| `CheckSDCFirmwareVersion` (`$C0`, `'V'`) | `$C0` + P1=`$56` | **Implemented** — BCD **1.27** (`$0127`) in `$FF4A/$FF4B` |
| `SDCReset` program-mode handshake | `$1C` | **Implemented** — `'P'/'M'` in `$FF49/$FF4A` |
| Mount raw file / eject | `$E0/$E1` `m:` / `M:` | **Implemented** — 256-byte `"m:path"` / `"M:"`; missing path → `FAILED\|$10` |
| Create+mount raw file | `$E0/$E1` `n:` / `N:` | **Implemented** — creates if missing; `N:` with B=X=0 pre-sizes a 630-sector DSK |
| Write logical block | `$A0/$A1` | **Implemented** — 256 bytes at LSN×256 in the mounted file |
| Read logical block | `$80/$81` | **Implemented** — 256-byte payload; last partial sector is zero-padded |
| Get info for mounted file | `$C0/$C1` + `'I'` | **Implemented** — 32-byte directory record (size **LSB first** at 28–31) |
| Directory page | `$C0` + `'>'` | **Implemented** — 16×16-byte records (size **MSB first** at 12–15).  First `L:pattern` (`$E0`) |
| Current directory | `$C0` + `'C'` | **Implemented** — leaf 8.3 name.  Volume root sets bits 4+7 (`FAILED\|$10`) as in the User Guide |
| Set CWD / mkdir / delete | `$E0` `D:` / `K:` / `X:` | **Implemented** (used by FileAccess; leaf only for `K:`) |
| Stream (`OpenSDC_File_X` / BIGLOADM / Play) | `$90/$91` | **Implemented** — 512-byte sectors from LSN×512; `BUSY\|READY` per sector; last short sector zero-padded; EOF clears `BUSY`; next sector presented on the last DATREG read (Play interleaved load) |
| Abort stream | `$D0` or `$FF40=0` | **Implemented** — `$D0` is Not Busy in the `$FF48` write (Play BREAK); `CLR $FF40` also aborts |
| Play DAC / analog mux (`$FF20`) | | **Not in host tests** — register/stream contract only; 44750 Hz playback needs a live emulator |
| CSM media-player menu / extra opcodes | | **Not used by Studio Play/FileAccess** (`.CSM` is a file format that also streams with `$90`) |
| Floppy-emulation mode (non-`$43` latch) | | **Not emulated** — Play/FileAccess only write `$00` to leave command mode; latch is stored, `$FF48` reads 0 |

Files mounted with `m:` / `n:` are a raw array of 256-byte blocks (the
FileAccess model).  `M:` / `N:` mount the same way for LSN access (no JVC /
VDK / SDF header parse in this phase).  Two slots (`$E0` / `$E1`); the same
host file cannot be mounted in both (`FAILED\|$20`).

A missing `-sdc-root`, or a path that does not exist, fails the command
cleanly — CommSDC does not hang.

Stream is a separate transfer from CommSDC’s 256-byte `$FF4A/$FF4B` loop.
`OpenSDC_File_X_At_Start` mounts with `$E0/$E1`, writes a 24-bit LSN, then
issues `$90/$91` and polls `READY` — **without** leaving command mode
between mount and stream (unlike CommSDC, which clears `$FF40` after every
call).  Each sector is 512 bytes (the SD card native size).  `BUSY` remains
set until the last sector has been read or `$D0` / `$FF40=0` aborts.
Unmounted / LSN-past-EOF `$90` sets `FAILED` (waitForIt / `bmi`);
StreamFile’s `POLLREADY` assumes the mount succeeded.

`SDC_LoadmSavem.asm` uses FileAccess byte I/O (`$80`/`$A0`), not stream —
that path was Phase B.

## Phase D vs later (Studio Run / Mac Play smoke)

**Phase D is done** when the host tests below pass.  That includes Phases
A–C plus Play’s open/stream/abort against a mounted file under a temp
`sdc-root` (16-bit `$FF4A` words, interleaved 512-byte loads, `$D0` BREAK
without a BUSY poll, `CLR $FF40`).  Verifiable on Linux CI without a Mac,
a CoCo, or an emulator binary.

Full 44750 Hz Play audio cannot be proven here: `SDC_Play.asm` clocks
samples to the CoCo DAC (`$FF20`) with cycle-counted delays.  Host tests
check the register/stream contract only.

| In Phase D (this branch) | Still not done |
| --- | --- |
| `$FF40`/`$FF48–$FF4B` CommSDC wait-loop | Floppy-emulation latch (WD-style non-`$43` `$FF40`) — **not required** by Studio Play/FileAccess (they only `CLR $FF40` to leave command mode) |
| VERSION `$C0` `'V'` (BCD 1.27), `$1C` `PM` | Studio **Run** media integration (wire CoCo BASIC Studio to XRoar) |
| Mount/eject `$E0/$E1` `m:`/`n:`/`M:` against `-sdc-root` | JVC/VDK/SDF header parse; FDC floppy image geometry |
| LSN `$80/$81` `$A0/$A1` (256 bytes at LSN×256) | Mount-next / disk-set (`+` / `#`) |
| Info / dir page / CWD (`'I'` / `'>'` / `'C'`, plus `L:`/`D:`/`K:`/`X:`) | Remaining User Guide extras not used by the Studio libraries |
| `$90/$91` 512-byte **stream** from LSN×512; Play interleaved refill; `$D0` abort-on-write | Audible Play through the emulator sound path (Mac smoke with `SDC_Play.asm`) |
| | Zippster `.CSM` media-player menu (not a Studio library opcode) |

`$9X` bit 1 (8-bit transfers via `$FF4B` only, `$92`/`$93`) is decoded.
Studio BIGLOADM / StreamFile / Play use 16-bit `$90`/`$91` (`LDD` /
`LDU`/`LDX $FF4A`).

An upstream PR to Ciaran is intentionally not part of this work.

## Linux / CI (primary verification)

No SDL, autotools, ROMs, or CoCo required:

```text
./tools/run-cocosdc-tests.sh
```

That compiles `tools/cocosdc_hw_test.c` (wait-loop, LSN latch, 256-byte RX,
512-byte stream-sector READY/BUSY, Play `$D0` abort-on-write) and
`tools/cocosdc_fs_test.c` + `src/cocosdc_fs.c` against a `mkdtemp` sdc-root
(mount, missing-path `FAILED|$10`, both slots, in-use, dir pages, CWD,
sequential LSN write/read, mkdir/delete, `$90/$91` multi-sector stream,
`$D0` abort, Play `OpenSDC_File_X` + interleaved 512-byte words).

After `./configure`, the same programs are `make -C src check` (`TESTS`).
GitHub Actions workflow `.github/workflows/cocosdc-host.yml` runs the
script on push.

## macOS build

Linux CI cannot produce a Mac `.app`.  The tree is still autotools, same as
upstream XRoar.

**A pure white/blank window is GTK+ 3 on Mac.**  GTK is untested on macOS
(upstream README).  Do not use `-ui gtk3`.  There is **no** `-ui sdl2`
module: that name prints `UI module sdl2 not found: trying gtk3` and
stays white.  Working video is the **SDL3 UI**, module name `sdl`.

**`CRC32 INVALID` is not a bad `coco3.rom` when the file is 32768 bytes
and `zlib.crc32` is `0xb4c88d6c`.**  That value is the documented NTSC
Super ECB CRC.  Compare does not endian-swap it, and it uses the same
CRC-32 as Python `zlib`.  INVALID also does **not** stop BASIC from
running (tape can play on a black screen).  CoCo 3 black with SDL3 is a
video path issue, not a rejected ROM.

### A/B: Homebrew 1.12.1 vs this fork (Glen)

Confirmed with the **same** `~/Library/XRoar/roms/coco3.rom` (`0xb4c88d6c`):

| Binary | UI | CRC log | Picture |
| --- | --- | --- | --- |
| `/opt/homebrew/bin/xroar` **1.12.1** | `macosx` + **SDL2** | `CRC32 0xb4c88d6c` **valid**, Disk Extended Color BASIC OK prompt | Works |
| `~/xroar/src/xroar` this tip (before vo fix) | `sdl` **SDL3**/Metal | `CRC32 INVALID` | **Black** |

Stock also mounts `disk11.rom` / `rsdos` by default (DECB banner).  That
cart is **not** required for video: 1.12.1 still showed BASIC with the
DECB banner, so a missing DOS ROM does not explain a black framebuffer.
GIME in this fork is not patched vs `main`; the regression is the SDL3
vo (Metal default blend + packed formats with an alpha channel).  1.12.1
uses Cocoa menus on top of **SDL2** `vo_sdl2`, which does not have that
bug.

This tip: `@coco3` plus the NTSC value preloaded by `coco3.c` both
accept `0xb4c88d6c` (same as 1.12.1); SDL3 uses opaque **XRGB** textures,
`BLENDMODE_NONE`, and on Darwin prefers an **OpenGL** renderer unless
`SDL_RENDER_DRIVER` is set.

### Homebrew + configure (SDL3 UI)

```text
xcode-select --install
brew install autoconf automake pkg-config sdl3 libpng
```

Texinfo is **not** needed for the emulator.  Do not install `gtk+3` for
this (if Homebrew already has it, this fork skips GTK on Darwin unless
you pass `--with-gtk3`).

```text
./autogen.sh
./configure --without-gtk3
make -C src
```

`--without-gtk3` is the flag that avoids the white window.  SDL3 is
found via `pkg-config sdl3`.  That sets `HAVE_SDL3` and **`WANT_UI_SDL`**
in `config.h`, which compiles the `sdl` UI.

Confirm:

```text
grep -E 'HAVE_SDL3|WANT_UI_SDL|HAVE_GTK3' config.h
src/xroar -ui help
```

Expect `#define HAVE_SDL3 1`, `#define WANT_UI_SDL 1`, no `HAVE_GTK3`,
and `-ui help`:

```text
	sdl        SDL3 UI
	null       No UI
```

Never pass `-ui sdl2`.  Use `-ui sdl` (the default on Mac after this
fork skips GTK, but still the name to use).

```text
mkdir -p ~/sdc-root
printf 'hello from sdc\n' > ~/sdc-root/HELLO.TXT
src/xroar -machine coco3 -cart cocosdc -sdc-root ~/sdc-root -ui sdl -v 2
```

At `-v 2` the UI module line is:

```text
[module:sdl/ui] SDL3 UI
```

If you see `[module:gtk3/ui] GTK+ 3 UI` instead, the window will be
white — rebuild with `--without-gtk3` or pass `-ui sdl` on an SDL3
build that still listed `sdl` in `-ui help`.

### White vs black window

| Window | Log | Cause | Fix |
| --- | --- | --- | --- |
| **White** / blank | `[module:gtk3/ui] GTK+ 3 UI` (or `UI module sdl2 not found: trying gtk3`) | GTK+ 3 UI on Mac | Use the **SDL3 UI**: `-ui sdl` after an SDL3 `--without-gtk3` build. **Never `-ui sdl2`.** |
| **Black**, SDL3 UI is up, ROM loaded, tape plays | `CRC32 INVALID` (even for `0xb4c88d6c`) | SDL3 Metal vo (blend/scale-on-NULL), **not** a bad NTSC dump. INVALID is a list/conf diagnostic. | Rebuild this tip.  A/B with brew `xroar` (Cocoa) and `SDL_RENDER_DRIVER=opengl`.  `-ram 2048` is not the cause. |
| **CoCo 2 VDG garbage** on SDL3 | `Colour BASIC` / `Extended Colour BASIC CRC32 INVALID` | Same SDL3 vo (wrong colours / alpha) plus possible list wipe; CoCo 2 ROMs are separate from `coco3.rom`. | Same A/B.  Need headerless `bas13.rom` + `extbas11.rom` for a real prompt. |

Once `[module:sdl/ui] SDL3 UI` is in the log, a black screen is **not GTK3**.
Accepted Super ECB CRCs (`-crclist-print`, list `coco3`):

| Dump | Filename | Size | CRC32 |
| --- | --- | --- | --- |
| NTSC Super Extended Colour BASIC | `coco3.rom` | 32768 | **`0xb4c88d6c`** |
| PAL Super Extended Colour BASIC | `coco3p.rom` | 32768 | `0xff050d80` (`-machine coco3p`) |

Python `zlib.crc32` of a 32768-byte `coco3.rom` matching `0xb4c88d6c` is
the right file.  XRoar hashes with the same CRC-32 (`crc32_block` /
zlib).  The `@coco3` table stores `0xb4c88d6c` / `0xff050d80` as hex
strings (`strtoul` base 16, `0x` prefix OK — not a swapped constant).
**CRC verify does not gate fetch/execute or GIME output** (`has_secb` is
only assigned).  So INVALID + black with a confirmed `0xb4c88d6c` dump
means the compare list failed *and* the SDL3 framebuffer path failed;
it does **not** mean the ROM was rejected.

If Slot 0 already prints `CRC32 0xb4c88d6c` and you still see INVALID,
the `@coco3` **list** was empty or overwritten (often
`~/Library/XRoar/xroar.conf` from brew Cocoa autosave writing
`crclist coco3=`).  This tip ignores empty `crclist` assigns, falls back
to the documented Super ECB CRCs, and logs `got 0x…, list @coco3=…`.
Try `-no-c` to skip user conf.

**Why an SDL3 Mac build can miss `~/Library/XRoar/roms/coco3.rom`.**  Mac
`ROMPATH` used to be gated on the Cocoa UI (`UI_COCOA`).  SDL3 disables
SDL2, so Cocoa is off and the binary searched the Unix path only:

```text
~/.xroar/roms:<prefix>/share/xroar/roms:<cwd>
```

A good dump in `~/Library/XRoar/roms/` is then never opened.  Slot 0 is
`(unpopulated)` → `CRC32 INVALID (no image loaded)`.  Or a *different*
`coco3.rom` from `~/.xroar/roms/` or the current directory is opened
(`[rom] opened:` at `-v 2` shows the full path).  This tip prepends
`~/Library/XRoar/roms` on Darwin even without Cocoa.

At `-v 2` you want:

```text
[xroar] rompath: ~/Library/XRoar/roms:~/.xroar/roms:...
[rom] opened: /Users/…/Library/XRoar/roms/coco3.rom (32768 bytes)
[coco3:rom] Super Extended Colour BASIC (1 x 32K)
	Slot   0: CRC32 0xb4c88d6c FILE …/coco3.rom
	Super Extended Colour BASIC CRC32 valid
```

If you still see `INVALID`, read the rest of that line:

| Log | Meaning |
| --- | --- |
| `Slot 0: (unpopulated)` + `CRC32 INVALID (no image loaded)` + `BASIC ROM not found (romlist @coco3, rompath …)` | XRoar never found a file.  Copy or pass `-rompath`. |
| `CRC32 INVALID (got 0xb4c88d6c, list @coco3=…)` or `(empty)` / `(not defined)` | File **is** NTSC Super ECB.  List failed (conf).  Video can still be black — that is SDL3 vo, not this CRC. |
| `CRC32 INVALID (got 0x……, list @coco3=0xb4c88d6c,0xff050d80)` with a *different* got | Loaded a different image.  Check `[rom] opened:`. |

Until you rebuild this tip, force Library roms **and** skip a Cocoa
autosave conf that may have empty `crclist` lines:

```text
src/xroar -no-c -rompath ~/Library/XRoar/roms -ui sdl -machine coco3 -v 2
```

**coco2b sanity check** (does not use `coco3.rom`):

```text
ls -l ~/Library/XRoar/roms/bas13.rom ~/Library/XRoar/roms/extbas11.rom
src/xroar -no-c -rompath ~/Library/XRoar/roms -ui sdl -machine coco2b -v 2
```

Need headerless `bas13.rom` (8192, Colour BASIC 1.3) and `extbas11.rom`
(8192, Extended Colour BASIC 1.1).  VDG garbage on SDL3 with a good
pair is the same Metal/blend vo bug (CoCo 2 pixels show; CoCo 3 RGB
often looks fully black).  NTSC sibling: `-machine coco2bus`.

### A/B: stock XRoar vs this tip (CoCo 3 + SDL)

cocosdc does **not** patch GIME or `vo_sdl3` vs this repo’s `main`.
Black CoCo 3 is the naive SDL3 vo (Metal default blend, scale-mode on
a NULL texture), not a ROM dump and not a CoCoSDC regression.

1. **This tip, skip user conf** (CRC lists + rompath defaults only):

   ```text
   src/xroar -no-c -ui sdl -machine coco3 -v 2
   ```

   Want `[sdl/vo] renderer …`, Slot 0 `CRC32 0xb4c88d6c`, and
   `CRC32 valid`.  `renderer metal` is the Mac default.

2. **Stock Homebrew 1.12.1** (SDL2 + Cocoa, no cocosdc, no SDL3 Metal):

   ```text
   brew install xroar
   xroar -machine coco3
   ```

   A CoCo 3 picture here means Glen’s ROM is fine and only this tree’s
   `-ui sdl` vo is wrong.

3. **This tip, OpenGL instead of Metal:**

   ```text
   SDL_RENDER_DRIVER=opengl src/xroar -no-c -ui sdl -machine coco3 -v 2
   ```

   Log should show `[sdl/vo] renderer opengl`.

4. **Upstream 1.13 SDL3 tag without cocosdc** uses the same `vo_sdl3.c`
   as this tip’s parent `main`.  Prefer brew 1.12.1 as the working A/B.

The emulator binary is `src/xroar`.  Optional: `sudo make install`
(default prefix `/usr/local`).

A top-level `make` also tries to build `doc/xroar.info`.  macOS does
not ship `makeinfo`, so that can fail with `makeinfo: command not
found` / Error 127.  That does **not** mean cocosdc failed.  Autotools
builds `src` before `doc`, so `src/xroar` is already linked; a later
`make -C src` then reports `Nothing to be done`.  Confirm with
`-cart-type help` (`[part:cocosdc]`) and `-h` (`-sdc-root`).

Optional — only if you want a full top-level `make` to also build the
info manual: `brew install texinfo` and put Homebrew’s keg-only
binary on PATH (`/opt/homebrew/opt/texinfo/bin` or
`/usr/local/opt/texinfo/bin`).

ROM images go in `~/Library/XRoar/roms/` (see `README`, “Getting started
under Mac OS X+”).  CoCo 3 needs `coco3.rom` as above; CoCo 2B needs
`bas13.rom` + `extbas11.rom`.  A white window is still GTK3 (`-ui sdl`).
Black + `CRC32 INVALID` on SDL3 with a confirmed `0xb4c88d6c` dump is
the Metal vo (and maybe an empty crclist in `xroar.conf`), not a bad
NTSC file.  Use `-no-c` and the A/B section above.

Host-side tests (same as CI; no emulator):

```text
./tools/run-cocosdc-tests.sh
```

### Configure flags for `WANT_UI_SDL` / `sdl` without GTK3

`WANT_UI_SDL` is **not** the same as `HAVE_SDL2`.  `HAVE_SDL2 1` with
`WANT_UI_SDL` undefined is expected on Mac when Cocoa is found: the
basic `sdl` UI is turned off in favour of `-ui macosx`.

| Flag | Effect |
| --- | --- |
| `--without-gtk3` | Do not probe GTK+ 3 (no white window).  **Default on Darwin** in this fork. |
| *(SDL3 found)* | Sets `HAVE_SDL3` and `WANT_UI_SDL` automatically.  UI name is `sdl` (“SDL3 UI”). |
| `--enable-ui-sdl` | Force the basic `sdl` UI even if Cocoa would disable it.  **Not needed for SDL3.**  Needed for SDL2 if you want `-ui sdl` as well as/instead of Cocoa. |
| `--without-cocoa` | Do not build `-ui macosx` (SDL2 Mac menus). |
| `--with-sdl2` | Prefer SDL2 over SDL3.  Does **not** create `-ui sdl2`.  On Mac this usually builds Cocoa and leaves `WANT_UI_SDL` undefined. |
| `--without-sdl3` | Skip SDL3 so an SDL2/Cocoa build can proceed if both are installed. |

`./configure --help` lists the rest.  This fork does not add a CMake path.

### SDL2 + Cocoa (menus; not the SDL3 path)

If you only have SDL 2:

```text
brew install autoconf automake pkg-config sdl2 libpng
./configure --without-gtk3 --without-sdl3
```

`-ui help` lists `macosx` (Mac OS X+ SDL2 UI), not `sdl`, unless you
also pass `--enable-ui-sdl`.  Video works with `-ui macosx`.  `config.h`
will have `HAVE_SDL2` and typically `HAVE_COCOA`; `WANT_UI_SDL` stays
undefined.  That is expected.  Still never `-ui sdl2`.

### Optional Mac / CoCo smoke (not required for Phase D)

Phase D acceptance is the Linux host tests.  When a Mac and CoCo program
are available later:

1. `mkdir -p ~/sdc-root` and put a small file there, e.g. `HELLO.TXT`.
2. Rebuild with `make -C src` (Texinfo / `makeinfo` not required).
3. Run `src/xroar -machine coco3 -cart cocosdc -sdc-root ~/sdc-root -ui sdl -v 2`.
4. Confirm `[module:sdl/ui] SDL3 UI` (white window = GTK3 — not this command),
   `[sdl/vo] renderer …`, Slot 0 `CRC32 0xb4c88d6c` and `CRC32 valid`
   (use `-no-c` if INVALID with that CRC), `[part:cocosdc]`, and
   `SD card root:` in the log.  Black CoCo 3 with SDL3 after that is the
   vo path — A/B with `brew install xroar` / `SDL_RENDER_DRIVER=opengl`.
5. If you have a minimal CommSDC probe (or Studio FileAccess):
   - `SDCOpenFile` / `$E0` with `"m:HELLO.TXT"` (256-byte name block), then
     `$80` LSN 0 — should return the file’s first 256 bytes (zero-padded).
   - Missing name → `FAILED` with bit `$10` (file not found), no hang.
   - `$C0`+`'I'` after a successful mount returns the 8.3 name and size.
   - `$E0` `"L:*.*"` then `$C0`+`'>'` returns a directory page (does not hang).
   - StreamFile / BIGLOADM: `m:` mount then `$90` — 512-byte sectors with
     READY between them; `$D0` or `$FF40=0` aborts without hanging.
   - Play (`SDC_Play.asm`): `m:` + `OpenSDC_File_X_At_Start` on a raw PCM
     file under `-sdc-root` (ffmpeg u8 44750 Hz as in the Play comments);
     BREAK should abort (`$D0`); end of file should return without hang.
     **Audible** 44750 Hz output through `$FF20` is this Mac smoke — host
     tests do not exercise the emulator sound path.

Studio **Run** (launching media from CoCo BASIC Studio into this XRoar)
is still later work.  Do not expect floppy-latch FDC behaviour or a
Zippster `.CSM` menu on this branch.
