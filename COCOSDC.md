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
upstream XRoar.  On a Mac:

1. Install toolchain and SDL 2 (menus/audio; GTK 3 is untested on macOS):

   ```text
   xcode-select --install
   brew install autoconf automake pkg-config sdl2 libpng
   ```

   Texinfo is **not** needed for the emulator.

2. From a git checkout, build `src/xroar` (preferred; skips the manual):

   ```text
   ./autogen.sh
   ./configure
   make -C src
   ```

   The emulator binary is `src/xroar`.  Optional: `sudo make install` (default
   prefix `/usr/local`).

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

3. ROM images (Colour BASIC / Super Extended BASIC, etc.) go in
   `~/Library/XRoar/roms/`.  See `README` (“Getting started under Mac OS X+”).

4. Example:

   ```text
   mkdir -p ~/sdc-root
   printf 'hello from sdc\n' > ~/sdc-root/HELLO.TXT
   src/xroar -machine coco3 -cart cocosdc -sdc-root ~/sdc-root -v 2
   ```

Host-side tests (same as CI; no emulator):

```text
./tools/run-cocosdc-tests.sh
```

`./configure --help` lists UI/audio backends.  If SDL 2 is found, the Mac build
gets the usual XRoar menu extras.  This fork does not add a CMake path;
configure/make is what the tree already uses.

### Optional Mac / CoCo smoke (not required for Phase D)

Phase D acceptance is the Linux host tests.  When a Mac and CoCo program
are available later:

1. `mkdir -p ~/sdc-root` and put a small file there, e.g. `HELLO.TXT`.
2. Rebuild with `make -C src` (Texinfo / `makeinfo` not required).
3. Run `src/xroar -machine coco3 -cart cocosdc -sdc-root ~/sdc-root -v 2`.
4. Confirm `[part:cocosdc]` and `SD card root:` in the log.
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
