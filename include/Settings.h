#pragma once

// Reads and writes configuration settings from INI.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>

#include "MapTheme.h"
#include "EntityMarkerSource.h"

#include <string>

class Settings {
public:
    static constexpr float kUnitsPerMeter = 70.0f;
    static Settings* GetSingleton() {
        static Settings singleton;
        return &singleton;
    }
    void Load();
    void Save();
    void SavePositions();
    std::int32_t listPanelKey      = 0x2E;
    std::int32_t listPanelModifier = 0;
    std::int32_t pathTrackKey      = 0x14;
    std::int32_t pathTrackModifier = 0;
    std::string language = "CustomMarkers_en";
    std::int32_t fontSize = 18;
    std::int32_t headingFontSize = 28;
    std::string logLevel = "info";
    std::int32_t menuSettingsKey      = 0x2D;
    std::int32_t menuSettingsModifier = 8;
    std::int32_t menuActiveTab = 5;
    std::int32_t menuPosX = 310;
    std::int32_t menuPosY = 300;
    std::int32_t menuWidth = 480;
    std::int32_t menuHeight = 640;
    float        titleScale = 0.85f;
    float        bodyScale  = 0.95f;
    std::int32_t listPanelWidth = 275;
    std::int32_t listPanelPosX = 1530;
    std::int32_t listPanelPosY = 60;
    bool         listShowOverview     = true;
    bool         listShowActiveQuests = true;
    std::int32_t listActiveTab            = 0;
    std::int32_t listPanelMinHeightPct    = 35;
    std::int32_t listPanelMaxHeightPct    = 70;
    bool         listPanelUseCustomBg = false;
    std::int32_t listPanelBgIndex     = 6;
    bool         showFilterPanel = true;
    std::int32_t filterPosX = 120;
    std::int32_t filterPosY = 45;
    bool         filterCollapsed = true;
    float        noteTextScale = 0.85f;
    std::int32_t statCardAnchor  = 3;
    std::int32_t statCardOffsetX = 12;
    std::int32_t statCardOffsetY = 0;
    std::int32_t titleColorR = 255, titleColorG = 220, titleColorB = 140;
    std::int32_t bodyColorR  = 236, bodyColorG  = 210, bodyColorB  = 160;
    std::int32_t bgColorR = 18, bgColorG = 16, bgColorB = 12, bgColorA = 255;
    std::int32_t borderColorR = 140, borderColorG = 115, borderColorB = 65, borderColorA = 255;
    ImU32 BgCol()        const { return IM_COL32(bgColorR, bgColorG, bgColorB, bgColorA); }
    ImVec4 BgColVec4()   const { return ImVec4(bgColorR / 255.0f, bgColorG / 255.0f,
                                               bgColorB / 255.0f, bgColorA / 255.0f); }
    ImU32 BorderCol()    const { return IM_COL32(borderColorR, borderColorG, borderColorB, borderColorA); }
    ImVec4 BorderColVec4() const { return ImVec4(borderColorR / 255.0f, borderColorG / 255.0f,
                                                 borderColorB / 255.0f, borderColorA / 255.0f); }
    bool bgShadow    = true;
    bool bgGradient  = true;
    bool bgVignette  = true;
    bool bgHighlight = true;
    bool bgBorder    = true;
    bool navGlanceInteriorVisible  = true;
    bool navGlanceExteriorVisible  = true;
    bool navInspectInteriorVisible = true;
    bool navInspectExteriorVisible = true;
    std::int32_t navInspectKey      = 0x31;
    std::int32_t navInspectModifier = 0;
    int navGlancePosX = 1625;
    int navGlancePosY = 25;
    int navGlanceSize = 190;
    int navInspectSizePct = 72;
    bool navRefreshOnInspect = true;
    std::int32_t navInspectModeToggleKey      = 0x13;
    std::int32_t navInspectModeToggleModifier = 0;
    int navInspectDefaultMode = 0;
    float navInspectRadarRangeUnits = 3000.0f;
    std::int32_t navQuickMarkKey      = 0x21;
    std::int32_t navQuickMarkModifier = 0;
    bool         lookPinEnabled  = true;
    std::int32_t lookPinKey      = 0x31;
    std::int32_t lookPinModifier = 2;
    float navGlanceRangeUnits = 1200.0f;
    bool navGlanceEntContChest      = true;
    bool navGlanceEntContBarrel     = false;
    bool navGlanceEntContSack       = false;
    bool navGlanceEntContUrn        = false;
    bool navGlanceEntContCoffin     = false;
    bool navGlanceEntContStrongbox  = true;
    bool navGlanceEntLootWeapon     = true;
    bool navGlanceEntLootAmmo       = false;
    bool navGlanceEntLootArmor      = true;
    bool navGlanceEntLootGold       = true;
    bool navGlanceEntLootKey        = true;
    bool navGlanceEntLootPotion     = true;
    bool navGlanceEntLootFood       = false;
    bool navGlanceEntLootDrink      = false;
    bool navGlanceEntLootBook       = true;
    bool navGlanceEntLootSoulGem    = true;
    bool navGlanceEntLootScroll     = true;
    bool navGlanceEntCorpseHumanoid = true;
    bool navGlanceEntCorpseCreature = true;
    bool navGlanceEntDoorLoad       = true;
    bool navGlanceEntDoorRegular    = false;
    bool navGlanceEntIngrLoose      = false;
    bool navGlanceEntIngrPlant      = true;
    bool navInspectEntContChest      = true;
    bool navInspectEntContBarrel     = true;
    bool navInspectEntContSack       = true;
    bool navInspectEntContUrn        = true;
    bool navInspectEntContCoffin     = true;
    bool navInspectEntContStrongbox  = true;
    bool navInspectEntLootWeapon     = true;
    bool navInspectEntLootAmmo       = true;
    bool navInspectEntLootArmor      = true;
    bool navInspectEntLootGold       = true;
    bool navInspectEntLootKey        = true;
    bool navInspectEntLootPotion     = true;
    bool navInspectEntLootFood       = true;
    bool navInspectEntLootDrink      = true;
    bool navInspectEntLootBook       = true;
    bool navInspectEntLootSoulGem    = true;
    bool navInspectEntLootScroll     = true;
    bool navInspectEntCorpseHumanoid = true;
    bool navInspectEntCorpseCreature = true;
    bool navInspectEntDoorLoad       = true;
    bool navInspectEntDoorRegular    = true;
    bool navInspectEntIngrLoose      = true;
    bool navInspectEntIngrPlant      = true;
    std::int32_t navGlanceEntityIconSize  = 14;
    std::int32_t navInspectEntityIconSize = 20;
    std::int32_t navGlancePinSize  = 14;
    std::int32_t navInspectPinSize = 18;
    bool GlanceSubEnabled(EntityMarkerSource::Category a_cat, EntityMarkerSource::SubCategory a_sub) const {
        using Cat = EntityMarkerSource::Category;
        using Sub = EntityMarkerSource::SubCategory;
        switch (a_sub) {
            case Sub::ContChest:      return navGlanceEntContChest;
            case Sub::ContBarrel:     return navGlanceEntContBarrel;
            case Sub::ContSack:       return navGlanceEntContSack;
            case Sub::ContSatchel:    return navGlanceEntContSack;
            case Sub::ContUrn:        return navGlanceEntContUrn;
            case Sub::ContCoffin:     return navGlanceEntContCoffin;
            case Sub::ContStrongbox:  return navGlanceEntContStrongbox;
            case Sub::LootWeapon:     return navGlanceEntLootWeapon;
            case Sub::LootAmmo:       return navGlanceEntLootAmmo;
            case Sub::LootArmor:      return navGlanceEntLootArmor;
            case Sub::LootGold:       return navGlanceEntLootGold;
            case Sub::LootKey:        return navGlanceEntLootKey;
            case Sub::LootPotion:     return navGlanceEntLootPotion;
            case Sub::LootFood:       return navGlanceEntLootFood;
            case Sub::LootDrink:      return navGlanceEntLootDrink;
            case Sub::LootBook:       return navGlanceEntLootBook;
            case Sub::LootSoulGem:    return navGlanceEntLootSoulGem;
            case Sub::LootScroll:     return navGlanceEntLootScroll;
            case Sub::CorpseHumanoid: return navGlanceEntCorpseHumanoid;
            case Sub::CorpseCreature: return navGlanceEntCorpseCreature;
            case Sub::DoorLoad:       return navGlanceEntDoorLoad;
            case Sub::DoorRegular:    return navGlanceEntDoorRegular;
            case Sub::IngrLoose:      return navGlanceEntIngrLoose;
            case Sub::IngrPlant:      return navGlanceEntIngrPlant;
            default: break;
        }
        switch (a_cat) {
            case Cat::Container:  return navGlanceEntContChest || navGlanceEntContBarrel || navGlanceEntContSack ||
                                          navGlanceEntContUrn || navGlanceEntContCoffin || navGlanceEntContStrongbox;
            case Cat::LooseLoot:  return navGlanceEntLootWeapon || navGlanceEntLootAmmo || navGlanceEntLootArmor ||
                                          navGlanceEntLootGold ||
                                          navGlanceEntLootKey || navGlanceEntLootPotion || navGlanceEntLootFood ||
                                          navGlanceEntLootDrink || navGlanceEntLootBook || navGlanceEntLootSoulGem ||
                                          navGlanceEntLootScroll;
            case Cat::Corpse:     return navGlanceEntCorpseHumanoid || navGlanceEntCorpseCreature;
            case Cat::Door:       return navGlanceEntDoorLoad || navGlanceEntDoorRegular;
            case Cat::Ingredient: return navGlanceEntIngrLoose || navGlanceEntIngrPlant;
        }
        return false;
    }
    bool InspectSubEnabled(EntityMarkerSource::Category a_cat, EntityMarkerSource::SubCategory a_sub) const {
        using Cat = EntityMarkerSource::Category;
        using Sub = EntityMarkerSource::SubCategory;
        switch (a_sub) {
            case Sub::ContChest:      return navInspectEntContChest;
            case Sub::ContBarrel:     return navInspectEntContBarrel;
            case Sub::ContSack:       return navInspectEntContSack;
            case Sub::ContSatchel:    return navInspectEntContSack;
            case Sub::ContUrn:        return navInspectEntContUrn;
            case Sub::ContCoffin:     return navInspectEntContCoffin;
            case Sub::ContStrongbox:  return navInspectEntContStrongbox;
            case Sub::LootWeapon:     return navInspectEntLootWeapon;
            case Sub::LootAmmo:       return navInspectEntLootAmmo;
            case Sub::LootArmor:      return navInspectEntLootArmor;
            case Sub::LootGold:       return navInspectEntLootGold;
            case Sub::LootKey:        return navInspectEntLootKey;
            case Sub::LootPotion:     return navInspectEntLootPotion;
            case Sub::LootFood:       return navInspectEntLootFood;
            case Sub::LootDrink:      return navInspectEntLootDrink;
            case Sub::LootBook:       return navInspectEntLootBook;
            case Sub::LootSoulGem:    return navInspectEntLootSoulGem;
            case Sub::LootScroll:     return navInspectEntLootScroll;
            case Sub::CorpseHumanoid: return navInspectEntCorpseHumanoid;
            case Sub::CorpseCreature: return navInspectEntCorpseCreature;
            case Sub::DoorLoad:       return navInspectEntDoorLoad;
            case Sub::DoorRegular:    return navInspectEntDoorRegular;
            case Sub::IngrLoose:      return navInspectEntIngrLoose;
            case Sub::IngrPlant:      return navInspectEntIngrPlant;
            default: break;
        }
        switch (a_cat) {
            case Cat::Container:  return navInspectEntContChest || navInspectEntContBarrel || navInspectEntContSack ||
                                          navInspectEntContUrn || navInspectEntContCoffin || navInspectEntContStrongbox;
            case Cat::LooseLoot:  return navInspectEntLootWeapon || navInspectEntLootAmmo || navInspectEntLootArmor ||
                                          navInspectEntLootGold ||
                                          navInspectEntLootKey || navInspectEntLootPotion || navInspectEntLootFood ||
                                          navInspectEntLootDrink || navInspectEntLootBook || navInspectEntLootSoulGem ||
                                          navInspectEntLootScroll;
            case Cat::Corpse:     return navInspectEntCorpseHumanoid || navInspectEntCorpseCreature;
            case Cat::Door:       return navInspectEntDoorLoad || navInspectEntDoorRegular;
            case Cat::Ingredient: return navInspectEntIngrLoose || navInspectEntIngrPlant;
        }
        return false;
    }
    bool GlanceAnyEntity() const {
        return navGlanceEntContChest || navGlanceEntContBarrel || navGlanceEntContSack || navGlanceEntContUrn ||
               navGlanceEntContCoffin || navGlanceEntContStrongbox || navGlanceEntLootWeapon || navGlanceEntLootAmmo ||
               navGlanceEntLootArmor ||
               navGlanceEntLootGold || navGlanceEntLootKey || navGlanceEntLootPotion || navGlanceEntLootFood ||
               navGlanceEntLootDrink || navGlanceEntLootBook || navGlanceEntLootSoulGem || navGlanceEntLootScroll ||
               navGlanceEntCorpseHumanoid || navGlanceEntCorpseCreature || navGlanceEntDoorLoad ||
               navGlanceEntDoorRegular || navGlanceEntIngrLoose || navGlanceEntIngrPlant;
    }
    bool InspectAnyEntity() const {
        return navInspectEntContChest || navInspectEntContBarrel || navInspectEntContSack || navInspectEntContUrn ||
               navInspectEntContCoffin || navInspectEntContStrongbox || navInspectEntLootWeapon || navInspectEntLootAmmo ||
               navInspectEntLootArmor ||
               navInspectEntLootGold || navInspectEntLootKey || navInspectEntLootPotion || navInspectEntLootFood ||
               navInspectEntLootDrink || navInspectEntLootBook || navInspectEntLootSoulGem || navInspectEntLootScroll ||
               navInspectEntCorpseHumanoid || navInspectEntCorpseCreature || navInspectEntDoorLoad ||
               navInspectEntDoorRegular || navInspectEntIngrLoose || navInspectEntIngrPlant;
    }
    bool navActorShowAlly     = true;
    bool navActorShowHostile  = true;
    bool navActorShowNeutral  = true;
    bool navActorShowAnimal   = true;
    std::int32_t navGlanceActorDotSize  = 5;
    std::int32_t navInspectActorDotSize = 8;
    float navActorProximityUnits = 1200.0f;
    float navMarkerRange = 98000.0f;
    std::int32_t navMaxQuestMarkers = 2;
    std::int32_t navMaxCustomMarkers = 5;
    bool         navBeaconVisible = true;
    float        navBeaconPillarHeight = 250.0f;
    float        navBeaconNearScale = 0.35f;
    std::int32_t navBeaconPinSize = 22;
    float        navBeaconFadeNearUnits = 350.0f;
    bool         navBeaconEdgeIndicator = false;
    bool         beaconVerboseLog = false;
    bool         lootBeamPerfLog = false;
    bool         mapStatsLog = false;
    bool         questRowLog = false;
    bool         mapPinProjectionLog = false;
    float lootBeamRangeUnits = 2100.0f;
    float lootBeamZBandUnits = 350.0f;
    bool  lootBeamRequireLOS = true;
    bool  lootBeamOcclusion = true;
    bool  navEntityRequireLOS = true;
    bool         lootBeamVisible = true;
    bool         lootBeamHideInCombat = true;
    float        lootBeamBrightness = 0.7f;
    bool         lootBeamShowCorpseHumanoid = true;
    bool         lootBeamShowCorpseCreature = true;
    bool         lootBeamShowContChest      = true;
    bool         lootBeamShowContStrongbox  = true;
    bool         lootBeamShowContSatchel    = true;
    bool         lootBeamShowLootWeapon     = true;
    bool         lootBeamShowLootArmor      = true;
    bool         lootBeamShowLootGold       = true;
    bool         lootBeamShowLootPotion     = true;
    bool         lootBeamShowLootSoulGem    = true;
    bool         lootBeamShowLootScroll     = true;
    std::int32_t lootBeamMinGoldValue       = 30;
    bool LootBeamAnySubtype() const {
        return lootBeamShowCorpseHumanoid || lootBeamShowCorpseCreature ||
               lootBeamShowContChest || lootBeamShowContStrongbox || lootBeamShowContSatchel ||
               lootBeamShowLootWeapon || lootBeamShowLootArmor ||
               lootBeamShowLootGold || lootBeamShowLootPotion ||
               lootBeamShowLootSoulGem || lootBeamShowLootScroll;
    }
    bool         hideInBeastForm = true;
    bool         lootBeamHideOwned = true;
    float        lootBeamLengthUnits = 110.0f;
    std::int32_t lootBeamIconSize = 22;
    bool navActorAnyVisible() const {
        return navActorShowAlly || navActorShowHostile || navActorShowNeutral || navActorShowAnimal;
    }
    bool navGlanceShowTrail     = true;
    bool navInspectShowTrail    = true;
    bool         pathTrailVisible = true;
    std::int32_t pathTrailColorR = MapTheme::kPathTrailR, pathTrailColorG = MapTheme::kPathTrailG,
                 pathTrailColorB = MapTheme::kPathTrailB, pathTrailColorA = MapTheme::kPathTrailA;
    float        pathTrailThickness = MapTheme::kPathTrailThickness;
    bool         pathTrailFadeByAge = true;
    bool         pathTrailSmartSpacing = true;
    bool         pathPulseEnabled = true;
    std::int32_t pathPulseColorR = MapTheme::kPathPulseR, pathPulseColorG = MapTheme::kPathPulseG,
                 pathPulseColorB = MapTheme::kPathPulseB;
    float        markerIconScale = MapTheme::kMarkerIconScale;
    std::int32_t pinTintR = 255, pinTintG = 250, pinTintB = 58, pinTintA = 255;
    ImU32 PinTintCol() const { return IM_COL32(pinTintR, pinTintG, pinTintB, pinTintA); }
    bool         iconFx3D = true;
    std::int32_t markerHoverOffsetX = 25;
    std::int32_t markerHoverOffsetY = 40;
    float        favBadgeSize    = 9.0f;
    std::int32_t favBadgeColorR = 255, favBadgeColorG = 205, favBadgeColorB = 70, favBadgeColorA = 230;
    float        noteBadgeSize   = 6.0f;
    std::int32_t noteBadgeColorR = 70, noteBadgeColorG = 200, noteBadgeColorB = 190, noteBadgeColorA = 230;
    float        clearedBadgeSize   = 7.0f;
    std::int32_t clearedBadgeColorR = 90, clearedBadgeColorG = 200, clearedBadgeColorB = 110, clearedBadgeColorA = 235;
    bool  markerImageEnabled = true;
    float markerImageWidth   = 1.0f;
    float markerImageAlpha   = 1.0f;
    ImU32 FavBadgeCol()  const { return IM_COL32(favBadgeColorR, favBadgeColorG, favBadgeColorB, favBadgeColorA); }
    ImU32 NoteBadgeCol() const { return IM_COL32(noteBadgeColorR, noteBadgeColorG, noteBadgeColorB, noteBadgeColorA); }
    ImU32 ClearedBadgeCol() const { return IM_COL32(clearedBadgeColorR, clearedBadgeColorG, clearedBadgeColorB, clearedBadgeColorA); }
    ImU32 TitleCol()     const { return IM_COL32(titleColorR, titleColorG, titleColorB, 255); }
    ImU32 BodyCol()      const { return IM_COL32(bodyColorR,  bodyColorG,  bodyColorB,  255); }
    ImU32 BodyColHover() const {
        auto up = [](int c){ return c + (255 - c) * 25 / 100; };
        return IM_COL32(up(bodyColorR), up(bodyColorG), up(bodyColorB), 255);
    }
private:
    Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
};
