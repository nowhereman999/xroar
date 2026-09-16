# CoCoSDC in this XRoar fork (Phase A)

This fork adds a Dragon/CoCo cartridge type `cocosdc` so Studio’s CoCoSDC Run
media can be exercised on a Mac (or any Unix host) without a physical CoCoSDC.

It is **not** a VCC `SDC.dll` port.  The register contract is the one used by
Studio’s client libraries (`CommSDC` in `SDC_Comm.asm`): `$FF40` control latch,
`$FF48` command/status, `$FF49–$FF4B` parameter/data path.

Phase A is the plumbing only.  Mount, directory, file, BIGLOADM, and stream/Play
are later phases.

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

An optional `-cart-rom` can still be attached (SDC-DOS flash image).  Phase A
does not require a cart ROM; load your CoCo program some other way (`-run`,
cassette, floppy, etc.).

Confirm the type is registered:

```text
xroar -cart-type help
xroar -cart help
```

You should see `cocosdc`.

## What Phase A implements

Hardware, as used by `CommSDC`:

| Address | Role |
| --- | --- |
| `$FF40` write | `$43` enters command mode; `$00` leaves it (params are kept) |
| `$FF48` read | Status: `BUSY` bit 0, `READY` bit 1, `FAILED` bit 7 |
| `$FF48` write | Command (command mode only) |
| `$FF49` | Parameter 1 |
| `$FF4A` | Parameter 2 / data A |
| `$FF4B` | Parameter 3 / data B |

Command-mode behaviour:

- After `$43`, status is not busy so `waitForIt` / `POLLBUSY` return.
- Commands with **bit 5 set** (`$E0/$E1` mount, `$A0/$A1` write LSN, …) set
  `BUSY|READY`, accept 256 bytes on `$FF4A/$FF4B`, then clear `BUSY`.
- Other commands clear `BUSY` immediately (no 256-byte response in Phase A).

Host directory:

- Stored as the SD card root (`-sdc-root` / `sdc-root=`).
- Existence is checked and logged; **files are not opened in Phase A**.

### CommSDC ops: implemented vs stubbed

| Client use | Command | Phase A |
| --- | --- | --- |
| Enter/leave command mode, poll status | `$FF40` / `$FF48` | **Implemented** |
| `CheckSDCFirmwareVersion` (`$C0`, `'V'`) | `$C0` + P1=`$56` | **Implemented** — BCD **1.27** (`$0127`) left in `$FF4A/$FF4B` so the Studio check (`>= 127`) passes.  Stream/`m:` is still not emulated. |
| `SDCReset` program-mode handshake | `$1C` | **Implemented** — `'P'/'M'` in `$FF49/$FF4A` |
| Mount / eject (`$E0/$E1`, data block) | `$E0/$E1` | **Ack only** — 256-byte path is accepted so CommSDC does not hang; no file is mounted |
| Write logical block | `$A0/$A1` | **Ack only** — 256-byte block is accepted, not written |
| Read logical block | `$80/$81` | **Stub** — completes Not Busy (no 256-byte payload) |
| Get info / dir page / CWD (`$C0` + `'I'`, `'>'`, `'C'`) | `$C0/$C1` | **Stub** — Not Busy, no data block |
| Stream (`$90/$91`), abort (`$D0`), Play / BIGLOADM | | **Out of scope** |
| Floppy-emulation mode (non-`$43` latch) | | **Not emulated** (latch stored; `$FF48` reads 0) |

A minimal CoCo program that follows `CommSDC` (put `$43` in `$FF40`, wait for
not-busy, issue `$C0`/`'V'`, clear `$FF40`) will complete without hanging.

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
   src/xroar -machine coco3 -cart cocosdc -sdc-root ~/sdc-root -v 2
   ```

   `-v 2` logs VERSION/mount acks.  `-cart-type help` and `-h` mention
   `-sdc-root` without needing a ROM.

`./configure --help` lists UI/audio backends.  If SDL 2 is found, the Mac build
gets the usual XRoar menu extras.  This fork does not add a CMake path;
configure/make is what the tree already uses.

The CommSDC wait-loop against the register engine (no emulator needed):

```text
cc -std=c11 -Wall -Werror -Isrc -o /tmp/cocosdc_hw_test tools/cocosdc_hw_test.c
/tmp/cocosdc_hw_test
```

## Later phases (not in this branch)

- **Phase B** — mount, directory, buffered 256-byte R/W against the host folder
- **Later** — BIGLOADM / 512-byte stream / Play
- Upstream PR to Ciaran is intentionally not part of this work
