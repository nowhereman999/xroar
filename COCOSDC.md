# CoCoSDC in this XRoar fork (SDC-DOS floppy)

This fork adds a Dragon/CoCo cartridge type `cocosdc` so Studio’s CoCoSDC
client libraries can talk to a host folder on a Mac (or any Unix host)
without a physical CoCoSDC, and so **SDC-DOS Disk BASIC** can
`DRIVE` / `LOAD` / `RUN` / `LOADM` a mounted `.DSK` under `-sdc-root`.

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
Run).  This tip adds **FDC floppy-emulation** for SDC-DOS Disk BASIC on a
mounted `M:` image, plus MCU-style `STARTUP.CFG` auto-mount.

## SDC-DOS mounted-image fix (2026-09-17)

The blank `DIR` and successful-looking `SAVE` reported at `7778b0d3`
were caused by **command-mode sector dispatch**, not the FDC transfer loop.
The live SDC-DOS 1.75 CC3 trace issues `$82` reads and `$A2` writes. The
old dispatcher masked only the drive bit and accepted `$80/$81` and
`$A0/$A1`; `$82/$A2` fell through to a success response with no disk I/O.
Consequently `DSKINI0` and `SAVE"HEY"` appeared to succeed without changing
the mounted host image, and `DIR` displayed an empty buffer.

The dispatcher now accepts the single-sided LSN flag on both drives
(`$82/$83`, `$A2/$A3`) and the byte-transfer flag on reads (`$84–$87`).
Single-sided LSN addressing skips side 1 when the image is double-sided.
These flags follow the [CoCo SDC User Guide, page 31](https://www.macmess.org/downloads/CoCo%20SDC%20User%20Guide.pdf).
Unsupported top-level commands return failure, and sector writes report
host flush/sync failures instead of silently returning success. Writes beyond
the end of a disk image fail without resizing it; raw FileAccess files remain
extensible. JVC mounts reject unsupported sector geometry.

The former handoff correctly warned that host-test success alone was
insufficient: its tests exercised the FDC and `$80/$A0`, but missed the
actual SDC-DOS commands. Keep both the host regression suite and the
ROM integration test below. The integration test uses disposable images
and the user's own ROMs; it must never format a real game or work disk.

Verified on macOS with SDC-DOS 1.75 CC3: 108/108 ROM checks across raw,
JVC, and VDK images; 348 filesystem/protocol checks and 23 injected host
I/O failure checks. The original executable fails the ROM regression.
A copy of Studio's actual `LAUNCH.DSK` also lists all eight packed files.

Studio's existing disk packing remains appropriate: FAT on track 17
sector 2, directory on sectors 3–11, and `STARTUP.CFG` mounting `LAUNCH.DSK`.
The CoCoSDC path writes the mounted file directly; `-no-disk-write-back`
controls XRoar's separate floppy-image subsystem, not CoCoSDC writes.
Studio regenerates its build-time `sdc-root` on Run, so keep personal work
images in a separate persistent `-sdc-root` directory.

## Enable the cartridge

Built-in profile (same name as the type):

```text
xroar -machine coco3 -cart cocosdc -sdc-root /path/to/sdcard
```

That is the same built-in profile as **Hardware → Cartridge → CoCoSDC (Phase D)**.
Both now default `cart-rom @sdcdos` (search `sdcdos.rom` on the ROM path) so
Hard Reset boots SDC-DOS when the image is present.  Studio Run still wins if
it passes an explicit `-cart-rom`.

Equivalent using a cart-type option (useful in `xroar.conf`):

```text
cart cocosdc
  cart-type cocosdc
  cart-rom @sdcdos
  cart-opt sdc-root=/path/to/sdcard
```

Or on the command line:

```text
xroar -machine coco3 -cart cocosdc -cart-opt sdc-root=/path/to/sdcard
```

`-sdc-root` may also appear in `~/Library/XRoar/xroar.conf` (macOS) or
`~/.xroar/xroar.conf` (Linux).  A leading `~/` is expanded.  The Cartridge
menu does **not** set `-sdc-root`; without it SDC-DOS can still banner but
`DIR` / `DRIVE` have no SD volume.

MPI: insert the profile into a slot as with any other cart
(`-cart mpi -mpi-load-cart cocosdc`).  SCS (`$FF40–$FF5F`) follows the MPI
P2 routing like RS-DOS.

### SDC-DOS ROM (menu vs CLI)

| Path | ROM used |
| --- | --- |
| Studio / CLI `-cart cocosdc -cart-rom FILE` | `FILE` (unchanged) |
| CLI `-cart cocosdc` with no `-cart-rom` | `@sdcdos` → `sdcdos.rom` on the ROM path |
| Mac **Hardware → Cartridge → CoCoSDC (Phase D)** | same built-in profile, so also `@sdcdos` |

Place the flash image as one of:

```text
~/Library/XRoar/roms/sdcdos.rom     (macOS; searched even without Cocoa)
~/.xroar/roms/sdcdos.rom            (Linux)
```

Also accepted: `sdc-dos`, `sdc_dos`, `SDCDOS`, with `.rom` / `.ROM`.
Override with `-cart-rom /path/to/sdcdos.rom` or `-rompath DIR`.

CommSDC FileAccess (`m:` / stream / Play) still works with an empty `$C000`;
only the SDC-DOS banner needs the ROM.

**If the ROM is missing:** the menu checkmark still appears (the cart
*module* is attached).  stderr prints

```text
[cocosdc] WARNING: SDC-DOS ROM not found (cart-rom @sdcdos, rompath …).
Cartridge stays selected but $C000 is empty, so Hard Reset boots ECB / Super ECB OK.
Place sdcdos.rom in the ROM path (macOS: ~/Library/XRoar/roms/) or pass -cart-rom FILE.
```

That green ECB / Super ECB `OK` prompt is the missing-ROM fallback, not a
dead cart.  Previously this was silent (CRC32 INVALID only at `-v`).

Confirm the type is registered:

```text
xroar -cart-type help
xroar -cart help
```

You should see `cocosdc`.

## Glen: rebuild the local checkout

Rebuild `src/xroar` after updating the source. Restart an already running
XRoar process to use the new binary.

From the repo root (not `src/`):

```text
./configure --without-gtk2 --without-gtk3 --with-sdl2 && make
```

`make` from the root still tries `doc/xroar.info` if `makeinfo` is missing;
`src/xroar` is already linked.  On Mac, `--with-sdl2` is Cocoa/`macosx` (the
working 1.12.1-style video path).  Optional: `make -C src` skips the info
manual.  Host tests (no ROM): `./tools/run-cocosdc-tests.sh`.

Mac menu bar (`-ui macosx`): **Tool → Keyboard → Natural** (translated
host symbols, ⌘Z) or **Emulated** (raw CoCo/Dragon keys); **Tool → Speed
→ 100%** (realtime) or **Maximum** (unthrottled / `-no-ratelimit`). These
are the existing `ui_tag_kbd_translate` and `ui_tag_ratelimit_latch`
knobs, not new backends.

**Hardware → Cartridge → CoCoSDC (Phase D)** then **Hardware → Hard Reset**
should boot SDC-DOS the same as CLI `-cart cocosdc` when `sdcdos.rom` is
on the ROM path.  A missing ROM prints the WARNING above and stays on the
green ECB / Super ECB `OK` prompt (cart still checked).  `DIR` needs
`-sdc-root` (CLI or `xroar.conf`); the menu does not invent one.

SDC-DOS smoke (Glen’s Studio argv shape: DECB `.DSK` + `STARTUP.CFG` `0=….DSK`,
**not** a loose FAT `START.BAS`):

```text
# sdc-root contains STARTUP.CFG (bytes `0=LAUNCH.DSK\r\n`) and LAUNCH.DSK
src/xroar -machine coco3 -ram 2048 -cart cocosdc \
  -cart-rom ~/Library/XRoar/roms/sdcdos.rom \
  -sdc-root "/path/to/sdc-root" \
  -type 'RUN"START"\r' -no-disk-writeback

# menu-equivalent: no -cart-rom; sdcdos.rom must be on the ROM path
src/xroar -machine coco3 -cart cocosdc -sdc-root "/path/to/sdc-root"
```

Quoted `-sdc-root` is fine (Google Drive spaces).  Default log level prints
`SD card root:` and `STARTUP.CFG … drive 0: … (FDC)`.  A failed `0=` mount
is a **WARNING** on stderr.  `-v 2` or `-debug-fdc -1` logs each FDC command.

Use `-v 2` to see command-mode sector read/write opcodes and failures.
SDC-DOS normally accesses a mounted image through `$82/$A2`; the FDC
trace applies to software using the floppy-controller registers instead.
With no image mounted, disk operations should fail rather than report
success with an empty transfer. Disk BASIC reads its catalog from track
17 sector 3, not sector 2 (the FAT).

## Example `-sdc-root` layout

Treat the directory as the SD volume root.  Names are matched
case-insensitively (8.3-style).  Relative paths use the SDC current
directory (`D:` / `'C'`).  A leading `/` is absolute from the volume root.

```text
~/sdc-root/
  STARTUP.CFG          0=GAME.DSK   (MCU auto-mount at attach / hard reset)
  GAME.DSK             DECB image with START.BAS / MOVER.BIN
  HELLO.TXT            raw file for m: / n: (LSN N = byte offset N*256)
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

## What this cart implements

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

| Client use | Command | This tip |
| --- | --- | --- |
| Enter/leave command mode, poll status | `$FF40` / `$FF48` | **Implemented** |
| `CheckSDCFirmwareVersion` (`$C0`, `'V'`) | `$C0` + P1=`$56` | **Implemented** — BCD **1.27** (`$0127`) in `$FF4A/$FF4B` |
| `SDCReset` program-mode handshake | `$1C` | **Implemented** — `'P'/'M'` in `$FF49/$FF4A` |
| Mount raw file / eject | `$E0/$E1` `m:` / `M:` | **Implemented** — 256-byte `"m:path"` / `"M:path"` / `"M:"`; missing path → `FAILED\|$10`.  **`M:`** is a disk image (JVC/VDK header, FDC).  **`m:`** is raw 256-byte blocks (FileAccess/stream; **no FDC**) |
| Create+mount raw file | `$E0/$E1` `n:` / `N:` | **Implemented** — creates if missing; `N:` with B=X=0 pre-sizes a 630-sector DSK (FDC-capable) |
| Write logical block | `$A0–$A3` | **Implemented** — 256 bytes at LSN×256 in the mounted file |
| Read logical block | `$80–$87` | **Implemented** — 256-byte payload; last partial sector is zero-padded |
| Get info for mounted file | `$C0/$C1` + `'I'` | **Implemented** — 32-byte directory record (size **LSB first** at 28–31) |
| Directory page | `$C0` + `'>'` | **Implemented** — 16×16-byte records (size **MSB first** at 12–15).  First `L:pattern` (`$E0`) |
| Current directory | `$C0` + `'C'` | **Implemented** — leaf 8.3 name.  Volume root sets bits 4+7 (`FAILED\|$10`) as in the User Guide |
| Set CWD / mkdir / delete | `$E0` `D:` / `K:` / `X:` | **Implemented** (used by FileAccess; leaf only for `K:`) |
| Stream (`OpenSDC_File_X` / BIGLOADM / Play) | `$90/$91` | **Implemented** — 512-byte sectors from LSN×512; `BUSY\|READY` per sector; last short sector zero-padded; EOF clears `BUSY`; next sector presented on the last DATREG read (Play interleaved load) |
| Abort stream | `$D0` or `$FF40=0` | **Implemented** — `$D0` is Not Busy in the `$FF48` write (Play BREAK); `CLR $FF40` also aborts |
| Play DAC / analog mux (`$FF20`) | | **Not in host tests** — register/stream contract only; 44750 Hz playback needs a live emulator |
| CSM media-player menu / extra opcodes | | **Not used by Studio Play/FileAccess** (`.CSM` is a file format that also streams with `$90`) |
| Floppy-emulation mode (non-`$43` latch) | | **Implemented** — WD1773-ish restore/seek/read/write sector on an `M:`/`N:` image.  Type I completes `!BUSY` **without** INTRQ (DECB polls status after Seek; an instant NMI with `$FF40` bit 5 on returns empty `DIR` + `SAVE` that never hits the host file).  Unmounted or `m:` raw Type II stays `BUSY\|NOTREADY` **without** INTRQ so DECB’s DRQ poll times out (`?IO ERROR`).  Type II complete status is 0 (not Type I `TRACK0`/`$04`).  Write-track (`$F0`/`$F4`, DSKINI) fills that track’s 18 sectors with `$FF` in the same host `FILE*`.  `$FF40` bit 5 gates INTRQ→NMI; bit 7 is HALT (never asserted mid-sector).  While DRQ is set, `$FF4A` and `$FF4B` both supply data (`LDU $FF4A`).  SDF / copy-protection not emulated |
| `$FF43` flash bank probe | | **Stub** — returns bank 0 (enough for SDC-DOS to see an SDC) |

Files mounted with `m:` / `n:` are a raw array of 256-byte blocks (the
FileAccess model).  **`M:` / `N:`** mount a floppy or hard-disk image:
JVC (1–4 byte, 18 sectors/track, 256-byte sectors) and VDK (`dk` + header size) prefixes are skipped for LSN
and FDC access; SDF (`SDF1`) is rejected.  Headerless files must be a
multiple of 256 bytes and at least 82944 bytes (User Guide DSK minimum).
Geometry is 18 sectors/track; more than 720 sectors (and ≤ 2880) is
treated as double-sided; more than 2880 is a hard disk (FDC sees the
first 1440 as SS 80-track).  Two slots (`$E0` / `$E1`); the same host
file cannot be mounted in both (`FAILED\|$20`).

### STARTUP.CFG (MCU auto-mount)

On `-sdc-root` attach and on **hard** reset the cart reads `STARTUP.CFG`
from the volume root (case-insensitive), matching the real Atmega:

```text
0=GAME.DSK
1=UTILS.DSK
D=/GAMES
```

`0=` / `1=` are `M:` disk-image mounts (so FDC/Disk BASIC work).  `D=`
sets the SD current directory.  Lines are `key=path` with optional
spaces and quotes; `#` comments and a UTF-8 BOM are ignored.  A failed
`0=`/`1=` mount is a warning (`STARTUP.CFG … mount failed`).  Missing
`STARTUP.CFG` is logged at default verbosity and is not an error.

This is what SDC-DOS needs for `RUN"START"` at boot: Disk BASIC `RUN`
and `LOAD` look at the **mounted disk image** (DECB directory on track 17
**sectors 3–11**; FAT on sector 2), not at loose FAT files.  `DIR` with
no arguments is the same — no image mounted → `?IO ERROR` on real
hardware and here (not an empty `OK`).  Image mounted but catalog on
sector 2 only → empty `DIR` then `OK` (DECB never reads that sector as
a directory).

Studio may stage `GAME.DSK` + `startup.cfg` under `-sdc-root`, or you can
type `DRIVE 0,"GAME.DSK"` once SDC-DOS is up (`DRIVE` is SDC-DOS’s `M:`).

### Loose FAT files vs Disk BASIC `RUN"START"`

| What is on `-sdc-root` | What works |
| --- | --- |
| `START.DSK` (DECB image containing `START.BAS`) + `startup.cfg` `0=START.DSK` | **`RUN"START"`** / `LOAD` / `LOADM` via SDC-DOS sector commands |
| `DRIVE 0,"START.DSK"` then `RUN"START"` | Same, after the mount |
| Loose `START.BAS` / `MOVER.BIN` in the FAT | **Not** Disk BASIC `RUN"START"`.  Real SDC-DOS `RUN` does not load a FAT file.  Use `DIR -` / `DIR "START.BAS"` to *list* FAT; Studio FileAccess `m:` still reads those files.  Put them inside a `.DSK` (or mount one) for `RUN`/`LOADM`. |

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

## SDC-DOS floppy vs later

Host tests cover Phases A–D plus FDC DSKCON-style restore/read/write on a
synthetic 35-track DECB DSK (FAT T17 S2, directory T17 S3–S11), `M:` vs
`m:`, JVC header skip, `STARTUP.CFG` auto-mount (including Glen’s
`0=LAUNCH.DSK\r\n` bytes and an `sdc-root` path with spaces), DECB’s
NMI/`ANDA #$7C` sector loop **including Seek $17**, 16-bit `$FF4A`/`$FF4B`
FDC data, a Studio-style multi-file `LAUNCH.DSK`, a **mounted-but-DIR-empty**
case (catalog only on S2), **host-file read-back after mount**, **write then
independent fopen read-back** (HEY.BAS on T17 S3), DSKINI write-track
persist, and Seek-with-`nmi_enable` must not INTRQ (the “status OK / zero
DIR / no host mutation” regression).  Verifiable on Linux CI without a Mac,
a CoCo ROM, or an emulator binary.  **Green host tests did not predict
Glen’s live SDC-DOS**: it used `$82/$A2`, which are now covered directly
alongside the ROM integration test.

| In this tip | Still not done |
| --- | --- |
| `$FF40`/`$FF48–$FF4B` CommSDC wait-loop | SDF / DMK copy-protection tracks |
| VERSION `$C0` `'V'` (BCD 1.27), `$1C` `PM` | |
| Mount/eject `$E0/$E1` `m:`/`n:` raw and **`M:`/`N:` disk images** (JVC/VDK header) | Mount-next / disk-set (`+` / `#`) |
| LSN `$80–$87` `$A0–$A3` (256 bytes; header skipped on `M:`) | Remaining User Guide extras not used by Studio or SDC-DOS DSKCON |
| Info / dir page / CWD (`'I'` / `'>'` / `'C'`, plus `L:`/`D:`/`K:`/`X:`) | Audible Play through the emulator sound path |
| `$90/$91` 512-byte **stream**; Play interleaved refill; `$D0` abort-on-write | Zippster `.CSM` media-player menu |
| **FDC** restore/seek/read/write sector + write-track format + HALT/NMI (host-test DSKCON) | Cycle-accurate floppy-controller timing |
| **`STARTUP.CFG`** `0=`/`1=`/`D=` auto-mount on attach and hard reset | |

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
`tools/cocosdc_fs_test.c` + `src/cocosdc_fs.c` + `src/cocosdc_fdc.c` against
a `mkdtemp` sdc-root (mount, missing-path `FAILED|$10`, both slots, in-use,
dir pages, CWD, sequential LSN write/read, mkdir/delete, `$90/$91`
multi-sector stream, `$D0` abort, Play `OpenSDC_File_X` + interleaved
512-byte words, **`M:` DSK FDC DSKCON-style LOAD**, `m:` vs `M:`, JVC
header skip, `STARTUP.CFG` auto-mount, Glen `0=LAUNCH.DSK\r\n`, spaced
sdc-root, DECB NMI sector loop including Seek, 16-bit FDC data, DECB DIR T17 S3 vs
skewed S2 catalog, host-file write then independent read-back, DSKINI
write-track persist, Seek-must-not-NMI).

After `./configure`, the same programs are `make -C src check` (`TESTS`).
GitHub Actions workflow `.github/workflows/cocosdc-host.yml` runs the
script on push.

### SDC-DOS ROM integration test

Use a built emulator and your own headerless CoCo 3 and SDC-DOS ROMs:

```sh
python3 tools/run-cocosdc-rom-tests.py --xroar src/xroar \
  --coco-rom "$HOME/Library/XRoar/roms/coco3.rom" \
  --sdc-rom "$HOME/Library/XRoar/roms/sdcdos.rom"
```

The test boots the actual ROMs with XRoar's null UI, types BASIC commands,
and captures RAM at a completion trap. It checks the directory and FAT in
the host file independently, then launches fresh emulator processes to
reload the saved data. Coverage includes `DIR`, `LOAD`/`RUN`, `SAVE`,
`SAVEM`/`LOADM`, `DSKINI0`, and eject/remount. Each run creates private test
images; `--output` optionally selects an empty directory for the logs,
screen text, RAM captures, and images. Repeat with `--image-header jvc` or
`--image-header vdk` to test supported headers. ROMs are not included in this repo.

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

### Top scanline / top strip (Glen, 2026-09-17)

Not a GIME “bad first framebuffer line.”  Two different things show up as a
top strip:

1. **Playfield, stuck solid full-width row while the map scrolls.**  Default
   NTSC 60Hz picture is 200 lines centred on the 192-line active area, so
   about **four GIME top-border scanlines** stay on screen.  Hardware border
   does not move with HVEN/VRAM.  Solid orange/yellow across both playfield
   halves is border colour, not a tile row.  View → Picture Area → Zoomed
   (512×384) crops that border; it is not a vo off-by-one.
2. **BASIC `OK` green screen: thin dark/blue/white fringe at the top of the
   picture against the black letterbox.**  Mixed subpixel colours are
   scaler/compositor AA (NTSC 60Hz forces LINEAR because 480 % 200 ≠ 0),
   not a palette scanline.  GIME would paint a full-width green or black
   line.  `vo_sdl2` (Cocoa) is unchanged vs `main`; this tip’s Darwin SDL3
   path prefers OpenGL but does not write that fringe into the buffer.

GIME `set_active_area` y is `lTB+3` in **vo_render** scanline space
(`vo_vsync` on FS *rising*, four lines after FS falling).  Host tests do
not cover this.  No GIME/vo first-line code change on this tip.

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

### Optional Mac / CoCo smoke (SDC-DOS + Play)

Linux host tests are the CI gate.  When a Mac and CoCo program are available:
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
   - SDC-DOS: `STARTUP.CFG` `0=GAME.DSK` (DECB image with `START.BAS`) then
     `RUN"START"` / `LOADM"MOVER.BIN"` — mounted disk image, not loose FAT.  `DIR` with no
     arguments lists the mounted floppy; `DIR -` lists the SD FAT.

Studio **Run** stages `LAUNCH.DSK` and `STARTUP.CFG` under its build
folder and launches this cartridge. A Zippster `.CSM` menu is not on this branch.
