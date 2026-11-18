# XRoar upgrades: Codex / Grok Rev handoff

COCOWAR uses official **XRoar 1.13.1** plus a small, explicit patch series.
The authoritative pin is `vendor/xroar/release.json`; it records the upstream
archive SHA-256, feature-branch commit, ordered patches and their SHA-256s.
`make` builds this pin, not whichever source happens to be in `~/xroar`.
The AV_Banger renderer/daemon pin remains independent in `AV_BANGER_REV`.

## Locations on Glen's Mac

Relative to `/Users/glenhewlett/Documents/Codex/2026-11-04/i-x20/outputs`:

- `xroar-cocowar`: maintained emulator Git repository, branch `cocowar/1.13.1-rev2`.
- `cocowar-av/vendor/xroar`: portable, committed release pin and patch series.
- `cocowar-av/tools/build_av_xroar.py`: verified release/patch builder.
- `cocowar-av/tools/export_xroar_patches.py`: exports reviewed emulator commits.
- `xroar-upgrade-backup`: original Git bundle, uncommitted local patch and
  working `xroar-av` binary. `~/xroar` itself was left untouched.
- `rev-handoff/xroar-1.13.1-rev2.bundle`: emulator Git history for Rev.
- `rev-handoff/REV_FIXES_XROAR_1.13.1.md`: current follow-up and Linux handoff.

Rev: start with this document and the results file, not the old `~/xroar`
checkout. Do not copy an older COCOWAR renderer or AV_Banger source over the
pinned dependencies.

## Feature commits (Rev follow-up, 18 November 2026)

The maintained branch is now `cocowar/1.13.1-rev2`. The original
`cocowar/1.13.1` branch and its published bundle remain intact.
Apply the new series in order:

1. Host-backed CoCoSDC filesystem, stream/Play/FDC and its configuration.
2. Direct DECB/DragonDOS `-inject-bin` loader and its configuration.
3. macOS SDL3/Cocoa menus and display compatibility.
4. POSIX serial jack, including `-serial-fd` CLI/configuration, 38400/115200
   negotiation, checksum validation and BREAK reset. No hidden dependency on
   unrelated options in the CoCoSDC patch.
5. Optional AV_Banger mouse/keyboard socket. Windows/WASM compile a no-op
   adapter and retain normal joystick input; no POSIX headers in that path.
6. Frame-tail fix together with its 72-case regression test.
7. CoCoSDC and loader host tests/CI. Checked fixture formatting rejects
   truncation or encoding errors. `-Werror` remains enabled; warnings are
   not suppressed. This test commit depends on the earlier runtime features.
8. Upgrade documentation and Objective-C diff attribute.

Each commit has an `Upstream-status:` note. Review that note when upgrading;
1.13.1's experimental CoCoSDC is replaced, its ROM metadata is retained, and
its frame-tail clearing is corrected. The obsolete CRC comment is removed.
The GitHub workflow is for a fork's CI; no push is implied.

Serial and pointer sources live in the emulator repository. Do not restore
old game-local copies or string-rewriting build scripts. The game exporter
uses `--no-signature`, full blob IDs, fixed diff options, no diffstat, numbered
filenames and canonical @@ headers (no version-dependent function labels).
Thus compiler/Git versions do not become part of patch checksums. Export
with a second Git executable by setting `GIT=/path/to/git`; `--output DIR`
allows comparison without altering the production pin.

## Linux notes from Rev

Debian/Ubuntu build packages:
`build-essential autoconf automake libtool texinfo pkg-config libsdl2-dev
libasound2-dev libsndfile1-dev libgl-dev zlib1g-dev libpng-dev`.
Objective-C/Cocoa tooling applies only on macOS. GL headers are required by
the default configure; Text::Iconv was not needed in Rev's Linux build.

For Linux debugger tests use `-ui sdl` with `SDL_VIDEODRIVER=offscreen`, plus
`-ao null` or `SDL_AUDIODRIVER=dummy`. Upstream 1.13.1's null UI can starve
the debugger; this also occurs without our patches. COCOWAR's shared test
launcher now selects offscreen SDL on Linux. Mac tests retain the null UI.

## AV_Banger pin and connection timing

COCOWAR really uses `891a70803d4c6e1ed33fed6877e8ccb595e319b2`, one commit
after Rev's tested `40e6fb7`. It forwards the T training key in the renderer;
no daemon, serial protocol or audio code changed. The prior report's
"unchanged" meant unchanged during the XRoar upgrade, not identical to Rev's
last tested pin. `av_banger-891a708.bundle` supplies the missing history.

The old game guard was only a few milliseconds. Both startup code locations
now wait at least 50 ms at stock double speed before LINKCONFIRM; the
`verify_xroar_link.py` startup probe measures the exact emulated interval.
This delay is only during connection setup, not during game updates. It is
not a guarantee about host scheduling in an unlimited-speed emulator: Rev's
PTY/real-daemon timing test remains a separate Linux acceptance check.

## Build and run

From `cocowar-av`:

```
make build-raid-flyers BUILD_ARGS="--define BACKGROUND_MIDI=1"
```

Other existing game make targets work as before. The first emulator build
needs a C toolchain (also Objective-C on Mac), autotools, SDL dependencies and network access
to download the official release archive. Python needs tarfile's safe `data`
filter. Later builds use the checked local archive and compiled cache.
`build/av/xroar-av.version.json` records the installed source pin.

Cache: `build/deps/xroar-<version>-<content-hash>`. Changed patches get a new
cache directory. A failed build does not replace the working executable.
The release archive supplies `src/windows32/resources.h`; after autoreconf
we preserve this generated header, avoiding a needless Text::Iconv dependency
on macOS. Changing the Windows resources requires regenerating that header.

**Required launch option:** `-machine-opt readable-dat`. Earlier local XRoar
allowed MMU/DAT reads by default; upstream 1.13.1 makes that opt-in. COCOWAR's
launcher and test commands now pass it. Omitting it can corrupt bank restores
and cause a startup stall. This setting does not imply a hardware change.
Older external test binaries may not support the new option: use the pinned
`build/av/xroar-av` for current tests.

## Next upstream release

Work on a new branch; leave the known-good release branch and pin intact.
Coordinate one writer per branch with Rev. No rebasing a shared branch.

```
git remote add upstream https://www.6809.org.uk/git/xroar.git  # once, if absent
git fetch upstream --tags
git switch -c cocowar/NEW NEW
git cherry-pick <feature-commits-from-current-release-in-order>
```

Use `git log --reverse --oneline 1.13.1..cocowar/1.13.1-rev2` for the current list.
For later releases substitute their current base tag. Review upstream release
notes and diffs first: drop patches that upstream has fully incorporated,
but keep their regression coverage. Resolve conflicts by comparing behavior,
not by accepting an entire old file over a newer upstream implementation.
Do not revert upstream CPU/timing fixes just to preserve a benchmark number.

Download the new official tarball. After committing and reviewing the branch,
from `cocowar-av` run:

```
python3 tools/export_xroar_patches.py ../xroar-cocowar NEW /path/to/xroar-NEW.tar.gz
python3 tools/build_av_xroar.py build/av/xroar-av
```

The exporter rejects dirty tracked source and merge commits above the release
base. It rewrites only the pinned patch directory and manifest; review that
diff before committing. The archive hash pins exactly what was downloaded;
verify its provenance from the official site. To test without switching the
normal binary, pass a separate output path to the builder.

Run host and live acceptance tests before committing the new COCOWAR pin:

```
# In an emulator Git checkout (install autotools/SDL dependencies first):
autoreconf -fi
./configure
# If Text::Iconv is unavailable, copy src/windows32/resources.h from the
# matching official release archive, then touch it before make.
make -j8
make -C src check
sh tools/run-cocosdc-tests.sh
sh tools/run-vo-frame-tail-test.sh

# In cocowar-av:
make check BUILD_ARGS="--define BACKGROUND_MIDI=1"
make check-warships BUILD_ARGS="--define BACKGROUND_MIDI=1"
make check-build-raid-flyers BUILD_ARGS="--define BACKGROUND_MIDI=1"
PYTHONPATH=tools python3 -c 'from verify_av_mouse import verify; verify(raid=True,duel="raid-result")'
python3 tools/verify_xroar_link.py
```

Live tests need local sockets/shared memory, ROMs and Glen's extracted game
assets. Host tests need no copyrighted assets. Test both negotiated 115200
and 38400 fallback, mouse button edges, audio/MIDI, direct injection, GDB
breakpoints/memory and bank restoration. Keep real-hardware claims separate:
an emulator upgrade alone does not retest the Pi or physical CoCo.

Finally export a bundle and record the exact commits and results:

```
git bundle create ../rev-handoff/xroar-NEW-cocowar.bundle cocowar/NEW NEW
```

Rev can clone the bundle, select `cocowar/NEW`, run Linux tests and return a
separate branch/bundle. No push or production Pi deployment is implied.
To roll back the project, restore the preceding committed manifest/patches
and rebuild. The original emergency executable is also in the backup folder.
