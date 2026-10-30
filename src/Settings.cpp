#include "Settings.h"
#include "InputHandler.h"  // FormatHotkeyWithModifier for the load-time log line

#include <SimpleIni.h>
#include <REL/Module.h>
#include <algorithm> // std::clamp, std::max
#include <cmath>     // std::isfinite
#include <cstdio>    // std::snprintf
#include <cstdlib>   // std::strtoul
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

static fs::path GetIniPath() {
    const fs::path runtimePath{ REL::Module::get().filePath().data() };
    return (runtimePath.parent_path() / L"Data" / L"SKSE" / L"Plugins" /
            L"CustomMarkers.ini").lexically_normal();
}

static bool EnsureIniDirectory(const fs::path& a_path) {
    std::error_code ec;
    fs::create_directories(a_path.parent_path(), ec);
    if (ec) {
        logger::error("Failed to create CustomMarkers.ini directory '{}': {}",
                      a_path.parent_path().string(), ec.message());
        return false;
    }
    return true;
}

namespace {
    // Read 6-char RGB hex string (RRGGBB). Falls back to legacy keys if hex key is missing.
    void ReadColor3Hex(CSimpleIniA& a_ini, const char* a_section, const char* a_key,
                       const char* a_legacyPrefix,
                       std::int32_t& a_r, std::int32_t& a_g, std::int32_t& a_b) {
        const char* val = a_ini.GetValue(a_section, a_key, nullptr);
        if (val && val[0] != '\0') {
            const unsigned long rgb = std::strtoul(val, nullptr, 16);
            a_r = static_cast<std::int32_t>((rgb >> 16) & 0xFF);
            a_g = static_cast<std::int32_t>((rgb >> 8)  & 0xFF);
            a_b = static_cast<std::int32_t>( rgb        & 0xFF);
            return;
        }

        if (a_legacyPrefix) {
            std::string keyR = std::string(a_legacyPrefix) + "R";
            std::string keyG = std::string(a_legacyPrefix) + "G";
            std::string keyB = std::string(a_legacyPrefix) + "B";
            a_r = std::clamp(static_cast<std::int32_t>(a_ini.GetLongValue(a_section, keyR.c_str(), a_r)), 0, 255);
            a_g = std::clamp(static_cast<std::int32_t>(a_ini.GetLongValue(a_section, keyG.c_str(), a_g)), 0, 255);
            a_b = std::clamp(static_cast<std::int32_t>(a_ini.GetLongValue(a_section, keyB.c_str(), a_b)), 0, 255);
        }
    }

    // Read 8-char ARGB hex string (AARRGGBB). Falls back to legacy keys if hex key is missing.
    void ReadColor4Hex(CSimpleIniA& a_ini, const char* a_section, const char* a_key,
                       const char* a_legacyPrefix,
                       std::int32_t& a_r, std::int32_t& a_g, std::int32_t& a_b, std::int32_t& a_a) {
        const char* val = a_ini.GetValue(a_section, a_key, nullptr);
        if (val && val[0] != '\0') {
            const unsigned long argb = std::strtoul(val, nullptr, 16);
            a_a = static_cast<std::int32_t>((argb >> 24) & 0xFF);
            a_r = static_cast<std::int32_t>((argb >> 16) & 0xFF);
            a_g = static_cast<std::int32_t>((argb >> 8)  & 0xFF);
            a_b = static_cast<std::int32_t>( argb        & 0xFF);
            return;
        }

        if (a_legacyPrefix) {
            std::string keyR = std::string(a_legacyPrefix) + "R";
            std::string keyG = std::string(a_legacyPrefix) + "G";
            std::string keyB = std::string(a_legacyPrefix) + "B";
            std::string keyA = std::string(a_legacyPrefix) + "A";
            a_r = std::clamp(static_cast<std::int32_t>(a_ini.GetLongValue(a_section, keyR.c_str(), a_r)), 0, 255);
            a_g = std::clamp(static_cast<std::int32_t>(a_ini.GetLongValue(a_section, keyG.c_str(), a_g)), 0, 255);
            a_b = std::clamp(static_cast<std::int32_t>(a_ini.GetLongValue(a_section, keyB.c_str(), a_b)), 0, 255);
            a_a = std::clamp(static_cast<std::int32_t>(a_ini.GetLongValue(a_section, keyA.c_str(), a_a)), 0, 255);
        }
    }

    // Write 6-char RGB hex string (RRGGBB).
    void WriteColor3Hex(CSimpleIniA& a_ini, const char* a_section, const char* a_key,
                        std::int32_t a_r, std::int32_t a_g, std::int32_t a_b) {
        const unsigned long rgb =
            (static_cast<unsigned long>(a_r & 0xFF) << 16) |
            (static_cast<unsigned long>(a_g & 0xFF) << 8)  |
            (static_cast<unsigned long>(a_b & 0xFF));
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%06lX", rgb);
        a_ini.SetValue(a_section, a_key, hex);
    }

    // Write 8-char ARGB hex string (AARRGGBB).
    void WriteColor4Hex(CSimpleIniA& a_ini, const char* a_section, const char* a_key,
                        std::int32_t a_r, std::int32_t a_g, std::int32_t a_b, std::int32_t a_a) {
        const unsigned long argb =
            (static_cast<unsigned long>(a_a & 0xFF) << 24) |
            (static_cast<unsigned long>(a_r & 0xFF) << 16) |
            (static_cast<unsigned long>(a_g & 0xFF) << 8)  |
            (static_cast<unsigned long>(a_b & 0xFF));
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%08lX", argb);
        a_ini.SetValue(a_section, a_key, hex);
    }
}

void Settings::Load() {
    const auto path = GetIniPath();
    const bool fileExists = fs::exists(path);

    if (!fileExists) {
        logger::info("CustomMarkers.ini not found; creating it from defaults");
        Save();
        return;
    }

    CSimpleIniA ini;
    ini.SetUnicode();

    if (const SI_Error rc = ini.LoadFile(path.c_str()); rc < SI_OK) {
        logger::error("Failed to load CustomMarkers.ini from '{}': SimpleIni error {}",
                      path.string(), rc);
        return;
    }

    // Hotkeys. Each key pairs with its OWN modifier (see Settings.h). -1 = unbound.
    listPanelKey = static_cast<std::int32_t>(ini.GetLongValue("Hotkey", "ListPanelKey", listPanelKey));
    listPanelModifier = static_cast<std::int32_t>(ini.GetLongValue("Hotkey", "ListPanelModifier", listPanelModifier));

    pathTrackKey = static_cast<std::int32_t>(ini.GetLongValue("Hotkey", "PathTrackKey", pathTrackKey));
    pathTrackModifier = static_cast<std::int32_t>(ini.GetLongValue("Hotkey", "PathTrackModifier", pathTrackModifier));

    language = ini.GetValue("Localization", "Language", language.c_str());
    fontSize = std::clamp(static_cast<std::int32_t>(ini.GetLongValue("Localization", "FontSize", fontSize)), 10, 48);
    headingFontSize = std::clamp(static_cast<std::int32_t>(ini.GetLongValue("Localization", "HeadingFontSize", headingFontSize)), 0, 72);

    logLevel = ini.GetValue("Logging", "Level", logLevel.c_str());

    menuSettingsKey = static_cast<std::int32_t>(ini.GetLongValue("Menu", "SettingsKey", menuSettingsKey));
    menuSettingsModifier = static_cast<std::int32_t>(ini.GetLongValue("Menu", "SettingsModifier", menuSettingsModifier));

    menuActiveTab = static_cast<std::int32_t>(ini.GetLongValue("Menu", "ActiveTab", menuActiveTab));
    menuPosX = static_cast<std::int32_t>(ini.GetLongValue("Menu", "PosX", menuPosX));
    menuPosY = static_cast<std::int32_t>(ini.GetLongValue("Menu", "PosY", menuPosY));
    menuWidth = static_cast<std::int32_t>(ini.GetLongValue("Menu", "Width", menuWidth));
    menuHeight = static_cast<std::int32_t>(ini.GetLongValue("Menu", "Height", menuHeight));

    listPanelWidth = static_cast<std::int32_t>(ini.GetLongValue("ListPanel", "Width", listPanelWidth));
    listPanelVisible = (ini.GetLongValue("ListPanel", "Visible", listPanelVisible ? 1L : 0L) != 0);

    interfaceScale = std::clamp(static_cast<std::int32_t>(ini.GetLongValue("Appearance", "InterfaceScale", interfaceScale)), 50, 200);
    titleScale = std::clamp(static_cast<float>(ini.GetLongValue("Appearance", "TitleScale", static_cast<long>(titleScale * 100.0f + 0.5f))) / 100.0f, 0.5f, 3.0f);
    bodyScale = std::clamp(static_cast<float>(ini.GetLongValue("Appearance", "BodyScale", static_cast<long>(bodyScale * 100.0f + 0.5f))) / 100.0f, 0.5f, 3.0f);

    listPanelPosX = static_cast<std::int32_t>(ini.GetLongValue("ListPanel", "PosX", listPanelPosX));
    listPanelPosY = static_cast<std::int32_t>(ini.GetLongValue("ListPanel", "PosY", listPanelPosY));

    listShowOverview = (ini.GetLongValue("ListPanel", "ShowOverview", listShowOverview ? 1L : 0L) != 0);
    listShowActiveQuests = (ini.GetLongValue("ListPanel", "ShowActiveQuests", listShowActiveQuests ? 1L : 0L) != 0);
    listShowCompanions = (ini.GetLongValue("ListPanel", "ShowCompanions", listShowCompanions ? 1L : 0L) != 0);

    // Clamped on read: a stale index from an INI written by a build with more
    // tabs would index past the end of the tab table.
    listActiveTab = std::clamp(static_cast<std::int32_t>(ini.GetLongValue("ListPanel", "ActiveTab", listActiveTab)), 0, 3);

    listPanelMinHeightPct = std::clamp(static_cast<std::int32_t>(ini.GetLongValue("ListPanel", "MinHeightPct", listPanelMinHeightPct)), 0, 95);
    listPanelMaxHeightPct = std::clamp(static_cast<std::int32_t>(ini.GetLongValue("ListPanel", "MaxHeightPct", listPanelMaxHeightPct)), 30, 95);

    showFilterPanel = (ini.GetLongValue("Filters", "Show", showFilterPanel ? 1L : 0L) != 0);
    filterPosX = static_cast<std::int32_t>(ini.GetLongValue("Filters", "PosX", filterPosX));
    filterPosY = static_cast<std::int32_t>(ini.GetLongValue("Filters", "PosY", filterPosY));
    filterCollapsed = (ini.GetLongValue("Filters", "Collapsed", filterCollapsed ? 1L : 0L) != 0);

    listPanelUseCustomBg = (ini.GetLongValue("ListPanel", "UseCustomBg", listPanelUseCustomBg ? 1L : 0L) != 0);
    listPanelBgIndex = std::max(0, static_cast<int>(ini.GetLongValue("ListPanel", "BgIndex", listPanelBgIndex)));

    // Notes are free text and can run long, so they get their own scale.
    noteTextScale = std::clamp(static_cast<float>(ini.GetLongValue("StatCard", "NoteScale", static_cast<long>(noteTextScale * 100.0f + 0.5f))) / 100.0f, 0.5f, 3.0f);
    statCardAnchor = std::clamp(static_cast<std::int32_t>(ini.GetLongValue("StatCard", "Anchor", statCardAnchor)), 0, 3);
    statCardOffsetX = static_cast<std::int32_t>(ini.GetLongValue("StatCard", "OffsetX", statCardOffsetX));
    statCardOffsetY = static_cast<std::int32_t>(ini.GetLongValue("StatCard", "OffsetY", statCardOffsetY));
    statCardShowDetails = (ini.GetLongValue("StatCard", "ShowDetails", statCardShowDetails ? 1L : 0L) != 0);
    statCardShowType = (ini.GetLongValue("StatCard", "ShowType", statCardShowType ? 1L : 0L) != 0);
    statCardShowHold = (ini.GetLongValue("StatCard", "ShowHold", statCardShowHold ? 1L : 0L) != 0);
    statCardShowDistance = (ini.GetLongValue("StatCard", "ShowDistance", statCardShowDistance ? 1L : 0L) != 0);
    statCardShowBounty = (ini.GetLongValue("StatCard", "ShowBounty", statCardShowBounty ? 1L : 0L) != 0);
    statCardShowCleared = (ini.GetLongValue("StatCard", "ShowCleared", statCardShowCleared ? 1L : 0L) != 0);
    statCardShowBoss = (ini.GetLongValue("StatCard", "ShowBoss", statCardShowBoss ? 1L : 0L) != 0);
    statCardShowQuests = (ini.GetLongValue("StatCard", "ShowQuests", statCardShowQuests ? 1L : 0L) != 0);
    statCardShowServices = (ini.GetLongValue("StatCard", "ShowServices", statCardShowServices ? 1L : 0L) != 0);
    statCardShowPlayerHome = (ini.GetLongValue("StatCard", "ShowPlayerHome", statCardShowPlayerHome ? 1L : 0L) != 0);
    statCardShowSpouse = (ini.GetLongValue("StatCard", "ShowSpouse", statCardShowSpouse ? 1L : 0L) != 0);
    statCardShowCompanions = (ini.GetLongValue("StatCard", "ShowCompanions", statCardShowCompanions ? 1L : 0L) != 0);
    statCardDistanceUnits = std::clamp(static_cast<std::int32_t>(ini.GetLongValue("StatCard", "DistanceUnits", statCardDistanceUnits)), 0, 1);

    navGlanceInteriorVisible = (ini.GetLongValue("NavigationOverlay", "GlanceInteriorVisible", navGlanceInteriorVisible ? 1L : 0L) != 0);
    navGlanceExteriorVisible = (ini.GetLongValue("NavigationOverlay", "GlanceExteriorVisible", navGlanceExteriorVisible ? 1L : 0L) != 0);
    navInspectInteriorVisible = (ini.GetLongValue("NavigationOverlay", "InspectInteriorVisible", navInspectInteriorVisible ? 1L : 0L) != 0);
    navInspectExteriorVisible = (ini.GetLongValue("NavigationOverlay", "InspectExteriorVisible", navInspectExteriorVisible ? 1L : 0L) != 0);
    navInspectKey = static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "InspectKeyDIK", navInspectKey));
    navInspectModifier = static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "InspectModifier", navInspectModifier));
    navInspectActivation = std::clamp(static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "InspectActivation", navInspectActivation)), 0, 1);

    {
        const long gpx = ini.GetLongValue("NavigationOverlay", "GlancePosX", navGlancePosX);
        if (gpx >= 0) navGlancePosX = static_cast<int>(gpx);
        const long gpy = ini.GetLongValue("NavigationOverlay", "GlancePosY", navGlancePosY);
        if (gpy >= 0) navGlancePosY = static_cast<int>(gpy);
    }
    // Clamp to the slider range (100-500) so a hand-edited/garbage INI value can't
    // collapse the Glance disc to zero or blow it up off-screen.
    navGlanceSize = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "GlanceSize", navGlanceSize)), 100, 500);
    navGlanceShape = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "GlanceShape", navGlanceShape)), 0, 1);
    navGlanceOrientation = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "GlanceOrientation", navGlanceOrientation)), 0, 1);
    navGlanceShowCardinals = (ini.GetLongValue("NavigationOverlay", "GlanceShowCardinals", navGlanceShowCardinals ? 1L : 0L) != 0);
    navGlanceShowLocation = (ini.GetLongValue("NavigationOverlay", "GlanceShowLocation", navGlanceShowLocation ? 1L : 0L) != 0);
    navInspectSizePct = static_cast<int>(ini.GetLongValue("NavigationOverlay", "InspectSizePct", navInspectSizePct));
    navRefreshOnInspect = (ini.GetLongValue("NavigationOverlay", "RefreshOnInspect", navRefreshOnInspect ? 1L : 0L) != 0);

    navInspectModeToggleKey = static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "InspectModeToggleKeyDIK", navInspectModeToggleKey));
    navInspectModeToggleModifier = static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "InspectModeToggleModifier", navInspectModeToggleModifier));
    navInspectDefaultMode = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "InspectDefaultMode", navInspectDefaultMode)), 0, 1);
    navInspectRadarRangeUnits = std::max(static_cast<float>(ini.GetLongValue("NavigationOverlay", "InspectRadarRangeUnits", static_cast<long>(navInspectRadarRangeUnits))), 100.0f);
    navGlanceRangeUnits = std::max(static_cast<float>(ini.GetLongValue("NavigationOverlay", "GlanceRangeUnits", static_cast<long>(navGlanceRangeUnits))), 200.0f);
    navQuickMarkKey = static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "QuickMarkKeyDIK", navQuickMarkKey));
    navQuickMarkModifier = static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "QuickMarkModifier", navQuickMarkModifier));
    navGlanceToggleKey = static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "GlanceToggleKeyDIK", navGlanceToggleKey));
    navGlanceToggleModifier = static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "GlanceToggleModifier", navGlanceToggleModifier));
    lootBeamToggleKey = static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "LootBeamToggleKeyDIK", lootBeamToggleKey));
    lootBeamToggleModifier = static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "LootBeamToggleModifier", lootBeamToggleModifier));

    const auto loadControllerButton = [&](const char* a_key, std::int32_t a_default) {
        const auto value = static_cast<std::int32_t>(ini.GetLongValue("Controller", a_key, a_default));
        return value == -1 || (value >= 266 && value <= 281) ? value : a_default;
    };
    controllerInspectOpenButton = loadControllerButton("InspectOpenButton", controllerInspectOpenButton);
    controllerInspectOpenGesture = std::clamp(static_cast<std::int32_t>(
        ini.GetLongValue("Controller", "InspectOpenGesture", controllerInspectOpenGesture)), 0, 2);
    controllerInspectAddMarkerButton = loadControllerButton("InspectAddMarkerButton", controllerInspectAddMarkerButton);
    controllerInspectSwitchViewButton = loadControllerButton("InspectSwitchViewButton", controllerInspectSwitchViewButton);
    controllerInspectPathTrackingButton = loadControllerButton("InspectPathTrackingButton", controllerInspectPathTrackingButton);

    // Look & Pin. Own section on purpose -- see the Settings.h comment.
    lookPinEnabled = (ini.GetLongValue("LookPin", "Enabled", lookPinEnabled ? 1L : 0L) != 0);
    lookPinKey = static_cast<std::int32_t>(ini.GetLongValue("LookPin", "KeyDIK", lookPinKey));
    lookPinModifier = static_cast<std::int32_t>(ini.GetLongValue("LookPin", "Modifier", lookPinModifier));

    // Entity subtype filters, FLAT per view (Glance/Inspect), 22 keys each.
    auto loadEntBool = [&](const char* a_key, bool& a_field) {
        a_field = (ini.GetLongValue("NavigationOverlay", a_key, a_field ? 1L : 0L) != 0);
    };
    loadEntBool("GlanceEntContChest",      navGlanceEntContChest);
    loadEntBool("GlanceEntContBarrel",     navGlanceEntContBarrel);
    loadEntBool("GlanceEntContSack",       navGlanceEntContSack);
    loadEntBool("GlanceEntContUrn",        navGlanceEntContUrn);
    loadEntBool("GlanceEntContCoffin",     navGlanceEntContCoffin);
    loadEntBool("GlanceEntContStrongbox",  navGlanceEntContStrongbox);
    loadEntBool("GlanceEntLootWeapon",     navGlanceEntLootWeapon);
    loadEntBool("GlanceEntLootAmmo",       navGlanceEntLootAmmo);
    loadEntBool("GlanceEntLootArmor",      navGlanceEntLootArmor);
    loadEntBool("GlanceEntLootGold",       navGlanceEntLootGold);
    loadEntBool("GlanceEntLootKey",        navGlanceEntLootKey);
    loadEntBool("GlanceEntLootPotion",     navGlanceEntLootPotion);
    loadEntBool("GlanceEntLootFood",       navGlanceEntLootFood);
    loadEntBool("GlanceEntLootDrink",      navGlanceEntLootDrink);
    loadEntBool("GlanceEntLootBook",       navGlanceEntLootBook);
    loadEntBool("GlanceEntLootSoulGem",    navGlanceEntLootSoulGem);
    loadEntBool("GlanceEntLootScroll",     navGlanceEntLootScroll);
    loadEntBool("GlanceEntCorpseHumanoid", navGlanceEntCorpseHumanoid);
    loadEntBool("GlanceEntCorpseCreature", navGlanceEntCorpseCreature);
    loadEntBool("GlanceEntDoorLoad",       navGlanceEntDoorLoad);
    loadEntBool("GlanceEntDoorRegular",    navGlanceEntDoorRegular);
    loadEntBool("GlanceEntIngrLoose",      navGlanceEntIngrLoose);
    loadEntBool("GlanceEntIngrPlant",      navGlanceEntIngrPlant);

    loadEntBool("InspectEntContChest",      navInspectEntContChest);
    loadEntBool("InspectEntContBarrel",     navInspectEntContBarrel);
    loadEntBool("InspectEntContSack",       navInspectEntContSack);
    loadEntBool("InspectEntContUrn",        navInspectEntContUrn);
    loadEntBool("InspectEntContCoffin",     navInspectEntContCoffin);
    loadEntBool("InspectEntContStrongbox",  navInspectEntContStrongbox);
    loadEntBool("InspectEntLootWeapon",     navInspectEntLootWeapon);
    loadEntBool("InspectEntLootAmmo",       navInspectEntLootAmmo);
    loadEntBool("InspectEntLootArmor",      navInspectEntLootArmor);
    loadEntBool("InspectEntLootGold",       navInspectEntLootGold);
    loadEntBool("InspectEntLootKey",        navInspectEntLootKey);
    loadEntBool("InspectEntLootPotion",     navInspectEntLootPotion);
    loadEntBool("InspectEntLootFood",       navInspectEntLootFood);
    loadEntBool("InspectEntLootDrink",      navInspectEntLootDrink);
    loadEntBool("InspectEntLootBook",       navInspectEntLootBook);
    loadEntBool("InspectEntLootSoulGem",    navInspectEntLootSoulGem);
    loadEntBool("InspectEntLootScroll",     navInspectEntLootScroll);
    loadEntBool("InspectEntCorpseHumanoid", navInspectEntCorpseHumanoid);
    loadEntBool("InspectEntCorpseCreature", navInspectEntCorpseCreature);
    loadEntBool("InspectEntDoorLoad",       navInspectEntDoorLoad);
    loadEntBool("InspectEntDoorRegular",    navInspectEntDoorRegular);
    loadEntBool("InspectEntIngrLoose",      navInspectEntIngrLoose);
    loadEntBool("InspectEntIngrPlant",      navInspectEntIngrPlant);

    const auto glanceEntityIconSize = static_cast<int>(ini.GetLongValue("NavigationOverlay", "GlanceEntityIconSize", navGlanceEntityIconSize));
    // Match both settings sliders so saved sizes survive a restart.
    navGlanceEntityIconSize = std::clamp(glanceEntityIconSize, 12, 24);
    logger::debug("GlanceEntityIconSize load: INI={} applied={}", glanceEntityIconSize, navGlanceEntityIconSize);
    navInspectEntityIconSize = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "InspectEntityIconSize", navInspectEntityIconSize)), 10, 40);
    ReadColor4Hex(ini, "NavigationOverlay", "EntityIconBackground", nullptr,
                  navEntityIconBgR, navEntityIconBgG, navEntityIconBgB, navEntityIconBgA);
    ReadColor4Hex(ini, "NavigationOverlay", "EntityIconBorder", nullptr,
                  navEntityIconBorderR, navEntityIconBorderG, navEntityIconBorderB, navEntityIconBorderA);
    navEntityIconPadding = std::clamp(static_cast<float>(ini.GetLongValue(
        "NavigationOverlay", "EntityIconPadding", static_cast<long>(navEntityIconPadding * 100.0f + 0.5f))) / 100.0f,
        0.0f, 8.0f);

    navGlancePinSize = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "GlancePinSize", navGlancePinSize)), 6, 24);
    navInspectPinSize = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "InspectPinSize", navInspectPinSize)), 12, 48);

    navGlanceShowTrail = (ini.GetLongValue("NavigationOverlay", "GlanceShowTrail", navGlanceShowTrail ? 1L : 0L) != 0);
    navInspectShowTrail = (ini.GetLongValue("NavigationOverlay", "InspectShowTrail", navInspectShowTrail ? 1L : 0L) != 0);

    navActorShowAlly = (ini.GetLongValue("NavigationOverlay", "ActorShowAlly", navActorShowAlly ? 1L : 0L) != 0);
    navActorShowHostile = (ini.GetLongValue("NavigationOverlay", "ActorShowHostile", navActorShowHostile ? 1L : 0L) != 0);
    navActorShowNeutral = (ini.GetLongValue("NavigationOverlay", "ActorShowNeutral", navActorShowNeutral ? 1L : 0L) != 0);
    navActorShowAnimal = (ini.GetLongValue("NavigationOverlay", "ActorShowAnimal", navActorShowAnimal ? 1L : 0L) != 0);
    navGlanceActorDotSize = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "GlanceActorDotSize", navGlanceActorDotSize)), 2, 16);
    navInspectActorDotSize = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "InspectActorDotSize", navInspectActorDotSize)), 3, 24);
    navActorProximityUnits = std::clamp(static_cast<float>(ini.GetLongValue("NavigationOverlay", "ActorProximityUnits", static_cast<long>(navActorProximityUnits))), 200.0f, 4000.0f);

    // MarkerRange is stored in the INI in METERS; navMarkerRange holds units.
    {
        const long defM = static_cast<long>(navMarkerRange / Settings::kUnitsPerMeter);
        const float meters = std::clamp(static_cast<float>(ini.GetLongValue("NavigationOverlay", "MarkerRange", defM)), 100.0f, 5000.0f);
        navMarkerRange = meters * Settings::kUnitsPerMeter;
    }
    navMaxQuestMarkers = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "MaxQuestMarkers", navMaxQuestMarkers)), 0, 10);
    navMaxCustomMarkers = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "MaxCustomMarkers", navMaxCustomMarkers)), 0, 30);

    navBeaconVisible = (ini.GetLongValue("NavigationOverlay", "BeaconVisible", navBeaconVisible ? 1L : 0L) != 0);
    navBeaconPillarHeight = std::clamp(static_cast<float>(ini.GetLongValue("NavigationOverlay", "BeaconPillarHeight", static_cast<long>(navBeaconPillarHeight))), 50.0f, 2000.0f);
    navBeaconNearScale = std::clamp(static_cast<float>(ini.GetLongValue("NavigationOverlay", "BeaconNearScale", static_cast<long>(navBeaconNearScale * 100.0f + 0.5f))) / 100.0f, 0.05f, 1.0f);
    navBeaconPinSize = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "BeaconPinSize", navBeaconPinSize)), 12, 48);
    navBeaconFadeNearUnits = std::clamp(static_cast<float>(ini.GetLongValue("NavigationOverlay", "BeaconFadeNearUnits", static_cast<long>(navBeaconFadeNearUnits))), 50.0f, 2000.0f);
    navBeaconClearOnArrival = (ini.GetLongValue("NavigationOverlay", "BeaconClearOnArrival", navBeaconClearOnArrival ? 1L : 0L) != 0);
    navBeaconEdgeIndicator = (ini.GetLongValue("NavigationOverlay", "BeaconEdgeIndicator", navBeaconEdgeIndicator ? 1L : 0L) != 0);
    lootBeamRangeUnits = std::clamp(static_cast<float>(ini.GetLongValue("NavigationOverlay", "LootBeamRangeUnits", static_cast<long>(lootBeamRangeUnits))), 200.0f, 6000.0f);
    lootBeamZBandUnits = std::clamp(static_cast<float>(ini.GetLongValue("NavigationOverlay", "LootBeamZBandUnits", static_cast<long>(lootBeamZBandUnits))), 50.0f, 2000.0f);
    lootBeamRequireLOS = (ini.GetLongValue("NavigationOverlay", "LootBeamRequireLOS", lootBeamRequireLOS ? 1L : 0L) != 0);
    lootBeamOcclusion = (ini.GetLongValue("NavigationOverlay", "LootBeamOcclusion", lootBeamOcclusion ? 1L : 0L) != 0);
    navEntityRequireLOS = (ini.GetLongValue("NavigationOverlay", "EntityRequireLOS", navEntityRequireLOS ? 1L : 0L) != 0);
    lootBeamVisible = (ini.GetLongValue("NavigationOverlay", "LootBeamVisible", lootBeamVisible ? 1L : 0L) != 0);
    lootBeamHideInCombat = (ini.GetLongValue("NavigationOverlay", "LootBeamHideInCombat", lootBeamHideInCombat ? 1L : 0L) != 0);
    lootBeamHideAfterLooting = (ini.GetLongValue("NavigationOverlay", "LootBeamHideAfterLooting", lootBeamHideAfterLooting ? 1L : 0L) != 0);
    lootBeamHideAfterInspecting = (ini.GetLongValue("NavigationOverlay", "LootBeamHideAfterInspecting", lootBeamHideAfterInspecting ? 1L : 0L) != 0);
    lootBeamTrackDroppedWeapons = (ini.GetLongValue("NavigationOverlay", "LootBeamTrackDroppedWeapons", lootBeamTrackDroppedWeapons ? 1L : 0L) != 0);
    lootBeamDesign = std::clamp(static_cast<std::int32_t>(ini.GetLongValue("NavigationOverlay", "LootBeamDesign", lootBeamDesign)), 0, 1);
    {
        const float value = static_cast<float>(ini.GetDoubleValue("NavigationOverlay", "LootBeamModernBottomWidth", lootBeamModernBottomWidth));
        lootBeamModernBottomWidth = std::clamp(std::isfinite(value) ? value : lootBeamModernBottomWidth, 0.0f, 36.0f);
    }
    {
        const float value = static_cast<float>(ini.GetDoubleValue("NavigationOverlay", "LootBeamModernTopWidth", lootBeamModernTopWidth));
        lootBeamModernTopWidth = std::clamp(std::isfinite(value) ? value : lootBeamModernTopWidth, 0.0f, 36.0f);
    }
    {
        const float value = static_cast<float>(ini.GetDoubleValue("NavigationOverlay", "LootBeamModernGroundGlow", lootBeamModernGroundGlow));
        if (std::isfinite(value)) lootBeamModernGroundGlow = std::clamp(value, 0.0f, 1.0f);
    }
    {
        const float value = static_cast<float>(ini.GetDoubleValue("NavigationOverlay", "LootBeamWispBottomWidth", lootBeamWispBottomWidth));
        lootBeamWispBottomWidth = std::clamp(std::isfinite(value) ? value : lootBeamWispBottomWidth, 0.0f, 20.0f);
    }
    {
        const float value = static_cast<float>(ini.GetDoubleValue("NavigationOverlay", "LootBeamWispTopWidth", lootBeamWispTopWidth));
        lootBeamWispTopWidth = std::clamp(std::isfinite(value) ? value : lootBeamWispTopWidth, 0.0f, 20.0f);
    }
    {
        const float value = static_cast<float>(ini.GetDoubleValue("NavigationOverlay", "LootBeamWispGroundGlow", lootBeamWispGroundGlow));
        if (std::isfinite(value)) lootBeamWispGroundGlow = std::clamp(value, 0.0f, 1.0f);
    }

    hideInBeastForm = (ini.GetLongValue("NavigationOverlay", "HideInBeastForm", hideInBeastForm ? 1L : 0L) != 0);
    lootBeamHideOwned = (ini.GetLongValue("NavigationOverlay", "LootBeamHideOwned", lootBeamHideOwned ? 1L : 0L) != 0);
    lootBeamLengthUnits = std::clamp(static_cast<float>(ini.GetLongValue("NavigationOverlay", "LootBeamLengthUnits", static_cast<long>(lootBeamLengthUnits))), 20.0f, 600.0f);
    lootBeamIconSize = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "LootBeamIconSize", lootBeamIconSize)), 10, 48);
    lootBeamBrightness = std::clamp(static_cast<float>(ini.GetLongValue("NavigationOverlay", "LootBeamBrightness", static_cast<long>(lootBeamBrightness * 100.0f + 0.5f))) / 100.0f, 0.10f, 1.0f);

    lootBeamShowCorpseHumanoid = (ini.GetLongValue("NavigationOverlay", "LootBeamShowCorpseHumanoid", lootBeamShowCorpseHumanoid ? 1L : 0L) != 0);
    lootBeamShowCorpseCreature = (ini.GetLongValue("NavigationOverlay", "LootBeamShowCorpseCreature", lootBeamShowCorpseCreature ? 1L : 0L) != 0);
    lootBeamShowContChest = (ini.GetLongValue("NavigationOverlay", "LootBeamShowContChest", lootBeamShowContChest ? 1L : 0L) != 0);
    lootBeamShowContStrongbox = (ini.GetLongValue("NavigationOverlay", "LootBeamShowContStrongbox", lootBeamShowContStrongbox ? 1L : 0L) != 0);
    lootBeamShowContSatchel = (ini.GetLongValue("NavigationOverlay", "LootBeamShowContSatchel", lootBeamShowContSatchel ? 1L : 0L) != 0);
    lootBeamShowLootWeapon = (ini.GetLongValue("NavigationOverlay", "LootBeamShowLootWeapon", lootBeamShowLootWeapon ? 1L : 0L) != 0);
    lootBeamShowLootArmor = (ini.GetLongValue("NavigationOverlay", "LootBeamShowLootArmor", lootBeamShowLootArmor ? 1L : 0L) != 0);
    lootBeamShowLootGold = (ini.GetLongValue("NavigationOverlay", "LootBeamShowLootGold", lootBeamShowLootGold ? 1L : 0L) != 0);
    lootBeamShowLootCoinPurse = (ini.GetLongValue("NavigationOverlay", "LootBeamShowLootCoinPurse", lootBeamShowLootCoinPurse ? 1L : 0L) != 0);
    lootBeamShowLootPotion = (ini.GetLongValue("NavigationOverlay", "LootBeamShowLootPotion", lootBeamShowLootPotion ? 1L : 0L) != 0);
    lootBeamShowLootSoulGem = (ini.GetLongValue("NavigationOverlay", "LootBeamShowLootSoulGem", lootBeamShowLootSoulGem ? 1L : 0L) != 0);
    lootBeamShowLootScroll = (ini.GetLongValue("NavigationOverlay", "LootBeamShowLootScroll", lootBeamShowLootScroll ? 1L : 0L) != 0);
    lootBeamShowLootBook = (ini.GetLongValue("NavigationOverlay", "LootBeamShowLootBook", lootBeamShowLootBook ? 1L : 0L) != 0);

    lootBeamMinGoldValue = std::clamp(static_cast<int>(ini.GetLongValue("NavigationOverlay", "LootBeamMinGoldValue", lootBeamMinGoldValue)), 0, 5000);

    inputVerboseLog = (ini.GetLongValue("Logging", "InputVerboseLog", inputVerboseLog ? 1L : 0L) != 0);
    cursorVerboseLog = (ini.GetLongValue("Logging", "CursorVerboseLog", cursorVerboseLog ? 1L : 0L) != 0);
    hudMarkerLog = (ini.GetLongValue("Logging", "HUDMarkerLog", hudMarkerLog ? 1L : 0L) != 0);
    beaconVerboseLog = (ini.GetLongValue("Logging", "BeaconVerboseLog", beaconVerboseLog ? 1L : 0L) != 0);
    lootBeamPerfLog = (ini.GetLongValue("Logging", "LootBeamPerfLog", lootBeamPerfLog ? 1L : 0L) != 0);

    questRowLog = (ini.GetLongValue("Logging", "QuestRowLog", questRowLog ? 1L : 0L) != 0);
    mapStatsLog = (ini.GetLongValue("Logging", "MapStatsLog", mapStatsLog ? 1L : 0L) != 0);
    mapPinProjectionLog = (ini.GetLongValue("Logging", "MapPinProjectionLog", mapPinProjectionLog ? 1L : 0L) != 0);
    minimapSnapshotLog = (ini.GetLongValue("Logging", "MinimapSnapshotLog", minimapSnapshotLog ? 1L : 0L) != 0);

    enablePathTracking = (ini.GetLongValue("PathTracking", "Enabled", enablePathTracking ? 1L : 0L) != 0);

    pathTrailVisible = (ini.GetLongValue("PathTrail", "Visible", pathTrailVisible ? 1L : 0L) != 0);
    ReadColor4Hex(ini, "PathTrail", "Color", "Color", pathTrailColorR, pathTrailColorG, pathTrailColorB, pathTrailColorA);
    pathTrailThickness = std::clamp(static_cast<float>(ini.GetLongValue("PathTrail", "Thickness", static_cast<long>(pathTrailThickness * 100.0f + 0.5f))) / 100.0f, 1.0f, 8.0f);
    pathTrailFadeByAge = (ini.GetLongValue("PathTrail", "FadeByAge", pathTrailFadeByAge ? 1L : 0L) != 0);
    pathTrailSmartSpacing = (ini.GetLongValue("PathTrail", "SmartSpacing", pathTrailSmartSpacing ? 1L : 0L) != 0);
    pathPulseEnabled = (ini.GetLongValue("PathTrail", "Pulse", pathPulseEnabled ? 1L : 0L) != 0);
    ReadColor3Hex(ini, "PathTrail", "PulseColor", "PulseColor", pathPulseColorR, pathPulseColorG, pathPulseColorB);

    markerIconScale = std::clamp(static_cast<float>(ini.GetLongValue("Markers", "IconScale", static_cast<long>(markerIconScale * 100.0f + 0.5f))) / 100.0f, 0.5f, 2.0f);
    ReadColor4Hex(ini, "Markers", "PinTint", "PinTint", pinTintR, pinTintG, pinTintB, pinTintA);
    iconFx3D = (ini.GetLongValue("Markers", "IconFx3D", iconFx3D ? 1L : 0L) != 0);
    // Older INIs had one switch for both passes. Preserve that choice until
    // Shadow is saved independently.
    iconShadow = (ini.GetLongValue("Markers", "IconShadow", iconFx3D ? 1L : 0L) != 0);
    // Existing INIs inherit their icon-effect choices once. Saving writes
    // independent entity keys; later changes to Appearance stay map-specific.
    navEntityIcon3D = (ini.GetLongValue("NavigationOverlay", "EntityIcon3D", iconFx3D ? 1L : 0L) != 0);
    navEntityIconShadow = (ini.GetLongValue("NavigationOverlay", "EntityIconShadow", iconShadow ? 1L : 0L) != 0);
    enableBadges = (ini.GetLongValue("Markers", "EnableBadges", enableBadges ? 1L : 0L) != 0);
    favBadgeSize = std::clamp(static_cast<float>(ini.GetLongValue("Markers", "FavSize", static_cast<long>(favBadgeSize * 100.0f + 0.5f))) / 100.0f, 1.0f, 12.0f);
    ReadColor4Hex(ini, "Markers", "FavColor", "FavColor", favBadgeColorR, favBadgeColorG, favBadgeColorB, favBadgeColorA);
    noteBadgeSize = std::clamp(static_cast<float>(ini.GetLongValue("Markers", "NoteSize", static_cast<long>(noteBadgeSize * 100.0f + 0.5f))) / 100.0f, 1.0f, 12.0f);
    ReadColor4Hex(ini, "Markers", "NoteColor", "NoteColor", noteBadgeColorR, noteBadgeColorG, noteBadgeColorB, noteBadgeColorA);
    clearedBadgeSize = std::clamp(static_cast<float>(ini.GetLongValue("Markers", "ClearedSize", static_cast<long>(clearedBadgeSize * 100.0f + 0.5f))) / 100.0f, 1.0f, 12.0f);
    ReadColor4Hex(ini, "Markers", "ClearedColor", "ClearedColor", clearedBadgeColorR, clearedBadgeColorG, clearedBadgeColorB, clearedBadgeColorA);
    markerImageEnabled = (ini.GetLongValue("Markers", "ImageEnabled", markerImageEnabled ? 1L : 0L) != 0);
    // Old ImageWidth was a fraction; not migrated, it would read as a px minimum.
    markerImageMinWidth = std::clamp(static_cast<int>(ini.GetLongValue("Markers", "ImageMinWidth", markerImageMinWidth)), 0, 800);
    markerImageAlpha = std::clamp(static_cast<float>(ini.GetLongValue("Markers", "ImageAlpha", static_cast<long>(markerImageAlpha * 100.0f + 0.5f))) / 100.0f, 0.0f, 1.0f);
    markerHoverOffsetX = static_cast<std::int32_t>(ini.GetLongValue("Markers", "HoverOffsetX", markerHoverOffsetX));
    markerHoverOffsetY = static_cast<std::int32_t>(ini.GetLongValue("Markers", "HoverOffsetY", markerHoverOffsetY));

    ReadColor3Hex(ini, "Appearance", "TitleColor", nullptr, titleColorR, titleColorG, titleColorB);
    ReadColor3Hex(ini, "Appearance", "BodyColor", nullptr, bodyColorR, bodyColorG, bodyColorB);
    ReadColor4Hex(ini, "Appearance", "BgColor", nullptr, bgColorR, bgColorG, bgColorB, bgColorA);
    ReadColor4Hex(ini, "Appearance", "BorderColor", nullptr, borderColorR, borderColorG, borderColorB, borderColorA);

    bgShadow = (ini.GetLongValue("Appearance", "BgShadow", bgShadow ? 1L : 0L) != 0);
    bgGradient = (ini.GetLongValue("Appearance", "BgGradient", bgGradient ? 1L : 0L) != 0);
    bgVignette = (ini.GetLongValue("Appearance", "BgVignette", bgVignette ? 1L : 0L) != 0);
    bgHighlight = (ini.GetLongValue("Appearance", "BgHighlight", bgHighlight ? 1L : 0L) != 0);
    bgBorder = (ini.GetLongValue("Appearance", "BgBorder", bgBorder ? 1L : 0L) != 0);

    logger::info("CustomMarkers.ini loaded:");
    logger::info("  Settings hotkey = {}", InputHandler::FormatHotkeyWithModifier(menuSettingsKey, menuSettingsModifier));
    logger::info("  Language = {}", language);
    logger::info("  Logging Level = {}", logLevel);
}

void Settings::Save() {
    const auto path = GetIniPath();

    CSimpleIniA ini;
    ini.SetUnicode();
    if (fs::exists(path)) {
        if (const SI_Error rc = ini.LoadFile(path.c_str()); rc < SI_OK) {
            logger::error("Refusing to overwrite unreadable CustomMarkers.ini at '{}': SimpleIni error {}",
                          path.string(), rc);
            return;
        }
    }

    ini.SetLongValue("Hotkey", "ListPanelKey", listPanelKey);
    ini.SetLongValue("Hotkey", "ListPanelModifier", listPanelModifier);
    ini.SetLongValue("Hotkey", "PathTrackKey", pathTrackKey);
    ini.SetLongValue("Hotkey", "PathTrackModifier", pathTrackModifier);

    ini.SetValue("Localization", "Language", language.c_str());
    ini.SetLongValue("Localization", "FontSize", fontSize);
    ini.SetLongValue("Localization", "HeadingFontSize", headingFontSize);

    ini.SetValue("Logging", "Level", logLevel.c_str());
    ini.SetLongValue("Logging", "InputVerboseLog", inputVerboseLog ? 1L : 0L);
    ini.SetLongValue("Logging", "CursorVerboseLog", cursorVerboseLog ? 1L : 0L);
    ini.SetLongValue("Logging", "HUDMarkerLog", hudMarkerLog ? 1L : 0L);
    ini.SetLongValue("Logging", "BeaconVerboseLog", beaconVerboseLog ? 1L : 0L);
    ini.SetLongValue("Logging", "LootBeamPerfLog", lootBeamPerfLog ? 1L : 0L);
    ini.SetLongValue("Logging", "QuestRowLog", questRowLog ? 1L : 0L);
    ini.SetLongValue("Logging", "MapStatsLog", mapStatsLog ? 1L : 0L);
    ini.SetLongValue("Logging", "MapPinProjectionLog", mapPinProjectionLog ? 1L : 0L);
    ini.SetLongValue("Logging", "MinimapSnapshotLog", minimapSnapshotLog ? 1L : 0L);

    ini.SetLongValue("Menu", "SettingsKey", menuSettingsKey);
    ini.SetLongValue("Menu", "SettingsModifier", menuSettingsModifier);
    ini.SetLongValue("Menu", "ActiveTab", menuActiveTab);
    ini.SetLongValue("Menu", "PosX", menuPosX);
    ini.SetLongValue("Menu", "PosY", menuPosY);
    ini.SetLongValue("Menu", "Width", menuWidth);
    ini.SetLongValue("Menu", "Height", menuHeight);

    ini.SetLongValue("ListPanel", "Width", listPanelWidth);
    ini.SetLongValue("ListPanel", "Visible", listPanelVisible ? 1L : 0L);
    ini.SetLongValue("ListPanel", "ShowOverview", listShowOverview ? 1L : 0L);
    ini.SetLongValue("ListPanel", "ShowActiveQuests", listShowActiveQuests ? 1L : 0L);
    ini.SetLongValue("ListPanel", "ShowCompanions", listShowCompanions ? 1L : 0L);
    ini.SetLongValue("ListPanel", "ActiveTab", listActiveTab);
    ini.SetLongValue("ListPanel", "PosX", listPanelPosX);
    ini.SetLongValue("ListPanel", "PosY", listPanelPosY);
    ini.SetLongValue("ListPanel", "MinHeightPct", listPanelMinHeightPct);
    ini.SetLongValue("ListPanel", "MaxHeightPct", listPanelMaxHeightPct);
    ini.SetLongValue("ListPanel", "UseCustomBg", listPanelUseCustomBg ? 1L : 0L);
    ini.SetLongValue("ListPanel", "BgIndex", listPanelBgIndex);
    ini.SetLongValue("Appearance", "InterfaceScale", interfaceScale);
    ini.SetLongValue("Appearance", "TitleScale", static_cast<long>(titleScale * 100.0f + 0.5f));
    ini.SetLongValue("Appearance", "BodyScale", static_cast<long>(bodyScale * 100.0f + 0.5f));

    ini.SetLongValue("Filters", "Show", showFilterPanel ? 1L : 0L);
    ini.SetLongValue("Filters", "PosX", filterPosX);
    ini.SetLongValue("Filters", "PosY", filterPosY);
    ini.SetLongValue("Filters", "Collapsed", filterCollapsed ? 1L : 0L);

    ini.SetLongValue("StatCard", "NoteScale", static_cast<long>(noteTextScale * 100.0f + 0.5f));
    ini.SetLongValue("StatCard", "Anchor", statCardAnchor);
    ini.SetLongValue("StatCard", "OffsetX", statCardOffsetX);
    ini.SetLongValue("StatCard", "OffsetY", statCardOffsetY);
    ini.SetLongValue("StatCard", "ShowDetails", statCardShowDetails ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowType", statCardShowType ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowHold", statCardShowHold ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowDistance", statCardShowDistance ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowBounty", statCardShowBounty ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowCleared", statCardShowCleared ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowBoss", statCardShowBoss ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowQuests", statCardShowQuests ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowServices", statCardShowServices ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowPlayerHome", statCardShowPlayerHome ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowSpouse", statCardShowSpouse ? 1L : 0L);
    ini.SetLongValue("StatCard", "ShowCompanions", statCardShowCompanions ? 1L : 0L);
    ini.SetLongValue("StatCard", "DistanceUnits", statCardDistanceUnits);

    ini.SetLongValue("NavigationOverlay", "GlanceInteriorVisible", navGlanceInteriorVisible ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "GlanceExteriorVisible", navGlanceExteriorVisible ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "InspectInteriorVisible", navInspectInteriorVisible ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "InspectExteriorVisible", navInspectExteriorVisible ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "InspectKeyDIK", navInspectKey);
    ini.SetLongValue("NavigationOverlay", "InspectModifier", navInspectModifier);
    ini.SetLongValue("NavigationOverlay", "InspectActivation", navInspectActivation);
    ini.SetLongValue("NavigationOverlay", "GlancePosX", navGlancePosX);
    ini.SetLongValue("NavigationOverlay", "GlancePosY", navGlancePosY);
    ini.SetLongValue("NavigationOverlay", "GlanceSize", navGlanceSize);
    ini.SetLongValue("NavigationOverlay", "GlanceShape", navGlanceShape);
    ini.SetLongValue("NavigationOverlay", "GlanceOrientation", navGlanceOrientation);
    ini.SetLongValue("NavigationOverlay", "GlanceShowCardinals", navGlanceShowCardinals ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "GlanceShowLocation", navGlanceShowLocation ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "InspectSizePct", navInspectSizePct);
    ini.SetLongValue("NavigationOverlay", "RefreshOnInspect", navRefreshOnInspect ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "InspectModeToggleKeyDIK", navInspectModeToggleKey);
    ini.SetLongValue("NavigationOverlay", "InspectModeToggleModifier", navInspectModeToggleModifier);
    ini.SetLongValue("NavigationOverlay", "InspectDefaultMode", navInspectDefaultMode);
    ini.SetLongValue("NavigationOverlay", "InspectRadarRangeUnits", static_cast<long>(navInspectRadarRangeUnits));
    ini.SetLongValue("NavigationOverlay", "GlanceRangeUnits", static_cast<long>(navGlanceRangeUnits));
    ini.SetLongValue("NavigationOverlay", "QuickMarkKeyDIK", navQuickMarkKey);
    ini.SetLongValue("NavigationOverlay", "QuickMarkModifier", navQuickMarkModifier);
    ini.SetLongValue("NavigationOverlay", "GlanceToggleKeyDIK", navGlanceToggleKey);
    ini.SetLongValue("NavigationOverlay", "GlanceToggleModifier", navGlanceToggleModifier);
    ini.SetLongValue("NavigationOverlay", "LootBeamToggleKeyDIK", lootBeamToggleKey);
    ini.SetLongValue("NavigationOverlay", "LootBeamToggleModifier", lootBeamToggleModifier);

    ini.SetLongValue("Controller", "InspectOpenButton", controllerInspectOpenButton);
    ini.SetLongValue("Controller", "InspectOpenGesture", controllerInspectOpenGesture);
    ini.SetLongValue("Controller", "InspectAddMarkerButton", controllerInspectAddMarkerButton);
    ini.SetLongValue("Controller", "InspectSwitchViewButton", controllerInspectSwitchViewButton);
    ini.SetLongValue("Controller", "InspectPathTrackingButton", controllerInspectPathTrackingButton);

    // Look & Pin, own [LookPin] section -- mirrors the Load() block above.
    ini.SetLongValue("LookPin", "Enabled", lookPinEnabled ? 1L : 0L);
    ini.SetLongValue("LookPin", "KeyDIK", lookPinKey);
    ini.SetLongValue("LookPin", "Modifier", lookPinModifier);

    // Entity subtype filters, FLAT per view.
    auto saveEntBool = [&](const char* a_key, bool a_field) {
        ini.SetLongValue("NavigationOverlay", a_key, a_field ? 1L : 0L);
    };
    saveEntBool("GlanceEntContChest",      navGlanceEntContChest);
    saveEntBool("GlanceEntContBarrel",     navGlanceEntContBarrel);
    saveEntBool("GlanceEntContSack",       navGlanceEntContSack);
    saveEntBool("GlanceEntContUrn",        navGlanceEntContUrn);
    saveEntBool("GlanceEntContCoffin",     navGlanceEntContCoffin);
    saveEntBool("GlanceEntContStrongbox",  navGlanceEntContStrongbox);
    saveEntBool("GlanceEntLootWeapon",     navGlanceEntLootWeapon);
    saveEntBool("GlanceEntLootAmmo",       navGlanceEntLootAmmo);
    saveEntBool("GlanceEntLootArmor",      navGlanceEntLootArmor);
    saveEntBool("GlanceEntLootGold",       navGlanceEntLootGold);
    saveEntBool("GlanceEntLootKey",        navGlanceEntLootKey);
    saveEntBool("GlanceEntLootPotion",     navGlanceEntLootPotion);
    saveEntBool("GlanceEntLootFood",       navGlanceEntLootFood);
    saveEntBool("GlanceEntLootDrink",      navGlanceEntLootDrink);
    saveEntBool("GlanceEntLootBook",       navGlanceEntLootBook);
    saveEntBool("GlanceEntLootSoulGem",    navGlanceEntLootSoulGem);
    saveEntBool("GlanceEntLootScroll",     navGlanceEntLootScroll);
    saveEntBool("GlanceEntCorpseHumanoid", navGlanceEntCorpseHumanoid);
    saveEntBool("GlanceEntCorpseCreature", navGlanceEntCorpseCreature);
    saveEntBool("GlanceEntDoorLoad",       navGlanceEntDoorLoad);
    saveEntBool("GlanceEntDoorRegular",    navGlanceEntDoorRegular);
    saveEntBool("GlanceEntIngrLoose",      navGlanceEntIngrLoose);
    saveEntBool("GlanceEntIngrPlant",      navGlanceEntIngrPlant);

    saveEntBool("InspectEntContChest",      navInspectEntContChest);
    saveEntBool("InspectEntContBarrel",     navInspectEntContBarrel);
    saveEntBool("InspectEntContSack",       navInspectEntContSack);
    saveEntBool("InspectEntContUrn",        navInspectEntContUrn);
    saveEntBool("InspectEntContCoffin",     navInspectEntContCoffin);
    saveEntBool("InspectEntContStrongbox",  navInspectEntContStrongbox);
    saveEntBool("InspectEntLootWeapon",     navInspectEntLootWeapon);
    saveEntBool("InspectEntLootAmmo",       navInspectEntLootAmmo);
    saveEntBool("InspectEntLootArmor",      navInspectEntLootArmor);
    saveEntBool("InspectEntLootGold",       navInspectEntLootGold);
    saveEntBool("InspectEntLootKey",        navInspectEntLootKey);
    saveEntBool("InspectEntLootPotion",     navInspectEntLootPotion);
    saveEntBool("InspectEntLootFood",       navInspectEntLootFood);
    saveEntBool("InspectEntLootDrink",      navInspectEntLootDrink);
    saveEntBool("InspectEntLootBook",       navInspectEntLootBook);
    saveEntBool("InspectEntLootSoulGem",    navInspectEntLootSoulGem);
    saveEntBool("InspectEntLootScroll",     navInspectEntLootScroll);
    saveEntBool("InspectEntCorpseHumanoid", navInspectEntCorpseHumanoid);
    saveEntBool("InspectEntCorpseCreature", navInspectEntCorpseCreature);
    saveEntBool("InspectEntDoorLoad",       navInspectEntDoorLoad);
    saveEntBool("InspectEntDoorRegular",    navInspectEntDoorRegular);
    saveEntBool("InspectEntIngrLoose",      navInspectEntIngrLoose);
    saveEntBool("InspectEntIngrPlant",      navInspectEntIngrPlant);

    ini.SetLongValue("NavigationOverlay", "GlanceEntityIconSize", navGlanceEntityIconSize);
    ini.SetLongValue("NavigationOverlay", "InspectEntityIconSize", navInspectEntityIconSize);
    WriteColor4Hex(ini, "NavigationOverlay", "EntityIconBackground",
                   navEntityIconBgR, navEntityIconBgG, navEntityIconBgB, navEntityIconBgA);
    WriteColor4Hex(ini, "NavigationOverlay", "EntityIconBorder",
                   navEntityIconBorderR, navEntityIconBorderG, navEntityIconBorderB, navEntityIconBorderA);
    ini.SetLongValue("NavigationOverlay", "EntityIconPadding", static_cast<long>(navEntityIconPadding * 100.0f + 0.5f));
    ini.SetLongValue("NavigationOverlay", "EntityIcon3D", navEntityIcon3D ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "EntityIconShadow", navEntityIconShadow ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "GlancePinSize", navGlancePinSize);
    ini.SetLongValue("NavigationOverlay", "InspectPinSize", navInspectPinSize);
    ini.SetLongValue("NavigationOverlay", "GlanceShowTrail", navGlanceShowTrail ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "InspectShowTrail", navInspectShowTrail ? 1L : 0L);

    ini.SetLongValue("NavigationOverlay", "ActorShowAlly", navActorShowAlly ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "ActorShowHostile", navActorShowHostile ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "ActorShowNeutral", navActorShowNeutral ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "ActorShowAnimal", navActorShowAnimal ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "GlanceActorDotSize", navGlanceActorDotSize);
    ini.SetLongValue("NavigationOverlay", "InspectActorDotSize", navInspectActorDotSize);
    ini.SetLongValue("NavigationOverlay", "ActorProximityUnits", static_cast<long>(navActorProximityUnits));

    // Written in METERS (navMarkerRange is units internally).
    ini.SetLongValue("NavigationOverlay", "MarkerRange", static_cast<long>(navMarkerRange / Settings::kUnitsPerMeter));
    ini.SetLongValue("NavigationOverlay", "MaxQuestMarkers", navMaxQuestMarkers);
    ini.SetLongValue("NavigationOverlay", "MaxCustomMarkers", navMaxCustomMarkers);
    ini.SetLongValue("NavigationOverlay", "BeaconVisible", navBeaconVisible ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "BeaconPillarHeight", static_cast<long>(navBeaconPillarHeight));
    ini.SetLongValue("NavigationOverlay", "BeaconNearScale", static_cast<long>(navBeaconNearScale * 100.0f + 0.5f));
    ini.SetLongValue("NavigationOverlay", "BeaconPinSize", navBeaconPinSize);
    ini.SetLongValue("NavigationOverlay", "BeaconFadeNearUnits", static_cast<long>(navBeaconFadeNearUnits));
    ini.SetLongValue("NavigationOverlay", "BeaconClearOnArrival", navBeaconClearOnArrival ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "BeaconEdgeIndicator", navBeaconEdgeIndicator ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamRangeUnits", static_cast<long>(lootBeamRangeUnits));
    ini.SetLongValue("NavigationOverlay", "LootBeamZBandUnits", static_cast<long>(lootBeamZBandUnits));
    ini.SetLongValue("NavigationOverlay", "LootBeamRequireLOS", lootBeamRequireLOS ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamOcclusion", lootBeamOcclusion ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "EntityRequireLOS", navEntityRequireLOS ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamVisible", lootBeamVisible ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamHideInCombat", lootBeamHideInCombat ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamHideAfterLooting", lootBeamHideAfterLooting ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamHideAfterInspecting", lootBeamHideAfterInspecting ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamTrackDroppedWeapons", lootBeamTrackDroppedWeapons ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamDesign", lootBeamDesign);
    ini.SetDoubleValue("NavigationOverlay", "LootBeamModernBottomWidth", lootBeamModernBottomWidth);
    ini.SetDoubleValue("NavigationOverlay", "LootBeamModernTopWidth", lootBeamModernTopWidth);
    ini.SetDoubleValue("NavigationOverlay", "LootBeamModernGroundGlow", lootBeamModernGroundGlow);
    ini.SetDoubleValue("NavigationOverlay", "LootBeamWispBottomWidth", lootBeamWispBottomWidth);
    ini.SetDoubleValue("NavigationOverlay", "LootBeamWispTopWidth", lootBeamWispTopWidth);
    ini.SetDoubleValue("NavigationOverlay", "LootBeamWispGroundGlow", lootBeamWispGroundGlow);
    ini.SetLongValue("NavigationOverlay", "HideInBeastForm", hideInBeastForm ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamHideOwned", lootBeamHideOwned ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamLengthUnits", static_cast<long>(lootBeamLengthUnits));
    ini.SetLongValue("NavigationOverlay", "LootBeamIconSize", lootBeamIconSize);
    ini.SetLongValue("NavigationOverlay", "LootBeamBrightness", static_cast<long>(lootBeamBrightness * 100.0f + 0.5f));
    ini.SetLongValue("NavigationOverlay", "LootBeamShowCorpseHumanoid", lootBeamShowCorpseHumanoid ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowCorpseCreature", lootBeamShowCorpseCreature ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowContChest", lootBeamShowContChest ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowContStrongbox", lootBeamShowContStrongbox ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowContSatchel", lootBeamShowContSatchel ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowLootWeapon", lootBeamShowLootWeapon ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowLootArmor", lootBeamShowLootArmor ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowLootGold", lootBeamShowLootGold ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowLootCoinPurse", lootBeamShowLootCoinPurse ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowLootPotion", lootBeamShowLootPotion ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowLootSoulGem", lootBeamShowLootSoulGem ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowLootScroll", lootBeamShowLootScroll ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamShowLootBook", lootBeamShowLootBook ? 1L : 0L);
    ini.SetLongValue("NavigationOverlay", "LootBeamMinGoldValue", lootBeamMinGoldValue);

    ini.SetLongValue("PathTracking", "Enabled", enablePathTracking ? 1L : 0L);
    ini.SetLongValue("PathTrail", "Visible", pathTrailVisible ? 1L : 0L);
    WriteColor4Hex(ini, "PathTrail", "Color", pathTrailColorR, pathTrailColorG, pathTrailColorB, pathTrailColorA);
    ini.SetLongValue("PathTrail", "Thickness", static_cast<long>(pathTrailThickness * 100.0f + 0.5f));
    ini.SetLongValue("PathTrail", "FadeByAge", pathTrailFadeByAge ? 1L : 0L);
    ini.SetLongValue("PathTrail", "SmartSpacing", pathTrailSmartSpacing ? 1L : 0L);
    ini.SetLongValue("PathTrail", "Pulse", pathPulseEnabled ? 1L : 0L);
    WriteColor3Hex(ini, "PathTrail", "PulseColor", pathPulseColorR, pathPulseColorG, pathPulseColorB);

    ini.SetLongValue("Markers", "IconScale", static_cast<long>(markerIconScale * 100.0f + 0.5f));
    WriteColor4Hex(ini, "Markers", "PinTint", pinTintR, pinTintG, pinTintB, pinTintA);
    ini.SetLongValue("Markers", "IconFx3D", iconFx3D ? 1L : 0L);
    ini.SetLongValue("Markers", "IconShadow", iconShadow ? 1L : 0L);
    ini.SetLongValue("Markers", "EnableBadges", enableBadges ? 1L : 0L);
    ini.SetLongValue("Markers", "FavSize", static_cast<long>(favBadgeSize * 100.0f + 0.5f));
    WriteColor4Hex(ini, "Markers", "FavColor", favBadgeColorR, favBadgeColorG, favBadgeColorB, favBadgeColorA);
    ini.SetLongValue("Markers", "NoteSize", static_cast<long>(noteBadgeSize * 100.0f + 0.5f));
    WriteColor4Hex(ini, "Markers", "NoteColor", noteBadgeColorR, noteBadgeColorG, noteBadgeColorB, noteBadgeColorA);
    ini.SetLongValue("Markers", "ClearedSize", static_cast<long>(clearedBadgeSize * 100.0f + 0.5f));
    WriteColor4Hex(ini, "Markers", "ClearedColor", clearedBadgeColorR, clearedBadgeColorG, clearedBadgeColorB, clearedBadgeColorA);
    ini.SetLongValue("Markers", "ImageEnabled", markerImageEnabled ? 1L : 0L);
    ini.SetLongValue("Markers", "ImageMinWidth", markerImageMinWidth);
    ini.Delete("Markers", "ImageWidth");
    ini.SetLongValue("Markers", "ImageAlpha", static_cast<long>(markerImageAlpha * 100.0f + 0.5f));
    ini.SetLongValue("Markers", "HoverOffsetX", markerHoverOffsetX);
    ini.SetLongValue("Markers", "HoverOffsetY", markerHoverOffsetY);

    WriteColor3Hex(ini, "Appearance", "TitleColor", titleColorR, titleColorG, titleColorB);
    WriteColor3Hex(ini, "Appearance", "BodyColor", bodyColorR, bodyColorG, bodyColorB);
    WriteColor4Hex(ini, "Appearance", "BgColor", bgColorR, bgColorG, bgColorB, bgColorA);
    WriteColor4Hex(ini, "Appearance", "BorderColor", borderColorR, borderColorG, borderColorB, borderColorA);

    ini.SetLongValue("Appearance", "BgShadow", bgShadow ? 1L : 0L);
    ini.SetLongValue("Appearance", "BgGradient", bgGradient ? 1L : 0L);
    ini.SetLongValue("Appearance", "BgVignette", bgVignette ? 1L : 0L);
    ini.SetLongValue("Appearance", "BgHighlight", bgHighlight ? 1L : 0L);
    ini.SetLongValue("Appearance", "BgBorder", bgBorder ? 1L : 0L);

    if (!EnsureIniDirectory(path)) {
        return;
    }
    if (const SI_Error rc = ini.SaveFile(path.c_str()); rc < SI_OK) {
        logger::error("Failed to save CustomMarkers.ini to '{}': SimpleIni error {}",
                      path.string(), rc);
    }
}

void Settings::SavePositions() {
    const auto path = GetIniPath();

    CSimpleIniA ini;
    ini.SetUnicode();
    if (!fs::exists(path)) {
        Save();
        return;
    }
    if (const SI_Error rc = ini.LoadFile(path.c_str()); rc < SI_OK) {
        logger::error("Refusing to overwrite unreadable CustomMarkers.ini at '{}': SimpleIni error {}",
                      path.string(), rc);
        return;
    }

    ini.SetLongValue("Menu", "ActiveTab", menuActiveTab);
    ini.SetLongValue("Menu", "PosX", menuPosX);
    ini.SetLongValue("Menu", "PosY", menuPosY);
    ini.SetLongValue("ListPanel", "PosX", listPanelPosX);
    ini.SetLongValue("ListPanel", "PosY", listPanelPosY);
    ini.SetLongValue("ListPanel", "ActiveTab", listActiveTab);
    ini.SetLongValue("ListPanel", "Visible", listPanelVisible ? 1L : 0L);
    ini.SetLongValue("Filters", "PosX", filterPosX);
    ini.SetLongValue("Filters", "PosY", filterPosY);
    ini.SetLongValue("Filters", "Collapsed", filterCollapsed ? 1L : 0L);

    if (!EnsureIniDirectory(path)) {
        return;
    }
    if (const SI_Error rc = ini.SaveFile(path.c_str()); rc < SI_OK) {
        logger::error("Failed to save CustomMarkers.ini positions to '{}': SimpleIni error {}",
                      path.string(), rc);
    }
}
