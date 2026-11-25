#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <streams/file_stream.h>
#include <libretro.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-declaration"
#include "common.h"
#pragma GCC diagnostic pop
#include "gba_cc_lut.h"
#include "gpsp_bridge.h"

#if defined(_3DS) && defined(HAVE_DYNAREC)
#include "3ds/3ds_utils.h"
#endif

//---------------------------------------------------------
// Globals required by gpSP when linked without libretro.c
//---------------------------------------------------------
u32 skip_next_frame = 0;
int dynarec_enable = 0;
boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;
u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
u32 netplay_num_clients = 0;
u32 netplay_client_id = 0;

void netpacket_send(uint16_t client_id, const void* buf, size_t len) {
    (void)client_id;
    (void)buf;
    (void)len;
}

void netpacket_poll_receive(void) {
}

void set_fastforward_override(bool fastforward) {
    (void)fastforward;
}

// Non-header gpSP internals needed by ROM/BIOS loader and cleanup.
extern RFILE* gamepak_file_large;
extern u8* gamepak_buffers[32];
extern bool require_m1_hle_bios;

//---------------------------------------------------------
// Bridge local state
//---------------------------------------------------------
#define GBA_FPS ((float)GBC_BASE_RATE / (308.0f * 228.0f * 4.0f))

static uint32_t g_inputMask = 0;
static float g_audioSamplesPerFrame = 0.0f;
static float g_audioSamplesAccum = 0.0f;
static bool g_initialized = false;

static int16_t gpsp_input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) {
        return 0;
    }

    uint32_t mapped = 0;
    for (size_t i = 0; i < sizeof(btn_map) / sizeof(btn_map[0]); i++) {
        if (g_inputMask & (uint32_t)btn_map[i].gba) {
            mapped |= (1u << btn_map[i].retropad);
        }
    }

    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
        return (int16_t)mapped;
    }
    return (id < 16 && (mapped & (1u << id))) ? 1 : 0;
}

bool gpsp_bridge_init(void) {
    if (g_initialized) {
        return true;
    }

    // Native 32,768 Hz output matches 3DS NDSP rate cleanly with exact shifts.
    sound_freq_bits = 15;
    sound_frequency = 32768;
    g_audioSamplesPerFrame = (float)sound_frequency / GBA_FPS;
    g_audioSamplesAccum = 0.0f;

    // Keep heap headroom for 3DS shell/save/UI allocations on 64 MiB (.3dsx) launches;
    // 124 MiB (.cia) still gets the full 32 MiB ROM buffer.
    void* heapHeadroom = malloc(6 * 1024 * 1024);
    init_gamepak_buffer();
    free(heapHeadroom);
    if (gamepak_buffer_count == 0) {
        return false;
    }

    init_sound();

    if (!gba_screen_pixels) {
        gba_screen_pixels = (u16*)calloc(1, GBA_SCREEN_BUFFER_SIZE);
        if (!gba_screen_pixels) {
            memory_term();
            return false;
        }
    }

    retro_set_input_state(gpsp_input_state_cb);
    libretro_supports_bitmasks = true;
    libretro_supports_ff_override = false;

    g_initialized = true;
    return true;
}

void gpsp_bridge_finalize(void) {
    if (!g_initialized) {
        return;
    }

    memory_term();

    free(gba_screen_pixels);
    gba_screen_pixels = NULL;

    g_inputMask = 0;
    g_audioSamplesAccum = 0.0f;
    g_initialized = false;
}

#if defined(_3DS) && defined(HAVE_DYNAREC)
void gpsp_bridge_get_jit_regions(void** romVaddr, size_t* romSize, void** ramVaddr, size_t* ramSize) {
    if (romVaddr) *romVaddr = rom_translation_cache;
    if (romSize)  *romSize  = ROM_TRANSLATION_CACHE_SIZE;
    if (ramVaddr) *ramVaddr = ram_translation_cache;
    if (ramSize)  *ramSize  = RAM_TRANSLATION_CACHE_SIZE;
}

void gpsp_bridge_attach_jit(void* romBacking, void* ramBacking) {
    check_rosalina();
    rom_translation_cache_ptr = (u8*)romBacking;
    ram_translation_cache_ptr = (u8*)ramBacking;
    rom_translation_ptr = rom_translation_cache;
    ram_translation_ptr = ram_translation_cache;
    ctr_flush_invalidate_cache();
}

void gpsp_bridge_detach_jit(void) {
    dynarec_enable = 0;
    rom_translation_cache_ptr = NULL;
    ram_translation_cache_ptr = NULL;
}
#endif

static void fill_rom_meta(GpspRomMeta* out, bool usedOfficialBios) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    if (gamepak_buffers[0]) {
        for (int i = 0; i < 12; i++) {
            u8 c = gamepak_buffers[0][0xA0 + i];
            if (c == 0) break;
            out->title[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : ' ';
        }
        for (int i = (int)strlen(out->title) - 1; i >= 0; i--) {
            if (out->title[i] == ' ') out->title[i] = '\0';
            else break;
        }

        bool validCode = false;
        for (int i = 0; i < 4; i++) {
            u8 c = gamepak_buffers[0][0xAC + i];
            out->gameCode[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                validCode = true;
            }
        }
        out->gameCode[4] = '\0';
        if (!validCode) {
            snprintf(out->gameCode, sizeof(out->gameCode), "UNKN");
        }
    } else {
        snprintf(out->gameCode, sizeof(out->gameCode), "UNKN");
    }

    out->romSizeBytes = gamepak_size;
    out->usingOfficialBios = usedOfficialBios;
    out->dynarecActive = (dynarec_enable != 0);

    switch (backup_type) {
        case BACKUP_SRAM:
            out->ramSizeBytes = 32768u;
            out->hasBattery = true;
            snprintf(out->backupName, sizeof(out->backupName), "SRAM 32KB");
            break;
        case BACKUP_FLASH:
            out->ramSizeBytes = (flash_bank_cnt == FLASH_SIZE_128KB) ? 131072u : 65536u;
            out->hasBattery = true;
            out->hasRtc = (flash_bank_cnt == FLASH_SIZE_128KB);
            snprintf(out->backupName, sizeof(out->backupName), "FLASH %uKB", (unsigned)(out->ramSizeBytes / 1024u));
            break;
        case BACKUP_EEPROM:
            out->ramSizeBytes = (eeprom_size == EEPROM_8_KBYTE) ? 8192u : 512u;
            out->hasBattery = true;
            if (out->ramSizeBytes >= 1024u) {
                snprintf(out->backupName, sizeof(out->backupName), "EEPROM %uKB", (unsigned)(out->ramSizeBytes / 1024u));
            } else {
                snprintf(out->backupName, sizeof(out->backupName), "EEPROM 512B");
            }
            break;
        default:
            out->ramSizeBytes = 0;
            out->hasBattery = false;
            snprintf(out->backupName, sizeof(out->backupName), "NONE / AUTO");
            break;
    }
}

bool gpsp_bridge_load_rom(const char* romPath, const char* biosPath,
                          bool allowOfficialBios, bool allowDynarec,
                          GpspRomMeta* outMeta) {
    if (!g_initialized || !romPath || romPath[0] == '\0') {
        return false;
    }

    // Close previous ROM stream handle before load_gamepak_raw opens the new one.
    if (gamepak_file_large) {
        filestream_close(gamepak_file_large);
        gamepak_file_large = NULL;
    }

    bool biosLoaded = false;
    if (allowOfficialBios && biosPath && biosPath[0] != '\0') {
        char mutablePath[512];
        snprintf(mutablePath, sizeof(mutablePath), "%s", biosPath);
        if (load_bios(mutablePath) == 0 && bios_rom[0] == 0x18) {
            biosLoaded = true;
        }
    }

    if (!biosLoaded) {
        memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
    }

    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
    rtc_set_time_source(RTC_TIME_SOURCE_SYSTEM);

    if (load_gamepak(NULL, romPath, FEAT_AUTODETECT, FEAT_AUTODETECT, SERIAL_MODE_AUTO) != 0) {
        return false;
    }

    if (biosLoaded && (gamepak_header_nonstandard || require_m1_hle_bios)) {
        memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
        biosLoaded = false;
    }

#if defined(_3DS) && defined(HAVE_DYNAREC)
    dynarec_enable = (allowDynarec && !gamepak_mini_materialized &&
                      rom_translation_cache_ptr != NULL && ram_translation_cache_ptr != NULL) ? 1 : 0;
#else
    (void)allowDynarec;
    dynarec_enable = 0;
#endif

    selected_boot_mode = boot_game;
    cheat_clear();
    reset_gba();
    sound_frequency_changed();
    sound_flush_ring();
    g_audioSamplesAccum = 0.0f;

    if (gba_screen_pixels) {
        memset(gba_screen_pixels, 0, GBA_SCREEN_BUFFER_SIZE);
    }

    fill_rom_meta(outMeta, biosLoaded);
    return true;
}

void gpsp_bridge_reset(void) {
    if (!g_initialized) return;

    reset_gba();
    sound_frequency_changed();
    sound_flush_ring();
    g_audioSamplesAccum = 0.0f;
}

void gpsp_bridge_set_input(uint32_t buttonsMask) {
    g_inputMask = buttonsMask;
}

void gpsp_bridge_run_frame(bool skipVideo, int16_t* audioOut, size_t audioCapFrames, size_t* outAudioFrames) {
    if (!g_initialized) {
        if (outAudioFrames) *outAudioFrames = 0;
        return;
    }

    update_input();
    rumble_frame_reset();
    skip_next_frame = skipVideo ? 1 : 0;

#if defined(_3DS) && defined(HAVE_DYNAREC)
    if (dynarec_enable) {
        execute_arm_translate(execute_cycles);
    } else
#endif
    {
        clear_gamepak_stickybits();
        execute_arm(execute_cycles);
    }

    u32 toRead = (u32)g_audioSamplesPerFrame;
    g_audioSamplesAccum += g_audioSamplesPerFrame - (float)toRead;
    if (g_audioSamplesAccum >= 1.0f) {
        toRead++;
        g_audioSamplesAccum -= 1.0f;
    }
    if (audioOut && (size_t)toRead > audioCapFrames) {
        toRead = (u32)audioCapFrames;
    }

    u32 produced = 0;
    if (audioOut && toRead > 0) {
        produced = sound_read_samples((s16*)audioOut, toRead);
    }
    if (outAudioFrames) {
        *outAudioFrames = (size_t)produced;
    }
}

const uint16_t* gpsp_bridge_get_framebuffer(void) {
    return gba_screen_pixels;
}

uint16_t gpsp_bridge_apply_cc(uint16_t rgb565) {
    return gba_cc_lut[((rgb565 & 0xFFC0u) >> 1) | (rgb565 & 0x1Fu)];
}

size_t gpsp_bridge_state_size(void) {
    return GBA_STATE_MEM_SIZE;
}

size_t gpsp_bridge_serialize(uint8_t* dst, size_t capacity) {
    if (!g_initialized || !dst || capacity < GBA_STATE_MEM_SIZE) {
        return 0;
    }
    gba_save_state(dst);
    return GBA_STATE_MEM_SIZE;
}

bool gpsp_bridge_unserialize(const uint8_t* src, size_t size) {
    if (!g_initialized || !src || size < GBA_STATE_MEM_SIZE) {
        return false;
    }
    if (!gba_load_state(src)) {
        return false;
    }
    sound_frequency_changed();
    g_audioSamplesAccum = 0.0f;
    return true;
}

void* gpsp_bridge_sram_ptr(void) {
    return gamepak_backup;
}

size_t gpsp_bridge_sram_size(void) {
    return sizeof(gamepak_backup);
}

void gpsp_bridge_cheats_clear(void) {
    cheat_clear();
}

bool gpsp_bridge_cheat_add(unsigned index, const char* code) {
    if (!code || index >= GPSP_MAX_ACTIVE_CHEATS) {
        return false;
    }
    return cheat_parse(index, code) == CheatNoError;
}
