#ifndef _3DSIMPL_H
#define _3DSIMPL_H

// Interface between the 3DS shell (menu, UI, saves, audio output) and the
// emulator core. Only 3dsimpl.cpp talks to the core (gpSP, GBA).

#include <3ds.h>
#include <string>

#include "3dscompat.h"
#include "3dssettings.h"

#define BTN3DS_A        0
#define BTN3DS_B        1
#define BTN3DS_X        2
#define BTN3DS_Y        3
#define BTN3DS_L        4
#define BTN3DS_R        5
#define BTN3DS_ZL       6
#define BTN3DS_ZR       7
#define BTN3DS_SELECT   8
#define BTN3DS_START    9

// Native Game Boy Advance screen.
#define CORE_SCREEN_WIDTH   240
#define CORE_SCREEN_HEIGHT  160

// Joypad bits; match gpSP's BUTTON_* enum in input.h / REG_P1 bits.
#define CORE_BTN_A       0x001
#define CORE_BTN_B       0x002
#define CORE_BTN_SELECT  0x004
#define CORE_BTN_START   0x008
#define CORE_BTN_RIGHT   0x010
#define CORE_BTN_LEFT    0x020
#define CORE_BTN_UP      0x040
#define CORE_BTN_DOWN    0x080
#define CORE_BTN_R       0x100
#define CORE_BTN_L       0x200

typedef struct
{
    u16                         x;
    u16                         y;
    u16                         width;
    u16                         height;
    float                       scale;
} ScreenshotRegion;

// Manual screenshot requested by the hotkey, taken on the next drawn frame.
typedef struct
{
    ScreenshotRegion            region;
    bool                        pending;
} CoreScreenshot;

extern CoreScreenshot screenshot;
extern bool skipNextFpsUpdate;

// Info about the loaded ROM, filled by impl3dsLoadROM.
typedef struct
{
    char    romFilename[PATH_MAX];  // full path
    char    romName[32];            // header title
    char    gameCode[8];            // 4-char GBA game code
    char    mapperName[32];         // backup type (e.g. "FLASH 128KB")
    u32     romCrc32;
    u32     romSizeBytes;
    u32     ramSizeBytes;           // battery save size
    bool    hasBattery;
    bool    hasRtc;
    bool    usingOfficialBios;
    bool    dynarecActive;
} CoreRomInfo;

extern CoreRomInfo coreRomInfo;

#define MAX_CHEATS 256

typedef struct
{
    char    name[64];
    char    code[64];       // one or more codes joined by '+'
    bool    enabled;
} CoreCheat;

typedef struct
{
    CoreCheat   c[MAX_CHEATS];
    u32         numCheats;
} CoreCheatData;

extern CoreCheatData coreCheats;

//---------------------------------------------------------
// Core lifecycle
//---------------------------------------------------------
bool impl3dsInitialize();
void impl3dsFinalize();

// Loads the ROM (and its battery save). romFilePath is a full path.
bool impl3dsLoadROM(char *romFilePath);
void impl3dsResetConsole();

// Re-applies palette / color correction settings that can change at runtime.
void impl3dsApplyCoreSettings();

// Executes one frame: input, emulation, audio push, drawing.
// firstFrame is true for the first frame after boot/returning from the menu.
void impl3dsRunOneFrame(bool firstFrame, bool skipDrawingFrame);

//---------------------------------------------------------
// Save data
//---------------------------------------------------------
// Serializes the emulation state into dst. Returns the byte count, 0 on failure.
size_t impl3dsSerializeState(u8 *dst, size_t capacity);
bool impl3dsUnserializeState(const u8 *src, size_t size);

bool impl3dsHasBatterySave();
// True when battery RAM / RTC differ from what was last saved.
bool impl3dsIsSRAMDirty();
// Copies battery RAM (+RTC) into dst. Returns the byte count, 0 if nothing to save.
// Marks SRAM as clean.
size_t impl3dsCopySRAM(u8 *dst, size_t capacity);
// Size of the largest buffer impl3dsCopySRAM may need (128 KiB Flash/SRAM).
#define CORE_SRAM_MAX_SIZE  (128 * 1024)

//---------------------------------------------------------
// Cheats (RetroArch .cht files; CodeBreaker GBA)
//---------------------------------------------------------
void impl3dsLoadCheats();
void impl3dsReloadCheats();
void impl3dsApplyCheats();
void impl3dsSaveCheats();
void impl3dsDeleteCheat(int index);

//---------------------------------------------------------
// Screenshots and thumbnails
//---------------------------------------------------------
void impl3dsPrepareScreenshot(float scale = 1.0f, bool centered = true);
// RGB8 capture of the last emulated frame; pixels needs CORE_SCREEN_WIDTH * CORE_SCREEN_HEIGHT * 3 bytes.
// renderFrame is ignored (the core's frame buffer is read directly).
void impl3dsCaptureScreenshot(u8* pixels, bool renderFrame, int* width, int* height);
// Half-size capture as RGB565 in column-major order, top row last (see img3dsDrawSwizzledRgb565).
bool impl3dsCaptureThumbnail(u16* pixels, size_t pixelsCapacity, int* width, int* height);

//---------------------------------------------------------
// Display
//---------------------------------------------------------
void impl3dsUpdateUiAssets();
void impl3dsFlushScreen(gfxScreen_t screen, bool isTopStereo = false, bool isWide = false);
void impl3dsInvalidateScreen(gfxScreen_t screen, bool isTopStereo = false, bool isWide = false);
void impl3dsClearTopFramebuffers();

// Draws the current game frame (plus bezel, background, pause prompt, notifications).
void impl3dsSceneRender(bool firstFrame, bool paused = false, const ScreenshotRegion* screenshotRegion = nullptr);
bool impl3dsPauseAnimationRunning();
// True while a frame was emulated and not drawn yet.
bool impl3dsHasPendingDraw();

// Human readable description of the loaded cart for the ROM info dialog.
std::string impl3dsGetRomInfo();

#endif
