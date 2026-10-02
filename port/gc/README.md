# GameCube port

Native GameCube build of Majora's Mask: the decomp's C source compiled for the
GameCube's PowerPC CPU with devkitPPC and libogc. This is not an emulator.

## Status

Verified in Dolphin 2609. Real-hardware testing (PicoBoot + Swiss + SD2SP2) is pending for
everything past Milestone 0.

| Milestone | State |
|---|---|
| 0. Toolchain | Done; a test DOL runs on hardware (SD2SP2 read/write verified) |
| 1. Matching N64 build | Done |
| 2. Main loop on GameCube | Done: libultra on libogc threads; title → File Select at the N64's 20 fps |
| 3. Renderer | Done (first pass): F3DZEX2 and S2DEX2 display lists interpreted on the CPU and drawn with GX. The N64 logo, title screen, attract cutscenes, gameplay and HUD render, with framebuffer effects (motion blur, VisMono). |
| 4. Audio | Done: MM's audio microcode runs on the CPU, bit-exact against the real microcode, with AI DMA output at 32 kHz |
| 5. Disc and memory | Done: bootable disc image built locally from your ROM with our own apploader; hot ROM data is cached in ARAM; saves go to SD or the memory card |
| 6. Polish | Not started: controller mapping review, performance on hardware, hi-res framebuffer (Bombers' Notebook) |

`port/gc/tools/run_host_tests.sh` builds and runs every host test and test DOL.

## Requirements

- Linux (WSL2 works) with the normal decomp requirements (see the top-level README)
- devkitPro with devkitPPC and libogc (`gamecube-dev`); `DEVKITPRO` defaults to `/opt/devkitpro`
- Your own N64 US Majora's Mask ROM (MD5 `2a0a8acb61538235bc1094d297fb6556`)

## Building

```bash
make init            # once: extracts assets and builds the matching N64 ROM
make -f Makefile.gc -j$(nproc)
make -f Makefile.gc sizes   # per-area code size report
```

The GameCube build reuses outputs of the N64 build (generated headers, asset
symbols), so the N64 build must exist first.

## Rules

- No ROM data or extracted assets are ever committed.
- The N64 build must keep matching. `port/gc/tools/check_n64.sh` verifies this;
  run it before every push.
- Changes in `src/` and `include/` go under `#ifdef TARGET_GC` (platform changes)
  or `#ifdef AVOID_UB` (undefined-behaviour fixes). Never change the N64 side.
- GameCube-only code lives in `port/gc/`, not `src/`.

## Layout

| Path | Contents |
|---|---|
| `Makefile.gc` | GameCube build |
| `port/gc/include/` | Headers for the port (`gc_prelude.h` is force-included in game code) |
| `port/gc/ultra/` | libultra API reimplemented on libogc |
| `port/gc/ogc/` | Platform code that uses libogc headers |
| `port/gc/game/` | C replacements for MIPS assembly and other game-side helpers |
| `port/gc/apploader/` | The disc image's apploader (CC0), built by `make -f Makefile.gc apploader` |
| `port/gc/tools/` | Host tools (run inside Linux/WSL), e.g. `mkiso.py` (disc image) and `mkdevdisc.py` (Dolphin dev disc) |
| `port/gc/tests/` | Stand-alone test programs for the shim |

## Testing in Dolphin

Builds run in Dolphin 2609 on Windows; the loop is driven from WSL:

```bash
make -C port/gc/tests/sd_probe          # storage/logging probe DOL (example)
port/gc/tools/run_dolphin.sh port/gc/tests/sd_probe/sd_probe.dol -Seconds 15
```

`run_dolphin.sh` converts paths and calls `run_dolphin.ps1` (which also works
directly from Windows PowerShell). The script refuses unpadded DOLs (Dolphin
fails with "Failed to init core"; every `.dol` rule must run `tools/dolpad.py`),
starts Dolphin in batch mode, records the USB Gecko log, waits (`-Seconds`,
default 20; `0` waits until you close Dolphin), screenshots the render window,
closes Dolphin with `WM_CLOSE` and prints the Gecko log and the new part of
Dolphin's log. Exit codes: 0 ok, 1 setup error, 2 Dolphin showed a dialog (its
text and a screenshot are printed), 3 Dolphin exited early, 4 Dolphin had to be
killed. Other options: `-Screenshot <png>`, `-NoSd` (no dev disc), `-NoGecko`.

**No SD card in Dolphin.** Dolphin 2609 has no SD adapter device on the
GameCube EXI bus (draft PR dolphin-emu/dolphin#12680 is unmerged), and its
"SD card" and "SD sync folder" settings only apply to the Wii. `__io_gcsd2`,
`__io_gcsda` and `__io_gcsdb` all report no card. A Dolphin run therefore gets
the SD folder through the DVD drive, and its log through an emulated USB Gecko:

| Path | Contents |
|---|---|
| `C:\_mmgcport\sdcard\` | The SD folder, laid out like the real card (`mmgcport\baserom.z64`). Local only. |
| `C:\_mmgcport\dolphin\mmgcport-dev.iso` | Dev disc: a GameCube disc header plus an ISO9660 copy of the SD folder, built by `tools/mkdevdisc.py` and rebuilt by the script when the folder changes. |
| `C:\_mmgcport\dolphin\gecko.log` | USB Gecko output of the last run (the platform log). |
| `C:\_mmgcport\dolphin\screenshot.png` | Render window at the end of the run; dialogs go to `screenshot-dialogN.png`. |
| `%APPDATA%\Dolphin Emulator\Logs\dolphin.log` | Dolphin's own log (appended across runs; file logging is on in `Config\Logger.ini`). |

The script passes these settings with `-C`; they are not saved to `Dolphin.ini`:

| Setting | Value | Why |
|---|---|---|
| `Dolphin.Core.SlotB` | `7` (USB Gecko) | Log channel; Dolphin serves it on TCP port 55020 |
| `Dolphin.Core.SlotA` | `255` (empty), or `1` (raw memory card) with `-MemCard` | Saves never carry over between runs unless asked for |
| `Dolphin.Core.DefaultISO` | the dev disc | Inserted when Dolphin boots a DOL (not with a disc image, which Dolphin boots itself) |
| `Dolphin.Core.FastDiscSpeed` | `True` | About 19 MB/s sequential reads |
| `Dolphin.Interface.ConfirmStop` | `False` | `WM_CLOSE` stops without a Yes/No box |

On the console side (see `tests/sd_probe/source/main.c` for working code):

- **Storage.** Try `fatMountSimple("sd", ...)` on `__io_gcsd2`, then
  `__io_gcsda`, then `__io_gcsdb`. If none mounts, mount the dev disc:
  `DVD_MountAsync` with a timeout (`DVD_Mount` never returns without a disc),
  then `ISO9660_Mount("dvd", &iface)` with a small `DISC_INTERFACE` that reads
  2048-byte sectors with `DVD_ReadPrio` through a 32-byte aligned bounce
  buffer. Paths are the same under `dvd:/`, but the disc is read-only.
- **Logging.** `if (usb_isgeckoalive(1)) usb_sendbuffer_safe(1, buf, len);`.
  Dolphin buffers the output until the script connects. Skip the slot B SD
  probe while a USB Gecko answers there: SD commands would corrupt the log.
- `log.txt` has nowhere to go in Dolphin. Saves go to the memory card in slot
  A when the run has one: `run_dolphin.sh ... -MemCard C:\path\card.raw`
  (Dolphin names the file `card.USA.59.raw`, a 59-block card). Without
  `-MemCard` the script leaves slot A empty, so every run starts without a save.

### Unattended runs: the input script

`make -f Makefile.gc GC_AUTOSTART=1` taps Start on the title screen until File
Select. `GC_AUTOSTART=2` plays a scripted controller 1 instead
(`game/input_script.c`): title -> File Select -> a new file (name entry) ->
the prologue (the Lost Woods as human Link, the Clock Tower's underground as
Deku Link with its Deku flower glides, the Happy Mask Salesman) -> Clock Town.
On the first morning it explores South Clock Town first: the HUD, every page
of the pause menu, the stick held in four directions, a spin attack, a talk
with one of Mutoh's carpenters. Then it opens the pause menu on every arrival
in South Clock Town and walks a tour South -> West -> South -> East (the first
time through the Treasure Chest Shop's door and back) -> North -> the Great
Fairy's fountain -> North -> South Clock Town (about 150 s a round) while the
three days pass, through the moon's fall and the restart of the cycle. The
script is a table of steps conditioned on the gamestate, scene, room, frames,
message boxes, cutscenes, player control and form; the active step is logged
(`script: ...`), and `trace.c` logs scene and room changes, message ids,
cutscenes and a status line every second (player, camera, time). The
coordinates come from the scenes' collision data in
`extracted/n64-us/assets/scenes/`.

The script asks for screenshots where it wants one: it logs `@shot <n>-<tag>`
and holds the picture still, and `run_dolphin.ps1` screenshots the render
window as soon as the line arrives, to `<screenshot>-<n>-<tag>.png` (the HUD,
the four pause pages, the walk, the spin, the carpenter's message box, the
shop's inside).

```bash
make -f Makefile.gc -j$(nproc) GC_AUTOSTART=2
port/gc/tools/run_dolphin.sh build/gc-n64-us/mm-gc.dol -Seconds 120 \
    -ExtraConfig Dolphin.Core.EmulationSpeed=0 -ScreenshotEvery 10
```

`BUILD_DIR=build/gc-<name>` on the make line builds in a directory of its own,
so that another build with other options (another workflow, another
`GC_AUTOSTART`) cannot replace the DOL between the build and the run.

With `Dolphin.Core.EmulationSpeed=0` (no speed limit) Dolphin runs the game 6-8
times faster than real time: Clock Town is reached after about 80 s, the whole
first cycle takes about 8 minutes. Log timestamps stay in game time.
`-ScreenshotEvery <n>` adds a screenshot every n seconds (wall time).

Crashes: `ogc/exc_hook.c` logs every unhandled CPU exception (kind, registers,
DAR/DSISR, return addresses, gamestate/scene/room) before libogc's panic dump;
resolve the addresses with
`powerpc-eabi-addr2line -f -e build/gc-n64-us/mm-gc.elf <addresses>`.
In Dolphin a data access to an unmapped address usually shows an "Invalid read"
dialog instead (run_dolphin exit code 2, with the dialog's text).

### Real hardware

1. Copy the DOL and `baserom.z64` to `SD:/mmgcport/` (the same layout as
   `C:\_mmgcport\sdcard\`).
2. Put the card in the SD2SP2 (serial port 2) and boot Swiss.
3. In Swiss, browse to `sd:/mmgcport/` and start the DOL.
4. The platform layer logs to `SD:/mmgcport/log.txt` (`sd_probe` writes
   `probe_log.txt`). Power off and read the file on the PC.

Or use the disc image (next section): it needs no separate ROM file.

## Bootable disc image

```bash
make -f Makefile.gc iso     # build/gc-n64-us/mm-gc.iso, about 39 MiB
port/gc/tools/run_dolphin.sh build/gc-n64-us/mm-gc.iso -Seconds 60 -MemCard 'C:\_mmgcport\dolphin\memcard-a.raw'
```

**The image contains your ROM.** `port/gc/tools/mkiso.py` copies
`baseroms/n64-us/baserom.z64` into it (after checking its size, header and
MD5). Build it yourself and keep it to yourself: never commit, upload or share
it. `build/` is gitignored. The repository holds no ROM data and no Nintendo
code. The apploader is the port's own (`port/gc/apploader/`, CC0).

The image is a GameCube disc that is also an ISO9660 volume, like the dev disc
(`mkiso.py` reuses `mkdevdisc.build_image`):

| Offset | Contents |
|---|---|
| `0x000000` | `boot.bin`: game ID `GMME00`, title, main DOL and FST offsets |
| `0x000440` | `bi2.bin`: country code 1 (NTSC-U), 24 MiB simulated memory |
| `0x002440` | Apploader header (date `2026/10/02`, entry `0x81200000`), then the apploader (about 4.8 KB) |
| `0x008000` | ISO9660 volume descriptors, path tables, directories |
| `0x00B000` | GameCube FST: `/mmgcport/baserom.z64` |
| `0x010000` | Main DOL (`mm-gc.dol`) |
| `0x678000` | `mmgcport/baserom.z64` (32 MiB), at a 32 KiB boundary |

- **Game ID `GMME00`.** `G` is the GameCube prefix, `MM` stands for Majora's
  Mask, `E` means NTSC-U and maker `00` is unassigned. No GameCube title uses
  the `GMM` prefix: GameTDB's list in Dolphin 2609 and Dolphin's GameSettings
  have no `GMM` entries. Dolphin and Swiss therefore apply no game-specific
  settings or patches. Change it with `mkiso.py --id`; the fourth letter sets
  the region.
- **Apploader** (`port/gc/apploader/apploader.c`, `apploader.ld`). It
  implements the interface YAGCD documents. The boot program loads it to
  `0x81200000` and calls its entry with pointers for `init`, `main` and
  `close`.
  - `main` asks for one DVD read per call: the disc header, the DOL header,
    then each DOL section straight to its load address. Every request is
    32-byte aligned with a length that is a multiple of 32; pieces that are
    not aligned go through a bounce buffer.
  - It then loads `bi2.bin` and the FST to the top of MEM1 and records them in
    low memory (`0x80000034`, `0x38`, `0x3C`, `0xF4`), as retail apploaders do.
  - `close` writes the data cache back, invalidates the instruction cache over
    the code and passes `argv[0] = "dvd:/"` in libogc's argv block. It returns
    the DOL's entry point.
  - In Dolphin, its messages show in `dolphin.log` as `OSREPORT_HLE` lines,
    next to Dolphin's `DVDRead` lines for each request.

### Where the ROM comes from

The loader passes `argv[0]`: the port's apploader passes `dvd:/`, and Swiss
passes the path of what it started. `gc_ogc_storage_open_rom()`
(`ogc/storage.c`) tries the sources in this order:

| Booted as | `argv[0]` | ROM sources, in order |
|---|---|---|
| The disc in the drive: Dolphin, the IPL with an optical drive emulator or a modchip, or Swiss on an ODE | `dvd:/...` | `dvd:/mmgcport/baserom.z64`, then the SD card |
| A disc image on the SD card, started by Swiss | `sd:/.../x.iso` (also `carda:`/`cardb:` for an SD Gecko) | The ROM inside that file (mounted as `img:`), then the SD card, then `dvd:` |
| A DOL: Swiss, Dolphin `-e`, any other loader | anything else, or none | The SD card, then `dvd:` (the dev disc, or the game disc) |

"The SD card" means `sd:/mmgcport/baserom.z64`, else the ROM inside a disc
image at `sd:/mmgcport/mm-gc.iso`. The second form serves loaders that do not
pass the image's path.

- **`dvd:` reads.** `dvd:` is mounted with libiso9660, and `bridge_rom.c`
  reads the ROM's sectors directly with `DVD_ReadPrio`.
- **`img:` reads.** `img:` is the image file on the SD card, mounted with
  libiso9660 through a `DISC_INTERFACE` that reads the file with libfat.
  Swiss's DVD emulation patches the DVD functions of Nintendo SDK programs,
  which it finds by signature. It never patches a libogc program, so this port
  reads the image file itself.
- **No ROM found.** The halt screen lists what was tried and why each source
  failed.

### Saves

| Storage at boot | Where saves go |
|---|---|
| SD card | `SD:/mmgcport/mm.fla`, as before (`log.txt` next to it) |
| No SD card, memory card in slot A (else slot B, unless a USB Gecko is there) | File `mmgcport_flash` (game code `GMME`, maker `00`). It takes 17 blocks: one header block with the comment "Majora's Mask (GC port) / Flash save, 128 KB", then the 128 KB flash image. A Memory Card 59 has room. |
| Neither | Not kept |

- **Writes.** A store writes only the 8 KB card blocks that changed. MM keeps
  each save twice, in separate 8 KB-aligned flash areas, so a power cut damages
  at most the copy being written, and the game falls back to the other.
- **Card safety.** The card is never formatted, and no other file is touched.
  If the card is full, the save is not kept and the log says so.
- **Measured in Dolphin.** Creating the file takes about 3 s. A typical store
  writes 2 blocks in 0.3 s, on the low-priority flash writer thread.

### Booting the image

- **Dolphin (GUI).** Use File > Open and pick `mm-gc.iso`, or add the folder to
  the game list. Dolphin boots it with its emulated IPL, which runs the
  apploader. Saves go to Dolphin's memory card in slot A: by default the GCI
  folder `Dolphin Emulator\GC\USA\Card A\00-GMME-mmgcport_flash.gci`.
- **Dolphin (script).** `run_dolphin.sh <iso>` boots the image like the GUI
  does, without inserting the dev disc.
- **Swiss, image on the SD card.**
  1. Copy `build/gc-n64-us/mm-gc.iso` to a FAT32 SD card, preferably as
     `SD:/mmgcport/mm-gc.iso`. From Windows the file is
     `\\wsl.localhost\Ubuntu-24.04\home\chris\mm\build\gc-n64-us\mm-gc.iso`.
     Copy it to a freshly formatted or defragmented card: Swiss may refuse a
     fragmented image ("Failed to setup the file (too fragmented?)"). No
     separate `baserom.z64` is needed.
  2. Put the card in the SD2SP2, or in an SD Gecko in slot A or B, and boot
     Swiss.
  3. Browse to the image and start it with A. Swiss loads the main DOL from
     the image itself (`boot.bin` offset `0x420`, no apploader) and passes the
     image's path in `argv`. The game then reads the ROM from the image on the
     same card.
     - **Swiss's "BS2 Boot" setting.** With it on, the IPL boots the image
       through Swiss's emulated drive, which the game cannot read once it
       runs. The game then finds the ROM only if the image is at
       `SD:/mmgcport/mm-gc.iso`, or if `SD:/mmgcport/baserom.z64` exists.
  4. `log.txt` and `mm.fla` go to `SD:/mmgcport/`.
- **Optical drive emulator or burned disc.** On a GC Loader, FlippyDrive,
  WKF/WODE or a modchipped console, the drive serves the image. The IPL, or
  Swiss on the ODE, boots it, and the ROM is read from the disc. SD card or
  memory card saves work as in the table above.
- **Region.** The image is NTSC-U (`E`). A PAL or Japanese console needs a
  region-free boot: Swiss provides one, and so do most ODEs and modchips.

## ROM reads and ARAM

The game reads the ROM through `osEPiStartDma`, which calls `gc_rom_read()`
(`ogc/bridge_rom.c`) synchronously in the calling thread. Four layers serve
the reads:

| Layer | Contents | Where |
|---|---|---|
| Resident ranges, loaded at boot | `0x20700-0x5E06E0`: audio data (5.75 MiB), which the audio thread streams. `0x65C9E0-0xA684D0` (4.05 MiB): `link_animetion` (read every gameplay frame), the item, map and message statics, the yar archives and the message data. | ARAM |
| Block cache | 8 blocks of up to 64 KB | MEM1 |
| File cache | Whole files of the ROM's dmadata table (objects, scenes, rooms and so on). The least recently used file is evicted first. Files are stored in 2 KB pages. | The rest of ARAM, 6.2 MiB |
| Disc | The SD card through libfat. On `dvd:` (the disc image or the dev disc), the file's sectors are read directly with `DVD_ReadPrio`. | |

- **Cache misses.** A miss inside a file reads the whole file from the disc in
  one sequential read and copies it to ARAM. Later loads of that file come
  from ARAM. For example, the 313 KB `gameplay_keep` takes about 7 ms from
  ARAM instead of about 210 ms from the disc.
- **Audio thread.** It only reads the audio range. That path takes only the
  audio range's own ARAM lock and bounce buffer, so it never waits for the
  disc or for another thread's resident read. The hot range has a separate
  lock, because the graph thread reads it at the lowest game priority.
- **Statistics.** Every 20 s the log gets `ROM: last N s:` lines. They give
  reads by layer, disc reads (count, KB, time), read latency (average,
  slowest call, calls over 2/16/50 ms) and a breakdown per thread.
- **Build options.** Pass them with
  `make -f Makefile.gc GC_ROM_FLAGS='...'`. See the top of `bridge_rom.c`.
  - `-DGC_ROM_TRACE=1` logs one line for each file a thread reads past the
    resident ranges. Use it for Dolphin measurements only: on hardware, every
    log line is an SD write.
  - `-DGC_ROM_HOT_RESIDENT=0 -DGC_ROM_FILE_CACHE=0 -DGC_ROM_DVD_DIRECT=0`
    restores the old reader.
- **Real drive timing in Dolphin.** Run
  `run_dolphin.sh <dol> -ExtraConfig 'Dolphin.Core.FastDiscSpeed=False'`.
  Reads then run at about 2 MB/s, with 50-100 ms per seek, like a GameCube
  drive.

Measured in Dolphin with real drive timing, on the dev disc:

| | Old reader | Now |
|---|---|---|
| Boot preload | Audio data: 2.98 s | Audio data: 2.70 s, plus the hot range: 1.99 s |
| Attract loop, first 140 s | 193 disc reads, 12.1 MB, 13.3 s waiting (9.5% of the time). 189 reads took over 16 ms. The game thread waited 1.0 s on archive reads. | 63 disc reads, 1.5 MB, 3.4 s waiting (3.0%). 48 reads took over 16 ms, each the first load of a file. The game thread never waits. |
| Attract loop, from the third minute on | Same as the first cycle | 0 disc reads. 10-14 us per read on average; the slowest read takes 1-4 ms. |
| Title, File Select, new game, first 100 s of the prologue (`GC_AUTOSTART=2`) | 101 disc reads, 6.3 MB, 6.1 s waiting (6.1% of the time). 95 reads took over 16 ms. | 34 disc reads, 1.2 MB, 1.6 s waiting (1.6%). 20 reads took over 16 ms. |

The audio thread had 0 underruns in every run, and its reads take 40 us on
average (0.1 ms at most), as before.
