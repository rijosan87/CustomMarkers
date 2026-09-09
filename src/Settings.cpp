// Reads and writes configuration settings from INI.

#include "Settings.h"

#include "InputHandler.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>

static std::wstring X100(float v) {
    return std::to_wstring(static_cast<int>(v * 100.0f + 0.5f));
}

static const wchar_t* GetIniPath() {
    static constexpr auto kPrimary = L"Data/SKSE/Plugins/CustomMarkers.ini";
    static constexpr auto kLegacy = L"Data/SKSE/Plugins/CustomMapMarkers.ini";
    if (std::filesystem::exists(kPrimary)) {
        return kPrimary;
    }
    if (std::filesystem::exists(kLegacy)) {
        logger::info("Found legacy CustomMapMarkers.ini, loading from legacy path");
        return kLegacy;
    }
    return kPrimary;
}

namespace {
    void ReadColor3Hex(const wchar_t* a_section, const wchar_t* a_key,
                       const wchar_t* a_legacyPrefix,
                       std::int32_t& a_r, std::int32_t& a_g, std::int32_t& a_b,
                       const wchar_t* a_path) {
        wchar_t buffer[256];
        GetPrivateProfileStringW(a_section, a_key, L"\0", buffer, 256, a_path);
        if (buffer[0] != L'\0') {
            const unsigned long rgb = std::wcstoul(buffer, nullptr, 16);
            a_r = static_cast<std::int32_t>((rgb >> 16) & 0xFF);
            a_g = static_cast<std::int32_t>((rgb >> 8)  & 0xFF);
            a_b = static_cast<std::int32_t>( rgb        & 0xFF);
            return;
        }
        if (a_legacyPrefix) {
            std::wstring keyR = std::wstring(a_legacyPrefix) + L"R";
            std::wstring keyG = std::wstring(a_legacyPrefix) + L"G";
            std::wstring keyB = std::wstring(a_legacyPrefix) + L"B";
            GetPrivateProfileStringW(a_section, keyR.c_str(), std::to_wstring(a_r).c_str(), buffer, 256, a_path);
            a_r = std::clamp(_wtoi(buffer), 0, 255);
            GetPrivateProfileStringW(a_section, keyG.c_str(), std::to_wstring(a_g).c_str(), buffer, 256, a_path);
            a_g = std::clamp(_wtoi(buffer), 0, 255);
            GetPrivateProfileStringW(a_section, keyB.c_str(), std::to_wstring(a_b).c_str(), buffer, 256, a_path);
            a_b = std::clamp(_wtoi(buffer), 0, 255);
        }
    }

    void ReadColor4Hex(const wchar_t* a_section, const wchar_t* a_key,
                       const wchar_t* a_legacyPrefix,
                       std::int32_t& a_r, std::int32_t& a_g, std::int32_t& a_b, std::int32_t& a_a,
                       const wchar_t* a_path) {
        wchar_t buffer[256];
        GetPrivateProfileStringW(a_section, a_key, L"\0", buffer, 256, a_path);
        if (buffer[0] != L'\0') {
            const unsigned long argb = std::wcstoul(buffer, nullptr, 16);
            a_a = static_cast<std::int32_t>((argb >> 24) & 0xFF);
            a_r = static_cast<std::int32_t>((argb >> 16) & 0xFF);
            a_g = static_cast<std::int32_t>((argb >> 8)  & 0xFF);
            a_b = static_cast<std::int32_t>( argb        & 0xFF);
            return;
        }
        if (a_legacyPrefix) {
            std::wstring keyR = std::wstring(a_legacyPrefix) + L"R";
            std::wstring keyG = std::wstring(a_legacyPrefix) + L"G";
            std::wstring keyB = std::wstring(a_legacyPrefix) + L"B";
            std::wstring keyA = std::wstring(a_legacyPrefix) + L"A";
            GetPrivateProfileStringW(a_section, keyR.c_str(), std::to_wstring(a_r).c_str(), buffer, 256, a_path);
            a_r = std::clamp(_wtoi(buffer), 0, 255);
            GetPrivateProfileStringW(a_section, keyG.c_str(), std::to_wstring(a_g).c_str(), buffer, 256, a_path);
            a_g = std::clamp(_wtoi(buffer), 0, 255);
            GetPrivateProfileStringW(a_section, keyB.c_str(), std::to_wstring(a_b).c_str(), buffer, 256, a_path);
            a_b = std::clamp(_wtoi(buffer), 0, 255);
            GetPrivateProfileStringW(a_section, keyA.c_str(), std::to_wstring(a_a).c_str(), buffer, 256, a_path);
            a_a = std::clamp(_wtoi(buffer), 0, 255);
        }
    }

    void WriteColor3Hex(const wchar_t* a_section, const wchar_t* a_key,
                        const wchar_t* a_legacyPrefix,
                        std::int32_t a_r, std::int32_t a_g, std::int32_t a_b,
                        const wchar_t* a_path) {
        const unsigned long rgb =
            (static_cast<unsigned long>(a_r & 0xFF) << 16) |
            (static_cast<unsigned long>(a_g & 0xFF) << 8)  |
            (static_cast<unsigned long>(a_b & 0xFF));
        wchar_t hex[16];
        swprintf_s(hex, L"%06lX", rgb);
        WritePrivateProfileStringW(a_section, a_key, hex, a_path);
        if (a_legacyPrefix) {
            std::wstring keyR = std::wstring(a_legacyPrefix) + L"R";
            std::wstring keyG = std::wstring(a_legacyPrefix) + L"G";
            std::wstring keyB = std::wstring(a_legacyPrefix) + L"B";
            WritePrivateProfileStringW(a_section, keyR.c_str(), nullptr, a_path);
            WritePrivateProfileStringW(a_section, keyG.c_str(), nullptr, a_path);
            WritePrivateProfileStringW(a_section, keyB.c_str(), nullptr, a_path);
        }
    }

    void WriteColor4Hex(const wchar_t* a_section, const wchar_t* a_key,
                        const wchar_t* a_legacyPrefix,
                        std::int32_t a_r, std::int32_t a_g, std::int32_t a_b, std::int32_t a_a,
                        const wchar_t* a_path) {
        const unsigned long argb =
            (static_cast<unsigned long>(a_a & 0xFF) << 24) |
            (static_cast<unsigned long>(a_r & 0xFF) << 16) |
            (static_cast<unsigned long>(a_g & 0xFF) << 8)  |
            (static_cast<unsigned long>(a_b & 0xFF));
        wchar_t hex[16];
        swprintf_s(hex, L"%08lX", argb);
        WritePrivateProfileStringW(a_section, a_key, hex, a_path);
        if (a_legacyPrefix) {
            std::wstring keyR = std::wstring(a_legacyPrefix) + L"R";
            std::wstring keyG = std::wstring(a_legacyPrefix) + L"G";
            std::wstring keyB = std::wstring(a_legacyPrefix) + L"B";
            std::wstring keyA = std::wstring(a_legacyPrefix) + L"A";
            WritePrivateProfileStringW(a_section, keyR.c_str(), nullptr, a_path);
            WritePrivateProfileStringW(a_section, keyG.c_str(), nullptr, a_path);
            WritePrivateProfileStringW(a_section, keyB.c_str(), nullptr, a_path);
            WritePrivateProfileStringW(a_section, keyA.c_str(), nullptr, a_path);
        }
    }
}

void Settings::Load() {
    const auto path = GetIniPath();

    wchar_t buffer[256];

    GetPrivateProfileStringW(L"Hotkey", L"ListPanelKey", std::to_wstring(listPanelKey).c_str(), buffer, 256, path);
    listPanelKey = _wtoi(buffer);
    GetPrivateProfileStringW(L"Hotkey", L"ListPanelModifier", std::to_wstring(listPanelModifier).c_str(), buffer, 256, path);
    listPanelModifier = _wtoi(buffer);

    GetPrivateProfileStringW(L"Hotkey", L"PathTrackKey", std::to_wstring(pathTrackKey).c_str(), buffer, 256, path);
    pathTrackKey = _wtoi(buffer);
    GetPrivateProfileStringW(L"Hotkey", L"PathTrackModifier", std::to_wstring(pathTrackModifier).c_str(), buffer, 256, path);
    pathTrackModifier = _wtoi(buffer);

    wchar_t langDefault[256];
    MultiByteToWideChar(CP_UTF8, 0, language.c_str(), -1, langDefault, 256);
    GetPrivateProfileStringW(L"Localization", L"Language", langDefault, buffer, 256, path);
    char languageBuf[256];
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, languageBuf, 256, nullptr, nullptr);
    language = std::string(languageBuf);

    GetPrivateProfileStringW(L"Localization", L"FontSize", std::to_wstring(fontSize).c_str(), buffer, 256, path);
    fontSize = std::clamp(_wtoi(buffer), 10, 48);

    GetPrivateProfileStringW(L"Localization", L"HeadingFontSize", std::to_wstring(headingFontSize).c_str(), buffer, 256, path);
    headingFontSize = std::clamp(_wtoi(buffer), 0, 72);

    wchar_t levelDefault[64];
    MultiByteToWideChar(CP_UTF8, 0, logLevel.c_str(), -1, levelDefault, 64);
    GetPrivateProfileStringW(L"Logging", L"Level", levelDefault, buffer, 256, path);
    char levelBuf[64];
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, levelBuf, 64, nullptr, nullptr);
    logLevel = std::string(levelBuf);

    GetPrivateProfileStringW(L"Menu", L"SettingsKey", std::to_wstring(menuSettingsKey).c_str(), buffer, 256, path);
    menuSettingsKey = _wtoi(buffer);

    if (menuSettingsKey < 1) {
        menuSettingsKey = 0x2D;
    }
    GetPrivateProfileStringW(L"Menu", L"SettingsModifier", std::to_wstring(menuSettingsModifier).c_str(), buffer, 256, path);
    menuSettingsModifier = _wtoi(buffer);

    GetPrivateProfileStringW(L"Menu", L"ActiveTab", std::to_wstring(menuActiveTab).c_str(), buffer, 256, path);
    menuActiveTab = _wtoi(buffer);

    GetPrivateProfileStringW(L"Menu", L"PosX", std::to_wstring(menuPosX).c_str(), buffer, 256, path);
    menuPosX = _wtoi(buffer);

    GetPrivateProfileStringW(L"Menu", L"PosY", std::to_wstring(menuPosY).c_str(), buffer, 256, path);
    menuPosY = _wtoi(buffer);

    GetPrivateProfileStringW(L"Menu", L"Width", std::to_wstring(menuWidth).c_str(), buffer, 256, path);
    menuWidth = _wtoi(buffer);

    GetPrivateProfileStringW(L"Menu", L"Height", std::to_wstring(menuHeight).c_str(), buffer, 256, path);
    menuHeight = _wtoi(buffer);

    GetPrivateProfileStringW(L"ListPanel", L"Width", std::to_wstring(listPanelWidth).c_str(), buffer, 256, path);
    listPanelWidth = _wtoi(buffer);

    GetPrivateProfileStringW(L"Appearance", L"TitleScale", X100(titleScale).c_str(), buffer, 256, path);
    titleScale = std::clamp(_wtoi(buffer) / 100.0f, 0.5f, 3.0f);

    GetPrivateProfileStringW(L"Appearance", L"BodyScale", X100(bodyScale).c_str(), buffer, 256, path);
    bodyScale = std::clamp(_wtoi(buffer) / 100.0f, 0.5f, 3.0f);

    GetPrivateProfileStringW(L"ListPanel", L"PosX", std::to_wstring(listPanelPosX).c_str(), buffer, 256, path);
    listPanelPosX = _wtoi(buffer);

    GetPrivateProfileStringW(L"ListPanel", L"PosY", std::to_wstring(listPanelPosY).c_str(), buffer, 256, path);
    listPanelPosY = _wtoi(buffer);

    GetPrivateProfileStringW(L"ListPanel", L"ShowOverview", std::to_wstring(listShowOverview ? 1 : 0).c_str(), buffer, 256, path);
    listShowOverview = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"ListPanel", L"ShowActiveQuests", std::to_wstring(listShowActiveQuests ? 1 : 0).c_str(), buffer, 256, path);
    listShowActiveQuests = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"ListPanel", L"ActiveTab", std::to_wstring(listActiveTab).c_str(), buffer, 256, path);
    listActiveTab = std::clamp(_wtoi(buffer), 0, 3);

    GetPrivateProfileStringW(L"ListPanel", L"MinHeightPct", std::to_wstring(listPanelMinHeightPct).c_str(), buffer, 256, path);
    listPanelMinHeightPct = std::clamp(_wtoi(buffer), 0, 95);

    GetPrivateProfileStringW(L"ListPanel", L"MaxHeightPct", std::to_wstring(listPanelMaxHeightPct).c_str(), buffer, 256, path);
    listPanelMaxHeightPct = std::clamp(_wtoi(buffer), 30, 95);

    GetPrivateProfileStringW(L"Filters", L"Show", std::to_wstring(showFilterPanel ? 1 : 0).c_str(), buffer, 256, path);
    showFilterPanel = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"Filters", L"PosX", std::to_wstring(filterPosX).c_str(), buffer, 256, path);
    filterPosX = _wtoi(buffer);

    GetPrivateProfileStringW(L"Filters", L"PosY", std::to_wstring(filterPosY).c_str(), buffer, 256, path);
    filterPosY = _wtoi(buffer);

    GetPrivateProfileStringW(L"Filters", L"Collapsed", std::to_wstring(filterCollapsed ? 1 : 0).c_str(), buffer, 256, path);
    filterCollapsed = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"ListPanel", L"UseCustomBg", std::to_wstring(listPanelUseCustomBg ? 1 : 0).c_str(), buffer, 256, path);
    listPanelUseCustomBg = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"ListPanel", L"BgIndex", std::to_wstring(listPanelBgIndex).c_str(), buffer, 256, path);
    listPanelBgIndex = std::max(0, _wtoi(buffer));

    GetPrivateProfileStringW(L"StatCard", L"NoteScale", X100(noteTextScale).c_str(), buffer, 256, path);
    noteTextScale = std::clamp(_wtoi(buffer) / 100.0f, 0.5f, 3.0f);

    GetPrivateProfileStringW(L"StatCard", L"Anchor", std::to_wstring(statCardAnchor).c_str(), buffer, 256, path);
    statCardAnchor = std::clamp(_wtoi(buffer), 0, 3);
    GetPrivateProfileStringW(L"StatCard", L"OffsetX", std::to_wstring(statCardOffsetX).c_str(), buffer, 256, path);
    statCardOffsetX = _wtoi(buffer);
    GetPrivateProfileStringW(L"StatCard", L"OffsetY", std::to_wstring(statCardOffsetY).c_str(), buffer, 256, path);
    statCardOffsetY = _wtoi(buffer);

    GetPrivateProfileStringW(L"NavigationOverlay", L"GlanceInteriorVisible", std::to_wstring(navGlanceInteriorVisible ? 1 : 0).c_str(), buffer, 256, path);
    navGlanceInteriorVisible = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"GlanceExteriorVisible", std::to_wstring(navGlanceExteriorVisible ? 1 : 0).c_str(), buffer, 256, path);
    navGlanceExteriorVisible = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectInteriorVisible", std::to_wstring(navInspectInteriorVisible ? 1 : 0).c_str(), buffer, 256, path);
    navInspectInteriorVisible = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectExteriorVisible", std::to_wstring(navInspectExteriorVisible ? 1 : 0).c_str(), buffer, 256, path);
    navInspectExteriorVisible = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectKeyDIK", std::to_wstring(navInspectKey).c_str(), buffer, 256, path);
    navInspectKey = _wtoi(buffer);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectModifier", std::to_wstring(navInspectModifier).c_str(), buffer, 256, path);
    navInspectModifier = _wtoi(buffer);

    GetPrivateProfileStringW(L"NavigationOverlay", L"GlancePosX", std::to_wstring(navGlancePosX).c_str(), buffer, 256, path);
    if (int v = _wtoi(buffer); v >= 0) navGlancePosX = v;
    GetPrivateProfileStringW(L"NavigationOverlay", L"GlancePosY", std::to_wstring(navGlancePosY).c_str(), buffer, 256, path);
    if (int v = _wtoi(buffer); v >= 0) navGlancePosY = v;
    GetPrivateProfileStringW(L"NavigationOverlay", L"GlanceSize", std::to_wstring(navGlanceSize).c_str(), buffer, 256, path);

    navGlanceSize = std::clamp(_wtoi(buffer), 100, 300);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectSizePct", std::to_wstring(navInspectSizePct).c_str(), buffer, 256, path);
    navInspectSizePct = _wtoi(buffer);
    GetPrivateProfileStringW(L"NavigationOverlay", L"RefreshOnInspect", std::to_wstring(navRefreshOnInspect ? 1 : 0).c_str(), buffer, 256, path);
    navRefreshOnInspect = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectModeToggleKeyDIK", std::to_wstring(navInspectModeToggleKey).c_str(), buffer, 256, path);
    navInspectModeToggleKey = _wtoi(buffer);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectModeToggleModifier", std::to_wstring(navInspectModeToggleModifier).c_str(), buffer, 256, path);
    navInspectModeToggleModifier = _wtoi(buffer);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectDefaultMode", std::to_wstring(navInspectDefaultMode).c_str(), buffer, 256, path);
    navInspectDefaultMode = std::clamp(_wtoi(buffer), 0, 1);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectRadarRangeUnits", std::to_wstring(static_cast<int>(navInspectRadarRangeUnits)).c_str(), buffer, 256, path);
    navInspectRadarRangeUnits = std::max(static_cast<float>(_wtoi(buffer)), 100.0f);
    GetPrivateProfileStringW(L"NavigationOverlay", L"GlanceRangeUnits", std::to_wstring(static_cast<int>(navGlanceRangeUnits)).c_str(), buffer, 256, path);
    navGlanceRangeUnits = std::max(static_cast<float>(_wtoi(buffer)), 200.0f);
    GetPrivateProfileStringW(L"NavigationOverlay", L"QuickMarkKeyDIK", std::to_wstring(navQuickMarkKey).c_str(), buffer, 256, path);
    navQuickMarkKey = _wtoi(buffer);
    GetPrivateProfileStringW(L"NavigationOverlay", L"QuickMarkModifier", std::to_wstring(navQuickMarkModifier).c_str(), buffer, 256, path);
    navQuickMarkModifier = _wtoi(buffer);

    GetPrivateProfileStringW(L"LookPin", L"Enabled", std::to_wstring(lookPinEnabled ? 1 : 0).c_str(), buffer, 256, path);
    lookPinEnabled = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"LookPin", L"KeyDIK", std::to_wstring(lookPinKey).c_str(), buffer, 256, path);
    lookPinKey = _wtoi(buffer);
    GetPrivateProfileStringW(L"LookPin", L"Modifier", std::to_wstring(lookPinModifier).c_str(), buffer, 256, path);
    lookPinModifier = _wtoi(buffer);

    auto loadEntBool = [&](const wchar_t* a_key, bool& a_field) {
        GetPrivateProfileStringW(L"NavigationOverlay", a_key, std::to_wstring(a_field ? 1 : 0).c_str(), buffer, 256, path);
        a_field = (_wtoi(buffer) != 0);
    };
    loadEntBool(L"GlanceEntContChest",      navGlanceEntContChest);
    loadEntBool(L"GlanceEntContBarrel",     navGlanceEntContBarrel);
    loadEntBool(L"GlanceEntContSack",       navGlanceEntContSack);
    loadEntBool(L"GlanceEntContUrn",        navGlanceEntContUrn);
    loadEntBool(L"GlanceEntContCoffin",     navGlanceEntContCoffin);
    loadEntBool(L"GlanceEntContStrongbox",  navGlanceEntContStrongbox);
    loadEntBool(L"GlanceEntLootWeapon",     navGlanceEntLootWeapon);
    loadEntBool(L"GlanceEntLootAmmo",       navGlanceEntLootAmmo);
    loadEntBool(L"GlanceEntLootArmor",      navGlanceEntLootArmor);
    loadEntBool(L"GlanceEntLootGold",       navGlanceEntLootGold);
    loadEntBool(L"GlanceEntLootKey",        navGlanceEntLootKey);
    loadEntBool(L"GlanceEntLootPotion",     navGlanceEntLootPotion);
    loadEntBool(L"GlanceEntLootFood",       navGlanceEntLootFood);
    loadEntBool(L"GlanceEntLootDrink",      navGlanceEntLootDrink);
    loadEntBool(L"GlanceEntLootBook",       navGlanceEntLootBook);
    loadEntBool(L"GlanceEntLootSoulGem",    navGlanceEntLootSoulGem);
    loadEntBool(L"GlanceEntLootScroll",     navGlanceEntLootScroll);
    loadEntBool(L"GlanceEntCorpseHumanoid", navGlanceEntCorpseHumanoid);
    loadEntBool(L"GlanceEntCorpseCreature", navGlanceEntCorpseCreature);
    loadEntBool(L"GlanceEntDoorLoad",       navGlanceEntDoorLoad);
    loadEntBool(L"GlanceEntDoorRegular",    navGlanceEntDoorRegular);
    loadEntBool(L"GlanceEntIngrLoose",      navGlanceEntIngrLoose);
    loadEntBool(L"GlanceEntIngrPlant",      navGlanceEntIngrPlant);

    loadEntBool(L"InspectEntContChest",      navInspectEntContChest);
    loadEntBool(L"InspectEntContBarrel",     navInspectEntContBarrel);
    loadEntBool(L"InspectEntContSack",       navInspectEntContSack);
    loadEntBool(L"InspectEntContUrn",        navInspectEntContUrn);
    loadEntBool(L"InspectEntContCoffin",     navInspectEntContCoffin);
    loadEntBool(L"InspectEntContStrongbox",  navInspectEntContStrongbox);
    loadEntBool(L"InspectEntLootWeapon",     navInspectEntLootWeapon);
    loadEntBool(L"InspectEntLootAmmo",       navInspectEntLootAmmo);
    loadEntBool(L"InspectEntLootArmor",      navInspectEntLootArmor);
    loadEntBool(L"InspectEntLootGold",       navInspectEntLootGold);
    loadEntBool(L"InspectEntLootKey",        navInspectEntLootKey);
    loadEntBool(L"InspectEntLootPotion",     navInspectEntLootPotion);
    loadEntBool(L"InspectEntLootFood",       navInspectEntLootFood);
    loadEntBool(L"InspectEntLootDrink",      navInspectEntLootDrink);
    loadEntBool(L"InspectEntLootBook",       navInspectEntLootBook);
    loadEntBool(L"InspectEntLootSoulGem",    navInspectEntLootSoulGem);
    loadEntBool(L"InspectEntLootScroll",     navInspectEntLootScroll);
    loadEntBool(L"InspectEntCorpseHumanoid", navInspectEntCorpseHumanoid);
    loadEntBool(L"InspectEntCorpseCreature", navInspectEntCorpseCreature);
    loadEntBool(L"InspectEntDoorLoad",       navInspectEntDoorLoad);
    loadEntBool(L"InspectEntDoorRegular",    navInspectEntDoorRegular);
    loadEntBool(L"InspectEntIngrLoose",      navInspectEntIngrLoose);
    loadEntBool(L"InspectEntIngrPlant",      navInspectEntIngrPlant);

    GetPrivateProfileStringW(L"NavigationOverlay", L"GlanceEntityIconSize", std::to_wstring(navGlanceEntityIconSize).c_str(), buffer, 256, path);
    navGlanceEntityIconSize = std::clamp(_wtoi(buffer), 4, 16);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectEntityIconSize", std::to_wstring(navInspectEntityIconSize).c_str(), buffer, 256, path);
    navInspectEntityIconSize = std::clamp(_wtoi(buffer), 10, 40);

    GetPrivateProfileStringW(L"NavigationOverlay", L"GlancePinSize", std::to_wstring(navGlancePinSize).c_str(), buffer, 256, path);
    navGlancePinSize = std::clamp(_wtoi(buffer), 6, 24);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectPinSize", std::to_wstring(navInspectPinSize).c_str(), buffer, 256, path);
    navInspectPinSize = std::clamp(_wtoi(buffer), 12, 48);

    GetPrivateProfileStringW(L"NavigationOverlay", L"GlanceShowTrail", std::to_wstring(navGlanceShowTrail ? 1 : 0).c_str(), buffer, 256, path);
    navGlanceShowTrail = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectShowTrail", std::to_wstring(navInspectShowTrail ? 1 : 0).c_str(), buffer, 256, path);
    navInspectShowTrail = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"NavigationOverlay", L"ActorShowAlly", std::to_wstring(navActorShowAlly ? 1 : 0).c_str(), buffer, 256, path);
    navActorShowAlly = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"ActorShowHostile", std::to_wstring(navActorShowHostile ? 1 : 0).c_str(), buffer, 256, path);
    navActorShowHostile = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"ActorShowNeutral", std::to_wstring(navActorShowNeutral ? 1 : 0).c_str(), buffer, 256, path);
    navActorShowNeutral = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"ActorShowAnimal", std::to_wstring(navActorShowAnimal ? 1 : 0).c_str(), buffer, 256, path);
    navActorShowAnimal = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"GlanceActorDotSize", std::to_wstring(navGlanceActorDotSize).c_str(), buffer, 256, path);
    navGlanceActorDotSize = std::clamp(_wtoi(buffer), 2, 16);
    GetPrivateProfileStringW(L"NavigationOverlay", L"InspectActorDotSize", std::to_wstring(navInspectActorDotSize).c_str(), buffer, 256, path);
    navInspectActorDotSize = std::clamp(_wtoi(buffer), 3, 24);
    GetPrivateProfileStringW(L"NavigationOverlay", L"ActorProximityUnits", std::to_wstring(static_cast<int>(navActorProximityUnits)).c_str(), buffer, 256, path);
    navActorProximityUnits = std::clamp(static_cast<float>(_wtoi(buffer)), 200.0f, 4000.0f);

    {
        const int defM = static_cast<int>(navMarkerRange / Settings::kUnitsPerMeter);
        GetPrivateProfileStringW(L"NavigationOverlay", L"MarkerRange", std::to_wstring(defM).c_str(), buffer, 256, path);
        const float meters = std::clamp(static_cast<float>(_wtoi(buffer)), 100.0f, 5000.0f);
        navMarkerRange = meters * Settings::kUnitsPerMeter;
    }
    GetPrivateProfileStringW(L"NavigationOverlay", L"MaxQuestMarkers", std::to_wstring(navMaxQuestMarkers).c_str(), buffer, 256, path);
    navMaxQuestMarkers = std::clamp(_wtoi(buffer), 0, 10);
    GetPrivateProfileStringW(L"NavigationOverlay", L"MaxCustomMarkers", std::to_wstring(navMaxCustomMarkers).c_str(), buffer, 256, path);
    navMaxCustomMarkers = std::clamp(_wtoi(buffer), 0, 30);

    GetPrivateProfileStringW(L"NavigationOverlay", L"BeaconVisible", std::to_wstring(navBeaconVisible ? 1 : 0).c_str(), buffer, 256, path);
    navBeaconVisible = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"BeaconPillarHeight", std::to_wstring(static_cast<int>(navBeaconPillarHeight)).c_str(), buffer, 256, path);
    navBeaconPillarHeight = std::clamp(static_cast<float>(_wtoi(buffer)), 50.0f, 2000.0f);
    GetPrivateProfileStringW(L"NavigationOverlay", L"BeaconNearScale", X100(navBeaconNearScale).c_str(), buffer, 256, path);
    navBeaconNearScale = std::clamp(_wtoi(buffer) / 100.0f, 0.05f, 1.0f);
    GetPrivateProfileStringW(L"NavigationOverlay", L"BeaconPinSize", std::to_wstring(navBeaconPinSize).c_str(), buffer, 256, path);
    navBeaconPinSize = std::clamp(_wtoi(buffer), 12, 48);
    GetPrivateProfileStringW(L"NavigationOverlay", L"BeaconFadeNearUnits", std::to_wstring(static_cast<int>(navBeaconFadeNearUnits)).c_str(), buffer, 256, path);
    navBeaconFadeNearUnits = std::clamp(static_cast<float>(_wtoi(buffer)), 50.0f, 2000.0f);
    GetPrivateProfileStringW(L"NavigationOverlay", L"BeaconEdgeIndicator", std::to_wstring(navBeaconEdgeIndicator ? 1 : 0).c_str(), buffer, 256, path);
    navBeaconEdgeIndicator = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamRangeUnits", std::to_wstring(static_cast<int>(lootBeamRangeUnits)).c_str(), buffer, 256, path);
    lootBeamRangeUnits = std::clamp(static_cast<float>(_wtoi(buffer)), 200.0f, 6000.0f);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamZBandUnits", std::to_wstring(static_cast<int>(lootBeamZBandUnits)).c_str(), buffer, 256, path);
    lootBeamZBandUnits = std::clamp(static_cast<float>(_wtoi(buffer)), 50.0f, 2000.0f);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamRequireLOS", std::to_wstring(lootBeamRequireLOS ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamRequireLOS = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamOcclusion", std::to_wstring(lootBeamOcclusion ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamOcclusion = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"EntityRequireLOS", std::to_wstring(navEntityRequireLOS ? 1 : 0).c_str(), buffer, 256, path);
    navEntityRequireLOS = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamVisible", std::to_wstring(lootBeamVisible ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamVisible = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamHideInCombat", std::to_wstring(lootBeamHideInCombat ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamHideInCombat = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"NavigationOverlay", L"HideInBeastForm", std::to_wstring(hideInBeastForm ? 1 : 0).c_str(), buffer, 256, path);
    hideInBeastForm = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamHideOwned", std::to_wstring(lootBeamHideOwned ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamHideOwned = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamLengthUnits", std::to_wstring(static_cast<int>(lootBeamLengthUnits)).c_str(), buffer, 256, path);
    lootBeamLengthUnits = std::clamp(static_cast<float>(_wtoi(buffer)), 20.0f, 600.0f);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamIconSize", std::to_wstring(lootBeamIconSize).c_str(), buffer, 256, path);
    lootBeamIconSize = std::clamp(_wtoi(buffer), 10, 48);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamBrightness", std::to_wstring(static_cast<int>(lootBeamBrightness * 100.0f + 0.5f)).c_str(), buffer, 256, path);
    lootBeamBrightness = std::clamp(static_cast<float>(_wtoi(buffer)) / 100.0f, 0.10f, 1.0f);

    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowCorpseHumanoid", std::to_wstring(lootBeamShowCorpseHumanoid ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowCorpseHumanoid = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowCorpseCreature", std::to_wstring(lootBeamShowCorpseCreature ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowCorpseCreature = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowContChest", std::to_wstring(lootBeamShowContChest ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowContChest = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowContStrongbox", std::to_wstring(lootBeamShowContStrongbox ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowContStrongbox = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowContSatchel", std::to_wstring(lootBeamShowContSatchel ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowContSatchel = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootWeapon", std::to_wstring(lootBeamShowLootWeapon ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowLootWeapon = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootArmor", std::to_wstring(lootBeamShowLootArmor ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowLootArmor = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootGold", std::to_wstring(lootBeamShowLootGold ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowLootGold = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootPotion", std::to_wstring(lootBeamShowLootPotion ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowLootPotion = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootSoulGem", std::to_wstring(lootBeamShowLootSoulGem ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowLootSoulGem = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootScroll", std::to_wstring(lootBeamShowLootScroll ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamShowLootScroll = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"NavigationOverlay", L"LootBeamMinGoldValue", std::to_wstring(lootBeamMinGoldValue).c_str(), buffer, 256, path);
    lootBeamMinGoldValue = std::clamp(_wtoi(buffer), 0, 5000);

    GetPrivateProfileStringW(L"Logging", L"BeaconVerboseLog", std::to_wstring(beaconVerboseLog ? 1 : 0).c_str(), buffer, 256, path);
    beaconVerboseLog = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"Logging", L"LootBeamPerfLog", std::to_wstring(lootBeamPerfLog ? 1 : 0).c_str(), buffer, 256, path);
    lootBeamPerfLog = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"Logging", L"QuestRowLog", std::to_wstring(questRowLog ? 1 : 0).c_str(), buffer, 256, path);
    questRowLog = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"Logging", L"MapStatsLog", std::to_wstring(mapStatsLog ? 1 : 0).c_str(), buffer, 256, path);
    mapStatsLog = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"Logging", L"MapPinProjectionLog", std::to_wstring(mapPinProjectionLog ? 1 : 0).c_str(), buffer, 256, path);
    mapPinProjectionLog = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"PathTrail", L"Visible", std::to_wstring(pathTrailVisible ? 1 : 0).c_str(), buffer, 256, path);
    pathTrailVisible = (_wtoi(buffer) != 0);

    ReadColor4Hex(L"PathTrail", L"Color", L"Color", pathTrailColorR, pathTrailColorG, pathTrailColorB, pathTrailColorA, path);

    GetPrivateProfileStringW(L"PathTrail", L"Thickness", X100(pathTrailThickness).c_str(), buffer, 256, path);
    pathTrailThickness = std::clamp(_wtoi(buffer) / 100.0f, 1.0f, 8.0f);

    GetPrivateProfileStringW(L"PathTrail", L"FadeByAge", std::to_wstring(pathTrailFadeByAge ? 1 : 0).c_str(), buffer, 256, path);
    pathTrailFadeByAge = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"PathTrail", L"SmartSpacing", std::to_wstring(pathTrailSmartSpacing ? 1 : 0).c_str(), buffer, 256, path);
    pathTrailSmartSpacing = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"PathTrail", L"Pulse", std::to_wstring(pathPulseEnabled ? 1 : 0).c_str(), buffer, 256, path);
    pathPulseEnabled = (_wtoi(buffer) != 0);

    ReadColor3Hex(L"PathTrail", L"PulseColor", L"PulseColor", pathPulseColorR, pathPulseColorG, pathPulseColorB, path);

    GetPrivateProfileStringW(L"Markers", L"IconScale", X100(markerIconScale).c_str(), buffer, 256, path);
    markerIconScale = std::clamp(_wtoi(buffer) / 100.0f, 0.5f, 2.0f);

    ReadColor4Hex(L"Markers", L"PinTint", L"PinTint", pinTintR, pinTintG, pinTintB, pinTintA, path);

    GetPrivateProfileStringW(L"Markers", L"IconFx3D", std::to_wstring(iconFx3D ? 1 : 0).c_str(), buffer, 256, path);
    iconFx3D = (_wtoi(buffer) != 0);

    GetPrivateProfileStringW(L"Markers", L"FavSize", X100(favBadgeSize).c_str(), buffer, 256, path);
    favBadgeSize = std::clamp(_wtoi(buffer) / 100.0f, 1.0f, 12.0f);

    ReadColor4Hex(L"Markers", L"FavColor", L"FavColor", favBadgeColorR, favBadgeColorG, favBadgeColorB, favBadgeColorA, path);

    GetPrivateProfileStringW(L"Markers", L"NoteSize", X100(noteBadgeSize).c_str(), buffer, 256, path);
    noteBadgeSize = std::clamp(_wtoi(buffer) / 100.0f, 1.0f, 12.0f);

    ReadColor4Hex(L"Markers", L"NoteColor", L"NoteColor", noteBadgeColorR, noteBadgeColorG, noteBadgeColorB, noteBadgeColorA, path);

    GetPrivateProfileStringW(L"Markers", L"ClearedSize", X100(clearedBadgeSize).c_str(), buffer, 256, path);
    clearedBadgeSize = std::clamp(_wtoi(buffer) / 100.0f, 1.0f, 12.0f);

    ReadColor4Hex(L"Markers", L"ClearedColor", L"ClearedColor", clearedBadgeColorR, clearedBadgeColorG, clearedBadgeColorB, clearedBadgeColorA, path);

    GetPrivateProfileStringW(L"Markers", L"ImageEnabled", markerImageEnabled ? L"1" : L"0", buffer, 256, path);
    markerImageEnabled = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"Markers", L"ImageWidth", X100(markerImageWidth).c_str(), buffer, 256, path);
    markerImageWidth = std::clamp(_wtoi(buffer) / 100.0f, 0.25f, 1.0f);
    GetPrivateProfileStringW(L"Markers", L"ImageAlpha", X100(markerImageAlpha).c_str(), buffer, 256, path);
    markerImageAlpha = std::clamp(_wtoi(buffer) / 100.0f, 0.0f, 1.0f);

    GetPrivateProfileStringW(L"Markers", L"HoverOffsetX", std::to_wstring(markerHoverOffsetX).c_str(), buffer, 256, path);
    markerHoverOffsetX = _wtoi(buffer);
    GetPrivateProfileStringW(L"Markers", L"HoverOffsetY", std::to_wstring(markerHoverOffsetY).c_str(), buffer, 256, path);
    markerHoverOffsetY = _wtoi(buffer);

    ReadColor3Hex(L"Appearance", L"TitleColor", nullptr, titleColorR, titleColorG, titleColorB, path);
    ReadColor3Hex(L"Appearance", L"BodyColor", nullptr, bodyColorR, bodyColorG, bodyColorB, path);
    ReadColor4Hex(L"Appearance", L"BgColor", nullptr, bgColorR, bgColorG, bgColorB, bgColorA, path);
    ReadColor4Hex(L"Appearance", L"BorderColor", nullptr, borderColorR, borderColorG, borderColorB, borderColorA, path);

    GetPrivateProfileStringW(L"Appearance", L"BgShadow", std::to_wstring(bgShadow ? 1 : 0).c_str(), buffer, 256, path);
    bgShadow = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"Appearance", L"BgGradient", std::to_wstring(bgGradient ? 1 : 0).c_str(), buffer, 256, path);
    bgGradient = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"Appearance", L"BgVignette", std::to_wstring(bgVignette ? 1 : 0).c_str(), buffer, 256, path);
    bgVignette = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"Appearance", L"BgHighlight", std::to_wstring(bgHighlight ? 1 : 0).c_str(), buffer, 256, path);
    bgHighlight = (_wtoi(buffer) != 0);
    GetPrivateProfileStringW(L"Appearance", L"BgBorder", std::to_wstring(bgBorder ? 1 : 0).c_str(), buffer, 256, path);
    bgBorder = (_wtoi(buffer) != 0);

    logger::info("CustomMarkers.ini loaded:");
    logger::info("  Settings hotkey = {}", InputHandler::FormatHotkeyWithModifier(menuSettingsKey, menuSettingsModifier));
    logger::info("  Language = {}", language);
    logger::info("  Logging Level = {}", logLevel);

    Save();
}

void Settings::Save() {
    const auto path = GetIniPath();

    WritePrivateProfileStringW(L"Hotkey", L"ListPanelKey", std::to_wstring(listPanelKey).c_str(), path);
    WritePrivateProfileStringW(L"Hotkey", L"ListPanelModifier", std::to_wstring(listPanelModifier).c_str(), path);
    WritePrivateProfileStringW(L"Hotkey", L"PathTrackKey", std::to_wstring(pathTrackKey).c_str(), path);
    WritePrivateProfileStringW(L"Hotkey", L"PathTrackModifier", std::to_wstring(pathTrackModifier).c_str(), path);

    wchar_t langBuf[256];
    MultiByteToWideChar(CP_UTF8, 0, language.c_str(), -1, langBuf, 256);
    WritePrivateProfileStringW(L"Localization", L"Language", langBuf, path);
    WritePrivateProfileStringW(L"Localization", L"FontSize", std::to_wstring(fontSize).c_str(), path);
    WritePrivateProfileStringW(L"Localization", L"HeadingFontSize", std::to_wstring(headingFontSize).c_str(), path);

    wchar_t levelBuf[64];
    MultiByteToWideChar(CP_UTF8, 0, logLevel.c_str(), -1, levelBuf, 64);
    WritePrivateProfileStringW(L"Logging", L"Level", levelBuf, path);
    WritePrivateProfileStringW(L"Logging", L"BeaconVerboseLog", std::to_wstring(beaconVerboseLog ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"Logging", L"LootBeamPerfLog", std::to_wstring(lootBeamPerfLog ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"Logging", L"QuestRowLog", std::to_wstring(questRowLog ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"Logging", L"MapStatsLog", std::to_wstring(mapStatsLog ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"Logging", L"MapPinProjectionLog", std::to_wstring(mapPinProjectionLog ? 1 : 0).c_str(), path);

    WritePrivateProfileStringW(L"Menu", L"SettingsKey", std::to_wstring(menuSettingsKey).c_str(), path);
    WritePrivateProfileStringW(L"Menu", L"SettingsModifier", std::to_wstring(menuSettingsModifier).c_str(), path);
    WritePrivateProfileStringW(L"Menu", L"ActiveTab", std::to_wstring(menuActiveTab).c_str(), path);
    WritePrivateProfileStringW(L"Menu", L"PosX", std::to_wstring(menuPosX).c_str(), path);
    WritePrivateProfileStringW(L"Menu", L"PosY", std::to_wstring(menuPosY).c_str(), path);
    WritePrivateProfileStringW(L"Menu", L"Width", std::to_wstring(menuWidth).c_str(), path);
    WritePrivateProfileStringW(L"Menu", L"Height", std::to_wstring(menuHeight).c_str(), path);

    WritePrivateProfileStringW(L"ListPanel", L"Width", std::to_wstring(listPanelWidth).c_str(), path);
    WritePrivateProfileStringW(L"ListPanel", L"ShowOverview", std::to_wstring(listShowOverview ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"ListPanel", L"ShowActiveQuests", std::to_wstring(listShowActiveQuests ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"ListPanel", L"ActiveTab", std::to_wstring(listActiveTab).c_str(), path);
    WritePrivateProfileStringW(L"ListPanel", L"MinHeightPct", std::to_wstring(listPanelMinHeightPct).c_str(), path);
    WritePrivateProfileStringW(L"ListPanel", L"MaxHeightPct", std::to_wstring(listPanelMaxHeightPct).c_str(), path);
    WritePrivateProfileStringW(L"ListPanel", L"UseCustomBg", std::to_wstring(listPanelUseCustomBg ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"ListPanel", L"BgIndex", std::to_wstring(listPanelBgIndex).c_str(), path);
    WritePrivateProfileStringW(L"Appearance", L"TitleScale", std::to_wstring(static_cast<int>(titleScale * 100.0f + 0.5f)).c_str(), path);
    WritePrivateProfileStringW(L"Appearance", L"BodyScale", std::to_wstring(static_cast<int>(bodyScale * 100.0f + 0.5f)).c_str(), path);

    WritePrivateProfileStringW(L"Filters", L"Show", std::to_wstring(showFilterPanel ? 1 : 0).c_str(), path);

    WritePrivateProfileStringW(L"StatCard", L"NoteScale", std::to_wstring(static_cast<int>(noteTextScale * 100.0f + 0.5f)).c_str(), path);
    WritePrivateProfileStringW(L"StatCard", L"Anchor", std::to_wstring(statCardAnchor).c_str(), path);
    WritePrivateProfileStringW(L"StatCard", L"OffsetX", std::to_wstring(statCardOffsetX).c_str(), path);
    WritePrivateProfileStringW(L"StatCard", L"OffsetY", std::to_wstring(statCardOffsetY).c_str(), path);

    WritePrivateProfileStringW(L"NavigationOverlay", L"GlanceInteriorVisible", std::to_wstring(navGlanceInteriorVisible ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"GlanceExteriorVisible", std::to_wstring(navGlanceExteriorVisible ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectInteriorVisible", std::to_wstring(navInspectInteriorVisible ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectExteriorVisible", std::to_wstring(navInspectExteriorVisible ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectKeyDIK", std::to_wstring(navInspectKey).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectModifier", std::to_wstring(navInspectModifier).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"GlancePosX", std::to_wstring(navGlancePosX).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"GlancePosY", std::to_wstring(navGlancePosY).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"GlanceSize", std::to_wstring(navGlanceSize).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectSizePct", std::to_wstring(navInspectSizePct).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"RefreshOnInspect", std::to_wstring(navRefreshOnInspect ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectModeToggleKeyDIK", std::to_wstring(navInspectModeToggleKey).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectModeToggleModifier", std::to_wstring(navInspectModeToggleModifier).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectDefaultMode", std::to_wstring(navInspectDefaultMode).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectRadarRangeUnits", std::to_wstring(static_cast<int>(navInspectRadarRangeUnits)).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"GlanceRangeUnits", std::to_wstring(static_cast<int>(navGlanceRangeUnits)).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"QuickMarkKeyDIK", std::to_wstring(navQuickMarkKey).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"QuickMarkModifier", std::to_wstring(navQuickMarkModifier).c_str(), path);

    WritePrivateProfileStringW(L"LookPin", L"Enabled", std::to_wstring(lookPinEnabled ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"LookPin", L"KeyDIK", std::to_wstring(lookPinKey).c_str(), path);
    WritePrivateProfileStringW(L"LookPin", L"Modifier", std::to_wstring(lookPinModifier).c_str(), path);

    auto saveEntBool = [&](const wchar_t* a_key, bool a_field) {
        WritePrivateProfileStringW(L"NavigationOverlay", a_key, std::to_wstring(a_field ? 1 : 0).c_str(), path);
    };
    saveEntBool(L"GlanceEntContChest",      navGlanceEntContChest);
    saveEntBool(L"GlanceEntContBarrel",     navGlanceEntContBarrel);
    saveEntBool(L"GlanceEntContSack",       navGlanceEntContSack);
    saveEntBool(L"GlanceEntContUrn",        navGlanceEntContUrn);
    saveEntBool(L"GlanceEntContCoffin",     navGlanceEntContCoffin);
    saveEntBool(L"GlanceEntContStrongbox",  navGlanceEntContStrongbox);
    saveEntBool(L"GlanceEntLootWeapon",     navGlanceEntLootWeapon);
    saveEntBool(L"GlanceEntLootAmmo",       navGlanceEntLootAmmo);
    saveEntBool(L"GlanceEntLootArmor",      navGlanceEntLootArmor);
    saveEntBool(L"GlanceEntLootGold",       navGlanceEntLootGold);
    saveEntBool(L"GlanceEntLootKey",        navGlanceEntLootKey);
    saveEntBool(L"GlanceEntLootPotion",     navGlanceEntLootPotion);
    saveEntBool(L"GlanceEntLootFood",       navGlanceEntLootFood);
    saveEntBool(L"GlanceEntLootDrink",      navGlanceEntLootDrink);
    saveEntBool(L"GlanceEntLootBook",       navGlanceEntLootBook);
    saveEntBool(L"GlanceEntLootSoulGem",    navGlanceEntLootSoulGem);
    saveEntBool(L"GlanceEntLootScroll",     navGlanceEntLootScroll);
    saveEntBool(L"GlanceEntCorpseHumanoid", navGlanceEntCorpseHumanoid);
    saveEntBool(L"GlanceEntCorpseCreature", navGlanceEntCorpseCreature);
    saveEntBool(L"GlanceEntDoorLoad",       navGlanceEntDoorLoad);
    saveEntBool(L"GlanceEntDoorRegular",    navGlanceEntDoorRegular);
    saveEntBool(L"GlanceEntIngrLoose",      navGlanceEntIngrLoose);
    saveEntBool(L"GlanceEntIngrPlant",      navGlanceEntIngrPlant);

    saveEntBool(L"InspectEntContChest",      navInspectEntContChest);
    saveEntBool(L"InspectEntContBarrel",     navInspectEntContBarrel);
    saveEntBool(L"InspectEntContSack",       navInspectEntContSack);
    saveEntBool(L"InspectEntContUrn",        navInspectEntContUrn);
    saveEntBool(L"InspectEntContCoffin",     navInspectEntContCoffin);
    saveEntBool(L"InspectEntContStrongbox",  navInspectEntContStrongbox);
    saveEntBool(L"InspectEntLootWeapon",     navInspectEntLootWeapon);
    saveEntBool(L"InspectEntLootAmmo",       navInspectEntLootAmmo);
    saveEntBool(L"InspectEntLootArmor",      navInspectEntLootArmor);
    saveEntBool(L"InspectEntLootGold",       navInspectEntLootGold);
    saveEntBool(L"InspectEntLootKey",        navInspectEntLootKey);
    saveEntBool(L"InspectEntLootPotion",     navInspectEntLootPotion);
    saveEntBool(L"InspectEntLootFood",       navInspectEntLootFood);
    saveEntBool(L"InspectEntLootDrink",      navInspectEntLootDrink);
    saveEntBool(L"InspectEntLootBook",       navInspectEntLootBook);
    saveEntBool(L"InspectEntLootSoulGem",    navInspectEntLootSoulGem);
    saveEntBool(L"InspectEntLootScroll",     navInspectEntLootScroll);
    saveEntBool(L"InspectEntCorpseHumanoid", navInspectEntCorpseHumanoid);
    saveEntBool(L"InspectEntCorpseCreature", navInspectEntCorpseCreature);
    saveEntBool(L"InspectEntDoorLoad",       navInspectEntDoorLoad);
    saveEntBool(L"InspectEntDoorRegular",    navInspectEntDoorRegular);
    saveEntBool(L"InspectEntIngrLoose",      navInspectEntIngrLoose);
    saveEntBool(L"InspectEntIngrPlant",      navInspectEntIngrPlant);

    WritePrivateProfileStringW(L"NavigationOverlay", L"GlanceEntityIconSize", std::to_wstring(navGlanceEntityIconSize).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectEntityIconSize", std::to_wstring(navInspectEntityIconSize).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"GlancePinSize", std::to_wstring(navGlancePinSize).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectPinSize", std::to_wstring(navInspectPinSize).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"GlanceShowTrail", std::to_wstring(navGlanceShowTrail ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectShowTrail", std::to_wstring(navInspectShowTrail ? 1 : 0).c_str(), path);

    WritePrivateProfileStringW(L"NavigationOverlay", L"ActorShowAlly", std::to_wstring(navActorShowAlly ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"ActorShowHostile", std::to_wstring(navActorShowHostile ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"ActorShowNeutral", std::to_wstring(navActorShowNeutral ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"ActorShowAnimal", std::to_wstring(navActorShowAnimal ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"GlanceActorDotSize", std::to_wstring(navGlanceActorDotSize).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"InspectActorDotSize", std::to_wstring(navInspectActorDotSize).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"ActorProximityUnits", std::to_wstring(static_cast<int>(navActorProximityUnits)).c_str(), path);

    WritePrivateProfileStringW(L"NavigationOverlay", L"MarkerRange", std::to_wstring(static_cast<int>(navMarkerRange / Settings::kUnitsPerMeter)).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"MaxQuestMarkers", std::to_wstring(navMaxQuestMarkers).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"MaxCustomMarkers", std::to_wstring(navMaxCustomMarkers).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"BeaconVisible", std::to_wstring(navBeaconVisible ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"BeaconPillarHeight", std::to_wstring(static_cast<int>(navBeaconPillarHeight)).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"BeaconNearScale", std::to_wstring(static_cast<int>(navBeaconNearScale * 100.0f + 0.5f)).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"BeaconPinSize", std::to_wstring(navBeaconPinSize).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"BeaconFadeNearUnits", std::to_wstring(static_cast<int>(navBeaconFadeNearUnits)).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"BeaconEdgeIndicator", std::to_wstring(navBeaconEdgeIndicator ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamRangeUnits", std::to_wstring(static_cast<int>(lootBeamRangeUnits)).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamZBandUnits", std::to_wstring(static_cast<int>(lootBeamZBandUnits)).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamRequireLOS", std::to_wstring(lootBeamRequireLOS ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamOcclusion", std::to_wstring(lootBeamOcclusion ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"EntityRequireLOS", std::to_wstring(navEntityRequireLOS ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamVisible", std::to_wstring(lootBeamVisible ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamHideInCombat", std::to_wstring(lootBeamHideInCombat ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"HideInBeastForm", std::to_wstring(hideInBeastForm ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamHideOwned", std::to_wstring(lootBeamHideOwned ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamLengthUnits", std::to_wstring(static_cast<int>(lootBeamLengthUnits)).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamIconSize", std::to_wstring(lootBeamIconSize).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamBrightness", std::to_wstring(static_cast<int>(lootBeamBrightness * 100.0f + 0.5f)).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowCorpseHumanoid", std::to_wstring(lootBeamShowCorpseHumanoid ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowCorpseCreature", std::to_wstring(lootBeamShowCorpseCreature ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowContChest", std::to_wstring(lootBeamShowContChest ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowContStrongbox", std::to_wstring(lootBeamShowContStrongbox ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowContSatchel", std::to_wstring(lootBeamShowContSatchel ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootWeapon", std::to_wstring(lootBeamShowLootWeapon ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootArmor", std::to_wstring(lootBeamShowLootArmor ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootGold", std::to_wstring(lootBeamShowLootGold ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootPotion", std::to_wstring(lootBeamShowLootPotion ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootSoulGem", std::to_wstring(lootBeamShowLootSoulGem ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamShowLootScroll", std::to_wstring(lootBeamShowLootScroll ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"NavigationOverlay", L"LootBeamMinGoldValue", std::to_wstring(lootBeamMinGoldValue).c_str(), path);

    WritePrivateProfileStringW(L"PathTrail", L"Visible", std::to_wstring(pathTrailVisible ? 1 : 0).c_str(), path);
    WriteColor4Hex(L"PathTrail", L"Color", L"Color", pathTrailColorR, pathTrailColorG, pathTrailColorB, pathTrailColorA, path);
    WritePrivateProfileStringW(L"PathTrail", L"Thickness", std::to_wstring(static_cast<int>(pathTrailThickness * 100.0f + 0.5f)).c_str(), path);
    WritePrivateProfileStringW(L"PathTrail", L"FadeByAge", std::to_wstring(pathTrailFadeByAge ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"PathTrail", L"SmartSpacing", std::to_wstring(pathTrailSmartSpacing ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"PathTrail", L"Pulse", std::to_wstring(pathPulseEnabled ? 1 : 0).c_str(), path);
    WriteColor3Hex(L"PathTrail", L"PulseColor", L"PulseColor", pathPulseColorR, pathPulseColorG, pathPulseColorB, path);

    WritePrivateProfileStringW(L"Markers", L"IconScale", std::to_wstring(static_cast<int>(markerIconScale * 100.0f + 0.5f)).c_str(), path);
    WriteColor4Hex(L"Markers", L"PinTint", L"PinTint", pinTintR, pinTintG, pinTintB, pinTintA, path);
    WritePrivateProfileStringW(L"Markers", L"IconFx3D", std::to_wstring(iconFx3D ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"Markers", L"FavSize", std::to_wstring(static_cast<int>(favBadgeSize * 100.0f + 0.5f)).c_str(), path);
    WriteColor4Hex(L"Markers", L"FavColor", L"FavColor", favBadgeColorR, favBadgeColorG, favBadgeColorB, favBadgeColorA, path);
    WritePrivateProfileStringW(L"Markers", L"NoteSize", std::to_wstring(static_cast<int>(noteBadgeSize * 100.0f + 0.5f)).c_str(), path);
    WriteColor4Hex(L"Markers", L"NoteColor", L"NoteColor", noteBadgeColorR, noteBadgeColorG, noteBadgeColorB, noteBadgeColorA, path);
    WritePrivateProfileStringW(L"Markers", L"ClearedSize", std::to_wstring(static_cast<int>(clearedBadgeSize * 100.0f + 0.5f)).c_str(), path);
    WriteColor4Hex(L"Markers", L"ClearedColor", L"ClearedColor", clearedBadgeColorR, clearedBadgeColorG, clearedBadgeColorB, clearedBadgeColorA, path);
    WritePrivateProfileStringW(L"Markers", L"ImageEnabled", markerImageEnabled ? L"1" : L"0", path);
    WritePrivateProfileStringW(L"Markers", L"ImageWidth", std::to_wstring(static_cast<int>(markerImageWidth * 100.0f + 0.5f)).c_str(), path);
    WritePrivateProfileStringW(L"Markers", L"ImageAlpha", std::to_wstring(static_cast<int>(markerImageAlpha * 100.0f + 0.5f)).c_str(), path);
    WritePrivateProfileStringW(L"Markers", L"HoverOffsetX", std::to_wstring(markerHoverOffsetX).c_str(), path);
    WritePrivateProfileStringW(L"Markers", L"HoverOffsetY", std::to_wstring(markerHoverOffsetY).c_str(), path);

    WriteColor3Hex(L"Appearance", L"TitleColor", nullptr, titleColorR, titleColorG, titleColorB, path);
    WriteColor3Hex(L"Appearance", L"BodyColor", nullptr, bodyColorR, bodyColorG, bodyColorB, path);
    WriteColor4Hex(L"Appearance", L"BgColor", nullptr, bgColorR, bgColorG, bgColorB, bgColorA, path);
    WriteColor4Hex(L"Appearance", L"BorderColor", nullptr, borderColorR, borderColorG, borderColorB, borderColorA, path);

    WritePrivateProfileStringW(L"Appearance", L"BgShadow", std::to_wstring(bgShadow ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"Appearance", L"BgGradient", std::to_wstring(bgGradient ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"Appearance", L"BgVignette", std::to_wstring(bgVignette ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"Appearance", L"BgHighlight", std::to_wstring(bgHighlight ? 1 : 0).c_str(), path);
    WritePrivateProfileStringW(L"Appearance", L"BgBorder", std::to_wstring(bgBorder ? 1 : 0).c_str(), path);

    SavePositions();
}

void Settings::SavePositions() {
    const auto path = GetIniPath();

    WritePrivateProfileStringW(L"Menu", L"ActiveTab", std::to_wstring(menuActiveTab).c_str(), path);
    WritePrivateProfileStringW(L"Menu", L"PosX", std::to_wstring(menuPosX).c_str(), path);
    WritePrivateProfileStringW(L"Menu", L"PosY", std::to_wstring(menuPosY).c_str(), path);
    WritePrivateProfileStringW(L"ListPanel", L"PosX", std::to_wstring(listPanelPosX).c_str(), path);
    WritePrivateProfileStringW(L"ListPanel", L"PosY", std::to_wstring(listPanelPosY).c_str(), path);
    WritePrivateProfileStringW(L"ListPanel", L"ActiveTab", std::to_wstring(listActiveTab).c_str(), path);
    WritePrivateProfileStringW(L"Filters", L"PosX", std::to_wstring(filterPosX).c_str(), path);
    WritePrivateProfileStringW(L"Filters", L"PosY", std::to_wstring(filterPosY).c_str(), path);
    WritePrivateProfileStringW(L"Filters", L"Collapsed", std::to_wstring(filterCollapsed ? 1 : 0).c_str(), path);
}
