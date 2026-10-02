#!/usr/bin/env python3
"""Check the GameCube link's VROM symbols against the dmadata table of the user's ROM.

The GameCube build reads the user's unmodified N64 ROM, so every _<name>SegmentRomStart/End that abs_syms.ld
provides (taken from the matching N64 ELF) must equal the VROM start/end of the same file in the ROM's dmadata
table (16-byte entries at dmadata_start: vromStart, vromEnd, romStart, romEnd; names in ROM order come from
segments.csv). Exits non-zero on any mismatch.

Run from the repository root inside Linux/WSL.
"""

import argparse
import csv
import re
import struct
import sys

N64_MAGIC = 0x80371240  # big-endian (.z64) byte order
PROVIDE = re.compile(r"^\s*PROVIDE\(\s*([A-Za-z0-9_.$]+)\s*=\s*0x([0-9A-Fa-f]+)\s*\);", re.M)


def read_names(path):
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    if not rows or rows[0][0] != "Name":
        sys.exit(f"{path}: expected a 'Name,...' header")
    return [row[0] for row in rows[1:] if row]


def read_dmadata(rom, offset, count):
    """Entries until the all-zero terminator (at most count + 1 are read)."""
    entries = []
    for i in range(count + 1):
        raw = rom[offset + 16 * i : offset + 16 * (i + 1)]
        if len(raw) < 16:
            sys.exit(f"dmadata entry {i} runs past the end of the ROM")
        entry = struct.unpack(">4I", raw)
        if entry == (0, 0, 0, 0):
            break
        entries.append(entry)
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rom", default="baseroms/n64-us/baserom.z64")
    parser.add_argument("--segments", default="baseroms/n64-us/segments.csv", help="file names in ROM order")
    parser.add_argument("--abs-syms", default="build/gc-n64-us/abs_syms.ld")
    parser.add_argument("--dmadata-start", type=lambda s: int(s, 0), default=0x1A500)
    parser.add_argument("--expect", type=int, default=1552, help="expected number of dmadata entries")
    args = parser.parse_args()

    with open(args.rom, "rb") as f:
        rom = f.read()
    if struct.unpack(">I", rom[:4])[0] != N64_MAGIC:
        sys.exit(f"{args.rom}: not a big-endian (.z64) N64 ROM")

    names = read_names(args.segments)
    entries = read_dmadata(rom, args.dmadata_start, max(len(names), args.expect))
    with open(args.abs_syms) as f:
        syms = {m.group(1): int(m.group(2), 16) for m in PROVIDE.finditer(f.read())}

    errors = []
    if len(entries) != args.expect:
        errors.append(f"ROM dmadata has {len(entries)} entries, expected {args.expect}")
    if len(names) != len(entries):
        errors.append(f"{args.segments} names {len(names)} files, ROM dmadata has {len(entries)}")

    ok = 0
    for i, (name, (vrom_start, vrom_end, _, _)) in enumerate(zip(names, entries)):
        bad = []
        for edge, want in (("Start", vrom_start), ("End", vrom_end)):
            sym = f"_{name}SegmentRom{edge}"
            have = syms.get(sym)
            if have is None:
                bad.append(f"{sym} missing")
            elif have != want:
                bad.append(f"{sym} = 0x{have:08X}, ROM VROM {edge.lower()} 0x{want:08X}")
        if bad:
            errors.append(f"entry {i} ({name}): " + "; ".join(bad))
        else:
            ok += 1

    for e in errors[:50]:
        print(f"check_dmadata: {e}", file=sys.stderr)
    if len(errors) > 50:
        print(f"check_dmadata: ... {len(errors) - 50} more", file=sys.stderr)
    print(f"check_dmadata: {ok}/{len(entries)} dmadata entries OK ({args.rom})")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
