#ifndef _GPSP_BRIDGE_H_
#define _GPSP_BRIDGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPSP_SCREEN_WIDTH    240
#define GPSP_SCREEN_HEIGHT   160
#define GPSP_SCREEN_PITCH    240
#define GPSP_STATE_MAX_SIZE  (416 * 1024)
#define GPSP_SRAM_MAX_SIZE   (128 * 1024)
#define GPSP_MAX_ACTIVE_CHEATS 20

// Initializes gpSP memory pools, 32,768 Hz audio state, and screen buffer.
bool gpsp_bridge_init(void);
void gpsp_bridge_finalize(void);

// 3DS ARM11 dynarec translation cache virtual/backing attachment.
#ifdef __3DS__
void gpsp_bridge_get_jit_regions(void** romVaddr, size_t* romSize, void** ramVaddr, size_t* ramSize);
void gpsp_bridge_attach_jit(void* romBacking, void* ramBacking);
void gpsp_bridge_detach_jit(void);
#endif

typedef struct {
    char     title[13];
    char     gameCode[5];
    char     backupName[32];
    uint32_t romSizeBytes;
    uint32_t ramSizeBytes;
    bool     hasBattery;
    bool     hasRtc;
    bool     usingOfficialBios;
    bool     dynarecActive;
} GpspRomMeta;

// Loads a GBA ROM (and optional external BIOS) and resets CPU/hardware state.
bool gpsp_bridge_load_rom(const char* romPath, const char* biosPath,
                          bool allowOfficialBios, bool allowDynarec,
                          GpspRomMeta* outMeta);
void gpsp_bridge_reset(void);

// Updates the current GBA input bitmask (bits 0..9 match CORE_BTN_*).
void gpsp_bridge_set_input(uint32_t buttonsMask);

// Runs one emulated frame and extracts stereo PCM16 samples at 32,768 Hz.
void gpsp_bridge_run_frame(bool skipVideo, int16_t* audioOut, size_t audioCapFrames, size_t* outAudioFrames);

const uint16_t* gpsp_bridge_get_framebuffer(void);
uint16_t gpsp_bridge_apply_cc(uint16_t rgb565);

// Save states (fixed 416 KiB BSON image).
size_t gpsp_bridge_state_size(void);
size_t gpsp_bridge_serialize(uint8_t* dst, size_t capacity);
bool gpsp_bridge_unserialize(const uint8_t* src, size_t size);

// Battery save buffer (fixed 128 KiB backing array in gpSP).
void* gpsp_bridge_sram_ptr(void);
size_t gpsp_bridge_sram_size(void);

// Cheats (up to 20 active CodeBreaker cheats).
void gpsp_bridge_cheats_clear(void);
bool gpsp_bridge_cheat_add(unsigned index, const char* code);

#ifdef __cplusplus
}
#endif

#endif
