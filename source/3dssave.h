#ifndef _3DSSAVE_H_
#define _3DSSAVE_H_

#include <stddef.h>
#include <3ds/types.h>

// Savestate thumbnail: half the native GBA frame (240x160 -> 120x80), swizzled RGB565.
#define SAVE_THUMBNAIL_WIDTH         120
#define SAVE_THUMBNAIL_MAX_HEIGHT    80
#define SAVE_THUMBNAIL_PIXELS_SIZE   (SAVE_THUMBNAIL_WIDTH * SAVE_THUMBNAIL_MAX_HEIGHT * 2)

typedef enum {
    SAVE_WRITE_OK,
    SAVE_WRITE_SKIPPED,  // Previous file untouched.
    SAVE_WRITE_FAILED,   // Write/close failed after opening with wb; savestates attempt removal.
} SaveWriteResult;

bool sav3dsInitialize();
// Requires all worker jobs to have been collected.
void sav3dsFinalize();

SaveWriteResult sav3dsSaveSlot(int slotNumber);
bool sav3dsLoadSlot(int slotNumber);
// No-op without a loaded ROM or with AutoSavestate disabled.
void sav3dsSaveAuto();
void sav3dsLoadAuto();

// Uses CurrentSaveSlot; refuses to start while any worker job is active.
void sav3dsQuickSave();
void sav3dsQuickLoad();

// Defers until the worker is idle.
void sav3dsRequestSRAMSave();
// Synchronous battery save write (used on unload/suspend).
void sav3dsSaveSRAMSync();
// Writes SRAM unconditionally on unload.
void sav3dsSaveSRAMOnUnload();

// Captures the queued hotkey frame and writes asynchronously; refuses while a job is active.
void sav3dsTakeScreenshot();
// Renders a frame and writes synchronously; returns the PNG path in path.
bool sav3dsTakeMenuScreenshot(char* path, size_t bufferSize);

bool sav3dsIsSRAMSaveInProgress();
// Whether pausing should write SRAM.
bool sav3dsSRAMNeedsPauseSave();
// Quick saves and screenshots block hotkeys and defer menu entry; SRAM jobs don't.
bool sav3dsIsQuickSaveOrScreenshotInProgress();
// Main thread only; waitForCompletion waits for the active job and any deferred SRAM write.
void sav3dsProcessSaveCompletion(bool waitForCompletion);

bool sav3dsSlotHasState(int slotNumber);
void sav3dsUpdateSlotState(int slotNumber);
void sav3dsSelectSlot(int direction);

void sav3dsGetSlotPath(int slotNumber, char* out, size_t bufferSize);
void sav3dsGetSRAMPath(char* out, size_t bufferSize);
// Reads swizzled RGB565 pixels into a SAVE_THUMBNAIL_PIXELS_SIZE buffer.
bool sav3dsReadThumbnail(const char* path, u16* pixels, int* width, int* height);

#endif
