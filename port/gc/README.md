# GameCube port

Native GameCube build of Majora's Mask: the decomp's C source compiled for the
GameCube's PowerPC CPU with devkitPPC and libogc. This is not an emulator.

Status: **Milestone 2 in progress.** All game code compiles for PowerPC. Linking
and the libultra shim are next.

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
| `port/gc/tools/` | Host tools (run inside Linux/WSL) |
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
| `Dolphin.Core.DefaultISO` | the dev disc | Inserted when Dolphin boots a DOL |
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
- Anything that must write (`log.txt`, saves) has nowhere to go in Dolphin;
  test writes on hardware.

### Real hardware

1. Copy the DOL and `baserom.z64` to `SD:/mmgcport/` (the same layout as
   `C:\_mmgcport\sdcard\`).
2. Put the card in the SD2SP2 (serial port 2) and boot Swiss.
3. In Swiss, browse to `sd:/mmgcport/` and start the DOL.
4. The platform layer logs to `SD:/mmgcport/log.txt` (`sd_probe` writes
   `probe_log.txt`). Power off and read the file on the PC.
