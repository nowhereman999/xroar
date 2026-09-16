# CoCoSDC in this XRoar fork (Phase B)

This fork adds a Dragon/CoCo cartridge type `cocosdc` so Studio’s CoCoSDC
client libraries can talk to a host folder on a Mac (or any Unix host)
without a physical CoCoSDC.

It is **not** a VCC `SDC.dll` port.  The register contract is the one used by
Studio’s `CommSDC` (`SDC_Comm.asm`): `$FF40` control latch, `$FF48`
command/status, `$FF49–$FF4B` parameter/data path.  Mount, directory, and
buffered 256-byte R/W follow `SDC_FileAccess.asm`.  Firmware opcodes and
256-byte record layouts match Darren Atkinson’s CoCo SDC User Guide where
the client uses them.

Phase A was register plumbing only (VERSION, reset handshake, bit-5 command
*ack*).  Phase B maps those commands onto `-sdc-root`.

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

`-v 2` logs VERSION, mount, dir, and LSN commands.

## What Phase B implements

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
- Other commands clear `BUSY` with no payload (VERSION, `$1C`, …).

### CommSDC ops

| Client use | Command | Phase B |
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
| Stream (`$90/$91`), abort (`$D0`), Play / BIGLOADM | | **Out of scope** |
| Floppy-emulation mode (non-`$43` latch) | | **Not emulated** (latch stored; `$FF48` reads 0) |

Files mounted with `m:` / `n:` are a raw array of 256-byte blocks (the
FileAccess model).  `M:` / `N:` mount the same way for LSN access (no JVC /
VDK / SDF header parse in this phase).  Two slots (`$E0` / `$E1`); the same
host file cannot be mounted in both (`FAILED\|$20`).

A missing `-sdc-root`, or a path that does not exist, fails the command
cleanly — CommSDC does not hang.

## Phase B vs later (stream / BIGLOADM / Play)

**Phase B is done** when the host tests below pass.  It is the FileAccess
layer Studio uses for open/mount, directory listing, and buffered 256-byte
reads and writes.  That is verifiable on Linux CI without a Mac, a CoCo, or
an emulator binary.

| In Phase B (this branch) | Not in Phase B — later |
| --- | --- |
| `$FF40`/`$FF48–$FF4B` CommSDC wait-loop | Floppy-emulation latch (non-`$43` `$FF40`) |
| VERSION `$C0` `'V'` (BCD 1.27), `$1C` `PM` | BIGLOADM |
| Mount/eject `$E0/$E1` `m:`/`n:`/`M:` against `-sdc-root` | `$90/$91` 512-byte **stream** (Play / `m:` streaming) |
| LSN `$80/$81` `$A0/$A1` (256 bytes at LSN×256) | Abort-stream semantics beyond not hanging on `$D0` |
| Info / dir page / CWD (`'I'` / `'>'` / `'C'`, plus `L:`/`D:`/`K:`/`X:`) | Studio **Run** media integration |
| Two slots, `FAILED` bits `$04/$08/$10/$20` | JVC/VDK/SDF header parse; FDC floppy image geometry |

`$90` / `$D0` complete Not Busy so CommSDC does not hang; they do **not**
deliver 512-byte stream payloads.  A host test asserts that `$90` is Not
Busy with no READY block.

An upstream PR to Ciaran is intentionally not part of this work.

## Linux / CI (primary verification)

No SDL, autotools, ROMs, or CoCo required:

```text
./tools/run-cocosdc-tests.sh
```

That compiles `tools/cocosdc_hw_test.c` (wait-loop, LSN latch, 256-byte RX)
and `tools/cocosdc_fs_test.c` + `src/cocosdc_fs.c` against a `mkdtemp`
sdc-root (mount, missing-path `FAILED|$10`, both slots, in-use, dir pages,
CWD, sequential LSN write/read, mkdir/delete).

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

### Optional Mac / CoCo smoke (not required for Phase B)

Phase B acceptance is the Linux host tests.  When a Mac and CoCo program
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

Do not expect stream/Play/BIGLOADM or floppy-latch behaviour on this branch.
