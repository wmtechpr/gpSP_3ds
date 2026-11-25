//=============================================================================
// Interface between the 3DS shell and the gpSP (GBA) core.
//=============================================================================

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>
#include <zlib.h>

#include "3dscheat_parse.h"
#include "3dsfiles.h"
#include "3dsgpu.h"
#include "3dsimpl.h"
#include "3dsimpl_gpu.h"
#include "3dsinput.h"
#include "3dslog.h"
#include "3dsmenu.h"
#include "3dssave.h"
#include "3dssettings.h"
#include "3dssound.h"
#include "3dstimer.h"
#include "3dsui.h"
#include "3dsui_img.h"
#include "3dsui_notif.h"
#include "3dsutils.h"
#include "gpsp_bridge.h"

#include "shader_screen_shbin.h"

//---------------------------------------------------------
// Globals declared in 3dsimpl.h
//---------------------------------------------------------
CoreScreenshot screenshot = {};
bool skipNextFpsUpdate = false;
CoreRomInfo coreRomInfo = {};
CoreCheatData coreCheats = {};

//---------------------------------------------------------
// Constants & core state
//---------------------------------------------------------
static const int GAME_TEX_DIM = 256;
static const size_t GAME_FB_LINEAR_BYTES = GAME_TEX_DIM * GAME_TEX_DIM * sizeof(u16);
static const size_t GAME_FRAME_PIXELS = CORE_SCREEN_WIDTH * CORE_SCREEN_HEIGHT;
static const size_t AUDIO_FRAME_OUT_CAP = 2048;

static const int SRAM_DIRTY_CHECK_FRAMES = 120;
static const int SD_WRITE_FAIL_NOTIF_LIMIT = 3;

static const u64 PAUSE_ANIMATION_DURATION_MS = 180;
static const float PAUSE_ANIMATION_Y_OFFSET = 4.0f;

static u16* g_fbLinear = nullptr;
static std::vector<u16> g_fbPrev;
static std::vector<int16_t> g_audioOutBuf;

static std::vector<u8> g_sramShadow;
static int g_sramCheckCounter = 0;

static u32 g_turboFrameCounter = 0;
static bool g_hasPrevFrame = false;
static bool g_pendingDraw = false;
static bool g_hasValidFrame = false;
static u64 pauseAnimationStartedAt = 0;

// 3DS ARM dynarec cache mapping state
static bool g_jitMapped = false;
static Handle g_jitProcHandle = 0;
static void* g_jitRomVaddr = nullptr;
static size_t g_jitRomSize = 0;
static void* g_jitRamVaddr = nullptr;
static size_t g_jitRamSize = 0;
static void* g_jitRomBacking = nullptr;
static void* g_jitRamBacking = nullptr;

typedef struct {
    int sx0;
    int sy0;
    int sx1;
    int sy1;
    int sWidth;
    int sHeight;
    int cHeight;
    float tx0;
    float ty0;
    float tx1;
    float ty1;
} GameScreenViewport;

typedef Result (*GSP_CacheCallback)(const void* addr, u32 size);

//---------------------------------------------------------
// Helpers
//---------------------------------------------------------
static inline u16 blendRgb565(u16 a, u16 b) {
    return static_cast<u16>(((a & 0xF7DEu) >> 1) + ((b & 0xF7DEu) >> 1) + (a & b & 0x0821u));
}

static u32 computeFileCrc32(const char* path, u32* outSize) {
    FILE* fp = file3dsOpen(path, "rb");
    if (!fp) return 0;

    std::vector<u8> buf(32 * 1024);
    uLong crc = crc32(0L, Z_NULL, 0);
    u32 total = 0;
    size_t n = 0;
    while ((n = fread(buf.data(), 1, buf.size(), fp)) > 0) {
        crc = crc32(crc, buf.data(), static_cast<uInt>(n));
        total += static_cast<u32>(n);
    }
    file3dsClose(fp);

    if (outSize) *outSize = total;
    return static_cast<u32>(crc);
}

static bool hasAnyNonFFBytes(const u8* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] != 0xFF) return true;
    }
    return false;
}

static void updateLinearBufferFromCore(bool allowBlend) {
    const u16* src = gpsp_bridge_get_framebuffer();
    if (!src || !g_fbLinear) return;

    bool doCc = settings3DS.GbaColorCorrection;
    bool doBlend = allowBlend && g_hasPrevFrame && !g_fbPrev.empty();

    for (int y = 0; y < CORE_SCREEN_HEIGHT; y++) {
        const u16* sRow = src + y * GPSP_SCREEN_PITCH;
        u16* dRow = g_fbLinear + y * GAME_TEX_DIM;
        u16* pRow = g_fbPrev.empty() ? nullptr : &g_fbPrev[y * CORE_SCREEN_WIDTH];

        for (int x = 0; x < CORE_SCREEN_WIDTH; x++) {
            u16 px = sRow[x];
            u16 out = px;

            if (doBlend && pRow) {
                out = blendRgb565(px, pRow[x]);
            }
            if (doCc) {
                out = gpsp_bridge_apply_cc(out);
            }

            dRow[x] = out;
            if (allowBlend && pRow) {
                pRow[x] = px;
            }
        }
    }

    if (allowBlend) {
        g_hasPrevFrame = true;
    } else {
        g_hasPrevFrame = false;
    }
}

static void clearFramebuffers() {
    if (g_fbLinear) {
        memset(g_fbLinear, 0, GAME_FB_LINEAR_BYTES);
    }
    if (!g_fbPrev.empty()) {
        std::fill(g_fbPrev.begin(), g_fbPrev.end(), 0);
    }
    g_hasPrevFrame = false;
    g_hasValidFrame = false;
    g_pendingDraw = false;

    if (GPU3DS.textures[GAME_MAIN].tex.data) {
        gpu3dsWaitForRenderQueue();
        gpu3dsClearTexture(&GPU3DS.textures[GAME_MAIN], 0);
    }
}

static u32 gatherCoreInputMask() {
    u32 held = input3dsGetCurrentKeysHeld();

    bool bindCirclePad = settings3DS.UseGlobalBindCirclePad
        ? settings3DS.GlobalBindCirclePad
        : settings3DS.BindCirclePad;
    const auto& mapping = settings3DS.UseGlobalButtonMappings
        ? settings3DS.GlobalButtonMapping
        : settings3DS.ButtonMapping;
    const auto& turbo = settings3DS.UseGlobalTurbo
        ? settings3DS.GlobalTurbo
        : settings3DS.Turbo;

    u32 gbaInput = 0;

    bool up    = (held & KEY_DUP)    || (bindCirclePad && (held & KEY_CPAD_UP));
    bool down  = (held & KEY_DDOWN)  || (bindCirclePad && (held & KEY_CPAD_DOWN));
    bool left  = (held & KEY_DLEFT)  || (bindCirclePad && (held & KEY_CPAD_LEFT));
    bool right = (held & KEY_DRIGHT) || (bindCirclePad && (held & KEY_CPAD_RIGHT));

    if (up && !down)       gbaInput |= CORE_BTN_UP;
    else if (down && !up)  gbaInput |= CORE_BTN_DOWN;

    if (left && !right)      gbaInput |= CORE_BTN_LEFT;
    else if (right && !left) gbaInput |= CORE_BTN_RIGHT;

    static const u32 keys3ds[10] = {
        KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_ZL, KEY_ZR, KEY_SELECT, KEY_START
    };

    for (int i = 0; i < 10; i++) {
        if ((held & keys3ds[i]) == 0) continue;

        if (i < 8 && turbo[i] > 0) {
            u32 period = static_cast<u32>(turbo[i]) * 2u;
            if ((g_turboFrameCounter % period) >= static_cast<u32>(turbo[i]))
                continue;
        }

        for (int j = 0; j < 4; j++) {
            gbaInput |= static_cast<u32>(mapping[i][j]);
        }
    }

    g_turboFrameCounter++;
    return gbaInput;
}

static void uploadLinearFrameToVram() {
    if (!g_fbLinear || !GPU3DS.textures[GAME_MAIN].tex.data) return;

    C3D_Tex* tex = &GPU3DS.textures[GAME_MAIN].tex;
    t3dsStartTimer(TIMER_UPLOAD_FRAME);
    GSPGPU_FlushDataCache(g_fbLinear, GAME_FB_LINEAR_BYTES);
    C3D_SyncDisplayTransfer(
        reinterpret_cast<u32*>(g_fbLinear), GX_BUFFER_DIM(GAME_TEX_DIM, GAME_TEX_DIM),
        reinterpret_cast<u32*>(tex->data),  GX_BUFFER_DIM(GAME_TEX_DIM, GAME_TEX_DIM),
        GX_TRANSFER_OUT_TILED(1) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
        GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565)
    );
    t3dsStopTimer(TIMER_UPLOAD_FRAME);
}

static void setupDynarecMapping() {
    if (!device3DS.isReal3DS) {
        log3dsWrite("Dynarec skipped (non-hardware environment)");
        return;
    }

    gpsp_bridge_get_jit_regions(&g_jitRomVaddr, &g_jitRomSize, &g_jitRamVaddr, &g_jitRamSize);
    if (!g_jitRomVaddr || !g_jitRamVaddr || g_jitRomSize == 0 || g_jitRamSize == 0) {
        return;
    }

    g_jitRomBacking = memalign(0x1000, g_jitRomSize);
    g_jitRamBacking = memalign(0x1000, g_jitRamSize);
    if (!g_jitRomBacking || !g_jitRamBacking) {
        free(g_jitRomBacking); g_jitRomBacking = nullptr;
        free(g_jitRamBacking); g_jitRamBacking = nullptr;
        log3dsWrite("Dynarec backing allocation failed; falling back to interpreter");
        return;
    }

    Result res = svcDuplicateHandle(&g_jitProcHandle, 0xFFFF8001);
    if (R_FAILED(res)) {
        free(g_jitRomBacking); g_jitRomBacking = nullptr;
        free(g_jitRamBacking); g_jitRamBacking = nullptr;
        log3dsWrite("svcDuplicateHandle failed (0x%08lX); falling back to interpreter", static_cast<unsigned long>(res));
        return;
    }

    u32 rwx = MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE;
    Result r1 = svcControlProcessMemory(g_jitProcHandle,
        reinterpret_cast<u32>(g_jitRomVaddr), reinterpret_cast<u32>(g_jitRomBacking),
        static_cast<u32>(g_jitRomSize), MEMOP_MAP, rwx);
    if (R_FAILED(r1)) {
        svcCloseHandle(g_jitProcHandle);
        g_jitProcHandle = 0;
        free(g_jitRomBacking); g_jitRomBacking = nullptr;
        free(g_jitRamBacking); g_jitRamBacking = nullptr;
        log3dsWrite("svcControlProcessMemory ROM map failed (0x%08lX); falling back to interpreter", static_cast<unsigned long>(r1));
        return;
    }

    Result r2 = svcControlProcessMemory(g_jitProcHandle,
        reinterpret_cast<u32>(g_jitRamVaddr), reinterpret_cast<u32>(g_jitRamBacking),
        static_cast<u32>(g_jitRamSize), MEMOP_MAP, rwx);
    if (R_FAILED(r2)) {
        svcControlProcessMemory(g_jitProcHandle,
            reinterpret_cast<u32>(g_jitRomVaddr), reinterpret_cast<u32>(g_jitRomBacking),
            static_cast<u32>(g_jitRomSize), MEMOP_UNMAP, 0b111);
        svcCloseHandle(g_jitProcHandle);
        g_jitProcHandle = 0;
        free(g_jitRomBacking); g_jitRomBacking = nullptr;
        free(g_jitRamBacking); g_jitRamBacking = nullptr;
        log3dsWrite("svcControlProcessMemory RAM map failed (0x%08lX); falling back to interpreter", static_cast<unsigned long>(r2));
        return;
    }

    g_jitMapped = true;
    gpsp_bridge_attach_jit(g_jitRomBacking, g_jitRamBacking);
    log3dsWrite("ARM11 dynarec cache mapped (%lu KB ROM, %lu KB RAM)",
        static_cast<unsigned long>(g_jitRomSize / 1024),
        static_cast<unsigned long>(g_jitRamSize / 1024));
}

static void teardownDynarecMapping() {
    if (!g_jitMapped) return;

    gpsp_bridge_detach_jit();

    if (g_jitProcHandle) {
        svcControlProcessMemory(g_jitProcHandle,
            reinterpret_cast<u32>(g_jitRomVaddr), reinterpret_cast<u32>(g_jitRomBacking),
            static_cast<u32>(g_jitRomSize), MEMOP_UNMAP, 0b111);
        svcControlProcessMemory(g_jitProcHandle,
            reinterpret_cast<u32>(g_jitRamVaddr), reinterpret_cast<u32>(g_jitRamBacking),
            static_cast<u32>(g_jitRamSize), MEMOP_UNMAP, 0b111);
        svcCloseHandle(g_jitProcHandle);
        g_jitProcHandle = 0;
    }

    free(g_jitRomBacking); g_jitRomBacking = nullptr;
    free(g_jitRamBacking); g_jitRamBacking = nullptr;
    g_jitMapped = false;
}

//---------------------------------------------------------
// Core lifecycle
//---------------------------------------------------------
bool impl3dsInitialize() {
    log3dsWrite("load up and initialize screen shader");
    gpu3dsLoadShader(SPROGRAM_SCREEN,
        const_cast<u32*>(reinterpret_cast<const u32*>(shader_screen_shbin)),
        shader_screen_shbin_size, 0);

    if (!gpu3dsInitializeShaderUniformLocations()) {
        return false;
    }

    u32 gameTexParams = GPU_TEXTURE_MAG_FILTER(GPU_NEAREST)
                      | GPU_TEXTURE_MIN_FILTER(GPU_NEAREST)
                      | GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_BORDER)
                      | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_BORDER);

    const SGPUTextureConfig gameTexConfig = {
        gameTexParams, GAME_MAIN, GPU_RGB565, GAME_TEX_DIM, GAME_TEX_DIM
    };

    if (!gpu3dsAllocVramTexture(&GPU3DS.textures[GAME_MAIN], &gameTexConfig)) {
        log3dsWrite("Unable to allocate VRAM texture GAME_MAIN");
        return false;
    }

    int vboScreenSize = static_cast<int>(gpu3dsGetNextPowerOf2(sizeof(SQuadVertex) * MAX_VERTICES_QUAD * 2));
    SVertexListInfo screenVboInfo = {
        VBO_SCREEN, vboScreenSize, sizeof(SQuadVertex), 4,
        {
            { GPU_FLOAT, 4 },
            { GPU_FLOAT, 2 },
            { GPU_UNSIGNED_BYTE, 4 },
            { GPU_UNSIGNED_BYTE, 4 }
        }
    };

    if (!gpu3dsAllocVertexList(&screenVboInfo)) {
        log3dsWrite("Unable to allocate VBO_SCREEN");
        return false;
    }

    g_fbLinear = static_cast<u16*>(linearAlloc(GAME_FB_LINEAR_BYTES));
    if (!g_fbLinear) {
        log3dsWrite("Unable to allocate linear frame buffer");
        return false;
    }

    g_fbPrev.resize(GAME_FRAME_PIXELS, 0);
    g_audioOutBuf.resize(AUDIO_FRAME_OUT_CAP * 2, 0);

    setupDynarecMapping();

    if (!gpsp_bridge_init()) {
        teardownDynarecMapping();
        log3dsWrite("Unable to initialize gpSP core");
        return false;
    }

    clearFramebuffers();

    log3dsWrite("gpSP core initialized");
    return true;
}

void impl3dsFinalize() {
    for (int i = 0; i < VBO_COUNT; i++) {
        gpu3dsDeallocVertexList(&GPU3DS.vertices[i]);
    }
    for (int i = 0; i < TEX_COUNT; i++) {
        gpu3dsDestroyTexture(&GPU3DS.textures[i]);
    }

    teardownDynarecMapping();
    gpsp_bridge_finalize();

    if (g_fbLinear) {
        linearFree(g_fbLinear);
        g_fbLinear = nullptr;
    }

    g_fbPrev.clear();
    g_audioOutBuf.clear();
    g_sramShadow.clear();
}

static void impl3dsCacheRomAssetInfo(const char* romFilePath) {
    static const struct { const char* ext; const char* folderName; } assets[] = {
        { ".png", GAME_SCREEN_BG_DIR },
        { ".png", SECOND_SCREEN_BG_DIR },
        { ".png", OVERLAYS_DIR },
        { ".cht", CHEATS_DIR },
    };

    char path[PATH_MAX];
    for (const auto& asset : assets) {
        file3dsGetRelatedPath(romFilePath, path, sizeof(path), asset.ext, asset.folderName, true);
        file3dsGetFileInfo(path);
    }
}

bool impl3dsLoadROM(char* romFilePath) {
    if (!romFilePath || romFilePath[0] == '\0') return false;

    impl3dsCacheRomAssetInfo(romFilePath);

    u32 diskSize = 0;
    u32 fileCrc = computeFileCrc32(romFilePath, &diskSize);
    if (fileCrc == 0 || diskSize == 0) {
        log3dsWrite("Unable to read GBA ROM: %s", romFilePath);
        return false;
    }

    char biosPath[PATH_MAX];
    snprintf(biosPath, sizeof(biosPath), "%s/gba_bios.bin", settings3DS.RootDir);
    bool allowOfficialBios = (settings3DS.GbaBiosMode == Setting::GbaBiosMode::Auto);

    GpspRomMeta meta = {};
    if (!gpsp_bridge_load_rom(romFilePath, biosPath, allowOfficialBios, settings3DS.GbaDynarec, &meta)) {
        log3dsWrite("gpSP failed to load ROM: %s", romFilePath);
        return false;
    }

    memset(&coreRomInfo, 0, sizeof(coreRomInfo));
    snprintf(coreRomInfo.romFilename, sizeof(coreRomInfo.romFilename), "%s", romFilePath);
    if (meta.title[0] != '\0') {
        snprintf(coreRomInfo.romName, sizeof(coreRomInfo.romName), "%s", meta.title);
    } else {
        utils3dsGetBasename(romFilePath, coreRomInfo.romName, sizeof(coreRomInfo.romName), false);
    }
    snprintf(coreRomInfo.gameCode, sizeof(coreRomInfo.gameCode), "%s", meta.gameCode);
    snprintf(coreRomInfo.mapperName, sizeof(coreRomInfo.mapperName), "%s", meta.backupName);
    coreRomInfo.romCrc32 = fileCrc;
    coreRomInfo.romSizeBytes = diskSize;
    coreRomInfo.ramSizeBytes = meta.ramSizeBytes;
    coreRomInfo.hasBattery = meta.hasBattery;
    coreRomInfo.hasRtc = meta.hasRtc;
    coreRomInfo.usingOfficialBios = meta.usingOfficialBios;
    coreRomInfo.dynarecActive = meta.dynarecActive;

    // Load .sav into gpSP's 128 KiB backup buffer.
    size_t sramBufSize = gpsp_bridge_sram_size();
    void* sramPtr = gpsp_bridge_sram_ptr();
    g_sramShadow.assign(sramBufSize, 0xFF);

    if (sramPtr && sramBufSize > 0) {
        char sramPath[PATH_MAX];
        sav3dsGetSRAMPath(sramPath, sizeof(sramPath));
        FILE* sav = file3dsOpen(sramPath, "rb");
        if (sav) {
            size_t got = fread(sramPtr, 1, sramBufSize, sav);
            if (got > 0) {
                memcpy(g_sramShadow.data(), sramPtr, got);
                coreRomInfo.hasBattery = true;
            }
            file3dsClose(sav);
        }
    }

    g_sramCheckCounter = 0;
    snd3dsClearRingBuffer();
    clearFramebuffers();

    impl3dsApplyCoreSettings();
    impl3dsLoadCheats();

    return true;
}

void impl3dsResetConsole() {
    gpsp_bridge_reset();
    snd3dsClearRingBuffer();
    g_hasPrevFrame = false;
    g_hasValidFrame = false;
    g_pendingDraw = false;

    impl3dsApplyCoreSettings();
    impl3dsApplyCheats();
}

void impl3dsApplyCoreSettings() {
    if (g_hasValidFrame) {
        updateLinearBufferFromCore(false);
        uploadLinearFrameToVram();
    }
}

void impl3dsUpdateUiAssets() {
    const struct UiAssetConfig {
        SGPU_TEXTURE_ID id;
        int settingValue;
        const char* folderName;
    } assets[] = {
        { UI_OVERLAY,   static_cast<int>(settings3DS.GameOverlay),    OVERLAYS_DIR },
        { UI_BG_GAME,   static_cast<int>(settings3DS.GameScreenBg),   GAME_SCREEN_BG_DIR },
        { UI_BG_SECOND, static_cast<int>(settings3DS.SecondScreenBg), SECOND_SCREEN_BG_DIR }
    };

    const char* targetPath = (coreRomInfo.romFilename[0] != '\0') ? coreRomInfo.romFilename : "";
    char fileName[PATH_MAX];

    for (const auto& asset : assets) {
        Setting::AssetMode mode = static_cast<Setting::AssetMode>(asset.settingValue);
        bool externalAssetActive = false;

        if (targetPath[0] != '\0' &&
            (mode == Setting::AssetMode::Adaptive || mode == Setting::AssetMode::CustomOnly)) {
            file3dsGetRelatedPath(targetPath, fileName, sizeof(fileName), ".png", asset.folderName, true);
            if (file3dsGetFileInfo(fileName).exists) {
                externalAssetActive = img3dsLoadAsset(asset.id, fileName);
            }
        }

        if (!externalAssetActive) {
            img3dsLoadAsset(asset.id);
        }
    }
}

//---------------------------------------------------------
// Frame execution
//---------------------------------------------------------
void impl3dsRunOneFrame(bool firstFrame, bool skipDrawingFrame) {
    if (firstFrame) {
        g_pendingDraw = false;
    }

    sav3dsProcessSaveCompletion(false);

    if (session3DS.sdWriteFailPending &&
        session3DS.sdWriteFailNotifCount < SD_WRITE_FAIL_NOTIF_LIMIT) {
        session3DS.sdWriteFailPending = false;
        session3DS.sdWriteFailNotifCount++;
        notif3dsTrigger(Notif::Misc, Notif::Type::Error, 3000, "Writing to SD card failed!");
    }

    notif3dsTick();
    notif3dsSync();

    if (g_pendingDraw) {
        gpu3dsWaitForRenderQueue();
        uploadLinearFrameToVram();
    }

    const ScreenshotRegion* screenshotRegion = screenshot.pending ? &screenshot.region : nullptr;
    GPU3DS.screenshotScale = screenshotRegion ? screenshotRegion->scale : 0.0f;

    gpu3dsFrameBegin(0, g_pendingDraw);
    if (firstFrame || g_pendingDraw) {
        t3dsStartTimer(TIMER_DRAW_SCENE);
        impl3dsSceneRender(firstFrame, false, screenshotRegion);
        t3dsStopTimer(TIMER_DRAW_SCENE);
    }
    gpu3dsFrameEnd();
    GPU3DS.screenshotScale = 0.0f;

    g_pendingDraw = false;

    if (screenshot.pending && g_hasValidFrame) {
        sav3dsTakeScreenshot();
    }

    input3dsScanInputForEmulation();
    gpsp_bridge_set_input(gatherCoreInputMask());

    t3dsStartTimer(TIMER_CORE_RUN);
    size_t audioFrames = 0;
    gpsp_bridge_run_frame(skipDrawingFrame, g_audioOutBuf.data(), AUDIO_FRAME_OUT_CAP, &audioFrames);
    if (audioFrames > 0) {
        snd3dsPushSamples(g_audioOutBuf.data(), static_cast<int>(audioFrames));
    }

    if (!skipDrawingFrame) {
        bool allowBlend = settings3DS.InterframeBlending && !session3DS.turboMode;
        updateLinearBufferFromCore(allowBlend);
        g_hasValidFrame = true;
        g_pendingDraw = true;
    } else {
        g_pendingDraw = false;
    }
    t3dsStopTimer(TIMER_CORE_RUN);

    if (++g_sramCheckCounter >= SRAM_DIRTY_CHECK_FRAMES) {
        g_sramCheckCounter = 0;
        if (impl3dsIsSRAMDirty()) {
            sav3dsRequestSRAMSave();
        }
    }
}

bool impl3dsHasPendingDraw() {
    return g_pendingDraw;
}

//---------------------------------------------------------
// Save states & SRAM
//---------------------------------------------------------
size_t impl3dsSerializeState(u8* dst, size_t capacity) {
    if (!session3DS.isRomLoaded || !dst) return 0;
    return gpsp_bridge_serialize(dst, capacity);
}

bool impl3dsUnserializeState(const u8* src, size_t size) {
    if (!session3DS.isRomLoaded || !src || size == 0) return false;

    bool ok = gpsp_bridge_unserialize(src, size);
    if (ok) {
        snd3dsClearRingBuffer();
        g_hasPrevFrame = false;
    }
    return ok;
}

bool impl3dsHasBatterySave() {
    if (!session3DS.isRomLoaded) return false;
    if (coreRomInfo.hasBattery) return true;

    const u8* sramPtr = static_cast<const u8*>(gpsp_bridge_sram_ptr());
    size_t sramSize = gpsp_bridge_sram_size();
    return sramPtr && hasAnyNonFFBytes(sramPtr, sramSize);
}

bool impl3dsIsSRAMDirty() {
    if (!session3DS.isRomLoaded) return false;

    const u8* sramPtr = static_cast<const u8*>(gpsp_bridge_sram_ptr());
    size_t sramSize = gpsp_bridge_sram_size();
    if (!sramPtr || sramSize == 0 || g_sramShadow.size() != sramSize) {
        return false;
    }

    return memcmp(sramPtr, g_sramShadow.data(), sramSize) != 0;
}

size_t impl3dsCopySRAM(u8* dst, size_t capacity) {
    if (!session3DS.isRomLoaded || !dst) return 0;

    const u8* sramPtr = static_cast<const u8*>(gpsp_bridge_sram_ptr());
    size_t sramSize = gpsp_bridge_sram_size();
    if (!sramPtr || sramSize == 0 || sramSize > capacity) return 0;
    if (!impl3dsHasBatterySave() && !impl3dsIsSRAMDirty()) return 0;

    coreRomInfo.hasBattery = true;
    memcpy(dst, sramPtr, sramSize);
    if (g_sramShadow.size() == sramSize) {
        memcpy(g_sramShadow.data(), sramPtr, sramSize);
    }
    return sramSize;
}

//---------------------------------------------------------
// Cheats (RetroArch .cht; CodeBreaker GBA)
//---------------------------------------------------------
static void copyCheatsFromParsed(const std::vector<ParsedCheat>& parsed) {
    memset(&coreCheats, 0, sizeof(coreCheats));
    u32 count = static_cast<u32>(std::min(parsed.size(), static_cast<size_t>(MAX_CHEATS)));
    for (u32 i = 0; i < count; i++) {
        snprintf(coreCheats.c[i].name, sizeof(coreCheats.c[i].name), "%s", parsed[i].name.c_str());
        snprintf(coreCheats.c[i].code, sizeof(coreCheats.c[i].code), "%s", parsed[i].code.c_str());
        coreCheats.c[i].enabled = parsed[i].enabled;
    }
    coreCheats.numCheats = count;
}

void impl3dsApplyCheats() {
    if (!session3DS.isRomLoaded) return;

    gpsp_bridge_cheats_clear();

    unsigned activeIdx = 0;
    for (u32 i = 0; i < coreCheats.numCheats && activeIdx < GPSP_MAX_ACTIVE_CHEATS; i++) {
        if (!coreCheats.c[i].enabled) continue;

        std::string normalized = coreCheats.c[i].code;
        for (char& ch : normalized) {
            if (ch == '+' || ch == ';') ch = ' ';
        }
        if (gpsp_bridge_cheat_add(activeIdx, normalized.c_str())) {
            activeIdx++;
        }
    }
}

void impl3dsLoadCheats() {
    memset(&coreCheats, 0, sizeof(coreCheats));
    if (coreRomInfo.romFilename[0] == '\0') {
        impl3dsApplyCheats();
        return;
    }

    char path[PATH_MAX];
    file3dsGetRelatedPath(coreRomInfo.romFilename, path, sizeof(path), ".cht", CHEATS_DIR, true);

    if (file3dsGetFileInfo(path).exists) {
        FILE* fp = file3dsOpen(path, "rb");
        if (fp) {
            struct stat st = {};
            if (fstat(fileno(fp), &st) == 0 && st.st_size > 0 && st.st_size < 1024 * 1024) {
                std::string text(static_cast<size_t>(st.st_size), '\0');
                size_t got = fread(&text[0], 1, text.size(), fp);
                text.resize(got);
                std::vector<ParsedCheat> parsed;
                parseChtText(text.c_str(), parsed, MAX_CHEATS);
                copyCheatsFromParsed(parsed);
            }
            file3dsClose(fp);
        }
    }

    impl3dsApplyCheats();
}

void impl3dsReloadCheats() {
    if (!session3DS.isRomLoaded) return;

    impl3dsLoadCheats();
    session3DS.cheatsDirty = false;
}

void impl3dsSaveCheats() {
    if (!session3DS.cheatsDirty || !session3DS.isRomLoaded) return;

    char path[PATH_MAX];
    file3dsGetRelatedPath(coreRomInfo.romFilename, path, sizeof(path), ".cht", CHEATS_DIR, true);

    if (coreCheats.numCheats == 0) {
        if (remove(path) == 0) {
            file3dsSetFileInfo(path, { false, 0 });
        }
        session3DS.cheatsDirty = false;
        return;
    }

    std::vector<ParsedCheat> entries;
    entries.reserve(coreCheats.numCheats);
    for (u32 i = 0; i < coreCheats.numCheats; i++) {
        ParsedCheat pc;
        pc.name = coreCheats.c[i].name;
        pc.code = coreCheats.c[i].code;
        pc.enabled = coreCheats.c[i].enabled;
        entries.push_back(pc);
    }

    std::string text = serializeChtText(entries);
    FILE* fp = file3dsOpen(path, "wb");
    if (fp) {
        size_t written = fwrite(text.data(), 1, text.size(), fp);
        file3dsClose(fp);
        if (written == text.size()) {
            struct stat st = {};
            u64 mtime = (stat(path, &st) == 0) ? static_cast<u64>(st.st_mtime) : 0;
            file3dsSetFileInfo(path, { true, mtime });
        } else {
            session3DS.sdWriteFailPending = true;
        }
    } else {
        session3DS.sdWriteFailPending = true;
    }

    session3DS.cheatsDirty = false;
}

void impl3dsDeleteCheat(int index) {
    if (index < 0 || static_cast<u32>(index) >= coreCheats.numCheats) return;

    for (u32 i = static_cast<u32>(index); i + 1 < coreCheats.numCheats; i++) {
        coreCheats.c[i] = coreCheats.c[i + 1];
    }
    coreCheats.numCheats--;
    memset(&coreCheats.c[coreCheats.numCheats], 0, sizeof(CoreCheat));

    session3DS.cheatsDirty = true;
    impl3dsApplyCheats();
}

//---------------------------------------------------------
// Screenshots and thumbnails
//---------------------------------------------------------
void impl3dsPrepareScreenshot(float scale, bool centered) {
    if (screenshot.pending) return;

    screenshot.region.scale = scale;
    screenshot.region.width = static_cast<u16>(CORE_SCREEN_WIDTH * scale);
    screenshot.region.height = static_cast<u16>(CORE_SCREEN_HEIGHT * scale);

    if (centered) {
        screenshot.region.x = static_cast<u16>((settings3DS.GameScreenWidth - screenshot.region.width) / 2);
        screenshot.region.y = static_cast<u16>((SCREEN_HEIGHT - screenshot.region.height) / 2);
    } else {
        screenshot.region.x = static_cast<u16>(settings3DS.GameScreenWidth - screenshot.region.width);
        screenshot.region.y = static_cast<u16>(SCREEN_HEIGHT - screenshot.region.height);
    }
    screenshot.pending = true;
}

void impl3dsCaptureScreenshot(u8* pixels, bool renderFrame, int* width, int* height) {
    (void)renderFrame;
    screenshot.pending = false;

    if (!pixels || !g_fbLinear) {
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }

    u8* dst = pixels;
    for (int y = 0; y < CORE_SCREEN_HEIGHT; y++) {
        const u16* row = g_fbLinear + y * GAME_TEX_DIM;
        for (int x = 0; x < CORE_SCREEN_WIDTH; x++) {
            u16 p = row[x];
            u32 r5 = (p >> 11) & 0x1F;
            u32 g6 = (p >> 5) & 0x3F;
            u32 b5 = p & 0x1F;
            *dst++ = static_cast<u8>((r5 << 3) | (r5 >> 2));
            *dst++ = static_cast<u8>((g6 << 2) | (g6 >> 4));
            *dst++ = static_cast<u8>((b5 << 3) | (b5 >> 2));
        }
    }

    if (width) *width = CORE_SCREEN_WIDTH;
    if (height) *height = CORE_SCREEN_HEIGHT;
    skipNextFpsUpdate = true;
}

bool impl3dsCaptureThumbnail(u16* pixels, size_t pixelsCapacity, int* width, int* height) {
    const int thumbW = SAVE_THUMBNAIL_WIDTH;       // 120
    const int thumbH = SAVE_THUMBNAIL_MAX_HEIGHT;  // 80
    const size_t needed = static_cast<size_t>(thumbW * thumbH) * sizeof(u16);

    if (!pixels || !g_fbLinear || !g_hasValidFrame || needed > pixelsCapacity) {
        return false;
    }

    for (int y = 0; y < thumbH; y++) {
        const u16* row0 = g_fbLinear + (y * 2) * GAME_TEX_DIM;
        const u16* row1 = g_fbLinear + (y * 2 + 1) * GAME_TEX_DIM;
        int dstRow = thumbH - 1 - y;

        for (int x = 0; x < thumbW; x++) {
            u16 p00 = row0[x * 2];
            u16 p01 = row0[x * 2 + 1];
            u16 p10 = row1[x * 2];
            u16 p11 = row1[x * 2 + 1];

            u32 r = (((p00 >> 11) & 0x1F) + ((p01 >> 11) & 0x1F) +
                     ((p10 >> 11) & 0x1F) + ((p11 >> 11) & 0x1F) + 2) >> 2;
            u32 g = (((p00 >> 5)  & 0x3F) + ((p01 >> 5)  & 0x3F) +
                     ((p10 >> 5)  & 0x3F) + ((p11 >> 5)  & 0x3F) + 2) >> 2;
            u32 b = ((p00 & 0x1F)         + (p01 & 0x1F)         +
                     (p10 & 0x1F)         + (p11 & 0x1F)         + 2) >> 2;

            pixels[x * thumbH + dstRow] = static_cast<u16>((r << 11) | (g << 5) | b);
        }
    }

    if (width) *width = thumbW;
    if (height) *height = thumbH;
    return true;
}

//---------------------------------------------------------
// Display & cache control
//---------------------------------------------------------
static void impl3dsApplyCacheOp(gfxScreen_t screen, bool isStereo, bool isWide, GSP_CacheCallback cacheOp) {
    (void)isWide;
    u16 w = 0, h = 0;
    u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, &w, &h);

    u32 bpp = 2;
    switch (gfxGetScreenFormat(screen)) {
        case GSP_RGBA8_OES: bpp = 4; break;
        case GSP_BGR8_OES:  bpp = 3; break;
        default:            bpp = 2; break;
    }

    u32 dataSize = static_cast<u32>(w) * static_cast<u32>(h) * bpp;
    cacheOp(fb, dataSize);

    if (screen == GFX_TOP && isStereo) {
        u8* fbRight = gfxGetFramebuffer(screen, GFX_RIGHT, &w, &h);
        cacheOp(fbRight, dataSize);
    }
}

void impl3dsFlushScreen(gfxScreen_t screen, bool isTopStereo, bool isWide) {
    impl3dsApplyCacheOp(screen, isTopStereo, isWide, GSPGPU_FlushDataCache);
}

void impl3dsInvalidateScreen(gfxScreen_t screen, bool isTopStereo, bool isWide) {
    impl3dsApplyCacheOp(screen, isTopStereo, isWide, GSPGPU_InvalidateDataCache);
}

void impl3dsClearTopFramebuffers() {
    u32 bpp = 2;
    switch (gfxGetScreenFormat(GFX_TOP)) {
        case GSP_RGBA8_OES: bpp = 4; break;
        case GSP_BGR8_OES:  bpp = 3; break;
        default:            bpp = 2; break;
    }

    for (int page = 0; page < 2; page++) {
        u16 w = 0, h = 0;
        u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
        u32 dataSize = static_cast<u32>(w) * static_cast<u32>(h) * bpp;
        if (fb && dataSize > 0) {
            memset(fb, 0, dataSize);
            GSPGPU_FlushDataCache(fb, dataSize);
        }
        gfxScreenSwapBuffers(GFX_TOP, false);
    }
}

static float impl3dsGetPauseAnimationProgress(bool paused) {
    u64 now = osGetTime();

    if (!paused) {
        pauseAnimationStartedAt = 0;
        return 1.0f;
    }

    if (!pauseAnimationStartedAt) {
        pauseAnimationStartedAt = now;
    }

    u64 elapsed = now - pauseAnimationStartedAt;
    if (elapsed >= PAUSE_ANIMATION_DURATION_MS) {
        return 1.0f;
    }

    return ui3dsSmoothstep(static_cast<float>(elapsed) / PAUSE_ANIMATION_DURATION_MS);
}

bool impl3dsPauseAnimationRunning() {
    return pauseAnimationStartedAt && (osGetTime() - pauseAnimationStartedAt < PAUSE_ANIMATION_DURATION_MS);
}

void impl3dsSceneRender(bool firstFrame, bool paused, const ScreenshotRegion* screenshotRegion) {
    (void)firstFrame;

    SVertexList* list = &GPU3DS.vertices[VBO_SCREEN];
    GameScreenViewport vp = {};
    float pauseProgress = screenshotRegion ? 1.0f : impl3dsGetPauseAnimationProgress(paused);

    bool drawBackground = true;
    bool drawOverlays = true;
    bool balancedFilter = false;
    GPU_TEXTURE_FILTER_PARAM baseFilter = GPU_NEAREST;

    if (screenshotRegion) {
        vp.sWidth = screenshotRegion->width;
        vp.sHeight = screenshotRegion->height;
        vp.cHeight = screenshotRegion->height;
        vp.sx0 = screenshotRegion->x;
        vp.sy0 = screenshotRegion->y;
        vp.sx1 = vp.sx0 + vp.sWidth;
        vp.sy1 = vp.sy0 + vp.cHeight;
        vp.tx0 = 0.0f;
        vp.tx1 = static_cast<float>(CORE_SCREEN_WIDTH);
        vp.ty0 = static_cast<float>(GAME_TEX_DIM);
        vp.ty1 = static_cast<float>(GAME_TEX_DIM - CORE_SCREEN_HEIGHT);
        drawBackground = false;
        drawOverlays = false;
    } else {
        vp.sWidth = settings3DS.StretchWidth;
        vp.sHeight = (settings3DS.StretchHeight == -1) ? CORE_SCREEN_HEIGHT : settings3DS.StretchHeight;

        int cropTopSource = 0;
        int cropBottomSource = 0;
        if (settings3DS.CropEnabled) {
            cropTopSource = settings3DS.CropTop;
            cropBottomSource = settings3DS.CropBottom;
            if (cropTopSource < 0) cropTopSource = 0;
            if (cropBottomSource < 0) cropBottomSource = 0;
            if (cropTopSource + cropBottomSource >= CORE_SCREEN_HEIGHT - 16) {
                cropBottomSource = std::max(0, CORE_SCREEN_HEIGHT - 16 - cropTopSource);
            }
        }

        int cropTopPx = cropTopSource;
        int cropBottomPx = cropBottomSource;
        if (vp.sHeight != CORE_SCREEN_HEIGHT) {
            int topEdgePx = (cropTopSource * vp.sHeight + CORE_SCREEN_HEIGHT / 2) / CORE_SCREEN_HEIGHT;
            int bottomEdgePx = ((CORE_SCREEN_HEIGHT - cropBottomSource) * vp.sHeight + CORE_SCREEN_HEIGHT / 2) / CORE_SCREEN_HEIGHT;
            cropTopPx = topEdgePx;
            cropBottomPx = vp.sHeight - bottomEdgePx;
        }

        int cHeight = vp.sHeight - cropTopPx - cropBottomPx;
        bool overscanActive = settings3DS.Overscan && (cHeight > 0) && (cHeight < SCREEN_HEIGHT);
        if (overscanActive) {
            if (vp.sWidth < settings3DS.GameScreenWidth) {
                vp.sWidth = (vp.sWidth * SCREEN_HEIGHT + cHeight / 2) / cHeight;
            }
            vp.cHeight = SCREEN_HEIGHT;
        } else {
            vp.cHeight = cHeight;
        }

        vp.sx0 = (settings3DS.GameScreenWidth - vp.sWidth) / 2;
        vp.sy0 = (SCREEN_HEIGHT - vp.cHeight) / 2;
        vp.sx1 = vp.sx0 + vp.sWidth;
        vp.sy1 = vp.sy0 + vp.cHeight;

        float insetX = (vp.sWidth == CORE_SCREEN_WIDTH) ? 0.0f : 0.5f;
        float insetY = (vp.sHeight == CORE_SCREEN_HEIGHT && !overscanActive) ? 0.0f : 0.5f;

        vp.tx0 = insetX;
        vp.tx1 = static_cast<float>(CORE_SCREEN_WIDTH) - insetX;
        vp.ty0 = static_cast<float>(GAME_TEX_DIM - cropTopSource) - (cropTopSource == 0 ? insetY : 0.0f);
        vp.ty1 = static_cast<float>(GAME_TEX_DIM - (CORE_SCREEN_HEIGHT - cropBottomSource)) + (cropBottomSource == 0 ? insetY : 0.0f);

        bool isFullScreen = vp.sWidth >= settings3DS.GameScreenWidth && vp.cHeight >= SCREEN_HEIGHT;
        drawBackground = !isFullScreen;

        bool isStretched = (settings3DS.ScreenStretch != Setting::ScreenStretch::None) || overscanActive;
        if (isStretched && settings3DS.ScreenFilter == Setting::ScreenFilter::Smooth) {
            baseFilter = GPU_LINEAR;
        } else if (isStretched && settings3DS.ScreenFilter == Setting::ScreenFilter::Balanced) {
            balancedFilter = true;
        }
    }

    if (drawBackground) {
        gpu3dsClearScreen(settings3DS.GameScreen, false);
    }

    GPU3DS.activeSide = GFX_LEFT;
    gpu3dsSetDefaultRenderState(SPROGRAM_SCREEN, false);

    if (drawBackground) {
        img3dsDrawBackground(UI_BG_GAME, paused, 0.0f);
    }

    C3D_TexSetFilter(&GPU3DS.textures[GAME_MAIN].tex, baseFilter, baseFilter);
    GPU3DS.currentRenderState.textureEnv = TEX_ENV_REPLACE_TEXTURE0;
    GPU3DS.currentRenderState.textureBind = GAME_MAIN;

    gpu3dsAddSimpleQuadVertexes(
        static_cast<float>(vp.sx0), static_cast<float>(vp.sy0),
        static_cast<float>(vp.sx1), static_cast<float>(vp.sy1),
        vp.tx0, vp.ty0, vp.tx1, vp.ty1, 0.0f);
    gpu3dsDraw(list, NULL, list->count);

    if (balancedFilter) {
        C3D_TexSetFilter(&GPU3DS.textures[GAME_MAIN].tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexBind(0, &GPU3DS.textures[GAME_MAIN].tex);

        gpu3dsAddSimpleQuadVertexes(
            static_cast<float>(vp.sx0), static_cast<float>(vp.sy0),
            static_cast<float>(vp.sx1), static_cast<float>(vp.sy1),
            vp.tx0, vp.ty0, vp.tx1, vp.ty1, 0.0f, 0xFFFFFF88);

        GPU3DS.currentRenderState.textureEnv = TEX_ENV_REPLACE_TEXTURE0_VERTEX_ALPHA;
        GPU3DS.currentRenderState.textureBind = GAME_MAIN;
        GPU3DS.currentRenderState.alphaBlending = ALPHA_BLENDING_ENABLED;
        gpu3dsDraw(list, NULL, list->count);

        C3D_TexSetFilter(&GPU3DS.textures[GAME_MAIN].tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexBind(0, &GPU3DS.textures[GAME_MAIN].tex);
        GPU3DS.currentRenderState.alphaBlending = ALPHA_BLENDING_DISABLED;
        GPU3DS.currentRenderState.textureEnv = TEX_ENV_REPLACE_TEXTURE0;
    }

    if (drawOverlays) {
        img3dsDrawScanlines(
            static_cast<float>(vp.sx0), static_cast<float>(vp.sy0),
            static_cast<float>(vp.sx1), static_cast<float>(vp.sy1),
            vp.sWidth, vp.cHeight);

        img3dsDrawGameOverlay(UI_OVERLAY, vp.sWidth, vp.cHeight);

        if (paused) {
            img3dsDrawPause(UI_PAUSE, pauseProgress, PAUSE_ANIMATION_Y_OFFSET * (1.0f - pauseProgress));
        }

        notif3dsDraw(UI_NOTIF_MSG);
        notif3dsDraw(UI_NOTIF_FPS);
    }
}

std::string impl3dsGetRomInfo() {
    if (!session3DS.isRomLoaded) return "";

    char buf[256];
    snprintf(buf, sizeof(buf),
        "Title: %s [%s]\nBIOS: %s | CPU: %s\nBackup: %s%s\nROM: %lu KB | RAM: %lu KB\nCRC32: %08lX",
        coreRomInfo.romName,
        coreRomInfo.gameCode,
        coreRomInfo.usingOfficialBios ? "Official" : "Built-in HLE",
        coreRomInfo.dynarecActive ? "ARM Dynarec" : "Interpreter",
        coreRomInfo.mapperName,
        coreRomInfo.hasRtc ? " + RTC" : "",
        static_cast<unsigned long>(coreRomInfo.romSizeBytes / 1024),
        static_cast<unsigned long>(coreRomInfo.ramSizeBytes / 1024),
        static_cast<unsigned long>(coreRomInfo.romCrc32));
    return std::string(buf);
}
