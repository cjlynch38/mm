#!/usr/bin/env python3
"""Pad a .dol so every section's 32-byte-aligned extent lies within the file.

devkitPro's elf2dol records exact (unaligned) section sizes and does not pad the
final section. Apploaders, IOS and Dolphin round section sizes up to 32 bytes
when loading; recent Dolphin rejects the DOL outright ("Failed to init core")
if that rounded extent runs past end-of-file. Zero padding is harmless.

usage: dolpad.py FILE.dol [--check]
"""
import struct
import sys


def required_size(data):
    hdr = struct.unpack(">64I", data[:0x100])
    offsets, sizes = hdr[0:18], hdr[36:54]
    return max((o + ((s + 31) & ~31) for o, s in zip(offsets, sizes) if s), default=0x100)


def main():
    path = sys.argv[1]
    check_only = "--check" in sys.argv[2:]
    with open(path, "rb") as f:
        data = f.read()
    need = (required_size(data) + 31) & ~31
    if len(data) >= need:
        print(f"{path}: ok ({len(data)} bytes)")
        return 0
    if check_only:
        print(f"{path}: INVALID for Dolphin, {len(data)} bytes, needs {need}")
        return 1
    with open(path, "ab") as f:
        f.write(b"\0" * (need - len(data)))
    print(f"{path}: padded {len(data)} -> {need} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
