#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

#include <unistd.h>
#include <dirent.h>

#include <3ds.h>

#include "3dscompat.h"
#include "3dsutils.h"
#include "3dssettings.h"
#include "3dslog.h"
#include "3dstimer.h"
#include "3dsexit.h"
#include "3dsconfig.h"
#include "3dsfiles.h"
#include "3dsinput.h"
#include "3dssound.h"
#include "3dsgpu.h"
#include "3dsimpl.h"
#include "3dssave.h"
#include "3dsui.h"
#include "3dsui_notif.h"
#include "3dslcd.h"
#include "3dsui_img.h"
#include "3dsmenu.h"

inline std::string operator "" _s(const char* s, size_t length) {
    return std::string(s, length);
}

char romFileName[NAME_MAX + 1];
bool slotLoaded = false;

const char* hotkeysData[HOTKEYS_COUNT][3];

static bool cfgFileAvailable[2]; // global config, game config
static u32 lastLoadedRomCRC = 0;
static const DirectoryEntry* selectedEntry = nullptr;

// static globals to prevent heap fragmentation and speed up menu access.
// note: not thread-safe, but safe here due to sequential menu/emulator execution
static std::vector<SMenuTab> menuTabs;
static std::vector<DirectoryEntry> entries;

// file menu scroll offset per directory level: pushed on the way down, restored on the way back up
static std::vector<int> fileMenuScrollStack;

static const int hotkeyDisplayOrder[HOTKEYS_COUNT] = {
    HOTKEY_ENABLE,
    HOTKEY_OPEN_MENU,
    HOTKEY_FAST_FORWARD_TOGGLE,
    HOTKEY_FAST_FORWARD_HOLD,
    HOTKEY_SCREENSHOT,
    HOTKEY_QUICK_SAVE,
    HOTKEY_QUICK_LOAD,
    HOTKEY_SAVE_SLOT_NEXT,
    HOTKEY_SAVE_SLOT_PREV
};

//----------------------------------------------------------------------
// Menu options
//----------------------------------------------------------------------

namespace {
    template <typename T>
    bool CheckAndUpdate( T& oldValue, const T& newValue ) {
        if (oldValue != newValue) {
            oldValue = newValue;
            return true;
        }
        return false;
    }

    bool CheckAndUpdateToggle( bool& oldValue, const int& newValue ) {
        return CheckAndUpdate(oldValue, static_cast<bool>(newValue));
    }

    void AddMenuDialogOption(std::vector<SMenuItem>& items, int value, const std::string& text, const std::string& description = ""_s) {
        items.emplace_back(nullptr, MenuItemType::Action, text, description, value);
    }

    void AddMenuDisabledOption(std::vector<SMenuItem>& items, const std::string& text, int value = -1) {
        items.emplace_back(nullptr, MenuItemType::Disabled, text, ""_s, value);
    }

    void AddMenuSpacer(std::vector<SMenuItem>& items) {
        AddMenuDisabledOption(items, "");
    }

    // Adds one row per wrapped line; font or game screen change has to rebuild the tab
    void AddMenuNote(std::vector<SMenuItem>& items, const std::string& text, MenuItemType type = MenuItemType::Note) {
        size_t indentLength = text.find_first_not_of(' ');
        if (indentLength == std::string::npos)
            indentLength = 0;

        std::string indent = text.substr(0, indentLength);
        const char *body = text.c_str() + indentLength;
        int maxWidth = settings3DS.SecondScreenWidth - 2 * MENU_LIST_H_PADDING - ui3dsGetStringWidth(indent.c_str());

        int lineStart[UI_MAX_WRAPPED_LINES], lineEnd[UI_MAX_WRAPPED_LINES];
        int lineCount = ui3dsWrapLines(body, maxWidth, lineStart, lineEnd, UI_MAX_WRAPPED_LINES);
        for (int i = 0; i < lineCount; i++) {
            items.emplace_back(nullptr, type, indent + std::string(body + lineStart[i], lineEnd[i] - lineStart[i] + 1), "");
        }
    }

    void AddMenuHeader1(std::vector<SMenuItem>& items, const std::string& text) {
        items.emplace_back(nullptr, MenuItemType::Header1, text, ""_s);
    }

    void AddMenuHeader2(std::vector<SMenuItem>& items, const std::string& text) {
        items.emplace_back(nullptr, MenuItemType::Header2, text, ""_s);
    }

    void AddMenuCheckbox(std::vector<SMenuItem>& items, const std::string& text, int value, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Checkbox, text, ""_s, value);
    }

    void AddMenuGauge(std::vector<SMenuItem>& items, const std::string& text, int min, int max, int value, std::function<void(int)> callback, bool showValue = false) {
        items.emplace_back(callback, MenuItemType::Gauge, text, showValue ? "1"_s : ""_s, value, min, max);
    }

    void AddMenuPicker(std::vector<SMenuItem>& items, const std::string& text, const std::string& description, const std::vector<SMenuItem>& options, int value, int dialogType, bool showSelectedOptionInMenu, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Picker, text, ""_s, value, showSelectedOptionInMenu ? 1 : 0, 0, description, options, dialogType);
    }

    // Lets X reset the last added row to value.
    void SetMenuResetValue(std::vector<SMenuItem>& items, int value) {
        items.back().Resettable = true;
        items.back().ResetValue = value;
    }
}

std::vector<SMenuItem> makeOptionsForResetConfig() {
    std::vector<SMenuItem> items;
    items.reserve(4);

    AddMenuDialogOption(items, 0, "Cancel"_s, ""_s);

    if (cfgFileAvailable[0]) {
        AddMenuDialogOption(items, 1, "Global"_s, "settings.cfg"_s);
    }
     
    if (cfgFileAvailable[1]) {
        char gameConfigFilename[128];
        char basename[NAME_MAX + 1];
        utils3dsGetBasename(coreRomInfo.romFilename, basename, sizeof(basename), false);

        if (strlen(basename) > 38) {
            snprintf(gameConfigFilename, sizeof(gameConfigFilename), "%.38s..%s", basename, ".cfg");
        } else {
            snprintf(gameConfigFilename, sizeof(gameConfigFilename), "%s%s", basename, ".cfg");
        }

        AddMenuDialogOption(items, 2, "Game"_s, gameConfigFilename);
    }

    if (cfgFileAvailable[0] && cfgFileAvailable[1]) {
        AddMenuDialogOption(items, 3, "Both"_s, ""_s);
    }
    
    return items;
}

const std::vector<SMenuItem>& makeOptionsForGameThumbnail() {
    static std::vector<SMenuItem> items;

    items.clear();
    // Picker order; the value is the stored enum, so the two can differ.
    static const Setting::ThumbnailMode modes[] = {
        Setting::ThumbnailMode::CoverUS,
        Setting::ThumbnailMode::CoverJapan,
        Setting::ThumbnailMode::CoverPAL,
        Setting::ThumbnailMode::Title,
        Setting::ThumbnailMode::Gameplay,
    };
    items.reserve(1 + sizeof(modes) / sizeof(modes[0]));

    AddMenuDialogOption(items, static_cast<int>(Setting::ThumbnailMode::None),
                        file3dsThumbnailLabel(Setting::ThumbnailMode::None), "");

    for (Setting::ThumbnailMode mode : modes) {
        const char* label = file3dsThumbnailLabel(mode);
        if (file3dsThumbnailsAvailableByType(mode)) {
            AddMenuDialogOption(items, static_cast<int>(mode), label, "");
        } else {
            items.emplace_back(nullptr, MenuItemType::Disabled, label, "Not on SD card"_s, -1);
        }
    }

    return items;
}

std::vector<SMenuItem> makeOptionsForFileMenu(std::vector<FileMenuOption>& options, const char* selectedFileName) {
    std::vector<SMenuItem> items;
    options.clear();

    // set default Directory
    if (strcmp(settings3DS.defaultDir, file3dsGetCurrentDir()) != 0) {
        AddMenuDialogOption(items, options.size(), "Use current folder at startup", "");
        options.push_back(FileMenuOption::SetDefaultDir);
    }

    // reset default Directory
    if (settings3DS.defaultDir[0]) {
        char label[64];
        char dirStr[PATH_MAX];
        snprintf(dirStr, sizeof(dirStr), "%s", file3dsDisplayPath(settings3DS.defaultDir));
        
        size_t len = strlen(dirStr);
        if (len > 28) {
            snprintf(label, sizeof(label), "...%s", dirStr + (len - 28));
        } else {
            snprintf(label, sizeof(label), "%s", dirStr);
        }

        AddMenuDialogOption(items, options.size(), "Clear startup folder", label);
        options.push_back(FileMenuOption::ResetDefaultDir);
    }

    // rebuild cache
    char cachePath[PATH_MAX];
    file3dsGetCurrentDirCacheName(cachePath, sizeof(cachePath));
    
    if (IsFileExists(cachePath)) {
        const char* dateStr = file3dsGetCurrentDirCacheDate();

        AddMenuDialogOption(items, options.size(), "Refresh ROM list", dateStr ? dateStr : "");
        options.push_back(FileMenuOption::RescanDir);
    }

    // random game
    if (file3dsGetCurrentDirRomCount() > 1) {
        AddMenuDialogOption(items, options.size(), "Choose a random game in current folder", "");
        options.push_back(FileMenuOption::RandomGame);
    }

    // delete Game
    if (selectedFileName && selectedFileName[0] != '\0') {
        AddMenuDialogOption(items, options.size(), "Delete selected game", "");
        options.push_back(FileMenuOption::DeleteGame);
    }

    return items;
}

bool confirmDialog(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, std::vector<SMenuTab>& menuTabs, const std::string& title, const std::string& message, bool fade, bool hideAfter) {
    int result = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, message,
        Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakePickerOptions({ "Yes", "No" }), 1, fade);

    if (hideAfter) {
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, fade);
    }

    return result == 0;
}

static int saveLoadFirstSaveRow;

static void makeSaveLoadItems(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    items.clear();
    items.emplace_back(nullptr, MenuItemType::Action, menu3dsSubPageBackLabel(), "");
    AddMenuSpacer(items);

    AddMenuCheckbox(items, "  Overwrite slots without asking"_s, settings3DS.OverwriteSavestatesWithoutAsking,
        []( int val ) { CheckAndUpdateToggle( settings3DS.OverwriteSavestatesWithoutAsking, val ); });
    AddMenuNote(items, "  Quick save always overwrites the current slot without asking.");
    AddMenuSpacer(items);

    saveLoadFirstSaveRow = items.size();
    char slotInfo[32];

    for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot) {
        bool hasState = sav3dsSlotHasState(slot);
        bool isCurrent = (slot == settings3DS.CurrentSaveSlot);
        snprintf(slotInfo, sizeof(slotInfo), "  Save Slot #%d", slot);

        items.emplace_back(
            [slot, &menuTabs, &currentMenuTab](int) {
                SMenuTab dialogTab;
                bool isDialog = false;
                SaveWriteResult result;

                bool askFirst = sav3dsSlotHasState(slot) && !settings3DS.OverwriteSavestatesWithoutAsking;

                char title[32];
                snprintf(title, sizeof(title), "Save Slot #%d", slot);

                if (askFirst) {
                    bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, "This slot already has a save. Overwrite it?", true, false);

                    if (!confirmed) {
                        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);

                        return;
                    }
                }

                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, "Saving...", Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, std::vector<SMenuItem>(), -1, !askFirst);
                result = sav3dsSaveSlot(slot);

                if (result != SAVE_WRITE_OK) {
                    const char *failMessage = result == SAVE_WRITE_FAILED
                        ? "Saving failed. The incomplete save was deleted. Check the SD card."
                        : "Saving failed.";
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, failMessage, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk(), -1, false);
                    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
                }
                else
                {
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, "Saved.", Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, menu3dsMakeOptionsForOk(), -1, false);
                    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
                    CheckAndUpdate( settings3DS.CurrentSaveSlot, slot );
                }
            },
            MenuItemType::Action, slotInfo, isCurrent ? "Current" : ""
        );
        items.back().Dimmed = !hasState;
    }
    AddMenuSpacer(items);

    bool hasLoadRows = false;
    for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot) {
        if (!sav3dsSlotHasState(slot))
            continue;

        hasLoadRows = true;
        snprintf(slotInfo, sizeof(slotInfo), "  Load Slot #%d", slot);

        items.emplace_back([slot, &menuTabs, &currentMenuTab](int val) {
            bool result = sav3dsLoadSlot(slot);
            if (!result) {
                SMenuTab dialogTab;
                bool isDialog = false;

                char title[32];
                snprintf(title, sizeof(title), "Load Slot #%d", slot);

                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, "Unable to load this save.", Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk());
                menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
            } else {
                CheckAndUpdate( settings3DS.CurrentSaveSlot, slot );
                slotLoaded = true;
                session3DS.emulatorState = EMUSTATE_EMULATE;
            }
        }, MenuItemType::Action, slotInfo, ""_s);
    }
    if (!hasLoadRows)
        AddMenuNote(items, "  No saves to load yet.");
    AddMenuSpacer(items);

    AddMenuCheckbox(items, "  Automatically save and resume this game"_s, settings3DS.AutoSavestate,
        []( int val ) { CheckAndUpdateToggle( settings3DS.AutoSavestate, val ); });
    AddMenuNote(items, "  Saves when you quit or switch games. Uses a separate save.");
    AddMenuCheckbox(items, "  Show save slot previews"_s, settings3DS.SaveStateScreenshots,
        []( int val ) { CheckAndUpdateToggle( settings3DS.SaveStateScreenshots, val ); });
}

static void openSaveLoadPage(SMenuTab& tab, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    tab.subPage = { SUBPAGE_SAVE_LOAD, 0, {}, tab.SelectedItemIndex, tab.FirstItemIndex };
    tab.SubTitle.assign("Save and Load");
    tab.SubTitleRight.clear();
    tab.SelectedItemIndex = 0;
    tab.FirstItemIndex = 0;

    makeSaveLoadItems(tab.MenuItems, menuTabs, currentMenuTab);
}

void makeEmulatorMenu(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    items.clear();

    if (session3DS.isRomLoaded) {
        AddMenuHeader1(items, "CURRENT GAME"_s);
        items.emplace_back([](int val) {        
            session3DS.emulatorState = EMUSTATE_EMULATE;
        }, MenuItemType::Action, "  Resume"_s, ""_s);


        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Reset Console", "Restart the game? Unsaved progress is lost.", true, true);

            if (confirmed) {
                impl3dsResetConsole();
                session3DS.emulatorState = EMUSTATE_EMULATE;
            }
        }, MenuItemType::Action, "  Reset"_s, ""_s);

        items.emplace_back(nullptr, MenuItemType::Action, "  Save and Load", "\233",
                           MENU_ENTER_SUBPAGE - SUBPAGE_SAVE_LOAD);

        AddMenuSpacer(items);

        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "ROM Info", menu3dsGetRomInfo(), Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, menu3dsMakeOptionsForOk());
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
        }, MenuItemType::Action, "  ROM Info"_s, ""_s);

        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Screenshot", "Saving screenshot...", Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, std::vector<SMenuItem>(), -1, true, "", true);

            char path[PATH_MAX];

            if (sav3dsTakeMenuScreenshot(path, sizeof(path)))
            {
                char message[PATH_MAX];
                const size_t maxPathLen = sizeof(message) - strlen("Screenshot saved to .") - 1;
                snprintf(message, sizeof(message), "Screenshot saved to %.*s.", (int)maxPathLen, path);
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Screenshot", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, menu3dsMakeOptionsForOk(), -1, false);
            }
            else
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Screenshot", "Failed to save screenshot.", Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk(), -1, false);
                        
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);

        }, MenuItemType::Action, "  Take Screenshot"_s, ""_s);

        AddMenuSpacer(items);
    }

    AddMenuHeader1(items, "APPEARANCE"_s);

    const char* gameThumbnailMessage =
        "Missing covers fall back to another region if available\n"
        "(US > PAL > Japan). Place thumbnail .cache files in\n"
        "/3ds/gpSP_3ds/thumbnails on your SD card.";

    AddMenuPicker(items, "  Game Thumbnail"_s, gameThumbnailMessage, makeOptionsForGameThumbnail(), static_cast<int>(settings3DS.GameThumbnailType), DIALOG_TYPE_INFO, true,
        []( int val ) { 
            if (!CheckAndUpdate(settings3DS.GameThumbnailType, static_cast<Setting::ThumbnailMode>(val))) {
                return;
            }
            
            img3dsOpenThumbnailCache();
        });

    static const Setting::Theme themeOrder[] = {
        Setting::Theme::DarkMode,
        Setting::Theme::Original,
        Setting::Theme::RetroArch,
    };
    std::vector<SMenuItem> themeOptions;
    for (Setting::Theme theme : themeOrder)
        AddMenuDialogOption(themeOptions, static_cast<int>(theme), Themes[static_cast<int>(theme)].Name);

    AddMenuPicker(items, "  Theme"_s, ""_s, themeOptions, static_cast<int>(settings3DS.Theme), DIALOG_TYPE_INFO, true,
        []( int val ) {
            if (CheckAndUpdate(settings3DS.Theme, static_cast<Setting::Theme>(val)))
                menu3dsSetScreenDirty();
        });


    AddMenuPicker(items, "  Font"_s, ""_s, menu3dsMakePickerOptions({"Tempesta", "Ronda"}), static_cast<int>(settings3DS.Font), DIALOG_TYPE_INFO, true,
        []( int val ) { if ( CheckAndUpdate( settings3DS.Font, static_cast<Setting::Font>(val) ) ) { ui3dsSetFont(); menu3dsMarkAllTabsDirty(); } });

    AddMenuPicker(items, "  Game Screen"_s, "The screen the game is shown on. The menu uses the other one."_s, menu3dsMakePickerOptions({"Top", "Bottom"}), settings3DS.GameScreen, DIALOG_TYPE_INFO, true,
        [&menuTabs, &currentMenuTab]( int val ) { 
            if (!CheckAndUpdate(settings3DS.GameScreen, (gfxScreen_t)val)) {
                return;
            }

            SMenuTab dialogTab;
            bool isDialog = false;
            
            menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTabs);
            
            // screen swap requires framebuffer format update on both screens
            menu3dsSetScreenDirty(true, true);
            ui3dsSetScreenLayout();
            menu3dsMarkAllTabsDirty();

            log3dsWrite("screen swapped");
        });

    AddMenuSpacer(items);

    AddMenuHeader1(items, "OTHER"_s);

    AddMenuCheckbox(items, "  Enable Debug Log"_s, settings3DS.LogFileEnabled,
        []( int val ) {
            if (!CheckAndUpdateToggle(settings3DS.LogFileEnabled, val) || !settings3DS.LogFileEnabled)
                return;

            log3dsInitialize();
            if (session3DS.isRomLoaded)
                log3dsWrite("Logging enabled, ROM: %s", coreRomInfo.romFilename);
        });
    std::string logfileInfo = "  Keep off unless reporting a bug. Saved to /3ds/gpSP_3ds and replaced on every start. Restart to log startup.";
    AddMenuNote(items, logfileInfo);
    AddMenuSpacer(items);

    if (cfgFileAvailable[0] || cfgFileAvailable[1]) {
        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            const char *resetConfigDescription =
                !cfgFileAvailable[1] ? "Reset the global config to defaults." :
                !cfgFileAvailable[0] ? "Reset this game's config to defaults." :
                "Reset the global config, this game's config, or both to defaults.";

            SMenuTab dialogTab;
            bool isDialog = false;
            int option = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Reset Config"_s, resetConfigDescription, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, makeOptionsForResetConfig());
            
            // "Cancel" selected or B pressed
            if (option <= 0) {
                menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);

                return;
            }

            // 1=Global, 2=Game, 3=Both
            bool resetGlobal = (option == 1 || option == 3);
            bool resetGame   = (option == 2 || option == 3);

            if (resetGlobal) {
                settings3dsResetGlobalDefaults();
                cfgFileAvailable[0] = false;

                // config reset may change GameScreen - both screens need framebuffer format update
                menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTabs);
                menu3dsSetScreenDirty(true, true);
                ui3dsSetFont(); 
                ui3dsSetScreenLayout();
            }

            if (resetGame) {
                settings3dsResetGameDefaults();
                cfgFileAvailable[1] = false;
                menu3dsSetSnesScreenDirty();
            }

            settings3dsUpdate(resetGame);

            menu3dsMarkAllTabsDirty();
        }, MenuItemType::Action, "  Reset Config",  "\215");
    }

    items.emplace_back([&menuTabs, &currentMenuTab](int val) {
        SMenuTab dialogTab;
        bool isDialog = false;
        if (!confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Exit Emulator"_s, "Exit now? Your settings will be saved."_s, true, false)) {
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
            return;
        }

        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Exit Emulator"_s, "Exiting..."_s,
            Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, std::vector<SMenuItem>(), -1, false);

        session3DS.emulatorState = EMUSTATE_END;
    }, MenuItemType::Action, "  Exit Emulator", "");

    AddMenuSpacer(items);
    std::string info = std::string(settings3dsGetAppVersion("  gpSP for 3DS v")) + " \267 UI by matbo87 / bubble2k16";
    AddMenuNote(items, info);
}

const std::vector<SMenuItem>& makeOptionsForOnScreenDisplay() {
    static std::vector<SMenuItem> items;

    if (items.empty()) {
        items.reserve(4);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::None), "Off"_s,                       ""_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::Default), "Default"_s,                "Same image for every game"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::Adaptive), "Game or default"_s,       "Default if no game image"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::CustomOnly), "Game only"_s,           "Hidden if no game image"_s);
    }
    
    return items;
}

std::vector<SMenuItem> makeOptionsForStretch() {
    std::vector<SMenuItem> items;
    items.reserve(4);

    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::None), "No Stretch"_s,       "Pixel Perfect (240x160)"_s);
    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Fit_3_2), "3:2 Fit"_s,
        settings3DS.GameScreen == GFX_TOP ? "Original Ratio (360x240)"_s : "Original Ratio (320x213)"_s);
    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Fit_4_3), "4:3 Fit"_s,     "Full Height (320x240)"_s);

    if (settings3DS.GameScreen == GFX_TOP) {
        AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Full), "Fullscreen"_s, "Fills the screen (400x240)"_s);
    }

    return items;
}

const std::vector<SMenuItem>& makeOptionsForButtonMapping() {
    static std::vector<SMenuItem> items;

    if (items.empty()) {
        items.reserve(7);

        AddMenuDialogOption(items, 0,               "-"_s);
        AddMenuDialogOption(items, CORE_BTN_A,      "GBA A Button"_s);
        AddMenuDialogOption(items, CORE_BTN_B,      "GBA B Button"_s);
        AddMenuDialogOption(items, CORE_BTN_L,      "GBA L Button"_s);
        AddMenuDialogOption(items, CORE_BTN_R,      "GBA R Button"_s);
        AddMenuDialogOption(items, CORE_BTN_SELECT, "GBA SELECT Button"_s);
        AddMenuDialogOption(items, CORE_BTN_START,  "GBA START Button"_s);
    }

    return items;
}

// Options for one hotkey picker. Buttons used by another hotkey are left out,
// except the picker's own value, so duplicates from older configs still show.
std::vector<SMenuItem> makeOptionsFor3DSButtonMapping(int hotkeyIndex) {
    std::vector<SMenuItem> items;
    items.reserve(22);

    const auto& hotkeys = settings3DS.UseGlobalEmuControlKeys ? settings3DS.GlobalButtonHotkeys : settings3DS.ButtonHotkeys;
    u32 ownValue = hotkeys[hotkeyIndex].MappingBitmasks[0];
    auto addOption = [&](u32 key, const std::string& name) {
        if (key != ownValue) {
            for (int i = 0; i < HOTKEYS_COUNT; ++i)
                if (i != hotkeyIndex && hotkeys[i].MappingBitmasks[0] == key)
                    return;
        }
        AddMenuDialogOption(items, static_cast<int>(key), name);
    };

    AddMenuDialogOption(items, 0,   "-"_s);
    addOption(KEY_SELECT,           "3DS SELECT Button"_s);

    if (device3DS.isNew3DS) {
        addOption(KEY_ZL,           "ZL Button"_s);
        addOption(KEY_ZR,           "ZR Button"_s);
        addOption(KEY_CSTICK_UP,    "C-stick Up"_s);
        addOption(KEY_CSTICK_DOWN,  "C-stick Down"_s);
        addOption(KEY_CSTICK_LEFT,  "C-stick Left"_s);
        addOption(KEY_CSTICK_RIGHT, "C-stick Right"_s);
    }

    addOption(KEY_DUP,              "D-Pad Up"_s);
    addOption(KEY_DDOWN,            "D-Pad Down"_s);
    addOption(KEY_DLEFT,            "D-Pad Left"_s);
    addOption(KEY_DRIGHT,           "D-Pad Right"_s);

    addOption(KEY_CPAD_UP,          "Circle Pad Up"_s);
    addOption(KEY_CPAD_DOWN,        "Circle Pad Down"_s);
    addOption(KEY_CPAD_LEFT,        "Circle Pad Left"_s);
    addOption(KEY_CPAD_RIGHT,       "Circle Pad Right"_s);

    addOption(KEY_L,                "3DS L Button"_s);
    addOption(KEY_R,                "3DS R Button"_s);
    addOption(KEY_A,                "3DS A Button"_s);
    addOption(KEY_B,                "3DS B Button"_s);
    addOption(KEY_X,                "3DS X Button"_s);
    addOption(KEY_Y,                "3DS Y Button"_s);

    return items;
}

const std::vector<SMenuItem>& makeOptionsForGbaBiosMode() {
    static std::vector<SMenuItem> items;
    if (items.empty()) {
        items.reserve(2);
        AddMenuDialogOption(items, static_cast<int>(Setting::GbaBiosMode::Auto),    "Auto"_s,     "Use gba_bios.bin if valid, else built-in"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::GbaBiosMode::BuiltIn), "Built-in"_s, "Always use built-in open-source BIOS"_s);
    }
    return items;
}

const std::vector<SMenuItem>& makeOptionsForScreenFilter() {
    static std::vector<SMenuItem> items;
    if (items.empty()) {
        items.reserve(3);
        AddMenuDialogOption(items, static_cast<int>(Setting::ScreenFilter::Sharp), "Sharp"_s, "Crisp pixels"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::ScreenFilter::Smooth), "Smooth"_s, "Soft edges"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::ScreenFilter::Balanced), "Balanced"_s, "Crisp + soft blend"_s);
    }
    return items;
}

void makeOptionMenu(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    (void)menuTabs;
    (void)currentMenuTab;
    items.clear();

    AddMenuHeader1(items, "GENERAL SETTINGS"_s);
    AddMenuHeader2(items, "Video"_s);
    AddMenuPicker(items, "  Scaling"_s, "Sets how the picture fills the screen."_s, makeOptionsForStretch(), static_cast<int>(settings3DS.ScreenStretch), DIALOG_TYPE_INFO, true,
                  []( int val ) { 
                    bool wasNone = settings3DS.ScreenStretch == Setting::ScreenStretch::None;
                    if (CheckAndUpdate( settings3DS.ScreenStretch, static_cast<Setting::ScreenStretch>(val) )) { 
                        settings3dsApplyScreenStretch(); 
                        menu3dsSetScreenDirty(); 
                        if (wasNone != (settings3DS.ScreenStretch == Setting::ScreenStretch::None))
                            menu3dsMarkTabDirty(TAB_SETTINGS);
                    } 
                });
    
    AddMenuPicker(items, "  Scale Filter"_s, "Only affects scaled modes. Sharp can make some pixels look larger than others. Balanced may reduce performance slightly."_s,
        makeOptionsForScreenFilter(), static_cast<int>(settings3DS.ScreenFilter), DIALOG_TYPE_INFO, true,
        []( int val ) {
            if (CheckAndUpdate(settings3DS.ScreenFilter, static_cast<Setting::ScreenFilter>(val))) {
                menu3dsSetScreenDirty();
            }
        });
    // same condition as imageScaled in gpu3dsBindTexture
    items.back().Dimmed = settings3DS.ScreenStretch == Setting::ScreenStretch::None && !settings3DS.Overscan;

    AddMenuSpacer(items);

    AddMenuCheckbox(items, "  Crop & Overscan"_s, settings3DS.CropEnabled,
        []( int val ) {
            bool wasShown = settings3DS.CropEnabled;
            if (CheckAndUpdateToggle(settings3DS.CropEnabled, val)) {
                bool isShown = settings3DS.CropEnabled;
                if (isShown) {
                    settings3DS.CropTop = 4;
                    settings3DS.CropBottom = 4;
                    settings3DS.Overscan = true;
                } else {
                    settings3DS.CropTop = 0;
                    settings3DS.CropBottom = 0;
                    settings3DS.Overscan = false;
                }
                menu3dsSetScreenDirty();
                if (wasShown != isShown)
                    menu3dsMarkTabDirty(TAB_SETTINGS);
            }
        });

    if (settings3DS.CropEnabled) {
        AddMenuGauge(items, "  Crop Top Scanlines"_s, 0, 24, settings3DS.CropTop,
                        []( int val ) { if (CheckAndUpdate(settings3DS.CropTop, val)) menu3dsSetScreenDirty(); }, true);
        AddMenuGauge(items, "  Crop Bottom Scanlines"_s, 0, 24, settings3DS.CropBottom,
                        []( int val ) { if (CheckAndUpdate(settings3DS.CropBottom, val)) menu3dsSetScreenDirty(); }, true);
        AddMenuCheckbox(items, "  Overscan (zoom to fit height)"_s, settings3DS.Overscan,
            []( int val ) {
                if (CheckAndUpdateToggle(settings3DS.Overscan, val)) {
                    menu3dsSetScreenDirty();
                    if (settings3DS.ScreenStretch == Setting::ScreenStretch::None)
                        menu3dsMarkTabDirty(TAB_SETTINGS);
                }
            });
    }

    AddMenuSpacer(items);
    AddMenuHeader2(items, "Game Boy Advance"_s);
    AddMenuCheckbox(items, "  Color Correction"_s, settings3DS.GbaColorCorrection,
        []( int val ) {
            if (CheckAndUpdateToggle(settings3DS.GbaColorCorrection, val)) {
                if (session3DS.isRomLoaded) {
                    impl3dsApplyCoreSettings();
                    menu3dsSetScreenDirty();
                }
            }
        });
    AddMenuPicker(items, "  BIOS"_s, "Optional /3ds/gpSP_3ds/gba_bios.bin.\nTakes effect the next time a game is loaded."_s,
        makeOptionsForGbaBiosMode(), static_cast<int>(settings3DS.GbaBiosMode), DIALOG_TYPE_INFO, true,
        []( int val ) { CheckAndUpdate(settings3DS.GbaBiosMode, static_cast<Setting::GbaBiosMode>(val)); });
    AddMenuCheckbox(items, "  ARM Dynarec"_s, settings3DS.GbaDynarec,
        []( int val ) { CheckAndUpdateToggle(settings3DS.GbaDynarec, val); });
    AddMenuCheckbox(items, "  Interframe Blending"_s, settings3DS.InterframeBlending,
        []( int val ) {
            if (CheckAndUpdateToggle(settings3DS.InterframeBlending, val)) {
                if (session3DS.isRomLoaded) {
                    impl3dsApplyCoreSettings();
                    menu3dsSetScreenDirty();
                }
            }
        });

    AddMenuSpacer(items);
    AddMenuHeader2(items, "On-Screen Display"_s);

    AddMenuPicker(items, "  Game Screen Overlay"_s, "Shows an image over the game, such as a bezel.\nPlace PNGs in /3ds/gpSP_3ds/overlays."_s,
        makeOptionsForOnScreenDisplay(), static_cast<int>(settings3DS.GameOverlay), DIALOG_TYPE_INFO, true,
                  []( int val ) {
                    bool wasShown = settings3DS.GameOverlay != Setting::AssetMode::None;
                    if (CheckAndUpdate( settings3DS.GameOverlay, static_cast<Setting::AssetMode>(val) )) {
                        impl3dsUpdateUiAssets();
                        menu3dsSetScreenDirty();
                        
                        bool isShown = settings3DS.GameOverlay != Setting::AssetMode::None;
                        if (wasShown != isShown)
                            menu3dsMarkTabDirty(TAB_SETTINGS);
                    }
                }
            );

    if (settings3DS.GameOverlay != Setting::AssetMode::None) {
        AddMenuCheckbox(items, "  Auto-Fit Bezel (follows Video \273 Scaling)", settings3DS.GameOverlayAutoFit,
            []( int val ) { if (CheckAndUpdateToggle( settings3DS.GameOverlayAutoFit, val )) menu3dsSetScreenDirty(); });
    }

    AddMenuPicker(items, "  Game Screen BG"_s, "Shows a background behind the game.\nPlace PNGs in /3ds/gpSP_3ds/backgrounds/game_screen."_s,
        makeOptionsForOnScreenDisplay(), static_cast<int>(settings3DS.GameScreenBg), DIALOG_TYPE_INFO, true,
                    []( int val ) {
                        bool wasShown = settings3DS.GameScreenBg != Setting::AssetMode::None;
                        if (CheckAndUpdate(settings3DS.GameScreenBg, static_cast<Setting::AssetMode>(val))) {
                            impl3dsUpdateUiAssets();
                            menu3dsSetScreenDirty();
                            
                            bool isShown = settings3DS.GameScreenBg != Setting::AssetMode::None;
                            if (wasShown != isShown)
                                menu3dsMarkTabDirty(TAB_SETTINGS);
                        }
                    }
                );

    if (settings3DS.GameScreenBg != Setting::AssetMode::None) {
        AddMenuGauge(items, "  Game Screen BG Opacity"_s, 1, OPACITY_STEPS, settings3DS.GameScreenBgOpacity,
                        []( int val ) { if (CheckAndUpdate( settings3DS.GameScreenBgOpacity, val )) menu3dsSetScreenDirty(); });
    }

    AddMenuPicker(items, "  Second Screen BG"_s, "Shows a background on the second screen.\nPlace PNGs in /3ds/gpSP_3ds/backgrounds/second_screen."_s,
        makeOptionsForOnScreenDisplay(), static_cast<int>(settings3DS.SecondScreenBg), DIALOG_TYPE_INFO, true,
                    []( int val ) {
                        bool wasShown = settings3DS.SecondScreenBg != Setting::AssetMode::None;
                        if (CheckAndUpdate(settings3DS.SecondScreenBg, static_cast<Setting::AssetMode>(val))) {
                            
                            bool isShown = settings3DS.SecondScreenBg != Setting::AssetMode::None;
                            if (wasShown != isShown)
                                menu3dsMarkTabDirty(TAB_SETTINGS);
                        }
                    }
                );

    if (settings3DS.SecondScreenBg != Setting::AssetMode::None) {
        AddMenuGauge(items, "  Second Screen BG Opacity"_s, 1, OPACITY_STEPS, settings3DS.SecondScreenBgOpacity,
                        []( int val ) { CheckAndUpdate( settings3DS.SecondScreenBgOpacity, val ); });
    }

    AddMenuSpacer(items);

    AddMenuGauge(items, "  Scanlines"_s, 0, SCANLINE_INTENSITY_MAX, settings3DS.ScanlineIntensity,
        []( int val ) {
            if (CheckAndUpdate(settings3DS.ScanlineIntensity, val)) {
                img3dsUpdateScanlineTexture();
                menu3dsSetScreenDirty();
            }
        });

    AddMenuCheckbox(items, "  Show FPS", settings3DS.ShowFPS,
        []( int val ) { CheckAndUpdateToggle( settings3DS.ShowFPS, val ); });

    AddMenuSpacer(items);

    AddMenuHeader1(items, "GAME-SPECIFIC SETTINGS"_s);
    AddMenuHeader2(items, "Video"_s);
    AddMenuPicker(items, "  Frameskip"_s, "Skips drawing frames when the game runs slow. Higher values help maintain game speed but make motion less smooth."_s,
        menu3dsMakePickerOptions({"Off", "Up to 1 frame", "Up to 2 frames", "Up to 3 frames", "Up to 4 frames"}), settings3DS.MaxFrameSkips, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.MaxFrameSkips, val ); });

    AddMenuSpacer(items);

    AddMenuHeader2(items, "Audio"_s);
    AddMenuCheckbox(items, "  Apply volume to all games"_s, settings3DS.UseGlobalVolume,
                []( int val )
                {
                    CheckAndUpdateToggle( settings3DS.UseGlobalVolume, val );
                    if (settings3DS.UseGlobalVolume) {
                        settings3DS.GlobalVolume = settings3DS.Volume;
                    } else {
                        settings3DS.Volume = settings3DS.GlobalVolume;
                    }
                });
    AddMenuPicker(items, "  Volume Amplification", "100% is the original volume.\nHigher values boost sound but may cause distortion.", menu3dsMakePickerOptions({"100%", "125%", "150%", "175%", "200%"}),
                settings3DS.UseGlobalVolume ? settings3DS.GlobalVolume : settings3DS.Volume, DIALOG_TYPE_INFO, true,
                []( int val ) {
                    if (settings3DS.UseGlobalVolume)
                        CheckAndUpdate( settings3DS.GlobalVolume, val );
                    else
                        CheckAndUpdate( settings3DS.Volume, val );
                });

    AddMenuPicker(items, "  Audio Buffer"_s, "Increase this only if the sound crackles.\nHigher values add a slight sound delay."_s, menu3dsMakePickerOptions({"Low", "Normal", "High"}), settings3DS.AudioBuffer, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.AudioBuffer, val ); });
};

void makeControlsMenu(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    items.clear();

    const char *t3dsButtonNames[10];
    t3dsButtonNames[BTN3DS_A] = "3DS A Button";
    t3dsButtonNames[BTN3DS_B] = "3DS B Button";
    t3dsButtonNames[BTN3DS_X] = "3DS X Button";
    t3dsButtonNames[BTN3DS_Y] = "3DS Y Button";
    t3dsButtonNames[BTN3DS_L] = "3DS L Button";
    t3dsButtonNames[BTN3DS_R] = "3DS R Button";
    t3dsButtonNames[BTN3DS_ZL] = "3DS ZL Button";
    t3dsButtonNames[BTN3DS_ZR] = "3DS ZR Button";
    t3dsButtonNames[BTN3DS_SELECT] = "3DS SELECT Button";
    t3dsButtonNames[BTN3DS_START] = "3DS START Button";

    AddMenuHeader1(items, "HOTKEYS"_s);


    AddMenuCheckbox(items, "  Apply hotkey mappings to all games"_s, settings3DS.UseGlobalEmuControlKeys,
                []( int val )
                {
                    CheckAndUpdateToggle( settings3DS.UseGlobalEmuControlKeys, val );
                    if (settings3DS.UseGlobalEmuControlKeys) {
                        for (int i = 0; i < HOTKEYS_COUNT; ++i)
                            settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0] = settings3DS.ButtonHotkeys[i].MappingBitmasks[0];
                    }
                    else {
                        for (int i = 0; i < HOTKEYS_COUNT; ++i)
                            settings3DS.ButtonHotkeys[i].MappingBitmasks[0] = settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0];
                    }
                });

    for (int displayIdx = 0; displayIdx < HOTKEYS_COUNT; ++displayIdx) {
        int i = hotkeyDisplayOrder[displayIdx];
        AddMenuPicker( items,  hotkeysData[i][1], hotkeysData[i][2], makeOptionsFor3DSButtonMapping(i), 
            settings3DS.UseGlobalEmuControlKeys ? settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0] : settings3DS.ButtonHotkeys[i].MappingBitmasks[0], DIALOG_TYPE_INFO, true,
            [i]( int val ) {
                u32& mapping = settings3DS.UseGlobalEmuControlKeys ? settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0] : settings3DS.ButtonHotkeys[i].MappingBitmasks[0];
                // the other pickers' options depend on this value
                if (CheckAndUpdate( mapping, (u32)val ))
                    menu3dsMarkTabDirty(TAB_CONTROLS);
            }
        );
        SetMenuResetValue(items, 0);
        if (i == HOTKEY_ENABLE)
            AddMenuSpacer(items);
    }

    AddMenuSpacer(items);

    AddMenuHeader1(items, "BUTTON CONFIGURATION"_s);
    AddMenuCheckbox(items, "  Apply button mappings to all games"_s, settings3DS.UseGlobalButtonMappings,
                []( int val )
                {
                    CheckAndUpdateToggle( settings3DS.UseGlobalButtonMappings, val );

                    if (settings3DS.UseGlobalButtonMappings) {
                        for (int i = 0; i < 10; i++)
                            for (int j = 0; j < 4; j++)
                                settings3DS.GlobalButtonMapping[i][j] = settings3DS.ButtonMapping[i][j];
                        settings3DS.GlobalBindCirclePad = settings3DS.BindCirclePad;
                    }
                    else {
                        for (int i = 0; i < 10; i++)
                            for (int j = 0; j < 4; j++)
                                settings3DS.ButtonMapping[i][j] = settings3DS.GlobalButtonMapping[i][j];
                        settings3DS.BindCirclePad = settings3DS.GlobalBindCirclePad;
                    }

                });
    AddMenuCheckbox(items, "  Apply rapid-fire settings to all games"_s, settings3DS.UseGlobalTurbo,
                []( int val )
                {
                    CheckAndUpdateToggle( settings3DS.UseGlobalTurbo, val );
                    if (settings3DS.UseGlobalTurbo) {
                        for (int i = 0; i < 8; i++)
                            settings3DS.GlobalTurbo[i] = settings3DS.Turbo[i];
                    }
                    else {
                        for (int i = 0; i < 8; i++)
                            settings3DS.Turbo[i] = settings3DS.GlobalTurbo[i];
                    }
                });
    
    
    AddMenuSpacer(items);
    AddMenuHeader2(items, "Analog to Digital Type"_s);
    AddMenuCheckbox(items, "  Bind Circle Pad to D-Pad"_s, settings3DS.UseGlobalButtonMappings ? settings3DS.GlobalBindCirclePad : settings3DS.BindCirclePad,
                  []( int val ) {
                    CheckAndUpdateToggle(settings3DS.UseGlobalButtonMappings ? settings3DS.GlobalBindCirclePad : settings3DS.BindCirclePad, val);
                });
                
    for (size_t i = 0; i < 10; ++i) {
        // skip option for ZL and ZR button when device is O3DS/O2DS
        if ((i == BTN3DS_ZL || i == BTN3DS_ZR) && !device3DS.isNew3DS) {
            continue;
        }

        std::string optionButtonName = std::string(t3dsButtonNames[i]);
        AddMenuSpacer(items);
        AddMenuHeader2(items, optionButtonName);

        std::string mappingDescription =
            "Choose the GB button that the " + optionButtonName +
            " presses.\nAll assigned buttons are pressed together.";

        for (size_t j = 0; j < 3; ++j) {
            AddMenuPicker( items, "  Maps to"_s, mappingDescription, makeOptionsForButtonMapping(),
                settings3DS.UseGlobalButtonMappings ? settings3DS.GlobalButtonMapping[i][j] : settings3DS.ButtonMapping[i][j],
                DIALOG_TYPE_INFO, true,
                [i, j]( int val ) {
                    if (settings3DS.UseGlobalButtonMappings)
                        CheckAndUpdate( settings3DS.GlobalButtonMapping[i][j], val );
                    else
                        CheckAndUpdate( settings3DS.ButtonMapping[i][j], val );
                }
            );
            SetMenuResetValue(items, j == 0 ? static_cast<int>(settings3dsGetDefaultButtonMapping(i)) : 0);
        }

        if (i < 8) {
            AddMenuGauge(items, "  Rapid-Fire Speed"_s, 0, 8, 
                settings3DS.UseGlobalTurbo ? settings3DS.GlobalTurbo[i] : settings3DS.Turbo[i],
                [i]( int val )
                {
                    if (settings3DS.UseGlobalTurbo)
                        CheckAndUpdate( settings3DS.GlobalTurbo[i], val ); 
                    else
                        CheckAndUpdate( settings3DS.Turbo[i], val ); 
                });
            SetMenuResetValue(items, 0);
        }
        
    }
}

//-------------------------------------------------------
// Sets up all the cheats to be displayed in the menu.
//-------------------------------------------------------

void makeCheatMenu(SMenuTab& tab)
{
    std::vector<SMenuItem>& items = tab.MenuItems;

    items.clear();
    items.reserve(coreCheats.numCheats > 0 ? coreCheats.numCheats : 1);

    if (coreCheats.numCheats > 0) {
        char buffer[sizeof(CoreCheat::name) + 4];

        for (uint32 i = 0; i < static_cast<uint32>(MAX_CHEATS) && i < coreCheats.numCheats; i++) {
            snprintf(buffer, sizeof(buffer), "  %s", coreCheats.c[i].name);

            items.emplace_back(
                nullptr,
                MenuItemType::Checkbox,
                buffer,
                "",
                coreCheats.c[i].enabled ? 1 : 0
            );
        }
    }
    else {
        char romName[NAME_MAX + 1];
        file3dsGetGameAssetName(coreRomInfo.romFilename, romName, sizeof(romName));

        static char message[PATH_MAX];
        snprintf(message, sizeof(message),
            "No cheats found for this game.\n\n"
            "To add your own codes, place \"%s.cht\" in "
            "%s on your SD card (RetroArch .cht format).",
            romName,
            "/3ds/gpSP_3ds/cheats");

        AddMenuNote(items, message, MenuItemType::BodyText);
    }

    menu3dsUpdateCheatsCount(tab);
}

//----------------------------------------------------------------------
// Read/write all possible game specific settings.
//----------------------------------------------------------------------
static void settingsReadWriteGameList(BufferedFileWriter& stream, bool writeMode)
{
    char version[16];
    snprintf(version, sizeof(version), "%.1f", GAME_CONFIG_FILE_TARGET_VERSION);
    config3dsReadWriteString(stream, writeMode, "# v%s\n", "# v%15[^\n]\n", version);

    (void)config3dsGetVersionFromFile(true, version);

    config3dsReadWriteInt32(stream, writeMode, "# Do not modify this file or risk losing your settings.\n", NULL, 0, 0);

    config3dsReadWriteEnum(stream, writeMode, "CropEnabled=%d\n", &settings3DS.CropEnabled, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "CropTop=%d\n", &settings3DS.CropTop, 0, 24);
    config3dsReadWriteInt32(stream, writeMode, "CropBottom=%d\n", &settings3DS.CropBottom, 0, 24);
    config3dsReadWriteEnum(stream, writeMode, "Overscan=%d\n", &settings3DS.Overscan, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "AudioBuffer=%d\n", &settings3DS.AudioBuffer, 0, 2);

    config3dsReadWriteInt32(stream, writeMode, "Frameskips=%d\n", &settings3DS.MaxFrameSkips, 0, 4);
    config3dsReadWriteInt32(stream, writeMode, "Vol=%d\n", &settings3DS.Volume, 0, SND3DS_VOLUME_MAX);

    config3dsReadWriteEnum(stream, writeMode, "AutoSavestate=%d\n", &settings3DS.AutoSavestate, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "BindCirclePad=%d\n", &settings3DS.BindCirclePad, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "LastSaveSlot=%d\n", &settings3DS.CurrentSaveSlot, 1, 5);

    static const char *buttonName[10] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "SELECT","START"};
    char keyBuf[64];

    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 3; ++j) {
            snprintf(keyBuf, sizeof(keyBuf), "ButtonMap%s_%d=%%d\n", buttonName[i], j);
            config3dsReadWriteInt32(stream, writeMode, keyBuf, &settings3DS.ButtonMapping[i][j]);
        }
    }

    static const char *turboButtonName[8] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR"};
    for (int i = 0; i < 8; ++i) {
        snprintf(keyBuf, sizeof(keyBuf), "Turbo%s=%%d\n", turboButtonName[i]);
        config3dsReadWriteInt32(stream, writeMode, keyBuf, &settings3DS.Turbo[i], 0, 8);
    }

    for (int i = 0; i < HOTKEYS_COUNT; ++i) {
        if (!strlen(hotkeysData[i][0])) {
            continue;
        }

        snprintf(keyBuf, sizeof(keyBuf), "ButtonMapping%s_0=%%d\n", hotkeysData[i][0]);
        config3dsReadWriteBitmask(stream, writeMode, keyBuf, &settings3DS.ButtonHotkeys[i].MappingBitmasks[0]);
    }
}


//----------------------------------------------------------------------
// Read/write all possible global settings.
//----------------------------------------------------------------------
static void settingsReadWriteGlobalList(BufferedFileWriter& stream, bool writeMode)
{
    char version[16];
    snprintf(version, sizeof(version), "%.1f", GLOBAL_CONFIG_FILE_TARGET_VERSION);
    config3dsReadWriteString(stream, writeMode, "# v%s\n", "# v%15[^\n]\n", version);

    (void)config3dsGetVersionFromFile(false, version);

    config3dsReadWriteInt32(stream, writeMode, "# Do not modify this file or risk losing your settings.\n", NULL, 0, 0);
    config3dsReadWriteEnum(stream, writeMode, "GameScreen=%d\n", &settings3DS.GameScreen, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "Theme=%d\n", &settings3DS.Theme, 0, TOTALTHEMECOUNT - 1);
    config3dsReadWriteEnum(stream, writeMode, "GameThumbnailType=%d\n", &settings3DS.GameThumbnailType, 0,
        static_cast<int>(Setting::ThumbnailMode::Count) - 1);
    config3dsReadWriteEnum(stream, writeMode, "ScreenStretch=%d\n", &settings3DS.ScreenStretch, 0, 3);
    config3dsReadWriteEnum(stream, writeMode, "SaveStateScreenshots=%d\n", &settings3DS.SaveStateScreenshots, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "ScanlineIntensity=%d\n", &settings3DS.ScanlineIntensity, 0, SCANLINE_INTENSITY_MAX);

    config3dsReadWriteEnum(stream, writeMode, "GameOverlay=%d\n", &settings3DS.GameOverlay, 0, 3);
    config3dsReadWriteEnum(stream, writeMode, "GameOverlayAutoFit=%d\n", &settings3DS.GameOverlayAutoFit, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "SecondScreenBg=%d\n", &settings3DS.SecondScreenBg, 0, 3);
    config3dsReadWriteInt32(stream, writeMode, "SecondScreenBgOpacity=%d\n", &settings3DS.SecondScreenBgOpacity, 1, OPACITY_STEPS);
    config3dsReadWriteEnum(stream, writeMode, "GameScreenBg=%d\n", &settings3DS.GameScreenBg, 0, 3);
    config3dsReadWriteInt32(stream, writeMode, "GameScreenBgOpacity=%d\n", &settings3DS.GameScreenBgOpacity, 1, OPACITY_STEPS);

    config3dsReadWriteEnum(stream, writeMode, "Font=%d\n", &settings3DS.Font, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "LogFileEnabled=%d\n", &settings3DS.LogFileEnabled, 0, 1);

    char formatBuf[64];
    snprintf(formatBuf, sizeof(formatBuf), "DefaultDir=%%%zu[^\n]\n", sizeof(settings3DS.defaultDir) - 1);
    config3dsReadWriteString(stream, writeMode, "DefaultDir=%s\n", formatBuf, settings3DS.defaultDir);
    snprintf(formatBuf, sizeof(formatBuf), "LastSelectedDir=%%%zu[^\n]\n", sizeof(settings3DS.lastSelectedDir) - 1);
    config3dsReadWriteString(stream, writeMode, "LastSelectedDir=%s\n", formatBuf, settings3DS.lastSelectedDir);
    snprintf(formatBuf, sizeof(formatBuf), "LastSelectedFilename=%%%zu[^\n]\n", sizeof(settings3DS.lastSelectedFilename) - 1);
    config3dsReadWriteString(stream, writeMode, "LastSelectedFilename=%s\n", formatBuf, settings3DS.lastSelectedFilename);

    config3dsReadWriteEnum(stream, writeMode, "OverwriteSavestatesWithoutAsking=%d\n", &settings3DS.OverwriteSavestatesWithoutAsking, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "OverwriteAssetsWithoutAsking=%d\n", &settings3DS.OverwriteAssetsWithoutAsking, 0, 1);

    config3dsReadWriteEnum(stream, writeMode, "GbaBiosMode=%d\n", &settings3DS.GbaBiosMode, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "GbaColorCorrection=%d\n", &settings3DS.GbaColorCorrection, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "GbaDynarec=%d\n", &settings3DS.GbaDynarec, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "InterframeBlending=%d\n", &settings3DS.InterframeBlending, 0, 1);

    config3dsReadWriteInt32(stream, writeMode, "Vol=%d\n", &settings3DS.GlobalVolume, 0, SND3DS_VOLUME_MAX);
    config3dsReadWriteEnum(stream, writeMode, "BindCirclePad=%d\n", &settings3DS.GlobalBindCirclePad, 0, 1);

    static const char *buttonName[10] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "SELECT","START"};
    char keyBuf[64];

    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 3; ++j) {
            snprintf(keyBuf, sizeof(keyBuf), "ButtonMap%s_%d=%%d\n", buttonName[i], j);
            config3dsReadWriteInt32(stream, writeMode, keyBuf, &settings3DS.GlobalButtonMapping[i][j]);
        }
    }

    static const char *turboButtonName[8] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR"};
    for (int i = 0; i < 8; ++i) {
        snprintf(keyBuf, sizeof(keyBuf), "Turbo%s=%%d\n", turboButtonName[i]);
        config3dsReadWriteInt32(stream, writeMode, keyBuf, &settings3DS.GlobalTurbo[i], 0, 8);
    }

    for (int i = 0; i < HOTKEYS_COUNT; ++i) {
        if (!strlen(hotkeysData[i][0])) {
            continue;
        }

        snprintf(keyBuf, sizeof(keyBuf), "ButtonMapping%s_0=%%d\n", hotkeysData[i][0]);
        config3dsReadWriteBitmask(stream, writeMode, keyBuf, &settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0]);
    }

    config3dsReadWriteEnum(stream, writeMode, "ScreenFilter=%d\n", &settings3DS.ScreenFilter, 0, 2);

    config3dsReadWriteEnum(stream, writeMode, "UseGlobalButtonMappings=%d\n", &settings3DS.UseGlobalButtonMappings, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "UseGlobalTurbo=%d\n", &settings3DS.UseGlobalTurbo, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "UseGlobalVolume=%d\n", &settings3DS.UseGlobalVolume, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "UseGlobalEmuControlKeys=%d\n", &settings3DS.UseGlobalEmuControlKeys, 0, 1);

    config3dsReadWriteEnum(stream, writeMode, "ShowFPS=%d\n", &settings3DS.ShowFPS, 0, 1);
}

typedef void (*ConfigListFn)(BufferedFileWriter& stream, bool writeMode);

// An empty cache forces a write.
static std::string cfgFileText[2]; // global config, game config

// A full config is a few KB.
static u8 cfgSerializeBuffer[32 * 1024];

static void settingsGetGlobalConfigPath(char* path, size_t size)
{
    snprintf(path, size, "%s/%s", settings3DS.RootDir, "settings.cfg");
}

static void settingsGetGameConfigPath(char* path, size_t size)
{
    file3dsGetRelatedPath(coreRomInfo.romFilename, path, size, ".cfg", "configs");
}

static void settingsReadFileText(FILE* fp, std::string& out)
{
    out.clear();
    if (fseek(fp, 0, SEEK_SET) != 0)
        return;

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        out.append(buf, n);
}

static bool settingsLoadConfig(const char* path, ConfigListFn list, std::string& fileText)
{
    fileText.clear();
    if (path[0] == '\0')
        return false;

    BufferedFileWriter stream;
    if (!stream.open(path, "r"))
        return false;
    config3dsResetParseWarning();

    list(stream, false);

    // Cache the original text so format changes also trigger a rewrite.
    settingsReadFileText(stream.get(), fileText);

    return true;
}

static bool settingsLoadGameConfig()
{
    char path[PATH_MAX];
    settingsGetGameConfigPath(path, sizeof(path));
    return settingsLoadConfig(path, settingsReadWriteGameList, cfgFileText[1]);
}

static bool settingsLoadGlobalConfig()
{
    char path[PATH_MAX];
    settingsGetGlobalConfigPath(path, sizeof(path));
    return settingsLoadConfig(path, settingsReadWriteGlobalList, cfgFileText[0]);
}

// Skip writes when serialization matches the cached file text.
// Returns true if unchanged or successfully written.
static bool settingsSaveConfig(const char* path, ConfigListFn list, std::string& fileText, const char* name)
{
    TickCounter timer;
    osTickCounterStart(&timer);

    BufferedFileWriter memory;
    memory.openMemory(cfgSerializeBuffer, sizeof(cfgSerializeBuffer));
    list(memory, true);
    size_t size = memory.memorySize();
    bool serialized = memory.close() == 0;

    osTickCounterUpdate(&timer);
    double serializeMs = osTickCounterRead(&timer);

    if (!serialized) {
        log3dsWrite("%s settings exceed %d bytes; not saved", name, (int)sizeof(cfgSerializeBuffer));
        return false;
    }

    if (size == fileText.size() && memcmp(cfgSerializeBuffer, fileText.data(), size) == 0) {
        log3dsWrite("%s settings unchanged, not written (serialized in %.3fms)", name, serializeMs);
        return true;
    }

    fileText.clear();
    if (path[0] == '\0')
        return false;

    osTickCounterStart(&timer);

    BufferedFileWriter stream;
    if (!stream.open(path, "w"))
        return false;
    stream.write(cfgSerializeBuffer, size);
    if (stream.close() != 0)
        return false;

    fileText.assign((const char*)cfgSerializeBuffer, size);

    osTickCounterUpdate(&timer);
    log3dsWrite("%s settings saved in %.3fms (serialized in %.3fms)", name, osTickCounterRead(&timer), serializeMs);
    return true;
}

//----------------------------------------------------------------------
// Save settings.
//----------------------------------------------------------------------
bool settingsSave(bool includeGameSettings)
{
    char path[PATH_MAX];

    settingsGetGlobalConfigPath(path, sizeof(path));
    cfgFileAvailable[0] = settingsSaveConfig(path, settingsReadWriteGlobalList, cfgFileText[0], "Global");

    if (includeGameSettings) {
        settingsGetGameConfigPath(path, sizeof(path));
        cfgFileAvailable[1] = settingsSaveConfig(path, settingsReadWriteGameList, cfgFileText[1], "Game");
    }

    return true;
}

//-------------------------------------------------------
// Load the ROM and reset the CPU.
//-------------------------------------------------------
bool emulatorLoadRom()
{
    if (session3DS.isRomLoaded) {
        // save previous game state
        sav3dsSaveSRAMOnUnload();
        sav3dsSaveAuto();
        impl3dsSaveCheats();
    }
    
    // save global + previous game config
    settingsSave(session3DS.isRomLoaded);

    char romFileNameFullPath[PATH_MAX];
    snprintf(romFileNameFullPath, sizeof(romFileNameFullPath), "%s%s", file3dsGetCurrentDir(), romFileName);

    // when impl3dsLoadROM fails, our previous game (if any) is also unusable
    // therefore we always set romCrc32 to 0
    coreRomInfo.romCrc32 = 0;
    session3DS.isRomLoaded = impl3dsLoadROM(romFileNameFullPath) && coreRomInfo.romCrc32;

    if (!session3DS.isRomLoaded) {
        return false;
    }

    log3dsWrite("ROM loaded: %s [%s] CRC=%08lX %s (%s/%s, %luKB ROM, %luKB RAM)",
                coreRomInfo.romName, coreRomInfo.gameCode, (unsigned long)coreRomInfo.romCrc32,
                coreRomInfo.mapperName,
                coreRomInfo.usingOfficialBios ? "BIOS" : "HLE",
                coreRomInfo.dynarecActive ? "JIT" : "INT",
                (unsigned long)(coreRomInfo.romSizeBytes / 1024),
                (unsigned long)(coreRomInfo.ramSizeBytes / 1024));

    // update global config
    snprintf(settings3DS.lastSelectedDir, sizeof(settings3DS.lastSelectedDir), "%s", file3dsGetCurrentDir());
    snprintf(settings3DS.lastSelectedFilename, sizeof(settings3DS.lastSelectedFilename), "%s", romFileName);

    settings3dsResetGameDefaults();
    menu3dsMarkTabDirty(TAB_SETTINGS);

    // if file exists, overwrite the defaults
    // if not, stay on defaults
    cfgFileAvailable[1] = settingsLoadGameConfig();

    settings3dsUpdate(true);
    impl3dsApplyCoreSettings();

    // cache which save slots already have a savestate for the loaded game
    for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot)
        sav3dsUpdateSlotState(slot);

    float targetFps = (float)TICKS_PER_SEC / settings3DS.TicksPerFrame;
        notif3dsFpsUpdate(targetFps);

    return true;
}

//----------------------------------------------------------------------
// Find the ID of the last selected item in the file list.
//----------------------------------------------------------------------
int findLastSelected(std::vector<DirectoryEntry>& entries, const char* name)
{
    if (name == NULL || name[0] == '\0') {
		return -1;
	}

    for (size_t i = 0; i < entries.size(); i++)
    {
       if (strncmp(entries[i].Filename, name, sizeof(entries[i].Filename) - 1) == 0)
            return static_cast<int>(i);
    }

    return -1;
}

//----------------------------------------------------------------------
// Handle menu cheats.
//----------------------------------------------------------------------

bool syncCheatsFromMenu(std::vector<SMenuItem>& cheatMenu, bool applyCheats)
{
    if (cheatMenu.empty() || coreCheats.numCheats == 0) return false;

    bool enabledFlagsChanged = false;
    int cheatIndex = 0;

    for (const auto& item : cheatMenu) {
        if (cheatIndex >= MAX_CHEATS || static_cast<uint32>(cheatIndex) >= coreCheats.numCheats) {
            break;
        }

        if (item.Type != MenuItemType::Checkbox) {
            continue;
        }

        bool enabledInMenu = (item.Value == 1);

        if (coreCheats.c[cheatIndex].enabled != enabledInMenu) {
            coreCheats.c[cheatIndex].enabled = enabledInMenu;
            enabledFlagsChanged = true;
        }

        cheatIndex++;
    }

    if (applyCheats) {
        impl3dsApplyCheats();
    }

    return enabledFlagsChanged;
}

// returns the index of the item matching 'selectedItemName', or 0 if not found/empty
int fillFileMenuEntries(std::vector<SMenuItem>& fileMenu, const char *selectedItemName) {
    fileMenu.clear();
    fileMenu.reserve(entries.size());

    int selectedItemIndex = 0;
    char childDirectoryPrefix[8];
    snprintf(childDirectoryPrefix, sizeof(childDirectoryPrefix), "  %c ", UI_ICON_FOLDER);

    for (size_t i = 0; i < entries.size(); ++i) {
        // get the permanent address of the item in the global vector
        const DirectoryEntry* entry = &entries[i];

        const char* prefix = MENU_PREFIX_FILE;

        if (entry->Type == FileEntryType::ChildDirectory)
            prefix = childDirectoryPrefix;
        else if (entry->Type == FileEntryType::ParentDirectory)
            prefix = MENU_PREFIX_PARENT_DIRECTORY;

        char label[NAME_MAX + 1];
        const size_t prefixLen = strlen(prefix);
        const size_t maxFilenameLen = (prefixLen < sizeof(label) - 1) ? (sizeof(label) - 1 - prefixLen) : 0;
        snprintf(label, sizeof(label), "%s%.*s", prefix, (int)maxFilenameLen, entry->Filename);

        if (selectedItemName && selectedItemName[0] != '\0') {
            // a msu1 folder is remembered as "<folder>/<rom>"
            size_t filenameLength = strlen(entry->Filename);
            if (strncmp(entry->Filename, selectedItemName, NAME_MAX) == 0
                || (entry->Type == FileEntryType::ChildDirectory
                    && strncmp(entry->Filename, selectedItemName, filenameLength) == 0
                    && selectedItemName[filenameLength] == '/')) {
                selectedItemIndex = i;
            }
        }

        fileMenu.emplace_back(
            // Do NOT use [&entry] here (previous implementation, dangling reference)
            // capture the pointer by value [entry]
            [entry](int val) { 
                selectedEntry = entry; 
            }, 
            MenuItemType::Action, 
            label, 
            "", 
            99999
        );
    }

    return selectedItemIndex;
}


// firstItemIndex < 0 keeps the current scroll offset (rescan/delete/initial build);
// >= 0 forces it (0 = top when entering a child, or a restored offset going back up).
void updateFileMenuTab(const char *selectedItemName, bool showCachingIndicator, int firstItemIndex = -1, bool reloadFiles = true) {
    SMenuTab& fileMenuTab = menuTabs.back();

    fileMenuTab.SubTitle.assign(file3dsDisplayPath(file3dsGetCurrentDir()));

    if (reloadFiles)
        file3dsGetFiles(entries, menuTabs, showCachingIndicator);
    fileMenuTab.SubTitleRight = file3dsIsCurrentDirLoadedFromCache()
        ? std::string(1, UI_ICON_CHECKMARK) + " Cached"
        : std::string();
    fileMenuTab.SelectedItemIndex = fillFileMenuEntries(fileMenuTab.MenuItems, selectedItemName);
    if (firstItemIndex >= 0) {
        fileMenuTab.FirstItemIndex = firstItemIndex;
    }
    fileMenuTab.MakeSureSelectionIsOnScreen(menu3dsGetListVisibleItems(fileMenuTab), 2);
}

static void restoreMenuSelection(SMenuTab& tab, bool preserveSelection) {
    int selected = tab.SelectedItemIndex;
    bool validSelection = preserveSelection
        && selected >= 0 && selected < static_cast<int>(tab.MenuItems.size())
        && tab.MenuItems[selected].IsHighlightable();

    if (!validSelection) {
        // A tab without selectable rows must not keep the old list's indices.
        tab.SelectedItemIndex = 0;
        tab.FirstItemIndex = 0;
        for (size_t j = 0; j < tab.MenuItems.size(); j++) {
            if (tab.MenuItems[j].IsHighlightable()) {
                tab.SelectedItemIndex = static_cast<int>(j);
                break;
            }
        }
    }

    tab.MakeSureSelectionIsOnScreen(menu3dsGetListVisibleItems(tab), 2);
}

void setupMenu(int& currentMenuTab) {
    int requiredTabs = session3DS.isRomLoaded ? 5 : 2;
    int fileMenuTabIndex = session3DS.isRomLoaded ? 4 : 1;
    bool isFirstRun = menuTabs.empty();
    bool requiredTabsChanged = menuTabs.size() != static_cast<size_t>(requiredTabs);
    bool romChanged = !isFirstRun && coreRomInfo.romCrc32 != lastLoadedRomCRC;
    bool preserveSelection = !requiredTabsChanged && !romChanged;

    // only reallocate if the size grows, otherwise reuse the buffer
    if (requiredTabsChanged) {
        menuTabs.resize(requiredTabs);
    }

    const char* tabsStart[] = { "Emulator", "Games" };
    const char* tabsGame[] = { "Emulator", "Settings", "Controls", "Cheats", "Games" };
    const char** tabs = session3DS.isRomLoaded ? tabsGame : tabsStart;

    for (int i = 0; i < requiredTabs; i++) {
        if (i != fileMenuTabIndex) {
            // skip clean tabs
            if (!(requiredTabsChanged || romChanged) && !session3DS.menuTabDirty[i])
                continue;

            if (!requiredTabsChanged && !romChanged && menuTabs[i].IsSubPage()) {
                int page = menuTabs[i].subPage.id;
                if (page == SUBPAGE_SAVE_LOAD) {
                    makeSaveLoadItems(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    restoreMenuSelection(menuTabs[i], true);
                    continue;
                }

                // The page's backing state changed, so return this tab to its root page.
                menuTabs[i].SelectedItemIndex = menuTabs[i].subPage.parentSelectedIndex;
                menuTabs[i].FirstItemIndex = menuTabs[i].subPage.parentFirstItemIndex;
            }

            menuTabs[i].subPage = {};

            menuTabs[i].SetTitle(tabs[i]);
            menuTabs[i].SubTitle.clear();
            menuTabs[i].SubTitleRight.clear();

            switch (i) {
                case TAB_EMULATOR:
                    makeEmulatorMenu(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    break;
                case TAB_SETTINGS:
                    makeOptionMenu(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    break;
                case TAB_CONTROLS:
                    makeControlsMenu(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    break;
                case TAB_CHEATS:
                    makeCheatMenu(menuTabs[i]);
                    break;
            }

            restoreMenuSelection(menuTabs[i], preserveSelection);
        } else {
            // file tab is expensive and content is layout-/navigation-driven, not ROM-driven
            if (!requiredTabsChanged)
                continue;

            menuTabs[i].SetTitle(tabs[i]);
            menuTabs[i].SubTitle.clear();
            menuTabs[i].SubTitleRight.clear();
            updateFileMenuTab(settings3DS.lastSelectedFilename, false, -1, isFirstRun);
        }
    }

    for (int i = 0; i < TAB_DIRTY_COUNT; i++)
        session3DS.menuTabDirty[i] = false;
}

FileMenuOption showFileMenuOptions(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab) {
    int selectedItemIndex = menuTabs[currentMenuTab].SelectedItemIndex;
    DirectoryEntry *entry = &entries[selectedItemIndex];

    char selectedFileName[NAME_MAX + 1];
    
    if (entry->Type == FileEntryType::File) {
        snprintf(selectedFileName, sizeof(selectedFileName), "%s", entry->Filename);
    } else {
        selectedFileName[0] = '\0';
    }

    std::vector<FileMenuOption> options;
    std::vector<SMenuItem> menuItems = makeOptionsForFileMenu(options, selectedFileName);

    std::string description = "Uses the folder you're currently browsing.\nIf cleared, the last game's folder opens.";
    if (std::find(options.begin(), options.end(), FileMenuOption::RescanDir) != options.end())
        description += "\nRefresh the ROM list after adding or removing ROMs.";
    
    int optionIndex = menu3dsShowDialog(
        dialogTab, isDialog, currentMenuTab, menuTabs, 
        "Options",
        description,
        Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, 
        menuItems
    );

    if (optionIndex < 0 || optionIndex >= static_cast<int>(options.size())) {
        if (isDialog) {
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
        }
        return FileMenuOption::None;
    }

    FileMenuOption option = options[optionIndex];

    switch (option) {
        case FileMenuOption::SetDefaultDir:
            file3dsSetDefaultDir(false);
            break;

        case FileMenuOption::ResetDefaultDir:
            file3dsSetDefaultDir(true);
            break;

        case FileMenuOption::RandomGame:
        {
            // implies directories first, then ROMs
            int minIndex = entries.size() - file3dsGetCurrentDirRomCount();
            int maxIndex = entries.size() - 1;

            // we could exclude lastSelectedFilename here, but let's keep it simple for now
            menu3dsSelectRandomGameIndex(menuTabs[currentMenuTab], minIndex, maxIndex, -1);

            break;
        }
            
        case FileMenuOption::RescanDir:
        {
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Refresh ROM List", "Scanning folder...",
                Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, std::vector<SMenuItem>(), -1, false);

            file3dsDeleteCurrentDirCache();
            updateFileMenuTab(NULL, false);

            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Refresh ROM List", "ROM list refreshed.", Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, menu3dsMakeOptionsForOk(), -1, false);
            
            break;
        }

        case FileMenuOption::DeleteGame:
        {
            char message[NAME_MAX + 64];
            snprintf(message, sizeof(message), "Delete \"%s\" from the SD card? This cannot be undone.", selectedFileName);
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
            bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Delete Game", message, true, false);

            if (confirmed) {
                // if current selected game is also last selected game, reset lastSelectedFilename
                if (strcmp(selectedFileName, settings3DS.lastSelectedFilename) == 0) {
                    settings3DS.lastSelectedFilename[0] = '\0';
                }

                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s%s", file3dsGetCurrentDir(), selectedFileName);
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Delete Game", "Deleting...",
                    Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, std::vector<SMenuItem>(), -1, false);
                
                if (remove(path) == 0) {
                    file3dsDeleteCurrentDirCache();

                    char nextSelectedFilename[NAME_MAX + 1] = ""; 
                    bool listIsEmpty = (entries.size() <= 1);
                    bool isLastItem = listIsEmpty || (selectedItemIndex >= static_cast<int>(entries.size()) - 1);

                    if (!listIsEmpty) {
                        // grab the next filename before we clear the vector
                        int nextIndex = isLastItem ? selectedItemIndex - 1 : selectedItemIndex + 1;
                        if (nextIndex >= 0 && nextIndex < static_cast<int>(entries.size())) {
                            snprintf(nextSelectedFilename, sizeof(nextSelectedFilename), "%s", entries[nextIndex].Filename);
                        }
                    }

                    updateFileMenuTab(listIsEmpty ? NULL : nextSelectedFilename, false);

                    snprintf(message, sizeof(message), "\"%s\" deleted.", selectedFileName);
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Delete Game", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, menu3dsMakeOptionsForOk(), -1, false);                
                } else {
                    snprintf(message, sizeof(message), "Could not delete \"%s\".", selectedFileName);
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Delete Game", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk(), -1, false);
                }
            }
            break;
        }
        default: 
            // none selected
            break;
    }

    if (isDialog) {
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, option != FileMenuOption::RandomGame);
    }

    return option;
}

static void showInfoDialog(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, const std::string& title, const std::string& body) {
    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, body,
        Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, menu3dsMakeOptionsForOk());

    if (isDialog)
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
}

// Cheat rows have the same indices as coreCheats.c.
static int cheatIndexForRow(const std::vector<SMenuItem>& items, int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(items.size()) || items[rowIndex].Type != MenuItemType::Checkbox)
        return -1;
    return rowIndex;
}

void showCheatInfo(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, int cheatIndex) {
    if (cheatIndex < 0 || static_cast<uint32>(cheatIndex) >= coreCheats.numCheats)
        return;

    const CoreCheat& cheat = coreCheats.c[cheatIndex];

    // Spaces let multi-part codes wrap.
    std::string codes = cheat.code;
    bool multiPart = codes.find('+') != std::string::npos || codes.find(';') != std::string::npos;
    std::replace(codes.begin(), codes.end(), '+', ' ');

    std::string body = std::string(cheat.name) + "\n\n"
        + (multiPart ? "CodeBreaker Codes:\n" : "CodeBreaker Code:\n") + codes;

    showInfoDialog(dialogTab, isDialog, currentMenuTab, "Cheat Details", body);
}

static void showCheatsHelp(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab) {
    const char *info =
        "gpSP supports unencrypted CodeBreaker GBA codes (up to 20 active cheats at once). "
        "Disabling cheats may not undo their effects in memory. "
        "If the game glitches, disable cheats and reset.\n\n"
        "Place RetroArch .cht files in /3ds/gpSP_3ds/cheats/<game>.cht.";

    showInfoDialog(dialogTab, isDialog, currentMenuTab, "Cheats Help", info);
}

static void deleteCheat(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, int cheatIndex) {
    if (cheatIndex < 0 || static_cast<uint32>(cheatIndex) >= coreCheats.numCheats)
        return;

    char name[sizeof(CoreCheat::name) + 4];
    ui3dsEllipsize(coreCheats.c[cheatIndex].name, name, sizeof(name), menu3dsGetDialogTextWidth());

    char message[sizeof(name) + 64];
    snprintf(message, sizeof(message), "Delete \"%s\"?", name);
    if (!confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Delete Cheat", message, true, false))
        return;

    // Sync pending toggles before rebuilding the menu.
    SMenuTab& tab = menuTabs[TAB_CHEATS];
    syncCheatsFromMenu(tab.MenuItems, false);
    impl3dsDeleteCheat(cheatIndex);
    impl3dsApplyCheats();
    session3DS.cheatsDirty = true; // Defer SD writes until ROM switch or exit.

    if (tab.SelectedItemIndex == static_cast<int>(tab.MenuItems.size()) - 1 && tab.SelectedItemIndex > 0)
        tab.SelectedItemIndex--;
    menu3dsMarkTabDirty(TAB_CHEATS);
    setupMenu(currentMenuTab);

    int maxFirstItemIndex = static_cast<int>(tab.MenuItems.size()) - menu3dsGetListVisibleItems(tab);
    if (tab.FirstItemIndex > maxFirstItemIndex)
        tab.FirstItemIndex = maxFirstItemIndex > 0 ? maxFirstItemIndex : 0;
}

void showCheatsOptions(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab) {
    enum { OPTION_DISABLE_ALL, OPTION_DELETE, OPTION_HELP };

    std::vector<SMenuItem>& cheatMenu = menuTabs[TAB_CHEATS].MenuItems;
    int cheatIndex = cheatIndexForRow(cheatMenu, menuTabs[TAB_CHEATS].SelectedItemIndex);

    std::vector<SMenuItem> options;
    AddMenuDialogOption(options, OPTION_HELP, "Help", "");
    if (menu3dsGetCheatsActive() > 0)
        AddMenuDialogOption(options, OPTION_DISABLE_ALL, "Disable all cheats", "");
    else
        AddMenuDisabledOption(options, "Disable all cheats", OPTION_DISABLE_ALL);
    AddMenuDialogOption(options, OPTION_DELETE, "Delete selected cheat", "");

    const char *text = "Press [SELECT] on a cheat to see its full name and code.";
    int option = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Options", text,
        Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, options);

    if (option == OPTION_DISABLE_ALL) {
        for (auto& item : cheatMenu)
            if (item.Type == MenuItemType::Checkbox)
                item.Value = 0;
        menu3dsUpdateCheatsCount(menuTabs[TAB_CHEATS]);
    } else if (option == OPTION_DELETE) {
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
        deleteCheat(dialogTab, isDialog, currentMenuTab, cheatIndex);
    } else if (option == OPTION_HELP) {
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
        showCheatsHelp(dialogTab, isDialog, currentMenuTab);
    }

    if (isDialog)
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
}

void onDirectoryEntrySelected(
    SMenuTab& dialogTab, 
    bool& isDialog, 
    int currentMenuTab, 
    bool& runNextGame,
    std::vector<SMenuItem>& cheatMenu,
    const DirectoryEntry*& entry,
    bool openFolder
) {
    if (openFolder && entry->Type == FileEntryType::File)
        return;

    bool loadRom = entry->Type == FileEntryType::File;

    if (loadRom)
    {
        snprintf(romFileName, sizeof(romFileName), "%s", entry->Filename);
    }
    else if (entry->Type == FileEntryType::ChildDirectory)
    {
        fileMenuScrollStack.push_back(menuTabs.back().FirstItemIndex);
        file3dsGoToChildDirectory(entry->Filename);
        updateFileMenuTab("", true, 0);
        return;
    }

    if (loadRom)
    {
        char basename[NAME_MAX + 1];
        utils3dsGetBasename(romFileName, basename, sizeof(basename), false);
        menu3dsRunRomLoadingDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Load Game", basename, Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, romFileName);
        
        // Apply pending cheat changes before saving the previous game's SRAM and auto savestate.
        if (syncCheatsFromMenu(cheatMenu, true)) {
            session3DS.cheatsDirty = true;
        }
        
        runNextGame = emulatorLoadRom();
        
        if (!runNextGame) {
            menu3dsHideRomLoadingDialog(dialogTab, isDialog, currentMenuTab, menuTabs, romFileName);
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "Load Game", "Unable to load this game.", Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk());
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
        } else {
            sav3dsLoadAuto();
            session3DS.emulatorState = EMUSTATE_EMULATE;
        }
    } 
    else if (entry->Type == FileEntryType::ParentDirectory) 
    {
        char lastDirectoryName[128] = "";
        file3dsGetCurrentDirName(lastDirectoryName, sizeof(lastDirectoryName));
        int restoreFirstItemIndex = fileMenuScrollStack.empty() ? 0 : fileMenuScrollStack.back();
        if (!fileMenuScrollStack.empty()) {
            fileMenuScrollStack.pop_back();
        }

        file3dsGoUpOrDownDirectory(*entry);
        updateFileMenuTab(lastDirectoryName, true, restoreFirstItemIndex);
    }
}

void showMenu() {
    static std::vector<SMenuItem> emptyCheats;
    // Persistent menu callbacks capture this by reference.
    static int currentMenuTab;
    currentMenuTab = menu3dsGetLastSelectedTabIndex();

    // Rebuild the Emulator tab or its open sub-page on menu entry.
    menu3dsMarkTabDirty(TAB_EMULATOR);

    if (session3DS.isRomLoaded && menuTabs.size() > TAB_SETTINGS && !menuTabs[TAB_SETTINGS].IsSubPage())
        menu3dsMarkTabDirty(TAB_SETTINGS);

    setupMenu(currentMenuTab);
    lastLoadedRomCRC = coreRomInfo.romCrc32;

    if (session3DS.saveSlotHotkeyUsed && menuTabs[TAB_EMULATOR].subPage.id == SUBPAGE_SAVE_LOAD) {
        menuTabs[TAB_EMULATOR].SelectedItemIndex = saveLoadFirstSaveRow + settings3DS.CurrentSaveSlot - 1;
        restoreMenuSelection(menuTabs[TAB_EMULATOR], true);
    }
    session3DS.saveSlotHotkeyUsed = false;

    std::vector<SMenuItem>& cheatMenu = session3DS.isRomLoaded ? menuTabs[3].MenuItems : emptyCheats;

    bool isDialog = false;
    bool runNextGame = false;
    SMenuTab dialogTab;

    // Avoid flashing the untouched pre-pause buffer on resume.
    GPU3DS.gameScreenBufferDesync = true;

    // Show the last emulated frame, so the paused image matches the emulated state.
    if (impl3dsHasPendingDraw())
        menu3dsSetSnesScreenDirty();

    static bool audioErrorShown = false;
    if (snd3DS.audioType == 0 && !audioErrorShown) {
        audioErrorShown = true;
        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "DSP firmware missing",
            "Games will still run, but without sound. To enable in-game audio, dump the DSP firmware to /3ds/dspfirm.cdc.",
            Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk());
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
    }

    while (aptMainLoop() && session3DS.emulatorState == EMUSTATE_PAUSEMENU) {
        int result = menu3dsMenuSelectItem(dialogTab, isDialog, currentMenuTab, menuTabs);

        if (menu3dsHasDirtyTabs())
        {
            setupMenu(currentMenuTab);
        }

        // selectedEntry is set for option FileMenuOption::RandomGame.
        if (result == MENU_ENTRY_CONTEXT_MENU)
        {
            if (currentMenuTab == TAB_CHEATS)
                showCheatsOptions(dialogTab, isDialog, currentMenuTab);
            else
                showFileMenuOptions(dialogTab, isDialog, currentMenuTab);
        }

        if (currentMenuTab == TAB_CHEATS && result == MENU_ENTRY_ITEM_INFO)
        {
            showCheatInfo(dialogTab, isDialog, currentMenuTab,
                cheatIndexForRow(menuTabs[currentMenuTab].MenuItems, menuTabs[currentMenuTab].SelectedItemIndex));
        }

        if (result <= MENU_ENTER_SUBPAGE)
        {
            switch (MENU_ENTER_SUBPAGE - result)
            {
            case SUBPAGE_SAVE_LOAD:
                openSaveLoadPage(menuTabs[currentMenuTab], menuTabs, currentMenuTab);
                break;
            }
        }

        // user pressed START button in pause menu -> continue game
        if (result == MENU_CONTINUE_GAME) 
        {
            session3DS.emulatorState = EMUSTATE_EMULATE;
        }
        else if (selectedEntry) 
        {
            onDirectoryEntrySelected(dialogTab, isDialog, currentMenuTab, runNextGame, cheatMenu, selectedEntry,
                                     result == MENU_ENTRY_OPEN_FOLDER);
            selectedEntry = nullptr;
        }
    }
    
    // load/resume game
    if (session3DS.emulatorState == EMUSTATE_EMULATE) {
        input3dsWaitForRelease();

        if (!runNextGame) {
            if (syncCheatsFromMenu(cheatMenu, true)) {
                session3DS.cheatsDirty = true;
            }
        
            settings3dsUpdate(true);
        }

        // point to first tab when running a new game
        menu3dsSetLastSelectedTabIndex(runNextGame ? 0 : currentMenuTab);

        impl3dsUpdateUiAssets();

        if (slotLoaded) {
            notif3dsTrigger(Notif::LoadState, Notif::Type::Success);
            slotLoaded = false;
        }
    }

    menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTabs);
    menu3dsSetScreenDirty();
}

//--------------------------------------------------------
// Initialize the emulator engine and everything else.
// This calls the impl3dsInitialize, which executes
// initialization code specific to the emulation core.
//--------------------------------------------------------
bool emulatorInitialize()
{
    log3dsInitialize();
    log3dsWrite("==== START Logging (%s, %s) ====", settings3dsGetAppVersion("v"), log3dsGetCurrentDate());
    log3dsWrite("Real 3DS: %s, New 3DS: %s, Model: %d, Region: %d",
        device3DS.isReal3DS ? "v" : "x", device3DS.isNew3DS ? "v" : "x", device3DS.model, device3DS.region);
    utils3dsInitSecondaryCore();

    GSPGPU_FramebufferFormat gpuBufFmt = (GSPGPU_FramebufferFormat)DISPLAY_TRANSFER_FMT;
    gfxInit(gpuBufFmt, gpuBufFmt, false);
    // draw our start up message as early as possible
    ui3dsPrepare();
    menu3dsShowSplashMessage("Loading");

    Result rc = romfsInit();
    session3DS.isRomFsLoaded = !R_FAILED(rc);

    if (session3DS.isRomFsLoaded) {
        file3dsSetRomNameMappings("romfs:/mappings.txt");
    }

    if (!file3dsInitialize()) return false;
    if (!gpu3dsInitialize()) return false;
    if (!ui3dsInitialize()) return false;
    if (!notif3dsInitialize()) return false;
    if (!impl3dsInitialize()) return false;
    if (!sav3dsInitialize()) return false;
    if (!img3dsInitialize()) return false;
    if (!snd3dsInitialize()) return false;

    enableAptHooks();

    #ifndef PROFILING_DISABLED
        t3dsResetTimers();
    #endif

	log3dsWrite("#### memory in use ####");
    log3dsWrite("linear: %dkb / %dkb", (GPU3DS.linearMemTotal - linearSpaceFree()) / 1024, GPU3DS.linearMemTotal / 1024);
    log3dsWrite("vram: %dkb / %dkb", (GPU3DS.vramTotal - vramSpaceFree()) / 1024, GPU3DS.vramTotal / 1024);
	log3dsWrite("---- initialized ----");

    return true;
}

//--------------------------------------------------------
// Finalize the emulator.
//--------------------------------------------------------
int emulatorFinalize()
{
	log3dsWrite("---- emulatorFinalize ----");
    consoleClear();
    disableAptHooks();
    lcd3dsSetBacklight(settings3DS.SecondScreen, true);

    // Wait for the GPU before freeing its resources.
    gpu3dsWaitForRenderQueue();

    sav3dsFinalize();
    snd3dsFinalize();
    utils3dsRestoreCpuLimit();
    impl3dsFinalize();
    img3dsFinalize();
    notif3dsFinalize();
    ui3dsFinalize();
    gpu3dsFinalize();
    file3dsFinalize();
    romfsExit();

    osSetSpeedupEnable(false);

    log3dsWrite("==== END Logging (%s, %s) ====", settings3dsGetAppVersion("v"), log3dsGetCurrentDate());
    log3dsClose();

    return 0;
}

// Last vblank consumed by the loop.
// Inspired by red-viper's pacer, but counts refreshes with C3D_FrameCounter
// instead of a worker thread.
// Do not call C3D_FrameRate(): it divides the counter without retiming display.
static u32 paceConsumedVBlanks = 0;

// The current 2 / 2 policy is a balanced default: discard a substantial stall,
// but skip once the loop has genuinely fallen a full additional refresh behind.
// MaxFrameSkips separately limits how many recovery skips may occur in a row.
//
static const long PACE_MAX_BACKLOG = 2;     // Do not chase more than this many unconsumed vblanks.
static const long PACE_SKIP_BACKLOG = 2;    // The first vblank pays for the completed frame; skip on the next one.

// Slowdown accepted before skipping at "Up to 1" (5%, ~57 fps); higher settings hold full speed.
static const long PACE_ALLOWED_SLOWDOWN_PERCENT = 5;
static long paceSlowdownAllowance = 0;

static bool paceFrameOnVBlankCounter(int &framesSkipped)
{
    int displayId = settings3DS.GameScreen == GFX_TOP ? 0 : 1;
    u32 vblankCount = C3D_FrameCounter(displayId);
    long unconsumedVBlanks = (long)(u32)(vblankCount - paceConsumedVBlanks);

    // Only backlog present before waiting may trigger a skip.
    long backlogBeforeWait = unconsumedVBlanks;

    if (unconsumedVBlanks == 0)
    {
        // Ignore stale wakeups until the counter records a new vblank.
        do
        {
            gpu3dsWaitForVBlankBanked(settings3DS.GameScreen);
            vblankCount = C3D_FrameCounter(displayId);
            unconsumedVBlanks = (long)(u32)(vblankCount - paceConsumedVBlanks);
        }
        while (unconsumedVBlanks == 0);
    }

    if (unconsumedVBlanks > PACE_MAX_BACKLOG)
        paceConsumedVBlanks = vblankCount - (PACE_MAX_BACKLOG - 1); // retain one credit
    else
        paceConsumedVBlanks++;

    paceSlowdownAllowance += PACE_ALLOWED_SLOWDOWN_PERCENT;
    if (paceSlowdownAllowance > 100)
        paceSlowdownAllowance = 100;

    if (backlogBeforeWait >= PACE_SKIP_BACKLOG && framesSkipped < settings3DS.MaxFrameSkips)
    {
        if (settings3DS.MaxFrameSkips != 1 || paceSlowdownAllowance < 100)
        {
            framesSkipped++;
            return true;
        }

        // No wait happened and the backlog was >= 2, so at least one vblank is left to write off.
        paceConsumedVBlanks++;
        paceSlowdownAllowance -= 100;
    }

    framesSkipped = 0;
    return false;
}

// Emulator fallback.
// lcd3dsSetEmulationRate only retimes the panel on real hardware,
// so it does not arrive at the emulated rate. Pace by sleeping.
static long paceTimingOffsetTicks = 0;

static void paceFrameBySleeping(long actualTicksThisFrame)
{
    paceTimingOffsetTicks += actualTicksThisFrame - settings3DS.TicksPerFrame;

    if (paceTimingOffsetTicks < 0)
    {
        svcSleepThread((s64)((double)-paceTimingOffsetTicks * 1e9 / TICKS_PER_SEC));
        paceTimingOffsetTicks = 0;
    }
    else if (paceTimingOffsetTicks > settings3DS.TicksPerFrame)
    {
        paceTimingOffsetTicks = settings3DS.TicksPerFrame;   // retain one frame of debt
    }
}

//---------------------------------------------------------
// Paces the completed emulation frame and returns whether to skip the next render.
//---------------------------------------------------------
bool paceFrame(long actualTicksThisFrame, int totalFrames, int &framesSkipped)
{
    if (session3DS.turboMode)
        return (totalFrames % 2) == 0;

    if (device3DS.isReal3DS)
        return paceFrameOnVBlankCounter(framesSkipped);

    paceFrameBySleeping(actualTicksThisFrame);

    return false;
}

//---------------------------------------------------------
// Prints profiling timer data to the second screen.
//---------------------------------------------------------

void updateProfilingOutput(int totalFrames)
{
    #ifndef PROFILING_DISABLED
        if (t3dsProfilingMode == PROFILING_OFF)
            return;

        if (totalFrames % PROFILING_WINDOW_FRAMES == 0) {
            t3dsPrintTimers(PROFILING_WINDOW_FRAMES);
            t3dsResetTimers();
        }
    #endif
}

//----------------------------------------------------------
// This is the main emulation loop. It calls the 
//    impl3dsRunOneFrame
//   (which must be implemented for any new core)
// for the execution of the frame.
//----------------------------------------------------------
void emulatorLoop()
{
    // menu is currently rendered via software and may have configured the screen for
    // a lower color depth than our other screen content rendered via GPU.
    // therefore we check the screen format first to ensure pixel data is interpreted correctly
    GSPGPU_FramebufferFormat gpuBufFmt = (GSPGPU_FramebufferFormat)DISPLAY_TRANSFER_FMT;

    if (gfxGetScreenFormat(settings3DS.SecondScreen) != gpuBufFmt) {
        gfxSetScreenFormat(settings3DS.SecondScreen, gpuBufFmt);
    }

    gpu3dsResetState();

    // render one frame before lcd3dsSetEmulationRate below,
    // otherwise game screen glitches and real hardware freezes.
    if (gpu3dsSetTopMode()) {
        gpu3dsFrameBegin(C3D_FRAME_SYNCDRAW, false);
            impl3dsSceneRender(true);
        gpu3dsFrameEnd();
    }

    if (t3dsProfilingMode == PROFILING_OFF) {
        for (int pass = 0; pass < 2; pass++) {
            gpu3dsFrameBegin(C3D_FRAME_SYNCDRAW, false, true);
                gpu3dsClearScreen(settings3DS.SecondScreen);
                img3dsDrawBackground(UI_BG_SECOND);
            gpu3dsFrameEnd();
        }
    } else {
        // consoleInit(...) sets double buffering to false
        // make sure to enable double buffering again when leaving emulatorLoop()
        consoleInit(settings3DS.SecondScreen, NULL);
        t3dsResetTimers();
    }

    int totalFrames = 0;
    int fpsFrameCount = 0;

    int  framesSkipped = 0;

    snd3dsStartPlaying();

    lcd3dsSetEmulationRate(settings3DS.TicksPerFrame);
    lcd3dsSetBacklight(settings3DS.SecondScreen,
        settings3DS.SecondScreenBg != Setting::AssetMode::None || t3dsProfilingMode != PROFILING_OFF);

    // Start pacing from the current vblank; C3D_FrameCounter advanced during the menu.
    paceConsumedVBlanks = C3D_FrameCounter(settings3DS.GameScreen == GFX_TOP ? 0 : 1);
    paceTimingOffsetTicks = 0;
    paceSlowdownAllowance = 0;

    u64 frameCountTick = svcGetSystemTick();
    bool firstFrame = true;
    bool skipDrawing = true; // skip first ingame render to show game screen faster after menu exit
    ProfilingMode lastProfilingMode = t3dsProfilingMode;

    log3dsSetBuffered(true);

    while (aptMainLoop() && session3DS.emulatorState == EMUSTATE_EMULATE)
    {
        u64 startFrameTick = svcGetSystemTick();

        if (t3dsProfilingMode != lastProfilingMode) {
            lcd3dsSetBacklight(settings3DS.SecondScreen,
                settings3DS.SecondScreenBg != Setting::AssetMode::None || t3dsProfilingMode != PROFILING_OFF);
            if (lastProfilingMode == PROFILING_OFF) {
                gpu3dsClearScreen(settings3DS.SecondScreen);
                consoleInit(settings3DS.SecondScreen, NULL);
            }

            lastProfilingMode = t3dsProfilingMode;
            consoleClear();
            t3dsResetTimers();
        }

        updateProfilingOutput(++totalFrames);

        t3dsStartTimer(TIMER_RUN_ONE_FRAME);
        impl3dsRunOneFrame(firstFrame, skipDrawing);
        t3dsStopTimer(TIMER_RUN_ONE_FRAME);


        long actualTicksThisFrame = (long)(svcGetSystemTick() - startFrameTick);
        skipDrawing = paceFrame(actualTicksThisFrame, totalFrames, framesSkipped);

        // FPS display (~every second)
        float targetFps = (float)TICKS_PER_SEC / settings3DS.TicksPerFrame;
        if (++fpsFrameCount >= (int)targetFps)
        {
            u64 now = svcGetSystemTick();

            if (skipNextFpsUpdate) {
                skipNextFpsUpdate = false;
                frameCountTick = now;
                fpsFrameCount = 0;
                firstFrame = false;
                continue;
            }

            float elapsed = (float)(now - frameCountTick) / TICKS_PER_SEC;
            float fps = fpsFrameCount / elapsed;

            notif3dsFpsUpdate(fps);
            frameCountTick = now;
            fpsFrameCount = 0;
        }

        firstFrame = false;
    }

    log3dsSetBuffered(false);
    lcd3dsSetBacklight(settings3DS.SecondScreen, true);

    // Don't wait for the pause menu; it collects the job itself.
    sav3dsProcessSaveCompletion(session3DS.emulatorState != EMUSTATE_PAUSEMENU);

    snd3dsStopPlaying();
    lcd3dsRestoreDefaultRate();

    gfxSetDoubleBuffering(settings3DS.SecondScreen, true);
}

//---------------------------------------------------------
// Main entrypoint.
//---------------------------------------------------------
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    osSetSpeedupEnable(true);
    utils3dsInitialize();

    // ---- load/update settings first ----
    menu3dsSetHotkeysData(hotkeysData);
    settings3dsResetGlobalDefaults();
    settings3dsResetGameDefaults();
    // load global config, overwrites defaults if file exists
    cfgFileAvailable[0] = settingsLoadGlobalConfig();
    settings3dsUpdate(false);

    if (!emulatorInitialize()) {
        return emulatorFinalize();
    }

    if (!file3dsThumbnailsAvailableByType(settings3DS.GameThumbnailType))
        settings3DS.GameThumbnailType = Setting::ThumbnailMode::None;
    img3dsOpenThumbnailCache();
    gfxSetDoubleBuffering(settings3DS.SecondScreen, true);

    session3DS.emulatorState = EMUSTATE_PAUSEMENU;
    
    while (aptMainLoop() && session3DS.emulatorState != EMUSTATE_END) {
        switch (session3DS.emulatorState) {
            case EMUSTATE_PAUSEMENU:
                if (session3DS.resumeAfterSuspend) {
                    session3DS.resumeAfterSuspend = false;
                    input3dsWaitForRelease();
                    session3DS.emulatorState = EMUSTATE_EMULATE;
                } else {
                    showMenu();
                }
                break;
            case EMUSTATE_EMULATE:
                emulatorLoop();
                break;
            default:
                session3DS.emulatorState = EMUSTATE_END;
        }
    }

    log3dsWrite("==== EXIT emulator ====");

    // Apply pending cheat changes before saving.
    // HOME exit after a ROM switch can leave the cached Cheats tab from the previous ROM.
    if (session3DS.isRomLoaded && menuTabs.size() > TAB_CHEATS && coreRomInfo.romCrc32 == lastLoadedRomCRC &&
        syncCheatsFromMenu(menuTabs[TAB_CHEATS].MenuItems, true)) {
        session3DS.cheatsDirty = true;
    }

    sav3dsSaveSRAMOnUnload();
    settingsSave(session3DS.isRomLoaded);
    sav3dsSaveAuto();
    impl3dsSaveCheats();

    // clear global vectors first
    entries.clear();
    menuTabs.clear();

    return emulatorFinalize();
}
