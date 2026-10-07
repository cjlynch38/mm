#!/usr/bin/env python3
"""Builds FAT12/16/32 disc images for the sd_host test of port/gc/ogc/fat_map.c, and a manifest of their contents.

Pure Python: WSL has neither mkfs.fat nor mtools. Every image is a sparse file. The volumes hold the same kind of tree
(long and 8.3 names, upper/lower/mixed case, Latin-1, CJK and non-BMP names, nested and multi-cluster directories,
files of 0 bytes to several MB) with clusters handed out interleaved and with random jumps, so files and directories
are fragmented and chains also run backwards. Directories also hold deleted entries, a volume label, an orphan long
name, a long name with a missing part and an entry after the end marker, none of which may be found.

File contents come from a generator that the C test repeats. One 64 KB block for all files:
    block[i] for i < 65536: x = (x * 1103515245 + 12345) mod 2^32 from x = 0x2545F491, block[i] = x >> 24
and for a file with seed s, the byte at offset n is
    block[(n + s * 7919) % 65536] ^ ((n // 65536 + s) % 256)

Manifest (the path is the rest of the line, it may hold spaces):
    image <file> ok <fat type> <bytes per cluster>
    image <file> fail <words of the expected error>
    file <size> <seed> <runs> <path>   a file: its size, content seed and number of runs of consecutive sectors
    alias <path>                       another path naming the file above
    missing <path>                     a lookup that must fail

    mkfat.py <output directory>
"""
import os
import random
import struct
import sys

SS = 512
LCG_MUL = 1103515245
LCG_ADD = 12345

XOR_TABLES = [bytes(v ^ k for v in range(256)) for k in range(256)]


def _block():
    x = 0x2545F491
    out = bytearray(65536)
    for i in range(65536):
        x = (x * LCG_MUL + LCG_ADD) & 0xFFFFFFFF
        out[i] = x >> 24
    return bytes(out)


BLOCK = _block()


def content(seed, size):
    rot = (seed * 7919) % 65536
    block = BLOCK[rot:] + BLOCK[:rot]
    parts = [block.translate(XOR_TABLES[(k + seed) & 0xFF]) for k in range((size + 65535) // 65536)]
    return b"".join(parts)[:size]


# ------------------------------------------------------------------------------------------------
# Names
# ------------------------------------------------------------------------------------------------

SFN_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")


def sfn_text(sfn):
    """The 11-byte 8.3 name as a path component"""
    base = sfn[:8].decode("latin-1").rstrip(" ")
    ext = sfn[8:].decode("latin-1").rstrip(" ")
    return base + ("." + ext if ext else "")


def plain_sfn(name):
    """(11-byte 8.3 name, case flags) when `name` needs no long name, else None"""
    if name.count(".") > 1:
        return None
    base, dot, ext = name.partition(".")
    if not base or len(base) > 8 or len(ext) > 3 or (dot and not ext):
        return None
    flags = 0
    for part, bit in ((base, 0x08), (ext, 0x10)):
        if any(c.upper() not in SFN_CHARS for c in part):
            return None
        letters = [c for c in part if c.isalpha()]
        if letters and all(c.islower() for c in letters):
            flags |= bit
        elif letters and not all(c.isupper() for c in letters):
            return None
    return (base.upper().ljust(8) + ext.upper().ljust(3)).encode("ascii"), flags


def tail_sfn(name, used):
    """An 8.3 name with a numeric tail for a long name, unique among `used`"""
    base, ext = name, ""
    if "." in name[1:]:
        base, _, ext = name.rpartition(".")
    keep = lambda s: "".join(c for c in s.upper() if c in SFN_CHARS)
    b = keep(base) or "X"
    e = keep(ext)[:3]
    for n in range(1, 100000):
        tail = "~%d" % n
        cand = (b[: 8 - len(tail)] + tail).ljust(8) + e.ljust(3)
        if cand not in used:
            used.add(cand)
            return cand.encode("ascii")
    raise RuntimeError("no 8.3 name left")


def sfn_sum(sfn):
    s = 0
    for c in sfn:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_entries(name, sfn, ordinal_first=None):
    """The long name entries for `name`, last part first, as they are stored"""
    units = name.encode("utf-16-le")
    chars = [units[i : i + 2] for i in range(0, len(units), 2)]
    assert 0 < len(chars) <= 255
    count = (len(chars) + 12) // 13
    if len(chars) % 13:
        chars.append(b"\0\0")
    chars += [b"\xff\xff"] * (count * 13 - len(chars))
    check = sfn_sum(sfn)
    out = []
    for ordinal in range(count, 0, -1):
        part = chars[(ordinal - 1) * 13 : ordinal * 13]
        e = bytearray(32)
        e[0] = ordinal | (0x40 if ordinal == count else 0)
        e[1:11] = b"".join(part[0:5])
        e[11] = 0x0F
        e[13] = check
        e[14:26] = b"".join(part[5:11])
        e[28:32] = b"".join(part[11:13])
        out.append(bytes(e))
    return out


# ------------------------------------------------------------------------------------------------
# Tree
# ------------------------------------------------------------------------------------------------


class Node:
    def __init__(self, name, parent, size=None, seed=0):
        self.name = name
        self.parent = parent
        self.is_dir = size is None
        self.size = size or 0
        self.seed = seed
        self.children = []
        self.clusters = []
        self.junk = []  # extra raw entries: list of (position, [32-byte entries])
        self.after_end = []  # entries written after the end marker
        if parent is not None:
            parent.children.append(self)

    def path(self):
        if self.parent is None:
            return ""
        return self.parent.path() + "/" + self.name

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()


EOC = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}


class Volume:
    def __init__(self, fat_type, spc, total, rng, reserved=None, nfats=2, root_entries=512, jump=0.0,
                 interleave=False, foreign=0.0, fat32_high_bits=False):
        self.type = fat_type
        self.spc = spc
        self.total = total
        self.rng = rng
        self.reserved = reserved or (32 if fat_type == 32 else 1)
        self.nfats = nfats
        self.root_entries = 0 if fat_type == 32 else root_entries
        self.root_secs = self.root_entries * 32 // SS
        self.jump = jump
        self.interleave = interleave
        self.fat32_high_bits = fat32_high_bits
        fatsz = 1
        while True:
            nclst = (total - self.reserved - nfats * fatsz - self.root_secs) // spc
            entries = nclst + 2
            need = {32: entries * 4, 16: entries * 2, 12: entries * 3 // 2 + (entries & 1)}[fat_type]
            need = (need + SS - 1) // SS
            if need <= fatsz:
                break
            fatsz = need
        self.fatsz = fatsz
        self.nclst = nclst
        self.entries = nclst + 2
        kind = 12 if nclst <= 0xFF5 else 16 if nclst <= 0xFFF5 else 32 if nclst <= 0x0FFFFFF5 else 0
        assert kind == fat_type, "volume of %d sectors with %d sectors per cluster is FAT%d, not FAT%d" % (
            total, spc, kind, fat_type)
        self.data = self.reserved + nfats * fatsz + self.root_secs
        self.fat = [0] * self.entries
        self.fat[0] = 0x0FFFFFF8 if fat_type == 32 else (0xFFF8 if fat_type == 16 else 0xFF8)
        self.fat[1] = EOC[fat_type]
        self.used = bytearray(self.entries)
        self.used[0] = self.used[1] = 1
        self.cursor = 2
        # Clusters of files nobody lists, so the free space has holes
        if foreign:
            c = 2
            while c < self.entries:
                if rng.random() < foreign:
                    run = rng.randint(1, 16)
                    for k in range(c, min(c + run, self.entries)):
                        self.used[k] = 1
                        self.fat[k] = EOC[fat_type]
                    c += run
                c += rng.randint(1, 24)

    def cluster_bytes(self):
        return self.spc * SS

    def take(self):
        if self.jump and self.rng.random() < self.jump:
            self.cursor = self.rng.randrange(2, self.entries)
        for _ in range(self.entries):
            c = self.cursor
            self.cursor = c + 1 if c + 1 < self.entries else 2
            if not self.used[c]:
                self.used[c] = 1
                return c
        raise RuntimeError("the volume is full")

    def allocate(self, nodes):
        need = {}
        for n in nodes:
            count = (n.alloc_bytes + self.cluster_bytes() - 1) // self.cluster_bytes()
            if count:
                need[n] = count
        order = list(need)
        if not self.interleave:
            for n in order:
                n.clusters = [self.take() for _ in range(need[n])]
            return
        pending = order[:]
        while pending:
            n = self.rng.choice(pending)
            k = min(need[n] - len(n.clusters), self.rng.choice([1, 1, 2, 3, 8, 32]))
            n.clusters += [self.take() for _ in range(k)]
            if len(n.clusters) == need[n]:
                pending.remove(n)

    def link(self, clusters):
        for a, b in zip(clusters, clusters[1:]):
            v = b
            if self.type == 32 and self.fat32_high_bits and self.rng.random() < 0.5:
                v |= self.rng.randrange(1, 16) << 28  # reserved bits that readers must ignore
            self.fat[a] = v
        if clusters:
            self.fat[clusters[-1]] = EOC[self.type]

    def fat_bytes(self):
        out = bytearray(self.fatsz * SS)
        if self.type == 32:
            for i, v in enumerate(self.fat):
                struct.pack_into("<I", out, i * 4, v)
        elif self.type == 16:
            for i, v in enumerate(self.fat):
                struct.pack_into("<H", out, i * 2, v)
        else:
            for i, v in enumerate(self.fat):
                o = i + i // 2
                if i & 1:
                    out[o] = (out[o] & 0x0F) | ((v << 4) & 0xF0)
                    out[o + 1] = (v >> 4) & 0xFF
                else:
                    out[o] = v & 0xFF
                    out[o + 1] = (out[o + 1] & 0xF0) | ((v >> 8) & 0x0F)
        return out


def runs_of(clusters):
    runs = 0
    for i, c in enumerate(clusters):
        if i == 0 or c != clusters[i - 1] + 1:
            runs += 1
    return runs


def sfn_entry(sfn, attr, flags, cluster, size, fat_type, rng):
    e = bytearray(32)
    e[0:11] = sfn
    e[11] = attr
    e[12] = flags
    struct.pack_into("<HHHHH", e, 14, 0x6000, 0x5946, 0x5946, (cluster >> 16) if fat_type == 32 else 0, 0x6000)
    if fat_type != 32 and rng.random() < 0.5:
        struct.pack_into("<H", e, 20, rng.randrange(1, 0x10000))  # FAT12/16 ignore the high word
    struct.pack_into("<H", e, 24, 0x5946)
    struct.pack_into("<H", e, 26, cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


def name_entries(vol, dirnode, child, used):
    """Entries for one child: its long name (if it needs one) and its 8.3 entry; returns (entries, sfn)"""
    plain = plain_sfn(child.name)
    attr = 0x10 if child.is_dir else 0x20
    cluster = child.clusters[0] if child.clusters else 0
    if plain is not None and plain[0].decode() not in used:
        sfn, flags = plain
        used.add(sfn.decode())
        return [sfn_entry(sfn, attr, flags, cluster, child.size, vol.type, vol.rng)], sfn
    sfn = tail_sfn(child.name, used)
    return lfn_entries(child.name, sfn) + [sfn_entry(sfn, attr, 0, cluster, child.size, vol.type, vol.rng)], sfn


def dir_entries(vol, node):
    """Every 32-byte entry of a directory, in order (end marker not included)"""
    used = set()
    out = []
    if node.parent is not None:
        out.append(sfn_entry(b".          ", 0x10, 0, node.clusters[0], 0, vol.type, vol.rng))
        parent = node.parent.clusters[0] if node.parent.parent is not None else 0
        out.append(sfn_entry(b"..         ", 0x10, 0, parent, 0, vol.type, vol.rng))
    node.sfns = {}
    junk = {}
    for pos, entries in node.junk:
        junk.setdefault(pos, []).extend(entries)
    for i, child in enumerate(node.children):
        out += junk.get(i, [])
        entries, sfn = name_entries(vol, node, child, used)
        node.sfns[child] = sfn
        out += entries
    out += junk.get(len(node.children), [])
    return out


# Names add_junk plants in every directory (and the volume label in the root), none of which may be found
MISSING_NAMES = ["deleted file.bin", "DELETE~1.BIN", "orphan long name.txt",
                 "a broken long name that takes three entries.bin", "after the end.bin", "AFTERT~1.BIN"]


def add_junk(vol, node):
    """Entries that must never be found: a deleted file, an orphan long name, a broken long name, a volume label
    and an entry after the end marker"""
    rng = vol.rng
    pos = lambda: rng.randint(0, len(node.children))
    # A deleted file with a long name: every entry starts with 0xE5
    sfn = b"DELETE~1BIN"
    entries = [b"\xe5" + e[1:] for e in lfn_entries("deleted file.bin", sfn)]
    entries.append(b"\xe5" + sfn_entry(sfn, 0x20, 0, 2, 1234, vol.type, rng)[1:])
    node.junk.append((pos(), entries))
    # A long name whose checksum belongs to another 8.3 name, before an entry with only an 8.3 name
    real = b"REAL0001TXT"
    entries = lfn_entries("orphan long name.txt", b"OTHER~1 TXT")
    entries.append(sfn_entry(real, 0x20, 0, 0, 0, vol.type, rng))
    node.junk.append((pos(), entries))
    # A long name with its middle part missing
    lfn = lfn_entries("a broken long name that takes three entries.bin", b"BROKEN~1BIN")
    entries = [lfn[0], lfn[2], sfn_entry(b"BROKEN~1BIN", 0x20, 0, 0, 0, vol.type, rng)]
    node.junk.append((pos(), entries))
    if node.parent is None:
        node.junk.append((pos(), [sfn_entry(b"TESTVOLUME ", 0x08, 0, 0, 0, vol.type, rng)]))
    # Found only if a reader went past the end marker
    node.after_end = lfn_entries("after the end.bin", b"AFTERT~1BIN") + [
        sfn_entry(b"AFTERT~1BIN", 0x20, 0, 0, 0, vol.type, rng)]


def build_tree(vol, big, seeds):
    """The standard tree; `big` scales the large files"""
    cb = vol.cluster_bytes()
    root = Node("", None)

    def f(parent, name, size):
        return Node(name, parent, size, next(seeds))

    mm = Node("mmgcport", root)
    f(mm, "baserom.z64", big)
    f(mm, "mm-gc.iso", big + 3 * cb + 77)
    f(mm, "mm.fla", 0x20000 if big >= 0x20000 else cb * 2)
    f(mm, "mm.fla.bak", 0x20000 if big >= 0x20000 else cb * 2)
    f(mm, "log-autoplay.txt", 5000)
    f(mm, "Mixed Case Name.Data", cb + 1)
    f(root, "README.TXT", 700)
    games = Node("Games", root)
    zelda = Node("Zelda - Majora's Mask (USA)", games)
    f(zelda, "mm-gc.iso", big // 2 + 513)
    f(zelda, "a very long file name that needs more than two long name entries to hold it, 80 chars.bin",
      3 * cb - 1)
    d = root
    for name in "abcde":
        d = Node(name, d)
    f(d, "deep.bin", 2 * cb + 3)
    many = Node("many", root)
    for i in range(70):
        f(many, ("file%03d.bin" % i) if i % 3 else ("Long file name number %d.dat" % i), (i * 997) % (3 * cb) + 1)
    uni = Node("unicode", root)
    f(uni, "Zelda Ñ ü.bin", 1000)
    f(uni, "日本語のファイル.bin", 2000)
    f(uni, "emoji \U0001F600.bin", 3000)
    sizes = Node("sizes", root)
    for size in sorted({0, 1, 511, 512, 513, cb - 1, cb, cb + 1, 2 * cb, 4 * cb + 5}):
        f(sizes, "s%d.bin" % size, size)
    return root


def write_image(path, vol, root, mbr, foreign_fat2=False):
    """Allocate, lay out and write the volume (behind an MBR when mbr is (slot, start, decoy slot or None))"""
    rng = vol.rng
    for node in root.walk():
        if node.is_dir:
            add_junk(vol, node)
    nodes = list(root.walk())
    # Directory sizes need their entries first (cluster numbers do not change entry counts)
    for node in nodes:
        if node.is_dir:
            for c in node.children:
                c.clusters = [2]  # placeholder, for the entry count
            node.alloc_bytes = (len(dir_entries(vol, node)) + 1 + len(node.after_end)) * 32
            if node.parent is None and vol.type != 32:
                node.alloc_bytes = 0
        else:
            node.alloc_bytes = node.size
    for node in nodes:
        node.clusters = []
    if vol.type != 32:
        assert len(dir_entries(vol, root)) + 1 + len(root.after_end) <= vol.root_entries, "the root directory is full"
    vol.allocate([n for n in nodes if n.alloc_bytes])
    for node in nodes:
        vol.link(node.clusters)

    base = 0 if mbr is None else mbr[1]
    size = (base + vol.total) * SS
    with open(path, "wb") as out:
        out.truncate(size)

        def put(sector, data):
            out.seek((base + sector) * SS)
            out.write(data)

        cb = vol.cluster_bytes()

        def put_clusters(clusters, data):
            for i, c in enumerate(clusters):
                chunk = data[i * cb : (i + 1) * cb]
                if chunk:
                    put(vol.data + (c - 2) * vol.spc, chunk)

        # Boot sector
        bs = bytearray(SS)
        bs[0:3] = b"\xEB\x58\x90" if vol.type == 32 else b"\xEB\x3C\x90"
        bs[3:11] = b"MSWIN4.1"
        small = vol.type != 32 and vol.total < 0x10000
        struct.pack_into("<HBHBHHBHHHII", bs, 11, SS, vol.spc, vol.reserved, vol.nfats, vol.root_entries,
                         vol.total if small else 0, 0xF8, 0 if vol.type == 32 else vol.fatsz, 63, 255, base,
                         0 if small else vol.total)
        if vol.type == 32:
            struct.pack_into("<IHHIHH", bs, 36, vol.fatsz, 0, 0, root.clusters[0], 1, 6)
            bs[64] = 0x80
            bs[66] = 0x29
            struct.pack_into("<I", bs, 67, 0x12345678)
            bs[71:82] = b"TESTVOLUME "
            bs[82:90] = b"FAT32   "
        else:
            bs[36] = 0x80
            bs[38] = 0x29
            struct.pack_into("<I", bs, 39, 0x12345678)
            bs[43:54] = b"TESTVOLUME "
            bs[54:62] = b"FAT12   " if vol.type == 12 else b"FAT16   "
        bs[510:512] = b"\x55\xAA"
        put(0, bs)
        if vol.type == 32:
            fsinfo = bytearray(SS)
            struct.pack_into("<I", fsinfo, 0, 0x41615252)
            struct.pack_into("<III", fsinfo, 484, 0x61417272, 0xFFFFFFFF, 0xFFFFFFFF)
            fsinfo[510:512] = b"\x55\xAA"
            put(1, fsinfo)
            put(6, bs)
        fat = vol.fat_bytes()
        for i in range(vol.nfats):
            if i == 1 and foreign_fat2:
                fat = rng.randbytes(len(fat))  # only the first FAT counts
            put(vol.reserved + i * vol.fatsz, fat)
        # Directories and files
        for node in nodes:
            if node.is_dir:
                entries = dir_entries(vol, node)
                data = b"".join(entries) + bytes(32) + b"".join(node.after_end)
                if node.parent is None and vol.type != 32:
                    put(vol.reserved + vol.nfats * vol.fatsz, data)
                else:
                    put_clusters(node.clusters, data)
            else:
                put_clusters(node.clusters, content(node.seed, node.size))
        # MBR
        if mbr is not None:
            slot, start, decoy = mbr
            m = bytearray(SS)
            m[0:3] = b"\xFA\x33\xC0"
            ptype = {12: 0x01, 16: 0x0E, 32: 0x0C}[vol.type]
            struct.pack_into("<B3sB3sII", m, 446 + 16 * slot, 0x80, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff", start,
                             vol.total)
            if decoy is not None:
                # A partition whose boot sector is valid but not FAT (an NTFS-like one): skipped
                dstart = 63
                struct.pack_into("<B3sB3sII", m, 446 + 16 * decoy, 0x00, b"\xfe\xff\xff", 0x07, b"\xfe\xff\xff",
                                 dstart, 1)
                d = bytearray(SS)
                d[0:3] = b"\xEB\x52\x90"
                d[3:11] = b"NTFS    "
                struct.pack_into("<HB", d, 11, SS, 8)
                d[510:512] = b"\x55\xAA"
                out.seek(dstart * SS)
                out.write(d)
            m[510:512] = b"\x55\xAA"
            out.seek(0)
            out.write(m)

    # Manifest lines
    lines = []
    for node in nodes:
        if node.is_dir:
            continue
        p = node.path()
        lines.append("file %d %d %d %s" % (node.size, node.seed, runs_of(node.clusters), p))
        for alias in aliases(node, rng):
            lines.append("alias " + alias)
    for node in nodes:
        if node.is_dir:
            for name in MISSING_NAMES + (["TESTVOLUME"] if node.parent is None else []):
                lines.append("missing " + node.path() + "/" + name)
            if node.parent is not None:
                lines.append("missing " + node.path())  # a directory is not a file
    lines.append("missing /mmgcport/baserom.z64/x")
    lines.append("missing /no/such/dir/file.bin")
    lines.append("missing /mmgcport/baserom.z6")
    return lines


def aliases(node, rng):
    """Other spellings of the file's path that FatFs accepts"""
    parts = node.path().split("/")[1:]
    out = []
    swapped = "/" + "/".join(p.swapcase() for p in parts)
    if swapped != node.path():
        out.append(swapped)
    # The 8.3 name of the file (and of its directories) instead of the long one
    sfn_parts = []
    n = node
    while n.parent is not None:
        sfn_parts.append(sfn_text(n.parent.sfns[n]))
        n = n.parent
    sfn_path = "/" + "/".join(reversed(sfn_parts))
    if sfn_path != node.path():
        out.append(sfn_path)
    if len(parts) > 1:
        out.append("\\".join([""] + parts))
        out.append("//" + parts[0] + "/./" + "/".join(parts[1:]))
        out.append("/" + parts[0] + "/../" + "/".join(parts))
    out.append("/../" + "/".join(parts))
    out.append(node.path() + ".")
    return [a for i, a in enumerate(out) if a not in out[:i]]


# ------------------------------------------------------------------------------------------------
# Images
# ------------------------------------------------------------------------------------------------


def image(outdir, name, fat_type, spc, total, big, mbr=None, seed=1, **kw):
    rng = random.Random(seed)
    vol = Volume(fat_type, spc, total, rng, **kw)
    seeds = iter(range(seed * 1000 + 1, seed * 1000 + 1000))
    root = build_tree(vol, big, seeds)
    foreign_fat2 = kw.get("nfats", 2) == 2 and seed % 2 == 0
    path = os.path.join(outdir, name)
    lines = write_image(path, vol, root, mbr, foreign_fat2)
    return ["image %s ok %d %d" % (name, fat_type, vol.cluster_bytes())] + lines


def bad_image(outdir, name, sectors, words):
    path = os.path.join(outdir, name)
    with open(path, "wb") as out:
        out.truncate(64 * SS)
        for sector, data in sectors:
            out.seek(sector * SS)
            out.write(data)
    return ["image %s fail %s" % (name, words)]


def boot_sector(jump=b"\xEB\x58\x90", oem=b"MSWIN4.1", bps=SS, sig=True, fat32=True):
    bs = bytearray(SS)
    bs[0:3] = jump
    bs[3:11] = oem
    struct.pack_into("<HBHBHHBH", bs, 11, bps, 8, 32, 2, 0, 0, 0xF8, 0)
    struct.pack_into("<I", bs, 32, 0x100000)
    struct.pack_into("<I", bs, 36, 1000)
    struct.pack_into("<I", bs, 44, 2)
    if fat32:
        bs[82:90] = b"FAT32   "
    if sig:
        bs[510:512] = b"\x55\xAA"
    return bytes(bs)


def mbr_sector(entries):
    m = bytearray(SS)
    m[0:3] = b"\xFA\x33\xC0"
    for slot, ptype, start in entries:
        struct.pack_into("<B3sB3sII", m, 446 + 16 * slot, 0, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff", start, 1000)
    m[510:512] = b"\x55\xAA"
    return bytes(m)


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    lines = []
    # FAT12: 512-byte and 2 KB clusters, interleaved and fragmented, with and without an MBR
    lines += image(outdir, "fat12_512.img", 12, 1, 3200, 300 * 1024, seed=1, root_entries=224, jump=0.05,
                   interleave=True, foreign=0.1)
    lines += image(outdir, "fat12_2k_mbr.img", 12, 4, 16000, 1024 * 1024, mbr=(0, 63, None), seed=2, jump=0.03,
                   interleave=True)
    # FAT16: 2 KB clusters, contiguous; 32 KB clusters behind an MBR whose slot 0 is empty and slot 1 a decoy
    lines += image(outdir, "fat16_2k.img", 16, 4, 70000, 3 * 1024 * 1024, seed=3, nfats=1)
    lines += image(outdir, "fat16_32k_mbr.img", 16, 64, 300000, 4 * 1024 * 1024, mbr=(2, 2048, 1), seed=4,
                   jump=0.02, interleave=True, foreign=0.2)
    # FAT32: 512 B clusters (directories of many clusters), 4 KB behind an MBR at 8192 (an SD card's layout),
    # 32 KB behind a decoy partition, 64 KB without an MBR
    lines += image(outdir, "fat32_512.img", 32, 1, 70000, 2 * 1024 * 1024, seed=5, jump=0.05, interleave=True,
                   foreign=0.1, fat32_high_bits=True)
    lines += image(outdir, "fat32_4k_mbr.img", 32, 8, 560000, 6 * 1024 * 1024, mbr=(0, 8192, None), seed=6,
                   jump=0.02, interleave=True, foreign=0.15, fat32_high_bits=True)
    lines += image(outdir, "fat32_32k_mbr.img", 32, 64, 4300000, 8 * 1024 * 1024, mbr=(1, 8192, 0), seed=7,
                   interleave=True)
    lines += image(outdir, "fat32_64k.img", 32, 128, 8500000, 8 * 1024 * 1024, seed=8, jump=0.1, interleave=True,
                   foreign=0.3)
    # Volumes fat_map_mount must refuse
    exfat = bytearray(boot_sector(jump=b"\xEB\x76\x90", oem=b"EXFAT   ", bps=0, fat32=False))
    lines += bad_image(outdir, "bad_exfat.img", [(0, bytes(exfat))], "exFAT")
    lines += bad_image(outdir, "bad_exfat_mbr.img", [(0, mbr_sector([(0, 0x07, 8)])), (8, bytes(exfat))], "exFAT")
    lines += bad_image(outdir, "bad_4k_sectors.img", [(0, boot_sector(bps=4096))], "sector size")
    lines += bad_image(outdir, "bad_no_signature.img", [(0, b"\x01\x02\x03" * 100)], "neither")
    lines += bad_image(outdir, "bad_mbr_no_fat.img", [(0, mbr_sector([(0, 0x0C, 8), (1, 0x0C, 9000)])),
                                                       (8, boot_sector(fat32=False, sig=True)[:3] + bytes(509))],
                       "no FAT volume")
    with open(os.path.join(outdir, "manifest.txt"), "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
