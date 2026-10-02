#!/usr/bin/env python3
"""Build a bootable GameCube disc image of the Majora's Mask port from the user's own ROM.

The image contains the user's ROM, so it is built locally and must never be
committed or distributed. `make -f Makefile.gc iso` runs this script and writes
build/gc-n64-us/mm-gc.iso (build/ is gitignored).

The image is a GameCube disc that is also an ISO9660 volume, built like the Dolphin
dev disc (mkdevdisc.build_image):
  0x000000  boot.bin   disc header: game ID, title, main DOL and FST offsets
  0x000440  bi2.bin    region (from the game ID's 4th letter), simulated memory size
  0x002440  apploader  header, then the apploader built from port/gc/apploader (CC0)
  0x008000  ISO9660    volume descriptors, path tables, directories
  ...       FST        the GameCube file system: /mmgcport/baserom.z64 (+ --file entries)
  ...       main DOL   at a 32 KiB boundary
  ...       files      at 32 KiB boundaries (the DVD's ECC block size)
The console side mounts the ISO9660 volume with libiso9660 as dvd: (or, for an image
on the SD card started by Swiss, as img:) and opens dvd:/mmgcport/baserom.z64 like
the dev disc; the GameCube FST is what Dolphin, Swiss and the IPL see.
The image is not padded to a full disc (1,459,978,240 bytes); Dolphin and Swiss boot
it as it is.

Game ID: GMME00 by default. 'G' is the GameCube disc prefix, 'MM' stands for
Majora's Mask, 'E' selects NTSC-U (bi2 country code 1), maker '00' is unassigned.
No GameCube title uses the GMM prefix (GameTDB's list in Dolphin 2609 and Dolphin's
GameSettings have no GMM entries), so Dolphin and Swiss apply no game-specific
settings or patches. The same code is the memory card save's game code.

usage: mkiso.py --dol DOL --apploader ELF --rom ROM [-o OUT] [--id ID6] [--title TEXT]
                [--file DISC_PATH=SRC ...] [--force]
"""
import argparse
import hashlib
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkdevdisc  # noqa: E402

DISC_MAGIC = 0xC2339F3D
APPLOADER_OFFSET = 0x2440
APPLOADER_BASE = 0x81200000
APPLOADER_LIMIT = 0x81300000
APPLOADER_DATE = b"2026/10/02"  # version string of port/gc/apploader (YYYY/MM/DD)
FILE_ALIGN = 0x8000
VOLUME_ID = b"MMGCPORT_GAME"
DEFAULT_ID = "GMME00"
DEFAULT_TITLE = "Majora's Mask - GameCube port (homebrew, built from your ROM)"
ROM_DISC_PATH = "mmgcport/baserom.z64"

# The ROM the port accepts (the checks of port/gc/ogc/bridge_rom.c, plus the MD5)
ROM_SIZE = 0x2000000
ROM_MD5 = "2a0a8acb61538235bc1094d297fb6556"
ROM_CRC = (0x5354631C, 0x03A2DEF0)
REGIONS = {"J": 0, "E": 1, "P": 2, "D": 2, "F": 2, "S": 2, "I": 2, "U": 2}


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def fail(message):
    raise SystemExit(f"mkiso: {message}")


def read_apploader(path):
    """The apploader image (PT_LOAD contents from 0x81200000 on) and its entry point, from the ELF."""
    with open(path, "rb") as f:
        elf = f.read()
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 2:
        fail(f"{path} is not a 32-bit big-endian ELF")
    entry, phoff = struct.unpack_from(">II", elf, 0x18)
    phentsize, phnum = struct.unpack_from(">HH", elf, 0x2A)
    image = bytearray()
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from(">IIIIII", elf, phoff + i * phentsize)
        if p_type != 1 or p_memsz == 0:  # PT_LOAD
            continue
        if p_vaddr < APPLOADER_BASE or p_vaddr + p_memsz > APPLOADER_LIMIT:
            fail(f"{path}: segment at {p_vaddr:08X} is outside {APPLOADER_BASE:08X}-{APPLOADER_LIMIT:08X}")
        start = p_vaddr - APPLOADER_BASE
        if len(image) < start + p_filesz:
            image.extend(bytes(start + p_filesz - len(image)))
        image[start : start + p_filesz] = elf[p_offset : p_offset + p_filesz]
    if not image:
        fail(f"{path} has no loadable contents")
    if not APPLOADER_BASE <= entry < APPLOADER_BASE + len(image):
        fail(f"{path}: entry point {entry:08X} is outside the image")
    image.extend(bytes(align(len(image), 32) - len(image)))
    return bytes(image), entry


def apploader_blob(image, entry):
    header = bytearray(0x20)
    header[0:10] = APPLOADER_DATE
    struct.pack_into(">III", header, 0x10, entry, len(image), 0)  # entry point, size, trailer size
    return bytes(header) + image


def check_dol(path):
    with open(path, "rb") as f:
        dol = f.read()
    if len(dol) < 0x100:
        fail(f"{path} is too small for a DOL")
    fields = struct.unpack_from(">57I", dol, 0)
    offsets, addresses, sizes = fields[0:18], fields[18:36], fields[36:54]
    entry = fields[56]
    need = 0x100
    in_text = False
    for i in range(18):
        if sizes[i] == 0:
            continue
        if not (0x80003000 <= addresses[i] and addresses[i] + sizes[i] <= APPLOADER_BASE):
            fail(f"{path}: section {i} at {addresses[i]:08X} is outside 0x80003000-{APPLOADER_BASE:08X}")
        need = max(need, offsets[i] + align(sizes[i], 32))
        if i < 7 and addresses[i] <= entry < addresses[i] + sizes[i]:
            in_text = True
    if not in_text:
        fail(f"{path}: entry point {entry:08X} is not in a text section")
    if len(dol) < need:
        fail(f"{path} is not padded (run port/gc/tools/dolpad.py on it)")
    return dol


def check_rom(path, force):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        header = f.read(0x40)
    problems = []
    if len(header) < 0x40 or struct.unpack_from(">I", header, 0)[0] != 0x80371240:
        problems.append("not a big-endian (.z64) N64 ROM")
    elif header[0x3B:0x3F] != b"NZSE":
        problems.append(f"game code {header[0x3B:0x3F]!r}, expected b'NZSE' (Majora's Mask USA)")
    elif struct.unpack_from(">II", header, 0x10) != ROM_CRC:
        problems.append("its header checksums are not those of US 1.0")
    if size != ROM_SIZE:
        problems.append(f"{size} bytes, expected {ROM_SIZE}")
    if not problems:
        md5 = hashlib.md5()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                md5.update(chunk)
        if md5.hexdigest() != ROM_MD5:
            problems.append(f"MD5 {md5.hexdigest()}, expected {ROM_MD5}")
    if problems and not force:
        fail(f"{path}: " + "; ".join(problems) + " (--force builds the image anyway)")
    for p in problems:
        print(f"mkiso: warning: {path}: {p}")


def boot_data(game_id, title, loader, fst_offset, fst_size, dol_offset):
    """boot.bin, bi2.bin and the apploader: the first 0x2440 + len(loader) bytes."""
    data = bytearray(APPLOADER_OFFSET + len(loader))
    data[0:6] = game_id.encode("ascii")
    struct.pack_into(">I", data, 0x1C, DISC_MAGIC)
    raw_title = title.encode("ascii")[: 0x3E0 - 1]
    data[0x20 : 0x20 + len(raw_title)] = raw_title
    struct.pack_into(">IIII", data, 0x420, dol_offset, fst_offset, fst_size, fst_size)
    struct.pack_into(">I", data, 0x440 + 0x04, 0x01800000)  # bi2: simulated memory size (24 MiB)
    struct.pack_into(">I", data, 0x440 + 0x18, REGIONS[game_id[3]])  # bi2: country code
    data[APPLOADER_OFFSET:] = loader
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dol", required=True, help="main DOL (padded, see dolpad.py)")
    parser.add_argument("--apploader", required=True, help="apploader ELF (port/gc/apploader)")
    parser.add_argument("--rom", required=True, help=f"your Majora's Mask (USA) ROM, stored as /{ROM_DISC_PATH}")
    parser.add_argument("-o", "--out", default="build/gc-n64-us/mm-gc.iso", help="output image")
    parser.add_argument("--id", default=DEFAULT_ID, help=f"6-character game ID (default {DEFAULT_ID})")
    parser.add_argument("--title", default=DEFAULT_TITLE, help="title in the disc header")
    parser.add_argument("--file", action="append", default=[], metavar="DISC_PATH=SRC", help="add a file")
    parser.add_argument("--force", action="store_true", help="accept a ROM that is not Majora's Mask USA 1.0")
    args = parser.parse_args()

    game_id = args.id
    if len(game_id) != 6 or not game_id.isascii() or not game_id.isalnum() or game_id != game_id.upper():
        fail(f"game ID {game_id!r} must be 6 upper-case letters or digits")
    if game_id[3] not in REGIONS:
        fail(f"game ID {game_id!r}: unknown region letter {game_id[3]!r} (E = NTSC-U, P = PAL, J = NTSC-J)")
    if not args.title.isascii():
        fail("the title must be ASCII")

    start = time.monotonic()
    check_rom(args.rom, args.force)
    dol = check_dol(args.dol)
    loader = apploader_blob(*read_apploader(args.apploader))
    files = [(ROM_DISC_PATH, args.rom)]
    for spec in args.file:
        if "=" not in spec:
            fail(f"--file {spec!r} is not DISC_PATH=SRC")
        disc_path, src = spec.split("=", 1)
        if not os.path.isfile(src):
            fail(f"--file {spec!r}: {src} is not a file")
        files.append((disc_path, src))
    root = mkdevdisc.tree_from_files(files)
    mtime = max(os.path.getmtime(p) for p in [args.dol, args.apploader] + [src for _, src in files])

    def system_area(fst_offset, fst_size, blob_offsets):
        return boot_data(game_id, args.title, loader, fst_offset, fst_size, blob_offsets[0])

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    nodes, total, fst_offset, fst_size, blob_offsets = mkdevdisc.build_image(
        root, args.out, system_area, VOLUME_ID, mtime, blobs=[dol], file_align=FILE_ALIGN
    )
    print(f"{args.out}: {total} bytes ({total / (1 << 20):.1f} MiB), game ID {game_id}, "
          f"{time.monotonic() - start:.1f} s")
    print(f"  apploader  0x{APPLOADER_OFFSET:08X}  {len(loader)} bytes")
    print(f"  FST        0x{fst_offset:08X}  {fst_size} bytes")
    print(f"  main DOL   0x{blob_offsets[0]:08X}  {len(dol)} bytes")
    for node in nodes:
        print(f"  file       0x{node.extent * mkdevdisc.SECTOR:08X}  {node.size} bytes  {node.path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
