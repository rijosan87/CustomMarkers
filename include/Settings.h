#pragma once
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

    std::int32_t listPanelKey      = 0x2E;  // C
    std::int32_t listPanelModifier = 0;
    std::int32_t pathTrackKey      = 0x14;  // T
    std::int32_t pathTrackModifier = 0;

    std::string language = "CustomMarkers_en";

    std::int32_t fontSize = 18;
    std::int32_t headingFontSize = 28;

    std::string logLevel = "info";

    std::int32_t menuSettingsKey      = 0x2D;  // X
    std::int32_t menuSettingsModifier = 8;
    // Last-open settings tile index (SettingsMenu::Tab). Persisted so reopening
    // the window lands on the tile you left, not always General.
    std::int32_t menuActiveTab = 5;
    std::int32_t menuPosX = 310;
    std::int32_t menuPosY = 300;
    std::int32_t menuWidth = 480;
    std::int32_t menuHeight = 640;

    // Master interface scale percentage (50-200%). Defaults to 100%.
    // INI: [Appearance] InterfaceScale.
    std::int32_t interfaceScale = 100;

    float        titleScale = 0.85f;
    float        bodyScale  = 0.95f;

    // World-map list panel (hotkey C). Drawn hand-rolled in
    // MapMarkerRenderer::DrawListPanel(); these replace the former literals.
    std::int32_t listPanelWidth = 275;   // panel width in px
    bool listPanelVisible = true;
    // Top-left screen position. -1 = unset, fall back to old top-right anchor.
    // Hand-drawn panel has no ImGui window, so drag is tracked off MenuCursor.
    std::int32_t listPanelPosX = 1530;
    std::int32_t listPanelPosY = 60;
    // Overview-tab sub-block toggles. All three live on the Overview tab; off also
    // skips that block's per-frame Refresh(), not just its draw.
    bool         listShowOverview     = true;
    bool         listShowActiveQuests = true;
    bool         listShowCompanions   = true;
    std::int32_t listActiveTab            = 0;
    std::int32_t listPanelMinHeightPct    = 35;   // % of screen height
    std::int32_t listPanelMaxHeightPct    = 70;   // % of screen height

    bool         listPanelUseCustomBg = false;
    std::int32_t listPanelBgIndex     = 6;

    // Map Filters window (real ImGui window). -1 = unset, ImGui picks default.
    bool         showFilterPanel = true;
    std::int32_t filterPosX = 120;
    std::int32_t filterPosY = 45;
    bool         filterCollapsed = true;

    float        noteTextScale = 0.85f;

    std::int32_t statCardAnchor  = 3;
    std::int32_t statCardOffsetX = 20;
    std::int32_t statCardOffsetY = 0;

    // Controls the Type, Hold, Bounty, and Cleared rows on vanilla marker hovers.
    // The marker name, note, and image remain independent.
    bool         statCardShowDetails = true;
    // Per-row toggles under Show Meta; statCardShowDetails is the master.
    bool         statCardShowType       = true;
    bool         statCardShowHold       = true;
    bool         statCardShowDistance   = true;
    bool         statCardShowBounty     = true;
    bool         statCardShowCleared    = true;
    bool         statCardShowBoss       = true;
    bool         statCardShowQuests     = true;
    bool         statCardShowServices   = true;
    bool         statCardShowPlayerHome = true;
    bool         statCardShowSpouse     = true;
    bool         statCardShowCompanions = true;
    // 0 = Metric (m, km), 1 = Imperial (ft, mi).
    std::int32_t statCardDistanceUnits  = 0;

    std::int32_t titleColorR = 255, titleColorG = 220, titleColorB = 140; // gold
    std::int32_t bodyColorR  = 236, bodyColorG  = 210, bodyColorB  = 160; // warm off-white

    std::int32_t bgColorR = 18, bgColorG = 16, bgColorB = 12, bgColorA = 255;

    std::int32_t borderColorR = 140, borderColorG = 115, borderColorB = 65, borderColorA = 255;

    ImU32 BgCol()        const { return IM_COL32(bgColorR, bgColorG, bgColorB, bgColorA); }
    ImVec4 BgColVec4()   const { return ImVec4(bgColorR / 255.0f, bgColorG / 255.0f,
                                               bgColorB / 255.0f, bgColorA / 255.0f); }
    ImU32 BorderCol()    const { return IM_COL32(borderColorR, borderColorG, borderColorB, borderColorA); }
    ImVec4 BorderColVec4() const { return ImVec4(borderColorR / 255.0f, borderColorG / 255.0f,
                                                 borderColorB / 255.0f, borderColorA / 255.0f); }

    // Background design layers (Appearance > Background Design)
    bool bgShadow    = true;
    bool bgGradient  = true;
    bool bgVignette  = true;
    bool bgHighlight = true;
    bool bgBorder    = true;

    bool navGlanceInteriorVisible  = true;
    bool navGlanceExteriorVisible  = true;
    bool navInspectInteriorVisible = true;
    bool navInspectExteriorVisible = true;
    std::int32_t navInspectKey        = 0x31;  // N
    std::int32_t navInspectModifier   = 0;
    std::int32_t navInspectActivation = 0;     // 0 = Hold, 1 = Toggle
    int navGlancePosX = 1625;
    int navGlancePosY = 25;
    int navGlanceSize = 190;
    int navGlanceShape = 0;  // 0 = Circle, 1 = Square
    int navGlanceOrientation = 0;  // 0 = Fixed North, 1 = Follow Camera
    // N/E/S/W drawn just outside the rim; they follow the same rotation as the glyphs.
    bool navGlanceShowCardinals = true;
    // Location name under the radar, same source and style as the Inspect title.
    bool navGlanceShowLocation = true;
    int navInspectSizePct = 72;
    bool navRefreshOnInspect = true;

    std::int32_t navInspectModeToggleKey      = 0x13;  // R
    std::int32_t navInspectModeToggleModifier = 0;
    int navInspectDefaultMode = 0;                     // 0 = Cell, 1 = Radar
    float navInspectRadarRangeUnits = 3000.0f;       // Radar frustum width, world units

    std::int32_t navQuickMarkKey      = 0x21;  // F
    std::int32_t navQuickMarkModifier = 0;

    std::int32_t navGlanceToggleKey       = -1;
    std::int32_t navGlanceToggleModifier  = 0;
    std::int32_t lootBeamToggleKey        = -1;
    std::int32_t lootBeamToggleModifier   = 0;

    std::int32_t controllerInspectOpenButton         = 275;  // RB / R1
    std::int32_t controllerInspectOpenGesture        = 1;    // 0 Single, 1 Double, 2 Long
    std::int32_t controllerInspectAddMarkerButton    = 276;  // A / Cross
    std::int32_t controllerInspectSwitchViewButton   = 279;  // Y / Triangle
    std::int32_t controllerInspectPathTrackingButton = 278;  // X / Square

    bool         lookPinEnabled  = true;
    std::int32_t lookPinKey      = 0x31;  // N
    std::int32_t lookPinModifier = 2;     // Ctrl

    float navGlanceRangeUnits = 2000.0f;

    bool navGlanceEntContChest      = true;
    bool navGlanceEntContBarrel     = false;
    bool navGlanceEntContSack       = false;
    bool navGlanceEntContUrn        = false;
    bool navGlanceEntContCoffin     = false;
    bool navGlanceEntContStrongbox  = true;
    bool navGlanceEntLootWeapon     = true;
    // Matches LootWeapon's default so splitting ammo off leaves radar behaviour
    // exactly as it was; the split exists for the loot beams, not the radar.
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

    std::int32_t navGlanceEntityIconSize  = 18;
    std::int32_t navInspectEntityIconSize = 20;

    std::int32_t navEntityIconBgR = 0, navEntityIconBgG = 0;
    std::int32_t navEntityIconBgB = 0, navEntityIconBgA = 225;
    std::int32_t navEntityIconBorderR = 255, navEntityIconBorderG = 255;
    std::int32_t navEntityIconBorderB = 255, navEntityIconBorderA = 170;
    float navEntityIconPadding = 1.0f;
    bool navEntityIcon3D = true;
    bool navEntityIconShadow = true;

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
            case Sub::LootGold:
            case Sub::LootCoinPurse:  return navGlanceEntLootGold;
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
            default: break;  // Sub::None: fall through to the category umbrella
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
            case Sub::LootGold:
            case Sub::LootCoinPurse:  return navInspectEntLootGold;
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
            default: break;  // Sub::None: fall through to the category umbrella
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
    bool navActorShowAnimal   = true;   // non-hostile wildlife dots (grey)
    std::int32_t navGlanceActorDotSize  = 5;
    std::int32_t navInspectActorDotSize = 6;
    float navActorProximityUnits = 1200.0f;
    float navMarkerRange = 98000.0f;  // 1400 m * 70
    std::int32_t navMaxQuestMarkers = 2;
    std::int32_t navMaxCustomMarkers = 5;

    bool         navBeaconVisible = true;
    // FAR height. The beam scales down toward navBeaconNearScale as the player
    // approaches, so this is the height seen at range, not a constant.
    float        navBeaconPillarHeight = 250.0f;
    // Fraction of the far height used when standing on the destination. 1.0
    // restores the old fixed-height beam.
    float        navBeaconNearScale = 0.35f;
    std::int32_t navBeaconPinSize = 22;
    float        navBeaconFadeNearUnits = 350.0f;
    bool         navBeaconClearOnArrival = true;
    bool         navBeaconEdgeIndicator = false;
    bool         inputVerboseLog = false;

    bool         cursorVerboseLog = false;

    bool         hudMarkerLog = false;

    bool         beaconVerboseLog = false;
    bool         lootBeamPerfLog = false;

    bool         mapStatsLog = false;

    bool         questRowLog = false;

    bool         mapPinProjectionLog = false;
    // Snapshot pipeline and GPU pixel readback diagnostics. Readback can stall; default off.
    bool         minimapSnapshotLog = false;

    float lootBeamRangeUnits = 2100.0f;
    float lootBeamZBandUnits = 350.0f;

    bool  lootBeamRequireLOS = true;
    bool  lootBeamOcclusion = true;
    bool  navEntityRequireLOS = true;

    bool         lootBeamVisible = true;
    bool         lootBeamHideInCombat = true;
    bool         lootBeamHideAfterLooting = true;
    // Opening the full vanilla container menu marks a chest or corpse inspected.
    // QuickLoot's preview does not open that menu.
    bool         lootBeamHideAfterInspecting = true;
    bool         lootBeamTrackDroppedWeapons = true;
    // 0 = Modern beam, 1 = Wisp. Both menus edit this same value.
    std::int32_t lootBeamDesign = 0;
    // Full pixel widths before interface scaling. Keep each design's original shape.
    float        lootBeamModernBottomWidth = 15.0f;
    float        lootBeamModernTopWidth = 10.0f;
    float        lootBeamModernGroundGlow = 0.5f;
    float        lootBeamWispBottomWidth = 6.0f;
    float        lootBeamWispTopWidth = 6.0f;
    float        lootBeamWispGroundGlow = 0.75f;
    // Loot beam brightness/alpha multiplier (0.1 .. 1.0). Scales the overall
    // alpha of the selected design.
    float        lootBeamBrightness = 0.7f;

    // Loot beam entity subtype toggles (default all true to match legacy IsBeamEligible)
    bool         lootBeamShowCorpseHumanoid = true;
    bool         lootBeamShowCorpseCreature = true;
    bool         lootBeamShowContChest      = true;
    bool         lootBeamShowContStrongbox  = true;
    bool         lootBeamShowContSatchel    = true;
    bool         lootBeamShowLootWeapon     = true;
    bool         lootBeamShowLootArmor      = true;
    bool         lootBeamShowLootGold       = true;
    bool         lootBeamShowLootCoinPurse  = true;
    bool         lootBeamShowLootPotion     = true;
    bool         lootBeamShowLootSoulGem    = true;
    bool         lootBeamShowLootScroll     = true;
    bool         lootBeamShowLootBook       = false;

    // Min gold value threshold for loose loot items (0 = Off / disabled)
    std::int32_t lootBeamMinGoldValue       = 30;

    bool LootBeamAnySubtype() const {
        return lootBeamShowCorpseHumanoid || lootBeamShowCorpseCreature ||
               lootBeamShowContChest || lootBeamShowContStrongbox || lootBeamShowContSatchel ||
               lootBeamShowLootWeapon || lootBeamShowLootArmor ||
               lootBeamShowLootGold || lootBeamShowLootCoinPurse || lootBeamShowLootPotion ||
               lootBeamShowLootSoulGem || lootBeamShowLootScroll || lootBeamShowLootBook;
    }

    bool         hideInBeastForm = true;
    bool         lootBeamHideOwned = true;
    float        lootBeamLengthUnits = 60.0f;
    // Icon drawn at the clipped top of the beam.
    std::int32_t lootBeamIconSize = 32;

    bool navActorAnyVisible() const {
        return navActorShowAlly || navActorShowHostile || navActorShowNeutral || navActorShowAnimal;
    }

    bool navGlanceShowTrail     = true;
    bool navInspectShowTrail    = true;

    // Master switch for interior path tracking recording and rendering.
    bool         enablePathTracking = true;

    bool         pathTrailVisible = true;
    std::int32_t pathTrailColorR = MapTheme::kPathTrailR, pathTrailColorG = MapTheme::kPathTrailG,
                 pathTrailColorB = MapTheme::kPathTrailB, pathTrailColorA = MapTheme::kPathTrailA;
    float        pathTrailThickness = MapTheme::kPathTrailThickness;
    // Older points draw dimmer and smaller, ramped per dot across the complete
    // trail (oldest = origin, newest = head). Recording logic is untouched.
    bool         pathTrailFadeByAge = true;
    bool         pathTrailSmartSpacing = true;
    bool         pathPulseEnabled = true;
    std::int32_t pathPulseColorR = MapTheme::kPathPulseR, pathPulseColorG = MapTheme::kPathPulseG,
                 pathPulseColorB = MapTheme::kPathPulseB;

    float        markerIconScale = MapTheme::kMarkerIconScale;

    std::int32_t pinTintR = 255, pinTintG = 250, pinTintB = 58, pinTintA = 255;
    ImU32 PinTintCol() const { return IM_COL32(pinTintR, pinTintG, pinTintB, pinTintA); }

    bool         iconFx3D = true;
    bool         iconShadow = true;

    std::int32_t markerHoverOffsetX = 25;
    std::int32_t markerHoverOffsetY = 40;

    // Master switch for badge overlays (favorites, notes, cleared dungeons).
    bool         enableBadges    = true;

    float        favBadgeSize    = 9.0f;
    std::int32_t favBadgeColorR = 255, favBadgeColorG = 205, favBadgeColorB = 70, favBadgeColorA = 230;
    float        noteBadgeSize   = 6.0f;
    std::int32_t noteBadgeColorR = 70, noteBadgeColorG = 200, noteBadgeColorB = 190, noteBadgeColorA = 230;

    float        clearedBadgeSize   = 7.0f;
    std::int32_t clearedBadgeColorR = 90, clearedBadgeColorG = 200, clearedBadgeColorB = 110, clearedBadgeColorA = 235;

    // Minimum card width in px at Interface Scale 1.0; 0 = card hugs content.
    bool  markerImageEnabled  = true;
    int   markerImageMinWidth = 0;
    float markerImageAlpha    = 1.0f;

    ImU32 FavBadgeCol()  const { return IM_COL32(favBadgeColorR, favBadgeColorG, favBadgeColorB, favBadgeColorA); }
    ImU32 NoteBadgeCol() const { return IM_COL32(noteBadgeColorR, noteBadgeColorG, noteBadgeColorB, noteBadgeColorA); }
    ImU32 ClearedBadgeCol() const { return IM_COL32(clearedBadgeColorR, clearedBadgeColorG, clearedBadgeColorB, clearedBadgeColorA); }

    ImU32 TitleCol()     const { return IM_COL32(titleColorR, titleColorG, titleColorB, 255); }
    ImU32 BodyCol()      const { return IM_COL32(bodyColorR,  bodyColorG,  bodyColorB,  255); }
    // Hover lift: lerp body 25% toward white so rows still brighten on hover.
    ImU32 BodyColHover() const {
        auto up = [](int c){ return c + (255 - c) * 25 / 100; };
        return IM_COL32(up(bodyColorR), up(bodyColorG), up(bodyColorB), 255);
    }

private:
    Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
};
