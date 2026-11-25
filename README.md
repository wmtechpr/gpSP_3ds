# gpSP for 3DS

A Nintendo 3DS port of the **gpSP** Game Boy Advance emulator (`libretro/gpsp`) built on the modernized 3DS UI and GPU/NDSP shell from [matbo87/snes9x_3ds](https://github.com/matbo87/snes9x_3ds) (derived from [bubble2k16/snes9x_3ds](https://github.com/bubble2k16/snes9x_3ds)).

Targeted at **New 3DS / New 3DS XL / New 2DS XL** consoles (`124 MB` N3DS memory mode + ARM11 dynamic recompiler when kernel JIT mapping is available, with automatic interpreter fallback).

## Features

- Game Boy Advance emulation via bundled `libretro/gpsp` (`ARM11` dynarec on hardware, interpreter fallback when unprivileged or under emulators)
- Built-in open-source GBA BIOS (`bios/open_gba_bios.bin`) plus optional external official BIOS (`sdmc:/3ds/gpSP_3ds/gba_bios.bin`)
- Native `32,768 Hz` stereo NDSP audio output buffered through a thread-safe ring buffer (`Low` / `Normal` / `High` depth & `100%`–`200%` volume boost)
- GBA LCD Color Correction (`gba_cc_lut`) and optional LCD Interframe Blending
- Hardware-accelerated PICA200 rendering with scaling (`None 240x160`, `Fit 3:2` `360x240` Top / `320x213` Bottom, `4:3`, `Full`), filtering (`Sharp`, `Smooth`, `Balanced`), scanlines, cropping, overscan, Top/Bottom screen swap, built-in and custom bezels/overlays (`romfs/gfx/overlay.png`), and built-in/custom game and second-screen backgrounds
- System-themed 3DS branding: custom `48x48` HOME icon (`resources/icon.png`), 2D HOME hover banner (`resources/banner.bnr`), 3-column parallax ROM-selector splash with GBA box art (`gfx/splash_*.png` → `romfs/gfx/splash.t3x`), blurred GBA in-game box-art wall (`romfs/gfx/background_game_screen.png`), Game Boy Advance touch-screen photo (`romfs/gfx/background_second_screen_{us,pal}.png`), and a built-in GBA bezel (`312x208` inner window; enable via **Settings → Display → Game Bezel**, automatically fitting scaled gameplay inside the frame when **Auto-Fit** is Off)
- UI themes (`DarkMode`, `RetroArch`, `Original`), fonts (`Tempesta`, `Ronda`), ROM boxart/title/gameplay thumbnails, and directory listing cache
- 5 save state slots (`.1.frz`–`.5.frz`) with embedded `120x80` thumbnails, optional auto-savestate, async SD save worker, and periodic dirty-backup `.sav` saving (`SRAM`/`FLASH`/`EEPROM`)
- In-game screenshots (PNG) and RetroArch `.cht` cheat support (up to 20 simultaneous unencrypted CodeBreaker cheats)
- Per-game and global button remapping (`A/B/L/R/SELECT/START`), turbo fire, Circle Pad D-pad binding, and hotkey combos

## Supported ROMs & SD Card Layout

Supported extensions: `.gba`, `.agb`, `.bin` (uncompressed; ZIP archives are not supported).

### Installation
- **`.cia`**: Install `output/gpSP_3ds.cia` with FBI (recommended for full ARM11 dynarec performance on CFW/Rosalina).
- **`.3dsx`**: Copy `output/gpSP_3ds.3dsx` to `sdmc:/3ds/gpSP_3ds/` and launch via Homebrew Launcher.
- Audio requires dumped 3DS DSP firmware (`sdmc:/3ds/dspfirm.cdc`).

Emulator files live under `sdmc:/3ds/gpSP_3ds/`:
- `gba_bios.bin` — optional official GBA BIOS dump (uses built-in HLE BIOS if absent or disabled)
- `saves/` — battery `.sav` files (`SRAM` / `FLASH` / `EEPROM`)
- `savestates/` — save state slots (`.1.frz`–`.5.frz`) and `.auto.frz`
- `screenshots/` — timestamped `.png` screenshots
- `cheats/` — RetroArch `.cht` files matched by trimmed ROM name
- `overlays/` — custom bezel `.png` files
- `backgrounds/game_screen/` & `backgrounds/second_screen/` — custom background `.png` files

## Building from Source

The Makefile uses `libs/citro3d` (`citro3d` `v1.7.1` + `patches/citro3d.patch`), compiles gpSP C/C++/ARM assembly + shell sources (`STRICT_WARNINGS=1`), and outputs `.elf`, `.smdh`, `.3dsx`, and `.cia` under `output/`.

Using Docker (`devkitpro/devkitarm`):

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/src" -w /src devkitpro/devkitarm \
  bash -c "git config --global --add safe.directory '*' && make release"
```

Outputs:
- `output/gpSP_3ds.3dsx`
- `output/gpSP_3ds.cia`
- `output/gpSP_3ds.smdh`

> **Asset packaging note:** `Makefile` does not list `romfs/gfx/*.png` or `resources/banner.bnr` as prerequisites of `.3dsx`/`.cia`. If you update branding PNGs or rebuild `resources/banner.bnr` (generated from `resources/banner.png` + WAV audio via `bannertool`), delete `romfs/gfx/splash.t3x` and `output/gpSP_3ds.{3dsx,cia,smdh}` before running `make release`.

### Host Self-Tests

Core execution in interpreter mode (synthetic GBA Mode 3 ROM + PSG audio + built-in BIOS), `416 KiB` save-state round-trip, CodeBreaker cheat parsing, `.cht` parser/serializer, and `StereoRingBuffer` can be tested natively on Linux (`tests/test_core.cpp`).

## Credits

- **Exophase & libretro contributors** for [gpSP](https://github.com/libretro/gpsp) (GPLv2) and `open_gba_bios`
- **matbo87** for [matbo87/snes9x_3ds](https://github.com/matbo87/snes9x_3ds)
- **bubble2k16** for the original [bubble2k16/snes9x_3ds](https://github.com/bubble2k16/snes9x_3ds) codebase
- **libretro-thumbnails** for the GBA box art composited into the splash screen and in-game background
- **Steveice10** for [bannertool](https://github.com/Steveice10/bannertool) (offline `banner.bnr` generation)
- **devkitPro** (`devkitARM`, `libctru`, `citro3d`, `tex3ds`)
