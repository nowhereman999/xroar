#!/usr/bin/env python3
"""Run actual SDC-DOS BASIC against private disposable disk images.

No ROM is distributed. Requires an XRoar build with null UI and trap support.
All emulated commands run through the supplied CoCo 3 and SDC-DOS ROMs.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

TRACK_SIZE = 18 * 256
FAT_OFFSET = 17 * TRACK_SIZE + 256
DIR_OFFSET = FAT_OFFSET + 256
SENTINEL = "POKE28672,32:POKE28673,254:EXEC28672"


def disk_with_start():
    image = bytearray([255]) * (35 * TRACK_SIZE)
    image[17 * TRACK_SIZE:FAT_OFFSET] = bytes(256)
    program = b'10 PRINT "LOADED START":POKE24576,123\r20 END\r'
    image[:len(program)] = program
    image[FAT_OFFSET] = 0xC1
    entry = bytearray(32)
    entry[:11] = b"START   BAS"
    entry[11:14] = bytes([0, 255, 0])  # BASIC, ASCII, first granule
    entry[14:16] = len(program).to_bytes(2, "big")
    image[DIR_OFFSET:DIR_OFFSET + 32] = entry
    return image


def payload(path):
    image = path.read_bytes()
    return image[len(image) % 256:]


def catalog(path):
    image = payload(path)
    entries = {}
    for pos in range(DIR_OFFSET, DIR_OFFSET + 9 * 256, 32):
        entry = image[pos:pos + 32]
        if entry[0] == 255:
            break
        if entry[0] == 0:
            continue
        name = entry[:8].decode("ascii").rstrip() + "." + entry[8:11].decode("ascii").rstrip()
        entries[name] = entry
    return entries


def extract_file(path, entry):
    image = payload(path)
    granule = entry[13]
    seen = set()
    data = bytearray()
    while granule not in seen and granule < 68:
        seen.add(granule)
        offset = (granule // 2 + (1 if granule >= 34 else 0)) * TRACK_SIZE
        offset += (granule % 2) * 9 * 256
        link = image[FAT_OFFSET + granule]
        if link >= 0xC0:
            length = (link - 0xC1) * 256 + int.from_bytes(entry[14:16], "big")
            return bytes(data + image[offset:offset + length])
        data.extend(image[offset:offset + 9 * 256])
        granule = link
    raise AssertionError("invalid/cyclic DECB FAT chain")


def screen_text(ram):
    cells = ram[0x70400:0x70600]
    return "\n".join("".join(chr((v & 63) + (64 if (v & 63) < 32 else 0))
                             for v in cells[pos:pos + 32]) for pos in range(0, 512, 32))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xroar", type=Path, required=True)
    parser.add_argument("--coco-rom", type=Path, required=True)
    parser.add_argument("--sdc-rom", type=Path, required=True)
    parser.add_argument("--image-header", choices=("raw", "jvc", "vdk"), default="raw")
    parser.add_argument("--output", type=Path,
                        help="New empty output directory; otherwise create a temporary directory")
    args = parser.parse_args()
    for path in (args.xroar, args.coco_rom, args.sdc_rom):
        if not path.is_file():
            parser.error(f"missing required local file: {path}")
    output = args.output or Path(tempfile.mkdtemp(prefix="xroar-sdc-rom-"))
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("output directory must be empty; existing disk images are never overwritten")
    volume = output / "volume"
    volume.mkdir()
    disk = volume / "TEST.DSK"
    headers = {"raw": b"", "jvc": bytes([18, 1, 1, 1]),
               "vdk": b"dk" + bytes([12, 0, 0x10, 0x10, 0, 0, 35, 1, 0, 0])}
    header = headers[args.image_header]
    original = header + disk_with_start()
    disk.write_bytes(original)
    (volume / "STARTUP.CFG").write_bytes(b"0=TEST.DSK\r\n")
    initial_hash = hashlib.sha256(original).hexdigest()
    results = []

    def check(condition, message):
        results.append((bool(condition), message))
        print(f"ROM_{'PASS' if condition else 'FAIL'} {message}", flush=True)

    def run(name, lines, sd_root=volume):
        capture = output / (name + ".ram")
        # SDC boot and ROM break-key polls call POLCAT too. Padding absorbs
        # those calls without feeding the first letter of the next command.
        text = "\r" * 64 + "".join(" " * 64 + line + "\r" for line in [*lines, SENTINEL])
        command = [str(args.xroar.resolve()), "-c", "/dev/null", "-ui", "null", "-ao", "null",
                   "-machine", "coco3", "-ram", "512", "-extbas", str(args.coco_rom.resolve()),
                   "-cart", "cocosdc", "-cart-rom", str(args.sdc_rom.resolve()),
                   "-sdc-root", str(sd_root.resolve()), "-no-ratelimit", "-no-disk-write-back", "-timeout", "60", "-v", "2",
                   "-debug-fdc", "-1", "-trap", "pc=0x7000", "-trap-range", "1",
                   "-trap-snap", str(capture.resolve()), "-trap-timeout", "0", "-type", text]
        (output / (name + ".commands.txt")).write_text("\n".join([*lines, SENTINEL]) + "\n")
        try:
            with (output / (name + ".log")).open("w") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=30)
        except subprocess.TimeoutExpired:
            check(False, name + " host watchdog expired")
            return None, ""
        check(result.returncode == 0 and capture.is_file(), name + " reached real BASIC completion sentinel")
        if not capture.is_file():
            return None, ""
        ram = capture.read_bytes()
        display = screen_text(ram)
        (output / (name + ".screen.txt")).write_text(display + "\n")
        check("ERROR" not in display, name + " completed without a BASIC error")
        return ram, display

    check("START.BAS" in catalog(disk), "fixture contains host START.BAS before emulator runs")
    _, display = run("01-dir", ["CLS", "DIR"])
    check("START" in display, "BASIC DIR lists host START.BAS")
    ram, _ = run("02-load-run", ['LOAD"START"', "RUN"])
    check(ram is not None and ram[0x76000] == 123, "BASIC LOAD/RUN executes bytes from mounted image")
    run("03-save", ["NEW", '10 PRINT "SAVED FROM ROM"', "20 POKE24578,77", 'SAVE"HEY"'])
    entries = catalog(disk)
    check("HEY.BAS" in entries, "BASIC SAVE creates HEY.BAS in independent host catalog read")
    check(hashlib.sha256(disk.read_bytes()).hexdigest() != initial_hash, "BASIC SAVE mutates mounted host file")
    check("HEY.BAS" in entries and b"SAVED FROM ROM" in extract_file(disk, entries["HEY.BAS"]),
          "saved FAT chain contains the actual BASIC program")
    ram, _ = run("04-restart-run", ['RUN"HEY"'])
    check(ram is not None and ram[0x76002] == 77, "new emulator process reloads and runs persisted HEY.BAS")

    pattern = bytes(range(17, 49))
    lines = []
    for first in range(0, 32, 8):
        lines.append(":".join(f"POKE{24832 + i},{pattern[i]}" for i in range(first, first + 8)))
    lines += ['SAVEM"BYTES",24832,24863,24832']
    run("05-savem", lines)
    entries = catalog(disk)
    check("BYTES.BIN" in entries, "BASIC SAVEM creates machine-language file on host disk")
    check("BYTES.BIN" in entries and extract_file(disk, entries["BYTES.BIN"])[5:37] == pattern,
          "host SAVEM payload matches all 32 source bytes")
    ram, _ = run("06-restart-loadm", ['LOADM"BYTES"'])
    check(ram is not None and ram[0x76100:0x76120] == pattern, "new emulator process LOADM restores all 32 bytes")

    # This formats a second disposable volume, preserving the first test's
    # persistence evidence. Never accept an existing user-owned image path.
    formatted = output / "format-volume"
    formatted.mkdir()
    (formatted / "TEST.DSK").write_bytes(original)
    (formatted / "STARTUP.CFG").write_bytes(b"0=TEST.DSK\r\n")
    run("07-dskini", ["CLS", "DSKINI0"], formatted)
    formatted_disk = formatted / "TEST.DSK"
    check(catalog(formatted_disk) == {}, "BASIC DSKINI clears the actual host catalog")
    check(payload(formatted_disk)[FAT_OFFSET:FAT_OFFSET + 68] == bytes([255]) * 68,
          "BASIC DSKINI frees all 68 host granules")
    run("08-format-save", ["NEW", "10 POKE24579,91", 'SAVE"FRESH"'], formatted)
    check("FRESH.BAS" in catalog(formatted_disk), "BASIC SAVE persists on newly formatted disk")
    ram, _ = run("09-format-restart", ['RUN"FRESH"'], formatted)
    check(ram is not None and ram[0x76003] == 91, "new process runs program saved after DSKINI")
    # DRIVE without a name ejects this image; explicitly remount the same path.
    ram, _ = run("10-remount", ['DRIVE0,""', 'DRIVE0,"TEST.DSK"', 'RUN"HEY"'])
    check(ram is not None and ram[0x76002] == 77, "BASIC eject/remount preserves host-saved program")
    check(all(path.read_bytes()[:len(header)] == header and len(path.read_bytes()) == len(original)
              for path in (disk, formatted_disk)), "writes and format preserve image header and size")
    passed = sum(ok for ok, _ in results)
    print(f"ROM_SUMMARY {passed}/{len(results)} checks passed; artifacts={output}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
