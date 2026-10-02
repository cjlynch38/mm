#!/usr/bin/env python3
"""
Turns the "audcap:" lines of an AUD_CAPTURE build's log (port/gc/audio/aud_task.c) into a capture file
for the audio host test and benchmark (see the Makefile next to this file):

    port/gc/tests/audio_host/audcap.py <gecko.log> <out.bin>

The file holds game data from the ROM (samples, codebooks): keep it out of the repository.

Format (big-endian u32s): "AUDCAP1\\0", the number of tasks, aspMainData (0x2E0 bytes), then per task:
the list's RAM address and size, the dram_stack address, DMEM before the task (4 KiB), the OSTask
(64 bytes), the number of RAM pieces, and each piece as address, size, bytes.
"""
import re
import struct
import sys

LINE = re.compile(r"audcap: (\w+)(?: (.*))?$")
UDATA_SIZE = 0x2E0
DMEM_SIZE = 0x1000
HDR_SIZE = 0x40


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    udata = bytearray()
    tasks = []
    cur = None
    with open(sys.argv[1], errors="replace") as f:
        for line in f:
            m = LINE.search(line.rstrip("\r\n"))
            if not m:
                continue
            kind, rest = m.group(1), (m.group(2) or "").split()
            if kind == "task":
                cur = {"alist": int(rest[1], 16), "size": int(rest[2]), "stack": int(rest[3], 16),
                       "dmem": bytearray(), "hdr": bytearray(), "ram": [], "end": False}
                tasks.append(cur)
            elif kind == "end":
                if cur is not None:
                    cur["end"] = True
                cur = None
            elif kind in ("udata", "dmem", "hdr", "ram"):
                addr = int(rest[0], 16)
                data = bytes.fromhex(rest[1])
                if kind == "udata":
                    udata += data
                elif cur is None:
                    sys.exit("audcap.py: %s line outside a task" % kind)
                elif kind == "ram":
                    last = cur["ram"][-1] if cur["ram"] else None
                    if last is not None and last[0] + len(last[1]) == addr and len(last[1]) % 128 == 0:
                        last[1].extend(data)  # continuation line of the same piece
                    else:
                        cur["ram"].append((addr, bytearray(data)))
                else:
                    cur[kind] += data
    # Drop a task whose log was cut off
    tasks = [t for t in tasks if t["end"] and len(t["dmem"]) == DMEM_SIZE and len(t["hdr"]) == HDR_SIZE]
    if len(udata) != UDATA_SIZE or not tasks:
        sys.exit("audcap.py: no complete capture in %s" % sys.argv[1])

    out = bytearray(b"AUDCAP1\0")
    out += struct.pack(">I", len(tasks))
    out += udata
    for t in tasks:
        out += struct.pack(">III", t["alist"], t["size"], t["stack"])
        out += t["dmem"] + t["hdr"]
        out += struct.pack(">I", len(t["ram"]))
        for addr, data in t["ram"]:
            out += struct.pack(">II", addr, len(data)) + data
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    print("%s: %d tasks, %d bytes" % (sys.argv[2], len(tasks), len(out)))


if __name__ == "__main__":
    main()
