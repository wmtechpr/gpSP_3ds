#ifndef _3DSSETTINGS_H_
#define _3DSSETTINGS_H_

#include <stdio.h>
#include <array>
#include <limits.h>
#include <3ds.h>

#ifndef VERSION_MAJOR
#define VERSION_MAJOR 0
#endif

#ifndef VERSION_MINOR
#define VERSION_MINOR 0
#endif

#ifndef VERSION_MICRO
#define VERSION_MICRO 0
#endif

#define SCREEN_TOP_WIDTH        400
#define SCREEN_BOTTOM_WIDTH     320
#define SCREEN_HEIGHT           240

#define SAVESLOTS_MAX   5

#define HOTKEY_OPEN_MENU            0
#define HOTKEY_FAST_FORWARD_TOGGLE  1
#define HOTKEY_SCREENSHOT           2
#define HOTKEY_QUICK_SAVE           3
#define HOTKEY_QUICK_LOAD           4
#define HOTKEY_SAVE_SLOT_NEXT       5
#define HOTKEY_SAVE_SLOT_PREV       6
#define HOTKEY_FAST_FORWARD_HOLD    7
#define HOTKEY_ENABLE               8
#define HOTKEYS_COUNT               9

#define OPACITY_STEPS               17
#define SCANLINE_INTENSITY_MAX      8   // 47% brightness

#define MENU_ENTRY_CONTEXT_MENU     -2
#define MENU_CONTINUE_GAME          -3
#define MENU_ENTRY_ITEM_INFO        -4
#define MENU_ENTRY_OPEN_FOLDER      -5
#define MENU_ENTER_SUBPAGE          -100

namespace Setting {
    enum class ScreenFilter {
        Sharp,      // GPU_NEAREST
        Smooth,     // GPU_LINEAR
        Balanced,   // GPU_NEAREST base + low-alpha GPU_LINEAR overlay
    };

    enum class ScreenStretch {
        None,                  // 1:1 Native (240x160)
        Fit_3_2,               // Original 3:2 ratio (360x240 Top, 320x213 Bottom)
        Fit_4_3,               // 4:3 Fit: Stretch to 320 x 240
        Full,                  // Fullscreen: Stretch to GameScreenWidth x 240
    };

    enum class ThumbnailMode {
        // Values are stored in the config; append only.
        None,
        CoverPAL,
        Title,
        Gameplay,
        CoverUS,
        CoverJapan,
        Count
    };

    enum class AssetMode {
        None,
        Default,       // Built-in
        Adaptive,      // Custom, else Default
        CustomOnly,    // Custom or nothing
    };

    enum class Theme {
        DarkMode,
        RetroArch,
        Original,
    };

    enum class Font {
        Tempesta,
        Ronda,
    };

    enum class GbaBiosMode {
        Auto,           // use sdmc:/3ds/gpSP_3ds/gba_bios.bin if valid, else built-in
        BuiltIn,        // always use built-in open-source BIOS
    };
}

template <int Count>
struct ButtonMapping {
    std::array<u32, Count> MappingBitmasks;

    bool operator==(const ButtonMapping& other) const {
        return this->MappingBitmasks == other.MappingBitmasks;
    }

    bool IsHeld(u32 held3dsButtons) const {
        for (u32 mapping : MappingBitmasks) {
            if (mapping != 0 && (mapping & held3dsButtons) == mapping) {
                return true;
            }
        }

        return false;
    }

    void SetSingleMapping(u32 mapping) {
        SetDoubleMapping(mapping, 0);
    }

    void SetDoubleMapping(u32 mapping0, u32 mapping1) {
        if (Count > 0) {
            MappingBitmasks[0] = mapping0;
        }
        if (Count > 1) {
            MappingBitmasks[1] = mapping1;
        }
        if (Count > 2) {
            for (size_t i = 2; i < MappingBitmasks.size(); ++i) {
                MappingBitmasks[i] = 0;
            }
        }
    }
};

typedef struct {

    // --- GENERAL ---
    Setting::Theme Theme;
    Setting::Font Font;
    Setting::ThumbnailMode GameThumbnailType;
    gfxScreen_t GameScreen;
    bool LogFileEnabled;    // Write logs to sdmc:/3ds/gpSP_3ds/debug_<APP_VERSION>_session.log
    int CurrentSaveSlot;    // remember last used save slot (1 - 5)

    // --- FILE MENU ---
    char defaultDir[PATH_MAX];
    char lastSelectedDir[PATH_MAX];
    char lastSelectedFilename[NAME_MAX + 1];

    // --- OSD & VIDEO ---
    Setting::AssetMode  GameOverlay;
    bool                GameOverlayAutoFit;
    int                 ScanlineIntensity;      // 0 - Off, 1..SCANLINE_INTENSITY_MAX - dark-row alpha
    Setting::AssetMode  GameScreenBg;
    int                 GameScreenBgOpacity;    // OPACITY_STEPS = maximum opacity
    Setting::AssetMode  SecondScreenBg;
    int                 SecondScreenBgOpacity;

    bool                ShowFPS;

    Setting::ScreenStretch ScreenStretch;
    Setting::ScreenFilter ScreenFilter;         // User preference for the game texture in stretched modes.
                                                // No Stretch enforces sharp (nearest) at render time.
    bool                CropEnabled;            // master toggle for the per-game crop/overscan settings
    int                 CropTop;                // top crop value in scanlines
    int                 CropBottom;             // bottom crop value in scanlines
    bool                Overscan;               // zoom (cropped) ingame screen to fit height

    // --- GAME BOY ADVANCE ---
    Setting::GbaBiosMode        GbaBiosMode;        // takes effect when a game is (re)loaded
    bool                        GbaColorCorrection; // soft GBA LCD color curve
    bool                        GbaDynarec;         // ARMv6 dynarec on real 3DS
    bool                        InterframeBlending; // mixes consecutive frames (LCD ghosting)

    // --- GAME-SPECIFIC ---
    int                 MaxFrameSkips;          // 0 - disable,
                                                // 1..4 - max consecutive skipped frames

    int                 Volume;                 // 0: 100%, 1: 125%, 2: 150%, 3: 175%, 4: 200%
    int                 GlobalVolume;

    int                 AudioBuffer;            // wavebuf depth: 0=Low(4), 1=Normal(8), 2=High(16)

    bool                AutoSavestate;          // Automatically save the the current state when the emulator is closed or the game is changed
    bool                SaveStateScreenshots;   // Store a thumbnail in each slot savestate
    bool                OverwriteSavestatesWithoutAsking;   // Opt-in: skip confirmation dialog
    bool                OverwriteAssetsWithoutAsking;       // Opt-in: skip confirmation dialog

    // --- CONTROLS ---
    std::array<::ButtonMapping<1>, HOTKEYS_COUNT> ButtonHotkeys;
    std::array<::ButtonMapping<1>, HOTKEYS_COUNT> GlobalButtonHotkeys;

    bool      BindCirclePad;                    // Use Circle Pad as D-Pad for gaming
    bool      GlobalBindCirclePad;

    std::array<std::array<int, 4>, 10> ButtonMapping;
    std::array<std::array<int, 4>, 10> GlobalButtonMapping;

    std::array<int, 8>   Turbo;                 // Turbo buttons: 0 - No turbo, 1 - Release/Press every alt frame.
                                                // Indexes: 0 - A, 1 - B, 2 - X, 3 - Y, 4 - L, 5 - R
    std::array<int, 8>   GlobalTurbo;

    bool      UseGlobalEmuControlKeys;          // Use global emulator control keys for all games
    bool      UseGlobalBindCirclePad;           // Use Circle Pad as D-Pad
    bool      UseGlobalButtonMappings;          // Use global button mappings for all games
    bool      UseGlobalTurbo;
    bool      UseGlobalVolume;

    // --- RUNTIME / CALCULATED ---
    // Not saved to config
    const char           *RootDir;

    gfxScreen_t         SecondScreen;
    int                 GameScreenWidth;
    int                 SecondScreenWidth;

    int                 StretchWidth;
    int                 StretchHeight;
    long                TicksPerFrame;

} EmuSettings3DS;

extern EmuSettings3DS settings3DS;

void settings3dsResetGlobalDefaults();
u32 settings3dsGetDefaultButtonMapping(int button3ds);
void settings3dsResetGameDefaults();
void settings3dsUpdate(bool includeGameSettings);
bool settingsSave(bool includeGameSettings);
void settings3dsApplyScreenLayout();
void settings3dsApplyScreenStretch();

const char *settings3dsGetAppVersion(const char *prefix);

#endif
