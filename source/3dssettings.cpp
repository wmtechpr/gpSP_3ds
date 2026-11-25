#include <cstring>

#include "3dssettings.h"
#include "3dsimpl.h"
#include "3dssound.h"
#include "3dslcd.h"
#include "3dsui.h"
#include "3dsutils.h"

EmuSettings3DS settings3DS;

u32 settings3dsGetDefaultButtonMapping(int button3ds)
{
    static const u32 defaultButtonMapping[10] = {
      CORE_BTN_A, CORE_BTN_B, CORE_BTN_A, CORE_BTN_B, CORE_BTN_L, CORE_BTN_R, CORE_BTN_L, CORE_BTN_R, CORE_BTN_SELECT, CORE_BTN_START
    };
    return defaultButtonMapping[button3ds];
}

void settings3dsResetGlobalDefaults() {
    settings3DS.RootDir = "sdmc:/3ds/gpSP_3ds";

    memset(settings3DS.defaultDir, 0, sizeof(settings3DS.defaultDir));
    memset(settings3DS.lastSelectedDir, 0, sizeof(settings3DS.lastSelectedDir));
    memset(settings3DS.lastSelectedFilename, 0, sizeof(settings3DS.lastSelectedFilename));

    settings3DS.Theme = Setting::Theme::DarkMode;
    settings3DS.Font  = Setting::Font::Tempesta;
    settings3DS.GameThumbnailType = Setting::ThumbnailMode::None;
    settings3DS.SaveStateScreenshots = true;
    settings3DS.OverwriteSavestatesWithoutAsking = false;
    settings3DS.OverwriteAssetsWithoutAsking = false;
    settings3DS.GameScreen = GFX_TOP;

    settings3DS.LogFileEnabled = false;

    settings3DS.ScreenStretch = Setting::ScreenStretch::Fit_3_2;
    settings3DS.ScreenFilter = Setting::ScreenFilter::Smooth;
    settings3dsApplyScreenStretch();

    settings3DS.TicksPerFrame = TICKS_PER_FRAME_GBA;
    settings3DS.GlobalVolume = 2;

    settings3DS.GbaBiosMode = Setting::GbaBiosMode::Auto;
    settings3DS.GbaColorCorrection = false;
    settings3DS.GbaDynarec = true;
    settings3DS.InterframeBlending = false;

    settings3DS.GameOverlay = Setting::AssetMode::None;
    settings3DS.GameOverlayAutoFit = false;
    settings3DS.ScanlineIntensity = 0;
    settings3DS.GameScreenBg = Setting::AssetMode::Adaptive;
    settings3DS.GameScreenBgOpacity = OPACITY_STEPS / 2;
    settings3DS.SecondScreenBg = Setting::AssetMode::Adaptive;
    settings3DS.SecondScreenBgOpacity = OPACITY_STEPS / 2;

    settings3DS.ShowFPS = false;

    settings3DS.UseGlobalEmuControlKeys = true;
    settings3DS.UseGlobalBindCirclePad = true;
    settings3DS.UseGlobalButtonMappings = true;
    settings3DS.UseGlobalTurbo = false;
    settings3DS.UseGlobalVolume = false;

    for (int i = 0; i < 10; i++)
      settings3DS.GlobalButtonMapping[i][0] = settings3dsGetDefaultButtonMapping(i);

    settings3DS.GlobalBindCirclePad = true;

    for (int i = 0; i < HOTKEYS_COUNT; ++i)
      settings3DS.ButtonHotkeys[i].SetSingleMapping(0);

    for (int i = 0; i < 8; i++)
      settings3DS.GlobalTurbo[i] = 0;
}

void settings3dsResetGameDefaults() {
    settings3DS.CropEnabled = false;
    settings3DS.CropTop = 0;
    settings3DS.CropBottom = 0;
    settings3DS.Overscan = false;
    settings3DS.Volume = settings3DS.GlobalVolume;
    settings3DS.MaxFrameSkips = 1;
    settings3DS.CurrentSaveSlot = 1;
    settings3DS.AutoSavestate = false;
    settings3DS.AudioBuffer = 1;

    // reset controls to global defaults (settings.cfg)
    //
    settings3DS.BindCirclePad = settings3DS.GlobalBindCirclePad;

    for (int i = 0; i < 10; i++) {
        for (int j = 0; j < 4; j++) {
            settings3DS.ButtonMapping[i][j] = settings3DS.GlobalButtonMapping[i][j];
        }
    }

    for (int i = 0; i < 8; i++) {
        settings3DS.Turbo[i] = settings3DS.GlobalTurbo[i];
    }

    for (int i = 0; i < HOTKEYS_COUNT; ++i) {
        settings3DS.ButtonHotkeys[i] = settings3DS.GlobalButtonHotkeys[i];
    }
}

void settings3dsApplyScreenStretch() {
    settings3DS.StretchWidth = CORE_SCREEN_WIDTH;
    settings3DS.StretchHeight = -1;

    switch (settings3DS.ScreenStretch)
    {
        case Setting::ScreenStretch::None:
            break;

        case Setting::ScreenStretch::Fit_3_2:
            if (settings3DS.GameScreen == GFX_TOP) {
                settings3DS.StretchWidth = 360;
                settings3DS.StretchHeight = SCREEN_HEIGHT;
            } else {
                settings3DS.StretchWidth = SCREEN_BOTTOM_WIDTH;
                settings3DS.StretchHeight = 213;
            }
            break;

        case Setting::ScreenStretch::Fit_4_3:
            settings3DS.StretchWidth = 320;
            settings3DS.StretchHeight = SCREEN_HEIGHT;
            break;

        case Setting::ScreenStretch::Full:
            settings3DS.StretchWidth = settings3DS.GameScreen == GFX_TOP ? SCREEN_TOP_WIDTH : SCREEN_BOTTOM_WIDTH;
            settings3DS.StretchHeight = SCREEN_HEIGHT;
            break;
    }
}

void settings3dsUpdate(bool includeGameSettings)
{
    settings3dsApplyScreenStretch();

    if (includeGameSettings)
    {
        settings3DS.TicksPerFrame = TICKS_PER_FRAME_GBA;

        snd3dsApplyOutputVolume();

        if (settings3DS.UseGlobalButtonMappings) {
            for (int i = 0; i < 10; i++)
                for (int j = 0; j < 4; j++)
                    settings3DS.ButtonMapping[i][j] = settings3DS.GlobalButtonMapping[i][j];

            settings3DS.BindCirclePad = settings3DS.GlobalBindCirclePad;
        }

        if (settings3DS.UseGlobalTurbo) {
            for (int i = 0; i < 8; i++)
                settings3DS.Turbo[i] = settings3DS.GlobalTurbo[i];
        }

        if (settings3DS.UseGlobalEmuControlKeys) {
             for (int i = 0; i < HOTKEYS_COUNT; ++i)
                settings3DS.ButtonHotkeys[i] = settings3DS.GlobalButtonHotkeys[i];
        }
    }
}

const char *settings3dsGetAppVersion(const char *prefix) {
    static char version[64];

    if (VERSION_MICRO > 0) {
        snprintf(version, sizeof(version), "%s%d.%d.%d", prefix, VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO);
    } else {
        snprintf(version, sizeof(version), "%s%d.%d", prefix, VERSION_MAJOR, VERSION_MINOR);
    }

    return version;
}
