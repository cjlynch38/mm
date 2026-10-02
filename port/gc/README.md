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
