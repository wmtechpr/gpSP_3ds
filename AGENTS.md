# AGENTS.md — gpSP_3ds

Onboarding and debugging guide for agents working on **gpSP_3ds**, a Nintendo 3DS port of `libretro/gpsp` (GBA) inside the modernized 3DS UI/GPU/NDSP shell from `matbo87/snes9x_3ds` (RetroAchievements and Online Updater intentionally omitted).

> **Workspace boundary:** Do **not** modify sibling `../gambatte_3ds/` unless explicitly asked; it is being investigated separately.

---

## 1. Target & Repository Map

- **Target hardware:** New 3DS / New 3DS XL / New 2DS XL (`APP_SYSTEM_MODE="64MB"`, `APP_SYSTEM_MODE_EXT="124MB"` in `Makefile` and `resources/app.rsf`; `resources/AppInfo`: `CTR-P-GBA3`, `0x3852`).
- **3DS shell (`source/3ds*.cpp/.h`, `source/png_utils.*`, `source/shader_screen.v.pica`):** UI/menu, PICA200 renderer, NDSP audio mixer thread (`32,768 Hz`), async SD save worker, LCD VTotal pacing, config/settings, cheats, and thumbnails.
- **Core integration (`source/3dsimpl.h`, `source/3dsimpl.cpp`):** Owns `GAME_MAIN` (`256x256` `GPU_RGB565` VRAM texture), `g_fbLinear` (`linearAlloc`), ARM11 JIT SVC mapping (`setupDynarecMapping` / `teardownDynarecMapping`), per-frame core/GPU orchestration (`impl3dsRunOneFrame`), screenshots (`240x160` RGB8), `120x80` thumbnails, cheats, savestates, and SRAM dirty polling.
- **C isolation bridge (`source/gpsp_bridge.h`, `source/gpsp_bridge.c`):** Isolates gpSP from `<3ds.h>`. Configures `sound_freq_bits = 15` (`32,768 Hz`), `gba_screen_pixels`, built-in `open_gba_bios_rom` + optional `sdmc:/3ds/gpSP_3ds/gba_bios.bin`, `416 KiB` savestates (`GBA_STATE_MEM_SIZE`), `128 KiB` `gamepak_backup`, up to 20 active CodeBreaker cheats, and `gba_cc_lut`.
- **Vendored core (`source/gpsp/`):** `libretro/gpsp` C/C++/ARM assembly (`cpu.cc`, `video.cc`, `cpu_threaded.c`, `arm/arm_stub.S`, `3ds/3ds_utils.c`, `3ds/3ds_cache_utils.S`, `bios_data.S`, and 10 `libretro/libretro-common/` C files; `libretro/libretro.c` is intentionally omitted). Only local patch inside `source/gpsp/` is the `#elif defined(_3DS)` `if (rom_translation_cache_ptr)` guard in `source/gpsp/main.c:119-123`.
- **Custom citro3d (`patches/citro3d.patch` -> `libs/citro3d`):** Clones/patches `devkitPro/citro3d` `v1.7.1` to add `C3Di_RenderQueueIsDone()` and the vertex uniform dirty-bound optimization. Always update `patches/citro3d.patch`, never `libs/citro3d` directly (`Makefile:248-260` resets `libs/citro3d` when the patch changes).
- **Host tests (`tests/test_core.cpp`):** Native Linux tests covering gpSP interpreter execution on a synthetic Mode 3 + PSG ROM, `416 KiB` savestate round-trip, CodeBreaker cheat parsing, `StereoRingBuffer`, and `.cht` parsing/serialization.

---

## 2. Build, ELF Layout Verification & Host Tests

### 3DS Release Build (Docker `devkitpro/devkitarm`)

`Makefile` produces `output/gpSP_3ds.elf`, `output/gpSP_3ds.smdh`, `output/gpSP_3ds.3dsx`, and `output/gpSP_3ds.cia`:

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/src" -w /src devkitpro/devkitarm \
  bash -c "git config --global --add safe.directory '*' && make release"
```

### Verify ELF Segment Bounds & `arm_stub.S` `.bss` Layout

Note: `devkitpro/devkitarm` does not put ARM binutils on default `PATH` in non-login shells; invoke `/opt/devkitpro/devkitARM/bin/arm-none-eabi-*` directly:

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/src" -w /src devkitpro/devkitarm \
  bash -c '/opt/devkitpro/devkitARM/bin/arm-none-eabi-readelf -l output/gpSP_3ds.elf && \
    /opt/devkitpro/devkitARM/bin/arm-none-eabi-nm -n output/gpSP_3ds.elf | \
    grep -E " (iwram|vram|ewram|reg|spsr|st_lookup_tables|reg_mode|oam_ram|palette_ram|memory_map_read|io_registers|palette_ram_converted)$"'
```

Expected invariants (verified on `output/gpSP_3ds.elf`):
- `LOAD` segment 3 (`0x00292000`, `MemSiz 0x1d5b1c`) ends at `0x00467b1c`, leaving `ram_translation_cache` (`0x01580000`, 512 KiB) and `rom_translation_cache` (`0x01600000..0x02000000`, 10 MiB) uncollided.
- `arm_stub.S` fixed offsets relative to `reg` (`0x004573b0`) hold: `iwram` `-0xA8000`, `vram` `-0x98000`, `ewram` `-0x80000`, `spsr` `+0x100`, `st_lookup_tables` `+0x118`, `reg_mode` `+0x400`, `oam_ram` `+0x500`, `palette_ram` `+0x900`, `memory_map_read` `+0xD00`, `io_registers` `+0x8D00`, `palette_ram_converted` `+0x9100`.

### Host Self-Tests (Linux, Interpreter Mode)

Compiles into `/tmp/gpsp_host_obj` without `-D_3DS` / `-DHAVE_DYNAREC`, runs all 3 suites, and cleans up `/tmp/gpsp_host_obj`:

```sh
rm -rf /tmp/gpsp_host_obj && mkdir -p /tmp/gpsp_host_obj && (
  cd /tmp/gpsp_host_obj
  S="$OLDPWD/source"
  T="$OLDPWD/tests"
  INC=(-I"$S" -I"$S/gpsp" -I"$S/gpsp/libretro" -I"$S/gpsp/libretro/libretro-common/include")
  DEFS="-DFRONTEND_SUPPORTS_RGB565 -DHAVE_STRINGS_H -DHAVE_STDINT_H -DHAVE_INTTYPES_H -D__LIBRETRO__ -DINLINE=inline"
  for f in \
    "$S"/gpsp/{main,gba_memory,savestate,input,sound,cheats,memmap,serial,gbp,rfu,serial_proto,gba_cc_lut}.c \
    "$S"/gpsp/libretro/libretro-common/compat/{compat_posix_string,compat_strl,fopen_utf8}.c \
    "$S"/gpsp/libretro/libretro-common/encodings/encoding_utf.c \
    "$S"/gpsp/libretro/libretro-common/file/{file_path,file_path_io}.c \
    "$S"/gpsp/libretro/libretro-common/streams/file_stream.c \
    "$S"/gpsp/libretro/libretro-common/string/stdstring.c \
    "$S"/gpsp/libretro/libretro-common/time/rtime.c \
    "$S"/gpsp/libretro/libretro-common/vfs/vfs_implementation.c; do
    gcc -O2 -std=gnu99 -w "${INC[@]}" $DEFS -c "$f"
  done
  gcc -O2 -std=gnu99 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-old-style-declaration "${INC[@]}" $DEFS -c "$S/gpsp_bridge.c"
  gcc -Wa,-I"$S/gpsp" -c "$S/gpsp/bios_data.S"
  g++ -O2 -std=gnu++17 -w "${INC[@]}" $DEFS -c "$S/gpsp/cpu.cc"
  g++ -O2 -std=gnu++17 -w "${INC[@]}" $DEFS -c "$S/gpsp/video.cc"
  g++ -O2 -std=gnu++17 -Wall -Wextra -Werror "${INC[@]}" "$T/test_core.cpp" *.o -lm -o test_core
  ./test_core
) && rm -rf /tmp/gpsp_host_obj
```

---

## 3. Hard Architecture Invariants & Pitfalls

1. **Header isolation (`u32` / `MAX_CHEATS` collision):** `gpsp/common.h:98` typedefs `u32` as `unsigned int`, whereas `<3ds/types.h>` typedefs `u32` as `uint32_t` (`unsigned long`); both `source/3dsimpl.h:77` and `source/gpsp/cheats.h` define `MAX_CHEATS`. **Never** `#include <3ds.h>` in `source/gpsp_bridge.c` and **never** `#include "common.h"` in `source/3ds*.cpp`. Keep `source/gpsp_bridge.h` pure standard C (`stdint.h` / `stdbool.h` / `stddef.h`).
2. **Compiler warnings (`Makefile:75-96`):** `STRICT_WARNINGS=1` (`-Werror`) is enforced on `source/3ds*.cpp` and `source/gpsp_bridge.c`; `source/gpsp/` builds with `-w`. Keep `-Wno-register` in `CXXFLAGS` only (not `CFLAGS`), and preserve the `#pragma GCC diagnostic` guard around `#include "common.h"` in `source/gpsp_bridge.c:8-11`.
3. **Linker script:** Intentionally uses default `3dsx.specs` (no custom `link.ld`) so `.bss` sits near `0x00467b1c` and leaves `0x01580000..0x02000000` free for JIT mapping.
4. **PICA200 frame & texture rules:** `C3D_SyncDisplayTransfer` must run **outside** `C3D_FrameBegin..C3D_FrameEnd` after `gpu3dsWaitForRenderQueue()`. Because `DISPLAY_TRANSFER_FLAGS` uses `GX_TRANSFER_FLIP_VERT(0)`, `GAME_MAIN` (`256x256`) uses inverted V coordinates (`ty0 = 256`, `ty1 = 96` in `source/3dsimpl.cpp:987-988, 1037-1038`).
5. **Savestates & audio ring:** Do **not** call `sound_flush_ring()` inside `gpsp_bridge_unserialize()`; clearing gpSP's internal ring wipes restored audio state and stalls frame 1 audio output.
6. **JIT fallback guard:** Keep the `#elif defined(_3DS)` `if (rom_translation_cache_ptr)` guard in `source/gpsp/main.c:119-123` so unprivileged launches and Citra/Azahar (`device3DS.isReal3DS == false`) do not write to unmapped `0x01600000` inside `init_dynarec_caches()` / `init_emitter()`.

---

## 4. Real-Hardware Fixes Applied & Ongoing Diagnostics

### Applied Fixes

1. **Startup GPU NULL dereference (`source/3dsgpu.cpp:418-455`, `source/3dsgpu.h:59-62`, `libs/citro3d/source/base.c:307`):** Because `SPROGRAM_SCREEN == 0`, `gpu3dsInitialize()` zeroing `GPU3DS` without calling `gpu3dsResetState()` caused the first splash draw in `showMenu()` to skip `C3D_BindProgram()` (`ctx->program == NULL`) and keep `DepthTest` enabled, crashing in `C3Di_UpdateContext()` (`ctx->program->geometryShader`).
   - **Applied fix:** `gpu3dsResetState();` is now called at the end of `gpu3dsInitialize()` (`source/3dsgpu.cpp:452`).
2. **Unsynchronized `gpu3dsClearTexture()` (`source/3dsimpl.cpp:177-180, 490`, `source/3dsgpu.cpp:61, 670-679`):** `clearFramebuffers()` (called during `impl3dsLoadROM()` right after `menu3dsRunRomLoadingDialog()` frames) now calls `gpu3dsWaitForRenderQueue()` before `gpu3dsClearTexture(&GPU3DS.textures[GAME_MAIN], 0)`.
3. **ARM11 JIT / heap headroom order (`source/3dsimpl.cpp:382-391`, `source/gpsp_bridge.c:90-98`):** `impl3dsInitialize()` now runs `setupDynarecMapping()` **before** `gpsp_bridge_init()`, and `gpsp_bridge_init()` temporarily holds a `6 MiB` `malloc` while `init_gamepak_buffer()` allocates 1 MiB ROM blocks so `10.5 MiB` JIT caches and `.3dsx` (64 MiB) shell/UI/save buffers are not starved while `.cia` (124 MiB) still receives the full 32 MiB ROM buffer.
4. **Main-thread stack overflow on ROM load (`source/3dsimpl.cpp:104-120`, `source/3dsmain.cpp:38-39`):** `computeFileCrc32()` previously allocated `u8 buf[32 * 1024]` on `libctru`'s default 32 KiB main-thread stack (`__stacksize__ = 0x8000`), overflowing `SP` below `0x08000000` (`FAR = 0x07ffe810` inside `fopen`, Luma3DS Data Abort `DFSR = 0x00000805`). `computeFileCrc32()` now uses `std::vector<u8> buf(32 * 1024)` and `source/3dsmain.cpp` overrides `__stacksize__ = 128 * 1024;` inside `extern "C"`.

### Fast Hardware Isolation Checklist (If Post-Menu Issues Remain)

1. **Make logs survive crashes/freezes (`source/3dslog.cpp:12,16`):** `LOG_BUFFER_IN_GAME` defaults to `true`, buffering in-game `log3dsWrite()` calls in a 32 KiB RAM buffer until `emulatorLoop()` exits cleanly — meaning zero in-game logs reach the SD card on a hang or abort. For hardware debugging, set `LOG_BUFFER_IN_GAME = false` (and optionally `FORCE_DEBUG_LOGS = true` so startup logs write immediately without toggling the menu), or enable **Other -> Enable Debug Log** and restart. Log path: `sdmc:/3ds/gpSP_3ds/debug_v1.0_session.log`.
2. **Isolate JIT vs GPU/Audio/Shell (`source/3dssettings.cpp:46`):** Toggle **Settings -> Game Boy Advance -> ARM Dynarec** (`settings3DS.GbaDynarec`, default `true`) to **OFF** before launching a ROM.
   - If interpreter mode still freezes/crashes immediately on launch -> fault is likely in GPU submission, LCD VTotal, NDSP audio, or ROM loading (`#1` or `#3` below).
   - If interpreter works and only dynarec crashes/freezes -> fault is in ARM11 JIT SVC/cache flushing (`#2` below).
3. **Inspect Luma3DS crash dumps:** Check `sdmc:/luma/dumps/arm11/` and resolve `pc` / `lr` against `output/gpSP_3ds.elf`:
   ```sh
   docker run --rm -v "$PWD:/src" -w /src devkitpro/devkitarm \
     /opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line -f -C -e output/gpSP_3ds.elf <addr>
   ```

### Remaining Suspects to Audit If Needed

1. **Empty `C3D_FrameBegin`/`C3D_FrameEnd` & `C3D_SyncDisplayTransfer` lifecycle (affects both ports):**
   - `emulatorLoop()` (`source/3dsmain.cpp:2277-2308`) enters with `firstFrame = true; skipDrawing = true;`.
   - In `impl3dsRunOneFrame()` (`source/3dsimpl.cpp:551-585`):
     - Frame 1 (`firstFrame = true`): renders `impl3dsSceneRender()`, submits frame, then runs `gpsp_bridge_run_frame(true, ...)` so `g_pendingDraw = false`.
     - Frame 2 (`firstFrame = false`, `g_pendingDraw = false`): calls `gpu3dsFrameBegin(0, false)` followed by **zero `C3D_FrameDrawOn` and zero draw calls**, then calls `gpu3dsFrameEnd()`. It also ignores the `bool` return value of `gpu3dsFrameBegin()`.
   - Also audit `uploadLinearFrameToVram()` (`source/3dsimpl.cpp:230-244`): `GSPGPU_FlushDataCache` + `C3D_SyncDisplayTransfer` (`GX_TRANSFER_OUT_TILED(1)` / `GX_TRANSFER_FMT_RGB565`) runs right after `gpu3dsWaitForRenderQueue()` (`C3Di_RenderQueueWaitDone`).

2. **ARM11 Dynarec SVC mapping & cache flush (`gpSP_3ds`-specific):**
   - **SVC & cache flush (`source/3dsimpl.cpp:266-306`, `source/gpsp/3ds/3ds_utils.c`, `source/gpsp/3ds/3ds_cache_utils.S`, `resources/app.rsf:92-147`):**
     - `setupDynarecMapping()` uses `svcDuplicateHandle(&g_jitProcHandle, 0xFFFF8001)` + `svcControlProcessMemory(..., MEMOP_MAP, RWX)` over `memalign(0x1000, ...)` heap buffers. Audit this against upstream `libretro/gpsp` 3DS port expectations (`MEMOP_MAP` permissions, `.3dsx` Rosalina `svcControlProcessMemory` availability).
     - `ctr_flush_invalidate_cache()` calls `ctr_clear_cache()` when Rosalina >= 8 (`svc 0x80` = `svcCustomBackdoor` running `cpsid aif` + CP15 cache flush in `3ds_cache_utils.S`), otherwise `svcBackdoor` (`0x7B`).
   - **First JIT execution:** `gpsp_bridge_attach_jit()` (`source/gpsp_bridge.c:136-143`) runs during `impl3dsInitialize()`, while `init_main()` (`source/gpsp/main.c:119-127`) writes the BIOS SWI entrypoint and `execute_arm_translate()` (`source/gpsp/cpu_threaded.c`) emits ARM code during the first emulated frame. Trace whether any crash happens inside `execute_arm_translate()` / `translate_icache_sync()`.

3. **LCD VTotal pacing & NDSP mixer (`source/3dsmain.cpp:2119-2164, 2238-2275`, `source/3dslcd.cpp:48-62`, `source/3dssound.cpp:67-108, 120-149`):**
   - Look closely at `source/3dsmain.cpp:2238-2244`:
     ```cpp
     // render one frame before lcd3dsSetEmulationRate below,
     // otherwise game screen glitches and real hardware freezes.
     if (gpu3dsSetTopMode()) { ... }
     ```
     Because top screen is already in 2D mode, `gpu3dsSetTopMode()` (`source/3dsgpu.cpp:386-400`) returns **`false`**! That skips the pre-VTotal `C3D_FrameBegin(C3D_FRAME_SYNCDRAW)` render on the top/game screen right before `lcd3dsSetEmulationRate(TICKS_PER_FRAME_GBA)` writes `PDC_VTOTAL_TOP` / `PDC_VTOTAL_BOTTOM`. Note also `paceFrameOnVBlankCounter` (`3dsmain.cpp:2128-2138`) loops on `gpu3dsWaitForVBlankBanked` + `C3D_FrameCounter(displayId)` which stalls forever if GSP interrupts/display stop advancing.
   - Check `snd3dsMixSamples()` (`source/3dssound.cpp:67-108`) calling `svcFlushProcessDataCache(CUR_PROCESS_HANDLE, ...)` from `device3DS.secondaryCore` (Core 2 on N3DS) immediately after `snd3dsStartPlaying()`.
