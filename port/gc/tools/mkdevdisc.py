#!/usr/bin/env python3
"""Build the Dolphin dev disc: a GameCube disc image that carries a copy of the
SD folder as an ISO9660 filesystem.

Dolphin 2609 cannot emulate an SD card on the GameCube (it has no SD adapter
EXI device; its SD card settings only feed the Wii's IOS), so Dolphin runs get
the SD folder through the DVD drive instead: run_dolphin.ps1 inserts this image
with -C Dolphin.Core.DefaultISO=<image>, and the console side mounts it with
libogc's ISO9660 driver as dvd:/, with the same paths as sd:/ on hardware.

Image layout (2048-byte sectors):
  0x0000  GameCube disc header (boot.bin, game ID MMGE00) and bi2.bin
  0x2440  apploader header stub (there is no apploader; Dolphin boots the DOL)
  0x8000  ISO9660 primary volume descriptor (sector 16), terminator (17)
  then    path tables, directories, a GameCube FST of the same files, file data
ISO9660 readers ignore the system area (sectors 0-15), so the GameCube header
fits there. The image ends with 16 spare sectors because libiso9660 always
reads 32 KiB at a time.

build_image() is shared with mkiso.py, which builds the bootable disc image the
same way (a GameCube disc that is also an ISO9660 volume) with an apploader and
the main DOL added.

usage: mkdevdisc.py [SRC_DIR] [OUT_IMAGE] [--if-stale]
"""
import argparse
import os
import struct
import sys
import time

SECTOR = 2048
SYSTEM_AREA = 16 * SECTOR  # ISO9660 leaves sectors 0-15 to the system: the GameCube boot data lives there
GAME_ID = b"MMGE00"
GC_MAGIC = 0xC2339F3D
GC_DISC_MAX = 1459978240
VOLUME_ID = b"MMGCPORT_SD"
DEFAULT_SRC = "/mnt/c/_mmgcport/sdcard"
DEFAULT_OUT = "/mnt/c/_mmgcport/dolphin/mmgcport-dev.iso"


class Node:
    """A directory or file of the image. `path` is the source file (files only)."""

    def __init__(self, name, path, is_dir, parent):
        self.name = name
        self.path = path
        self.is_dir = is_dir
        self.parent = parent if parent is not None else self
        self.children = []
        self.size = 0 if is_dir else os.path.getsize(path)
        self.extent = 0
        self.dirnum = 0


def sectors(nbytes):
    return (nbytes + SECTOR - 1) // SECTOR


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def check_name(name):
    raw = name.encode("ascii", errors="strict")
    if len(raw) > 30 or b";" in raw:
        raise SystemExit(f"mkdevdisc: unsupported file name {name!r} (ASCII, <= 30 chars, no ';')")
    return raw


def scan(path, name, parent):
    node = Node(name, path, True, parent)
    for entry in sorted(os.scandir(path), key=lambda e: e.name.upper()):
        if entry.name.startswith("."):
            continue
        check_name(entry.name)
        if entry.is_dir():
            node.children.append(scan(entry.path, entry.name, node))
        elif entry.is_file():
            node.children.append(Node(entry.name, entry.path, False, node))
    return node


def tree_from_files(files):
    """A tree for (disc_path, source_file) pairs, e.g. ("mmgcport/baserom.z64", "baserom.z64")."""
    root = Node("", None, True, None)
    for disc_path, src in files:
        parts = [p for p in disc_path.split("/") if p]
        if not parts:
            raise SystemExit(f"mkdevdisc: empty disc path for {src}")
        node = root
        for part in parts[:-1]:
            check_name(part)
            child = next((c for c in node.children if c.name == part), None)
            if child is None:
                child = Node(part, None, True, node)
                node.children.append(child)
            elif not child.is_dir:
                raise SystemExit(f"mkdevdisc: {disc_path}: {part} is a file")
            node = child
        check_name(parts[-1])
        if any(c.name.upper() == parts[-1].upper() for c in node.children):
            raise SystemExit(f"mkdevdisc: {disc_path} given twice")
        node.children.append(Node(parts[-1], src, False, node))
    for node in walk(root):
        if node.is_dir:
            node.children.sort(key=lambda c: c.name.upper())
    return root


def walk(node):
    yield node
    for child in node.children:
        if child.is_dir:
            yield from walk(child)
        else:
            yield child


def newest_mtime(root):
    newest = os.path.getmtime(root)
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames + filenames:
            newest = max(newest, os.path.getmtime(os.path.join(dirpath, name)))
    return newest


def iso_ident(node):
    return check_name(node.name) + (b"" if node.is_dir else b";1")


def dir_record(ident, extent, size, is_dir, stamp):
    length = 33 + len(ident) + (1 - len(ident) % 2)
    rec = bytearray(length)
    rec[0] = length
    rec[2:10] = both32(extent)
    rec[10:18] = both32(size)
    rec[18:25] = stamp
    rec[25] = 2 if is_dir else 0
    rec[28:32] = both16(1)
    rec[32] = len(ident)
    rec[33 : 33 + len(ident)] = ident
    return bytes(rec)


def dir_entries(node):
    """(ident, target) pairs in record order: '.', '..', then children."""
    return [(b"\x00", node), (b"\x01", node.parent)] + [(iso_ident(c), c) for c in node.children]


def dir_extent_size(node):
    used = 0
    for ident, _ in dir_entries(node):
        length = 33 + len(ident) + (1 - len(ident) % 2)
        if used % SECTOR + length > SECTOR:
            used += SECTOR - used % SECTOR
        used += length
    return sectors(used) * SECTOR


def path_table_order(root):
    order = [root]
    i = 0
    while i < len(order):
        order.extend(c for c in order[i].children if c.is_dir)
        i += 1
    for num, node in enumerate(order, 1):
        node.dirnum = num
    return order


def path_table(dirs, big_endian):
    out = bytearray()
    fmt = ">" if big_endian else "<"
    for node in dirs:
        ident = b"\x00" if node.parent is node else check_name(node.name)
        out += struct.pack(fmt + "BBIH", len(ident), 0, node.extent, node.parent.dirnum) + ident
        if len(ident) % 2:
            out += b"\x00"
    return bytes(out)


def gc_fst(root):
    """GameCube FST (pre-order entries + string table) for the same files."""
    entries = []
    strings = bytearray()

    def add(node, parent_index):
        index = len(entries)
        name_off = len(strings)
        strings.extend(check_name(node.name) + b"\x00")
        if node.is_dir:
            entries.append([1, name_off, parent_index, 0])
            for child in node.children:
                add(child, index)
            entries[index][3] = len(entries)
        else:
            entries.append([0, name_off, node.extent * SECTOR, node.size])

    entries.append([1, 0, 0, 0])
    for child in root.children:
        add(child, 0)
    entries[0][3] = len(entries)
    out = bytearray()
    for flags, name_off, a, b in entries:
        out += struct.pack(">III", (flags << 24) | name_off, a, b)
    return bytes(out + strings)


def gc_header(fst_offset, fst_size, stamp_text):
    hdr = bytearray(0x2460)
    hdr[0:6] = GAME_ID
    struct.pack_into(">I", hdr, 0x1C, GC_MAGIC)
    title = b"mmgcport dev disc (SD folder image)"
    hdr[0x20 : 0x20 + len(title)] = title
    struct.pack_into(">III", hdr, 0x424, fst_offset, fst_size, fst_size)
    struct.pack_into(">I", hdr, 0x440 + 0x04, 0x01800000)  # bi2: simulated memory size
    struct.pack_into(">I", hdr, 0x440 + 0x18, 1)  # bi2: region NTSC-U
    hdr[0x2440 : 0x2440 + 10] = stamp_text  # apploader date; size 0 = no apploader
    return bytes(hdr)


def pvd(total_sectors, pt_size, l_table, m_table, root, stamp, stamp17, volume_id):
    d = bytearray(SECTOR)
    d[0] = 1
    d[1:6] = b"CD001"
    d[6] = 1
    d[8:40] = b"GAMECUBE".ljust(32)
    d[40:72] = volume_id.ljust(32)
    d[80:88] = both32(total_sectors)
    d[120:124] = both16(1)
    d[124:128] = both16(1)
    d[128:132] = both16(SECTOR)
    d[132:140] = both32(pt_size)
    struct.pack_into("<I", d, 140, l_table)
    struct.pack_into(">I", d, 148, m_table)
    d[156:190] = dir_record(b"\x00", root.extent, dir_extent_size(root), True, stamp)
    d[190:318] = b"".ljust(128)
    d[318:446] = b"MMGCPORT".ljust(128)
    d[446:574] = b"MMGCPORT".ljust(128)
    d[574:702] = b"MMGCPORT MKDEVDISC.PY".ljust(128)
    d[702:813] = b"".ljust(111)
    d[813:830] = stamp17
    d[830:847] = stamp17
    d[847:864] = b"0" * 16 + b"\x00"
    d[864:881] = stamp17
    d[881] = 1
    return bytes(d)


def build_image(root, out, system_area, volume_id, mtime, blobs=(), file_align=SECTOR):
    """Write a GameCube disc image that is also an ISO9660 volume holding the files under `root`.

    system_area(fst_offset, fst_size, blob_offsets) returns the bytes of 0x0000-0x7FFF (boot.bin,
    bi2.bin, the apploader). `blobs` are byte strings placed after the FST, each at a `file_align`
    boundary (the main DOL); `file_align` (a multiple of SECTOR) also aligns every file.
    Returns (files, image size, fst offset, fst size, blob offsets)."""
    if file_align % SECTOR:
        raise SystemExit(f"mkdevdisc: file alignment {file_align} is not a multiple of {SECTOR}")
    align = file_align // SECTOR
    dirs = path_table_order(root)
    t = time.gmtime(mtime)
    stamp = bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])
    stamp17 = time.strftime("%Y%m%d%H%M%S00", t).encode() + b"\x00"

    # Layout: PVD 16, terminator 17, L table, M table, directories, FST, blobs, files.
    pt_size = len(path_table(dirs, False))
    l_table = 18
    m_table = l_table + sectors(pt_size)
    cur = m_table + sectors(pt_size)
    for node in dirs:
        node.extent = cur
        cur += dir_extent_size(node) // SECTOR
    fst_size = len(gc_fst(root))
    fst_sector = cur
    cur += sectors(fst_size)
    blob_sectors = []
    for blob in blobs:
        cur = (cur + align - 1) // align * align
        blob_sectors.append(cur)
        cur += sectors(len(blob))
    files = [n for n in walk(root) if not n.is_dir]
    for node in files:
        cur = (cur + align - 1) // align * align
        node.extent = cur if node.size else 0
        cur += sectors(node.size)
    total = (cur + 16 + 15) // 16 * 16
    if total * SECTOR > GC_DISC_MAX:
        raise SystemExit(f"mkdevdisc: {total * SECTOR} bytes does not fit on a GameCube disc")
    blob_offsets = [s * SECTOR for s in blob_sectors]
    head = system_area(fst_sector * SECTOR, fst_size, blob_offsets)
    if len(head) > SYSTEM_AREA:
        raise SystemExit(f"mkdevdisc: the boot data ({len(head)} bytes) does not fit below 0x{SYSTEM_AREA:X}")

    tmp = out + ".tmp"
    with open(tmp, "wb") as f:

        def put(sector, data):
            f.seek(sector * SECTOR)
            f.write(data)

        put(0, head)
        put(16, pvd(total, pt_size, l_table, m_table, root, stamp, stamp17, volume_id))
        put(17, b"\xffCD001\x01".ljust(SECTOR, b"\x00"))
        put(l_table, path_table(dirs, False))
        put(m_table, path_table(dirs, True))
        for node in dirs:
            data = bytearray()
            for ident, target in dir_entries(node):
                size = dir_extent_size(target) if target.is_dir else target.size
                rec = dir_record(ident, target.extent, size, target.is_dir, stamp)
                if len(data) % SECTOR + len(rec) > SECTOR:
                    data += bytes(SECTOR - len(data) % SECTOR)
                data += rec
            put(node.extent, bytes(data))
        put(fst_sector, gc_fst(root))
        for sector, blob in zip(blob_sectors, blobs):
            put(sector, blob)
        for node in files:
            f.seek(node.extent * SECTOR)
            with open(node.path, "rb") as src_file:
                while True:
                    chunk = src_file.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
        f.truncate(total * SECTOR)
    os.replace(tmp, out)
    return files, total * SECTOR, fst_sector * SECTOR, fst_size, blob_offsets


def build(src, out):
    root = scan(src, "", None)
    mtime = newest_mtime(src)
    date = time.strftime("%Y/%m/%d", time.gmtime(mtime)).encode()

    def system_area(fst_offset, fst_size, blob_offsets):
        return gc_header(fst_offset, fst_size, date)

    files, size, _, _, _ = build_image(root, out, system_area, VOLUME_ID, mtime)
    return files, size


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("src", nargs="?", default=DEFAULT_SRC, help=f"SD folder (default {DEFAULT_SRC})")
    parser.add_argument("out", nargs="?", default=DEFAULT_OUT, help=f"disc image (default {DEFAULT_OUT})")
    parser.add_argument("--if-stale", action="store_true", help="do nothing if the image is newer than the folder")
    args = parser.parse_args()

    if not os.path.isdir(args.src):
        raise SystemExit(f"mkdevdisc: {args.src} is not a directory")
    if args.if_stale and os.path.exists(args.out) and os.path.getmtime(args.out) >= newest_mtime(args.src):
        print(f"{args.out}: up to date")
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    files, size = build(args.src, args.out)
    print(f"{args.out}: {size} bytes, {len(files)} files from {args.src}")
    for node in files:
        rel = os.path.relpath(node.path, args.src).replace(os.sep, "/")
        print(f"  dvd:/{rel}  {node.size} bytes @ 0x{node.extent * SECTOR:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
