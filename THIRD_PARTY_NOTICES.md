# Third-Party Notices

This project depends on third-party code, libraries, tools, and visual assets. Their respective licenses apply to those components.

## Bundled Core

- **gpSP core & built-in open-source GBA BIOS** (in `source/gpsp/`):
  - License terms: GNU General Public License v2.0 (GPLv2)
  - Notice/source: `source/gpsp/COPYING`
  - Upstream: https://github.com/libretro/gpsp

## 3DS UI / GPU / NDSP Shell

- Derived from [matbo87/snes9x_3ds](https://github.com/matbo87/snes9x_3ds) and [bubble2k16/snes9x_3ds](https://github.com/bubble2k16/snes9x_3ds) (`source/3ds*`, `source/png_utils.*`, `source/shader_screen.v.pica`, with RetroAchievements and Online Updater removed).

## Linked Libraries (build/runtime)

Libraries linked by `Makefile`:

- `-lcitro3d` (citro3d, devkitPro ecosystem; zlib license) — built locally from `v1.7.1` with `patches/citro3d.patch` applied; the patched build is required (`C3Di_RenderQueueIsDone`).
- `-lctru` (libctru, devkitPro ecosystem)
- `-lpng` (libpng)
- `-lz` (zlib)
- `-lm` (system math library)

## Toolchain & Build Tools

- Built with devkitPro / devkitARM (`devkitARM`, `3dsxtool`, `smdhtool`, `tex3ds`).
- Prebuilt `makerom` binaries in `makerom/` for packaging `.cia` files (`makerom/BINARY_SOURCES.md`, https://github.com/3DSGuy/Project_CTR).
- `resources/banner.bnr` is generated offline from `resources/banner.png` using [Steveice10/bannertool](https://github.com/Steveice10/bannertool).

## Fonts & Visual Assets

- **Super Metroid (Intro) (Colour) (SNES)** by Patrick H. Lauke:
  - License terms: CC BY 3.0, https://creativecommons.org/licenses/by/3.0/
  - Source: https://fontstruct.com/fontstructions/show/2381707
  - Used to render the pause screen text texture (`gfx/pause.png`); the font file is not included.
- **Noto Sans** (Google Fonts; SIL Open Font License 1.1):
  - Used offline to rasterize typography in `resources/icon.png`, `resources/banner.png`, `gfx/splash_logo.png`, and `romfs/gfx/overlay.png`; font files are not bundled.
- **libretro-thumbnails**:
  - Official GBA box art thumbnails composited offline into `gfx/splash_{center,left,right}.png` and `romfs/gfx/background_game_screen.png` (https://github.com/libretro-thumbnails).

## Attribution and Compliance

- Keep original copyright and license headers.
- For distribution packages, include this file, `source/gpsp/COPYING`, and `LICENSE.md`.
