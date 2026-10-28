#pragma once
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>

#include "MapTheme.h"
#include "EntityMarkerSource.h"

#include <string>

class Settings {
public:
    // Skyrim's world unit ≈ 1.428 cm, so ~70 units per meter. Used to convert
    // navMarkerRange between the units it is stored in and the meters shown in
    // the INI / settings slider. Only the two range boundaries convert; every
    // distance compare stays in units.
    static constexpr float kUnitsPerMeter = 70.0f;

    static Settings* GetSingleton() {
        static Settings singleton;
        return &singleton;
    }

    void Load();
    void Save();

    // Writes lightweight window/panel view state to INI via a partial SimpleIni
    // transaction. Used for drag-release, active tabs, and the list-panel C-key
    // preference without rewriting unrelated settings.
    void SavePositions();

    // ---- Hotkeys ----------------------------------------------------------
    // Every hotkey is a key + its OWN modifier. One shared modifier cannot serve
    // seven binds. Keys are DirectInput scancodes (DIK, NOT virtual keys) in the
    // SKSE::InputMap keyspace: keyboard 1-255, mouse 256-263, gamepad 266-281.
    // -1 = unbound, dispatch skips any key < 1.
    //
    // Modifier: 0 none, 1 Alt, 2 Ctrl, 3 Shift, 4 LAlt, 5 RAlt, 6 LCtrl,
    // 7 RCtrl, 8 LShift, 9 RShift. Matching is STRICT (see ModifierMatches in
    // plugin.cpp): 0 means no modifier may be held, which is what keeps X and
    // Ctrl+X distinct. Press-to-capture records sided values, so a captured Ctrl
    // bind is 6 or 7; the generic 1-3 are reachable by hand-editing the INI.
    //
    // Marker list panel (world map only) and interior path tracking. Both were
    // hardcoded constexpr scancodes until the hotkey rework.
    std::int32_t listPanelKey      = 0x2E;  // C
    std::int32_t listPanelModifier = 0;
    std::int32_t pathTrackKey      = 0x14;  // T
    std::int32_t pathTrackModifier = 0;

    // Translation file basename under Data/SKSE/Plugins/Translations/
    // Also the font basename: FontManager loads
    // Data/SKSE/Plugins/fonts/<language>.ttf (falls back to Segoe UI Semibold,
    // then the ImGui default). One INI key drives both, like PartySheet.
    std::string language = "CustomMarkers_en";

    // Base font size in px ([Localization] FontSize). HeadingFontSize > FontSize
    // loads a second, larger font (skipped for CJK to save atlas memory).
    std::int32_t fontSize = 18;
    std::int32_t headingFontSize = 28;

    // spdlog threshold name. Valid: trace, debug, info, warn, err, critical, off.
    // Default is "info" so normal play stays quiet. Set to "debug" when
    // diagnosing a CTD or a render gap; the per-event diagnostics in the
    // hover, placement, and projection paths start logging.
    std::string logLevel = "info";

    // Menu settings. int32_t (not uint32_t) so the -1 unbound sentinel and the
    // "< 1" guards work uniformly across all seven hotkeys. SMF provides a way
    // back into the settings when this standalone window hotkey is unbound.
    std::int32_t menuSettingsKey      = 0x2D;  // X
    std::int32_t menuSettingsModifier = 8;
    // Last-open settings tile index (SettingsMenu::Tab). Persisted so reopening
    // the window lands on the tile you left, not always General.
    std::int32_t menuActiveTab = 5;
    std::int32_t menuPosX = 310;
    std::int32_t menuPosY = 300;
    std::int32_t menuWidth = 480;
    std::int32_t menuHeight = 640;

    // GLOBAL text scales for every map-drawn surface: list panel, Map Filters,
    // note dialog, context menus, local-map renderer, and the stat card. Named
    // listPanelTitleScale/listPanelBodyScale until 2026-07-14, which was a lie:
    // they were never list-panel-specific. INI: [Appearance] TitleScale/BodyScale.
    // Title = headings + the stat card's location name. Body = everything else.
    float        titleScale = 0.85f;
    float        bodyScale  = 0.95f;

    // World-map list panel (hotkey C). Drawn hand-rolled in
    // MapMarkerRenderer::DrawListPanel(); these replace the former literals.
    std::int32_t listPanelWidth = 275;   // panel width in px
    // User's explicit C-key visibility preference. No settings-menu control;
    // temporary map/SMF suppression and automatic reveal paths do not change it.
    bool listPanelVisible = true;
    // Top-left screen position. -1 = unset, fall back to old top-right anchor.
    // Hand-drawn panel has no ImGui window, so drag is tracked off MenuCursor.
    std::int32_t listPanelPosX = 1530;
    std::int32_t listPanelPosY = 60;
    // Overview-tab sub-block toggles. Both live on the Overview tab; off also
    // skips that block's per-frame Refresh(), not just its draw.
    bool         listShowOverview     = true;
    bool         listShowActiveQuests = true;
    // Active list-panel tab, persisted so the panel reopens where the player
    // left it. Indexes MapMarkerRenderer::ListTab, which is APPEND-ONLY before
    // Count: inserting a tab mid-list shifts every saved index.
    // Replaced the per-section collapse flags (CollapseCustom/Favorites/Notes/
    // ActiveQuests) -- with one section visible per tab there is nothing to fold.
    std::int32_t listActiveTab            = 0;
    // Panel height band, both % of screen height. The panel sizes to its
    // content between them, so a tab with one row does not collapse the panel
    // to one row: with tabs the content height changes on every switch, and an
    // unclamped floor made the panel jump around as you moved between them.
    // Min is clamped against Max at use, so a Min above Max cannot invert.
    std::int32_t listPanelMinHeightPct    = 35;   // % of screen height
    std::int32_t listPanelMaxHeightPct    = 70;   // % of screen height

    // List panel custom background image. Index is 1-BASED into the sorted
    // ListPanel-*.png set discovered by BackgroundManager; 0 = none picked.
    // ON by default with ListPanel-6: the shipped art is the intended look, and
    // a first-run player should see it without hunting through settings. Falls
    // back to the solid Appearance color on its own if the folder is missing or
    // the index does not resolve (GetBackground returns nullptr).
    bool         listPanelUseCustomBg = false;
    std::int32_t listPanelBgIndex     = 6;

    // Map Filters window (real ImGui window). -1 = unset, ImGui picks default.
    bool         showFilterPanel = true;
    std::int32_t filterPosX = 120;
    std::int32_t filterPosY = 45;
    // Collapsed = icon-only vertical strip, no title/labels/counts/eye. A click
    // toggle, not hover: hover would re-expand every time the cursor crossed the
    // strip while panning the map, and the expanded width would still be
    // reserved. Persisted so the choice survives a session.
    bool         filterCollapsed = true;

    // ---- Stat card (the hover layer over a vanilla marker) -----------------
    // REMOVED 2026-07-14: hoverTextOffsetX/Y, hoverTextAlign, hoverBgR/G/B/A,
    // noteTextOffsetX/Y, noteTextAlign, noteBgR/G/B/A, noteTextWidth,
    // noteTextPadding. Every one of them existed ONLY to dodge vanilla's hover
    // card, which recentred itself on every populate and whose width tracked the
    // location name, so our two floating text blocks had to be nudged around it
    // by hand. We now suppress that card and draw one self-laying-out panel:
    // rows cannot misalign, so there is nothing left to compensate for. Wrap
    // width and padding are panel-internal layout, not user knobs.
    //
    // The panel's colors and scales come from the GLOBAL appearance settings
    // above: titleScale (location name), bodyScale (rows), titleColor*,
    // bodyColor*, BgCol().
    //
    // This is the ONLY stat-card setting. It stays separate from bodyScale
    // because notes are free text and can run long, so they want to be smaller
    // than the card's other rows.
    float        noteTextScale = 0.85f;

    // Stat-card placement relative to the hovered pin. Direction-from-pin:
    // 0 = Top (above), 1 = Bottom (below), 2 = Left, 3 = Right. Default Top
    // matches the old fixed above-pin behavior. OffsetX/Y nudge on top of the
    // chosen direction (screen px). Read live by HoverImageRenderer::Draw.
    std::int32_t statCardAnchor  = 3;
    std::int32_t statCardOffsetX = 20;
    std::int32_t statCardOffsetY = 0;

    // Global text colors for the map-drawn panels (list panel + Map Filters).
    // Title = section/panel headings only; Body = all other row/label text.
    // RGB only, no alpha. Defaults match MapTheme::kHeaderText / kBodyText.
    std::int32_t titleColorR = 255, titleColorG = 220, titleColorB = 140; // gold
    std::int32_t bodyColorR  = 236, bodyColorG  = 210, bodyColorB  = 160; // warm off-white

    // Unified panel background, full ARGB. One color reused by every map-drawn
    // panel (list panel, Map Filters). Alpha lives in the A field. Default =
    // MapTheme::kPanelBg (18,16,12,245). Replaces the former per-panel bg colors.
    std::int32_t bgColorR = 18, bgColorG = 16, bgColorB = 12, bgColorA = 255;

    // Unified panel border, full ARGB. Sibling of the background above: one color
    // for every bordered surface (list panel, Legend, Inspect overlay, stat card,
    // local-map hover panel) plus the BASE of the Glance rim, which still lerps
    // toward MapTheme::kCombatBorder in combat. Default = MapTheme::kPanelBorder
    // (140,115,65,255). Border WIDTH stays fixed (MapTheme::kPanelBorderW).
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

    // Path-tracking trail (local map + Navigation Overlay). MapDraw::DrawPathTrail
    // reads these live each frame, so the same controls drive LocalMap, Glance,
    // and Inspect. Color defaults to MapTheme::kPathTrail*.
    // Alpha lives in the color itself (ColorEdit4 alpha channel) rather than a
    // separate slider, so one RGBA value drives every trail draw call (dots,
    // diamonds, segment caps, and head cone). Thickness = dot radius in px.
    // Visibility toggle only affects drawing; PathTracker keeps recording
    // regardless so the trail is intact if re-enabled mid-session.
    // Four independent visibility toggles (2026-07-17), replacing the former
    // navGlanceVisible/navGlanceExteriorVisible master pair: Glance and Inspect
    // are gated separately per interior/exterior so e.g. Inspect can work
    // outdoors with the Glance radar turned off outdoors (the old master bug --
    // GlanceExteriorVisible bailed the WHOLE frame build, killing Inspect too).
    // BuildFrame only early-returns when NONE of the four apply to the current
    // environment; the draw calls in Draw() gate individually off these.
    bool navGlanceInteriorVisible  = true;
    bool navGlanceExteriorVisible  = true;
    bool navInspectInteriorVisible = true;
    bool navInspectExteriorVisible = true;
    // DIK, not VK. Polled during gameplay through GetAsyncKeyState, which needs
    // a VK, so the poll sites convert with InputHandler::DIKToVK. That is also
    // why a mouse or gamepad bind cannot drive this key: GetAsyncKeyState cannot
    // see either device.
    //
    // Shares its default (N) with lookPinKey, separated only by Look & Pin's
    // Ctrl. That works ONLY because ModifierMatches is strict; a regression to
    // the old loose check fires both.
    std::int32_t navInspectKey        = 0x31;  // N
    std::int32_t navInspectModifier   = 0;
    std::int32_t navInspectActivation = 0;     // 0 = Hold, 1 = Toggle
    // Explicit top-left origin of the Glance disc, screen px. No auto sentinel:
    // defaults land the disc top-right on a 1920x1080 display (1920 - size - 22).
    int navGlancePosX = 1625;
    int navGlancePosY = 25;
    int navGlanceSize = 190;
    int navGlanceShape = 0;  // 0 = Circle, 1 = Square
    int navInspectSizePct = 72;
    bool navRefreshOnInspect = true;

    // Inspect view-mode toggle. The overlay has two framings, switched by a key
    // held while Inspect is shown: Cell (cover-fit the whole cell) and Radar
    // (player-centred crop, InspectRadarRangeUnits wide). InspectDefaultMode
    // (0=Cell, 1=Radar) seeds the runtime mode each session; toggling is
    // in-memory only and never writes the INI. Toggle key default R (0x52).
    // DIK (see navInspectKey above for why these are not VKs).
    std::int32_t navInspectModeToggleKey      = 0x13;  // R
    std::int32_t navInspectModeToggleModifier = 0;
    int navInspectDefaultMode = 0;                     // 0 = Cell, 1 = Radar
    float navInspectRadarRangeUnits = 3000.0f;       // Radar frustum width, world units

    // Quick-mark: drop a default-named custom marker at the player's position
    // while Inspect is held, no note dialog. DIK (see navInspectKey above);
    // MinimapRenderer::BuildFrame polls it through InputHandler::DIKToVK and
    // InputHook swallows the press so vanilla toggle-POV (default F) never fires
    // during gameplay.
    std::int32_t navQuickMarkKey      = 0x21;  // F
    std::int32_t navQuickMarkModifier = 0;

    // Runtime-only visibility toggles. The bindings persist, but their current
    // on/off state resets to visible when the plugin starts. Default unbound so
    // existing users keep the same controls until they choose keys.
    std::int32_t navGlanceToggleKey       = -1;
    std::int32_t navGlanceToggleModifier  = 0;
    std::int32_t lootBeamToggleKey        = -1;
    std::int32_t lootBeamToggleModifier   = 0;

    // Look & Pin: outdoors, the bind toggles a mode that raycasts from the
    // camera and pins whatever the player is looking at. Own [LookPin] INI
    // section, NOT [NavigationOverlay]: it is a gameplay-world feature, not a
    // radar surface, and a key read from the wrong section silently keeps its
    // default.
    //
    // Ctrl is no longer hardcoded, it is this bind's stored modifier, which is
    // what lets Ctrl+N here and plain N (navInspectKey) coexist. Both default to
    // N and rely on the strict ModifierMatches to stay apart. DIK, polled via
    // InputHandler::DIKToVK like the nav keys above.
    bool         lookPinEnabled  = true;
    std::int32_t lookPinKey      = 0x31;  // N
    std::int32_t lookPinModifier = 2;     // Ctrl

    // Glance radar zoom: world distance from disc center to rim. Scale is derived
    // (kGlanceRadius / navGlanceRangeUnits) rather than a fixed px-per-unit constant,
    // so this single knob controls both the trail/marker/entity projection AND the
    // snapshot frustum pad in RenderOneSnapshot's Glance branch (smaller range =
    // tighter pad = sharper backdrop, automatic). [NavigationOverlay] GlanceRangeUnits.
    float navGlanceRangeUnits = 1200.0f;

    // Entity layer sub-type toggles, FLAT per view (Glance / Inspect), 2026-07-17
    // restructure #2. Interior/exterior no longer factors into entity filtering
    // at all -- the former 20 per-context category bools (Int/Ext x Glance/
    // Inspect x 5 categories) AND the 16 global Container/LooseLoot subtype
    // bools are GONE, replaced by ONE flat set of 22 subtype bools per view.
    // A user who doesn't want barrels unchecks barrels; there's no separate
    // "indoors" and "outdoors" copy to keep in sync. Defaults are all-on: this
    // is a strict superset of what used to draw, so nothing that showed before
    // silently disappears on upgrade (the old Ext defaults being off/on per
    // view is gone with them -- simplest starting point is show everything).
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

    // Entity glyph/icon size, screen px, per mode (Glance's tiny disc radar vs
    // Inspect's large overlay want very different scales). Glance draws vector
    // glyphs sized off this; Inspect draws PNG icons boxed to this side length.
    // Shared across Interior/Exterior (only the category on/off split per
    // environment, not the size).
    std::int32_t navGlanceEntityIconSize  = 14;
    std::int32_t navInspectEntityIconSize = 20;

    // Custom-pin diameter on the Navigation Overlay only (Glance disc + Inspect
    // overlay), screen px. Deliberately separate from markerIconScale (the
    // world/local map pin scale factor): the overlay pins want their own knob
    // sized in the same px-diameter units as the entity icon sliders above, not
    // a scale multiplier on the map's base pin art.
    std::int32_t navGlancePinSize  = 14;
    std::int32_t navInspectPinSize = 18;

    // Per-view subtype gate. Takes BOTH category and subCategory because a
    // marker can carry sub == None (e.g. ClassifyContainer found no keyword
    // match): in that case there is no specific bool to check, so it falls
    // back to "is any subtype of this marker's category enabled" -- an
    // unclassified barrel-shaped-but-unknown container should still obey the
    // Containers umbrella rather than silently always drawing or never
    // drawing. Corpse/Door/Ingredient each have exactly 2 subs (Humanoid/
    // Creature, Load/Regular, Loose/Plant) with no "unclassified" case in
    // EntityMarkerSource, but they're routed through the same OR-fallback for
    // symmetry -- costs nothing and stays correct if a third sub is added later.
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

    // Any-of helpers over all 22 subtypes per view, used wherever a single "is
    // any entity at all wanted" gate is needed (the cell-walk trigger in
    // RenderOneSnapshot -- draw-site filtering happens per-marker via
    // Glance/InspectSubEnabled above). No longer takes an exterior param: since
    // the flat restructure, interior/exterior no longer factors into entity
    // filtering at all.
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

    // Actor radar layer (transient, no persistence -- see ActorMarkerSource.h).
    // One flat set of 3 allegiance toggles, shared by Glance AND Inspect (unlike
    // the entity subtype toggles, which are per-view): the spec only asks for
    // one radar-wide on/off per allegiance class, not independent Glance/Inspect
    // control, so a second copy would be unused surface area. Neutral defaults
    // ON (user call 2026-07-17, spec-findings-Actor-Radar-Layer.md Open
    // Question 3 -- overrides the spec text's original "possibly off").
    bool navActorShowAlly     = true;
    bool navActorShowHostile  = true;
    bool navActorShowNeutral  = true;
    bool navActorShowAnimal   = true;   // non-hostile wildlife dots (grey)
    // Actor dot radius in px, per view (Glance radar vs Inspect overlay), same
    // split reason as navGlancePinSize/navInspectPinSize: the two views are
    // different scales. Defaults match the old hardcoded 5.0 / 7.5.
    std::int32_t navGlanceActorDotSize  = 5;
    std::int32_t navInspectActorDotSize = 8;
    // World units within which an actor is "in proximity": scanned, live-tracked.
    // Beyond it the dot freezes and fades. Default matches the old
    // ActorMarkerSource::kActorProximityRadius constant (~one room).
    float navActorProximityUnits = 1200.0f;
    // World units within which a custom marker draws on the navigation overlay
    // (Glance + Inspect) and the HUD compass. Beyond it the marker is hidden (no
    // rim-clamp). One global range for all three. Stored INTERNALLY in units (all
    // consumers square it in units); the INI and the settings slider express it
    // in METERS (easier for users), converted via kUnitsPerMeter at those two
    // boundaries only. Default ~1000m so 50-100 saved markers don't clutter the
    // compass while everything nearby still shows.
    float navMarkerRange = 98000.0f;  // 1400 m * 70
    // Max active quest markers drawn on the Navigation overlay (Glance + Inspect).
    // COUNT cap, not a distance gate: quest targets are never range-gated. questTargets
    // arrive in engine/compass priority order, so the first N are the most relevant.
    // 0 = draw none. Default 2 (most players run one quest + maybe one miscellaneous).
    std::int32_t navMaxQuestMarkers = 2;
    // Max CUSTOM markers drawn on the GLANCE radar and the HUD COMPASS. Inspect is
    // deliberately NOT capped: the radar is a small disc that rim-clamps out-of-view
    // pins, so a dense area piles them on the edge and the cap is what keeps it
    // readable, and the compass is a ~180 degree strip that smears the same way.
    // Inspect is a full-cell overlay where every pin sits at its real position and
    // crowding never happens, so capping there would only hide real information.
    // (Extended to the compass 2026-08-27. The knob was always meant to be the
    // global "how many custom pins at once" answer; Inspect is the one surface whose
    // geometry makes it not apply. The compass applies it EXTERIOR only -- interior
    // compass markers match by cellID, not range, so there is no distance to rank.)
    //
    // COUNT cap layered on top of navMarkerRange. Kept are the N NEAREST by XY
    // distance (the marker list is cosave order, so an unsorted "first N" would
    // drop markers arbitrarily as the player walks). Destination markers are exempt
    // and do not consume a slot, same as the range gate. 0 = NO cap (unlike
    // MaxQuestMarkers, where 0 means none -- hiding every custom marker is what the
    // range slider is for, so 0 here is the "unlimited" end of the slider).
    std::int32_t navMaxCustomMarkers = 5;

    // In-world 3D destination beacon and screen-space waypoint system (spec-findings-World-Beacons.md).
    // Draws active destination beacons in the 3D gameplay view with vertical light pillars.
    bool         navBeaconVisible = true;
    // FAR height. The beam scales down toward navBeaconNearScale as the player
    // approaches, so this is the height seen at range, not a constant.
    float        navBeaconPillarHeight = 250.0f;
    // Fraction of the far height used when standing on the destination. 1.0
    // restores the old fixed-height beam.
    float        navBeaconNearScale = 0.35f;
    std::int32_t navBeaconPinSize = 22;
    // Outer proximity-fade radius when auto-clear is off. Clear on Arrival uses
    // the same value as its arrival radius and clears before the fade is drawn.
    float        navBeaconFadeNearUnits = 350.0f;
    // Clear the active destination once its horizontal distance enters the Fade
    // Near radius. Temporary Look & Pin/Inspect markers are deleted; manual and
    // vanilla markers persist after losing destination status.
    bool         navBeaconClearOnArrival = true;
    // Off-screen edge arrow pointing at the destination. Default off: the
    // compass and Glance radar already convey off-screen direction, so a third
    // indicator reads as clutter for a single destination. Useful when a marked
    // ITEM (no tall pillar to spot) is the destination.
    bool         navBeaconEdgeIndicator = false;
    // Input diagnostics, off by default: Skyrim characters/buttons, Windows
    // messages, ImGui processing/focus, and note edits/wrapping. Character
    // codepoints include typing outside our fields while this is enabled.
    bool         inputVerboseLog = false;

    // Cursor ownership diagnostics, off by default. Samples the Scaleform
    // CursorMenu movie while MapMenu is open and logs only state transitions
    // plus an end-of-session summary, so the check can run every frame without
    // flooding the ordinary debug log.
    bool         cursorVerboseLog = false;

    // Per-beacon ground-Z resolution logging. Off by default: it logs once per
    // marker per cell entry, not per frame, but it is pure diagnostics. The
    // playerZ-fallback warning logs regardless, since that path is a real bug
    // signal (the beacon tracks the player's altitude when it is hit).
    bool         beaconVerboseLog = false;
    // Loot beam perf instrumentation. SEPARATE key from beaconVerboseLog on
    // purpose: that one is the destination beacon's ground-Z//visibility
    // diagnostics, and sharing it meant turning on perf timing also flooded the
    // log with beacon lines. Kept permanently rather than added ad-hoc each time
    // -- it answered two real questions (which gate limits beams, what Max Beams
    // costs) and re-adding it from scratch is the expensive part.
    // Off by default: it logs once per collect (1 Hz) plus a 300-frame summary.
    // Also gates the per-ref crime-check log behind lootBeamHideOwned, which
    // fires per eligible ref per collect and is chattier than the perf lines.
    bool         lootBeamPerfLog = false;

    // Overview (MapStats) diagnostics. Off by default; logs ONCE per worldspace
    // change, not per frame, because Refresh's world-changed guard short-
    // circuits every same-world call. Answers the two questions the Overview
    // block keeps raising on modded/DLC maps: how many clearable locations get
    // dropped because worldLocMarker never resolved, and which MARKER_TYPE
    // values the current map actually uses (the stones counter matched only
    // kDoomstone=18 and read 0/0 on Solstheim, whose All-Maker Stones are
    // kDLC02StandingStone=55).
    bool         mapStatsLog = false;

    // Active Quests row diagnostics. Off by default. Logs one line per tracked
    // quest per Refresh (tens of quests, once per list-panel frame) with the
    // resolved worldSpaceID and which of the three position sources answered,
    // plus whether the worldspace filter kept the row. Exists because a row
    // silently vanishing and a row silently unclickable look identical on
    // screen: both are "the quest isn't there", and only the source + world
    // tells them apart.
    bool         questRowLog = false;

    // World-map pin projection diagnostics. Off by default. Logs one line per
    // custom marker per map frame: selected camera mode, stored/used Z, the raw
    // projected sx/sy/sz, and which gate rejected it (behind-camera vs
    // depth-invalid vs off-screen).
    //
    // A wrong Z does not fail loudly: WorldPtToScreenPt3 can return true with a
    // displaced point. The frame header records the live camera look vector,
    // selected projection mode, and WRLD map values for profile comparisons.
    bool         mapPinProjectionLog = false;
    // Snapshot pipeline and GPU pixel readback diagnostics. Readback can stall; default off.
    bool         minimapSnapshotLog = false;

    // Loot beam distance gates. Two axes, two problems, neither covers the other:
    // an item directly below your feet has XY distance ~0, and an item across the
    // room on your own floor has Z delta ~0.
    //
    // XY radius: a world beam has no frame to encode distance in, unlike a radar
    // icon that sits near the rim and reads as far. Without this the cap of 8 just
    // takes the 8 nearest however distant they are, so a sparse cell beams across
    // the whole room.
    float lootBeamRangeUnits = 2100.0f;
    // Z band: half-height, so the beam draws within +/- this of the player's Z.
    // Measured interior floor gaps are 253 (inn) to 750 (palace) and inconsistent
    // (reference_interior_floor_z_gaps), so no value is correct everywhere. 200
    // errs toward hiding, which is the right error for a beam painted over the
    // gameplay view: a missed beam is invisible, a wrong one is in your face.
    // Costs a real corpse mid-staircase. Accepted.
    float lootBeamZBandUnits = 350.0f;

    // Discover on Sight: require clear line of sight before an item is first
    // discovered/admitted in the 1Hz collect walk. Once discovered, stays discovered.
    bool  lootBeamRequireLOS = true;
    // Hide Behind Walls: live 4Hz Havok raycast occlusion in LootBeamRenderer::Draw().
    // When enabled, beams dim and cull behind solid geometry. When disabled, beams
    // render through walls.
    bool  lootBeamOcclusion = true;
    // Same first-reveal LOS gate for the Glance radar and Inspect overlay. Kept
    // SEPARATE from lootBeamRequireLOS so the fog can be wanted on one surface
    // and not the other. Note this one knob covers both radar surfaces: they
    // share a discovered set, so they cannot be gated independently.
    bool  navEntityRequireLOS = true;

    // Loot beam master gate. Beams drew unconditionally in the first pass; this
    // is the off switch the GUI needed. Destination beacons and loot beams are
    // separate features that happen to share a draw helper, so they get separate
    // toggles rather than one "beacons" switch.
    bool         lootBeamVisible = true;
    // Hide loot beams while the player is in combat. Default ON: beams over corpses
    // and containers is pure clutter exactly when the view must stay readable.
    // Loot is not going anywhere; the beams return when the fight ends.
    bool         lootBeamHideInCombat = true;
    // Session-only holder state. Taking one item from a chest or corpse marks it
    // looted; when enabled, its beam stays hidden even if other items remain.
    // Opening the loot menu without taking anything does not mark it.
    bool         lootBeamHideAfterLooting = true;
    // Loot beam brightness/alpha multiplier (0.1 .. 1.0). Scales the overall
    // alpha of the beam, ground rings, and badge.
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
    bool         lootBeamShowLootPotion     = true;
    bool         lootBeamShowLootSoulGem    = true;
    bool         lootBeamShowLootScroll     = true;

    // Min gold value threshold for loose loot items (0 = Off / disabled)
    std::int32_t lootBeamMinGoldValue       = 30;

    bool LootBeamAnySubtype() const {
        return lootBeamShowCorpseHumanoid || lootBeamShowCorpseCreature ||
               lootBeamShowContChest || lootBeamShowContStrongbox || lootBeamShowContSatchel ||
               lootBeamShowLootWeapon || lootBeamShowLootArmor ||
               lootBeamShowLootGold || lootBeamShowLootPotion ||
               lootBeamShowLootSoulGem || lootBeamShowLootScroll;
    }

    // Hide every gameplay-view overlay (radar, loot beams, world beacons,
    // Look & Pin) while the player is a Werewolf or Werebear. One shared knob:
    // beast form is an all-or-nothing state, so per-overlay toggles would be
    // four checkboxes for one decision. Vampire Lord is NOT affected — see
    // include/BeastForm.h for why.
    bool         hideInBeastForm = true;
    // Hide loot beams over refs whose activation would be considered a crime (theft).
    // Uses TESObjectREFR::IsCrimeToActivate(), so it folds in faction membership,
    // purchased housing and quest access rather than raw ownership.
    //
    // Default ON. The risk that kept this OFF initially was corpses: if the crime
    // test returned true on bodies it would silently kill the best beam category.
    // Measured in-game 2026-08-20 and it does not -- four CorpseCreature refs
    // (001060D6/D7/D8, 000DC57A) all read isCrime=false, as did an unowned
    // wilderness chest (000DC579), while Whiterun's stall wares and its owned
    // chest (00075163) read true. The filter keys on ownership, not category, so
    // dungeon looting is unaffected and towns stop lighting up every stall.
    bool         lootBeamHideOwned = true;
    // Beam length, world units. Short by design: the destination pillar's 250+ is
    // a sky beam seen across a valley, this sits ON the item. Roughly
    // waist-to-head above it.
    float        lootBeamLengthUnits = 60.0f;
    // Icon drawn at the clipped top of the beam.
    std::int32_t lootBeamIconSize = 22;

    bool navActorAnyVisible() const {
        return navActorShowAlly || navActorShowHostile || navActorShowNeutral || navActorShowAnimal;
    }

    // Per-mode trail draw gate: Glance (small HUD radar) vs Inspect (large
    // overlay) shown/hidden independently. Drawing only; PathTracker recording
    // is untouched so toggling reacts instantly without a refresh.
    bool navGlanceShowTrail     = true;
    bool navInspectShowTrail    = true;

    // Master switch for interior path tracking recording and rendering.
    bool         enablePathTracking = true;

    // Local-map (interior M+L view) trail visibility only. Glance/Inspect
    // have their own toggles (navGlanceShowTrail/navInspectShowTrail) above.
    // PathTracker keeps recording regardless of any of these — visibility
    // toggles never touch capture, so turning a view back on shows the
    // full trail immediately instead of just what was drawn from that point.
    bool         pathTrailVisible = true;
    std::int32_t pathTrailColorR = MapTheme::kPathTrailR, pathTrailColorG = MapTheme::kPathTrailG,
                 pathTrailColorB = MapTheme::kPathTrailB, pathTrailColorA = MapTheme::kPathTrailA;
    float        pathTrailThickness = MapTheme::kPathTrailThickness;
    // Older points draw dimmer and smaller, ramped per dot across the complete
    // trail (oldest = origin, newest = head). Recording logic is untouched.
    bool         pathTrailFadeByAge = true;
    bool         pathTrailSmartSpacing = true;
    // Head-cap pulse (the "you are here" ring at the trail's newest point).
    // Off = static cap, same radius, no sin() animation. Color independent of
    // the trail dot color so it can stay visually distinct as a status marker.
    bool         pathPulseEnabled = true;
    std::int32_t pathPulseColorR = MapTheme::kPathPulseR, pathPulseColorG = MapTheme::kPathPulseG,
                 pathPulseColorB = MapTheme::kPathPulseB;

    // Custom marker pins (world + local map). MapDraw::DrawPin reads
    // markerIconScale live via a scale param, shared by both renderers so
    // pin size stays consistent between maps. Does not touch vanilla marker
    // rendering, compass icons, or the fixed hit box in MapDraw::PinBox
    // (visual size only, not click area).
    float        markerIconScale = MapTheme::kMarkerIconScale;

    // Custom-pin icon tint. RGB picks the color, ALPHA is the strength/on-off:
    // DrawPin lerps white -> this color by alpha/255, so alpha 0 = pure white
    // multiply = icon art shows true (disabled), alpha 255 = full tint. The
    // icon never fades out because white (not transparency) is the "off" end.
    // Colored icons get muddied as alpha rises (multiply); white/mono icons
    // take the color cleanly. Default alpha 0 = off, unchanged for existing users.
    std::int32_t pinTintR = 255, pinTintG = 250, pinTintB = 58, pinTintA = 255;
    ImU32 PinTintCol() const { return IM_COL32(pinTintR, pinTintG, pinTintB, pinTintA); }

    // "3D Icons": drop shadow + vertical duotone on every PNG icon we draw
    // (custom pins on world/local/Glance/Inspect, plus the Glance/Inspect entity
    // icons). One switch for both passes on purpose: duotone WITHOUT the shadow
    // reads as a flat gradient smear rather than a lifted icon, so exposing them
    // separately only offers a combination that looks broken.
    //
    // Duotone REPLACES the art's RGB and keeps only its alpha, so it suits the
    // white/mono icon set this mod ships. Colored art would lose its internal
    // colors — if that ever changes, pass duotone=false to MapDraw::DrawIconFx
    // for the affected call site rather than turning the whole thing off.
    bool         iconFx3D = true;

    // Custom-pin info-only hover panel position nudge, screen px, on top of
    // the default anchor (MapDraw::ComputeInfoPanel's hardcoded +14 X / -8 Y
    // above-right-of-pin placement). 0,0 = unchanged default behavior. Shared
    // by MapMarkerRenderer (world map) and LocalMapRenderer (local map).
    std::int32_t markerHoverOffsetX = 25;
    std::int32_t markerHoverOffsetY = 40;

    // Master switch for badge overlays (favorites, notes, cleared dungeons).
    bool         enableBadges    = true;

    // Favorite/note badges drawn over vanilla markers (MapMarkerRenderer,
    // world map only — see kFavBadgeFill/kNoteBadgeFill comment in MapTheme).
    // Outline stays fixed (MapTheme::kFavBadgeOutline/kNoteBadgeOutline);
    // only fill color + radius are user-configurable.
    float        favBadgeSize    = 9.0f;
    std::int32_t favBadgeColorR = 255, favBadgeColorG = 205, favBadgeColorB = 70, favBadgeColorA = 230;
    float        noteBadgeSize   = 6.0f;
    std::int32_t noteBadgeColorR = 70, noteBadgeColorG = 200, noteBadgeColorB = 190, noteBadgeColorA = 230;

    // Cleared-dungeon badge (MapMarkerRenderer, world map only — see
    // kClearedBadgeFill comment in MapTheme). Outline stays fixed
    // (MapTheme::kClearedBadgeOutline); only fill color + radius are
    // user-configurable, same pattern as favBadge*/noteBadge*.
    float        clearedBadgeSize   = 7.0f;
    std::int32_t clearedBadgeColorR = 90, clearedBadgeColorG = 200, clearedBadgeColorB = 110, clearedBadgeColorA = 235;

    // Marker images (HoverImageRenderer, PNG above the stat card, keyed on
    // FormID via CMM_MarkerImages.json). Read live every frame, so toggling
    // takes effect without a map reopen. Width is a fraction of the panel:
    // 1.0 matches the card edge-to-edge, lower insets it and keeps it centred.
    bool  markerImageEnabled = true;
    float markerImageWidth   = 1.0f;
    float markerImageAlpha   = 1.0f;

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
