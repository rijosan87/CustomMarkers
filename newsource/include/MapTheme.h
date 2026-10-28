// include/MapTheme.h
// Central theme tokens for all Custom Markers ImGui surfaces.
// Change a value here and it propagates everywhere.
#pragma once
#include <imgui.h>
#include <cstdint>

namespace MapTheme {

    // -------------------------------------------------------------------------
    // Panel background / border
    // -------------------------------------------------------------------------
    constexpr ImU32 kPanelBg         = IM_COL32( 18,  16,  12, 245);  // near-black parchment
    constexpr ImU32 kPanelBorder     = IM_COL32(140, 115,  65, 255);  // dark gold
    constexpr ImU32 kPanelBorderHover= IM_COL32(200, 170,  90, 255);  // lighter gold on hover
    // 0 = square corners everywhere. Shared by every panel that draws a frame
    // (both stat cards, list panel, note dialog, Legend, minimap panels, Inspect
    // toasts, settings window), so changing it re-styles the whole UI at once --
    // that is the point of the token. Was 5.0f.
    constexpr float kPanelRounding   = 0.0f;
    constexpr float kPanelBorderW    = 1.5f;

    // Header / section labels
    // kHeaderText / kBodyText are the DEFAULTS for Settings' titleColor/bodyColor,
    // not draw-site colors. Any panel heading draws with settings->TitleCol() and
    // any panel row with settings->BodyCol()/BodyColHover(), or Appearance >
    // Global Styles silently doesn't reach it (the Legend's category labels and
    // the custom-pin hover header both stayed gold that way until 2026-08-19).
    constexpr ImU32 kHeaderText      = IM_COL32(255, 220, 140, 255);  // warm gold
    constexpr ImU32 kSeparator       = IM_COL32(140, 115,  65, 180);

    // Note / body text
    constexpr ImU32 kBodyText        = IM_COL32(230, 220, 200, 255);  // warm off-white
    constexpr ImU32 kBodyTextHover   = IM_COL32(255, 245, 210, 255);
    // Muted stays fixed on purpose: it marks placeholder ("(no markers)") and
    // disabled state, which should NOT track the user's body color.
    constexpr ImU32 kMutedText       = IM_COL32(160, 150, 130, 255);

    // -------------------------------------------------------------------------
    // Row hover band (list panel)
    // -------------------------------------------------------------------------
    constexpr ImU32 kRowHoverBg      = IM_COL32(140, 115,  65,  55);

    // -------------------------------------------------------------------------
    // List-panel tab strip
    //
    // Without a tray the strip read as a caption line, not a control: only the
    // active cell was painted, so the other tabs were bare text on the panel
    // background. All four alphas are deliberately low -- these composite OVER
    // the user's Appearance panel color, so they must tint it, never replace it
    // (a hardcoded opaque tray would clash with a red-themed panel).
    //
    // Ordering matters: kTabActiveBg must stay clearly stronger than
    // kTabHoverBg, or hovering an inactive tab looks identical to selecting it.
    // That was the original bug -- both used kRowHoverBg.
    // -------------------------------------------------------------------------
    constexpr ImU32 kTabTrayBg       = IM_COL32(  0,   0,   0,  60);  // recess behind the whole strip
    constexpr ImU32 kTabDivider      = IM_COL32(140, 115,  65,  70);  // between cells, shares the edge
    constexpr ImU32 kTabHoverBg      = IM_COL32(140, 115,  65,  40);  // inactive cell under cursor
    constexpr ImU32 kTabActiveBg     = IM_COL32(140, 115,  65,  90);  // selected cell

    // -------------------------------------------------------------------------
    // Remove / delete button  (red)
    // -------------------------------------------------------------------------
    constexpr ImU32 kRemoveBg        = IM_COL32( 80,  30,  30, 220);
    constexpr ImU32 kRemoveBgHover   = IM_COL32(170,  45,  45, 255);
    constexpr ImU32 kRemoveBorder    = IM_COL32(  0,   0,   0, 180);
    constexpr ImU32 kRemoveText      = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kRemoveRowText   = IM_COL32(220,  80,  80, 255);  // red label in menu row
    constexpr ImU32 kRemoveRowIcon   = IM_COL32(200,  70,  70, 255);
    constexpr float kBtnRounding     = 3.0f;
    constexpr float kBtnBorderW      = 1.0f;

    // -------------------------------------------------------------------------
    // Destination button  (blue)
    // -------------------------------------------------------------------------
    constexpr ImU32 kDestBg          = IM_COL32( 35,  55,  90, 220);
    constexpr ImU32 kDestBgHover     = IM_COL32( 60, 110, 175, 255);
    constexpr ImU32 kDestBorder      = IM_COL32(  0,   0,   0, 180);
    constexpr ImU32 kDestText        = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kDestRowBg       = IM_COL32( 30,  60, 105,  55);
    constexpr ImU32 kDestRowBgHover  = IM_COL32( 45,  85, 145,  95);
    constexpr ImU32 kDestBadgeTint   = IM_COL32(255, 255, 255, 255);

    // -------------------------------------------------------------------------
    // Edit Note button  (teal/green)
    // -------------------------------------------------------------------------
    constexpr ImU32 kEditBg          = IM_COL32( 30,  65,  55, 220);
    constexpr ImU32 kEditBgHover     = IM_COL32( 50, 120,  90, 255);
    constexpr ImU32 kEditBorder      = IM_COL32(  0,   0,   0, 180);
    constexpr ImU32 kEditText        = IM_COL32(255, 255, 255, 255);

    // -------------------------------------------------------------------------
    // Popup menus (terrain + vanilla marker right-click)
    // -------------------------------------------------------------------------
    // These are passed to ImGui::PushStyleColor so they use ImVec4 (0..1).
    inline ImVec4 kPopupBg()         { return ImVec4(0.07f, 0.06f, 0.05f, 0.97f); }
    inline ImVec4 kPopupBorder()     { return ImVec4(0.55f, 0.45f, 0.25f, 1.0f);  }
    inline ImVec4 kPopupHeaderText() { return ImVec4(1.0f,  0.86f, 0.55f, 1.0f);  }
    inline ImVec4 kPopupText()       { return ImVec4(0.90f, 0.86f, 0.78f, 1.0f);  }
    inline ImVec4 kPopupHoverBg()    { return ImVec4(0.55f, 0.45f, 0.25f, 0.40f); }
    inline ImVec4 kPopupHoverText()  { return ImVec4(1.0f,  1.0f,  0.90f, 1.0f);  }

    // -------------------------------------------------------------------------
    // Note dialog (ImGui::Begin window)
    // -------------------------------------------------------------------------
    inline ImVec4 kDlgBg()           { return ImVec4(0.07f, 0.06f, 0.05f, 1.0f);  }
    inline ImVec4 kDlgBorder()       { return ImVec4(0.55f, 0.45f, 0.25f, 1.0f);  }
    inline ImVec4 kDlgFrameBg()      { return ImVec4(0.14f, 0.12f, 0.09f, 1.0f);  }
    inline ImVec4 kDlgBtnBg()        { return ImVec4(0.27f, 0.22f, 0.14f, 1.0f);  }
    inline ImVec4 kDlgBtnHover()     { return ImVec4(0.55f, 0.45f, 0.25f, 1.0f);  }
    inline ImVec4 kDlgBtnActive()    { return ImVec4(0.70f, 0.58f, 0.32f, 1.0f);  }
    inline ImVec4 kDlgText()         { return ImVec4(0.90f, 0.86f, 0.78f, 1.0f);  }
    inline ImVec4 kDlgTitleText()    { return ImVec4(1.0f,  0.86f, 0.55f, 1.0f);  }
    inline ImVec4 kDlgCheckMark()    { return ImVec4(0.70f, 0.58f, 0.32f, 1.0f);  }

    // Interactive widgets (sliders, combos, scrollbar, collapsing headers).
    // Shared by the settings window so its controls read as gold-trimmed
    // parchment, not ImGui's default blue debug chrome. Grab = the moving
    // slider handle; the brighter active/hover values are the gold "touch".
    inline ImVec4 kDlgGrab()         { return ImVec4(0.62f, 0.50f, 0.28f, 1.0f);  }  // slider handle
    inline ImVec4 kDlgGrabActive()   { return ImVec4(0.85f, 0.70f, 0.38f, 1.0f);  }  // dragging
    inline ImVec4 kDlgHeaderBg()     { return ImVec4(0.20f, 0.16f, 0.10f, 1.0f);  }  // collapsing header rest
    inline ImVec4 kDlgHeaderHover()  { return ImVec4(0.34f, 0.27f, 0.15f, 1.0f);  }
    inline ImVec4 kDlgHeaderActive() { return ImVec4(0.44f, 0.35f, 0.19f, 1.0f);  }
    inline ImVec4 kDlgScrollBg()     { return ImVec4(0.10f, 0.08f, 0.06f, 0.6f);  }
    inline ImVec4 kDlgScrollGrab()   { return ImVec4(0.40f, 0.33f, 0.19f, 1.0f);  }
    inline ImVec4 kMutedTextV()      { return ImVec4(0.63f, 0.59f, 0.51f, 1.0f);  }  // muted hint text (ImVec4 twin of kMutedText)

    // -------------------------------------------------------------------------
    // Exit-door beacon panel (local map info-only pin)
    // -------------------------------------------------------------------------
    constexpr ImU32 kBeaconBg        = IM_COL32( 18,  16,  12, 245);
    constexpr ImU32 kBeaconBorder    = IM_COL32( 80, 130, 200, 255);  // blue to signal navigation
    constexpr ImU32 kBeaconText      = IM_COL32(220, 235, 255, 255);

    // -------------------------------------------------------------------------
    // Path trail (local map). Defaults for Settings::pathTrail*; user can
    // retheme via the Path Tracking settings tab. Dot color is the "amber"
    // used by the trail dots/diamonds before this became configurable.
    // -------------------------------------------------------------------------
    constexpr std::int32_t kPathTrailR = 255, kPathTrailG = 200, kPathTrailB = 100; // amber dots
    constexpr std::int32_t kPathTrailA = 230; // 0..255, folded into the trail color (was a separate Alpha slider)
    constexpr float        kPathTrailThickness = 2.5f;  // dot radius, px
    constexpr std::int32_t kPathPulseR = 255, kPathPulseG = 225, kPathPulseB = 140; // head cap
    constexpr ImU32 kPathTurnDiamond = IM_COL32(255, 200, 90, 255); // corner glyph

    // Elevation banding for the path trail. A trail point is coloured by its
    // world Z RELATIVE TO THE LIVE PLAYER Z, never by an absolute height and
    // never normalised to the path's own range (a flat corridor would then
    // stretch its ~8 units of noise across the whole ramp and scream "elevation
    // change" on level ground).
    //
    // Current floor keeps the user's own trail colour, so a single-level dungeon
    // looks exactly as it does today and the bands only appear when the trail
    // actually leaves the player's floor.
    //
    // kPathElevBand = half the SMALLEST floor gap measured in-game (inn
    // mezzanine, 253.5 units). Measured 2026-08-15 across three interior types:
    // inn 253, Nordic ruin 360-383, palace (Dragonsreach) 750. Floor height is
    // NOT an engine constant, it is a property of the build kit, so the band has
    // to be sized by the tightest case or a mezzanine reads as the same floor.
    // Same-floor Z noise measured <=8 units, so 125 still clears it 15:1.
    // Full data + method: memory reference_interior_floor_z_gaps.md.
    constexpr ImU32 kPathElevHigh = IM_COL32(120, 190, 255, 255); // cool blue, cell's highest
    constexpr ImU32 kPathElevLow  = IM_COL32(255, 130,  90, 255); // warm orange, cell's lowest

    // The colour is a property of the POINT, not of the viewer. The trail's own
    // min and max Z in the current cell define the ramp: lowest = orange,
    // highest = blue, everything else interpolated. No anchor, no reference
    // point, so the map looks identical no matter where the player stands.
    //
    // WHY NOT RELATIVE TO THE PLAYER (tried and rejected 2026-08-15, in-game):
    // colouring by `pointZ - playerZ` meant the whole map REPAINTED as you
    // walked. A corridor you knew was flat turned blue because you later went
    // down somewhere else. The trail had no stable colour, and reading the
    // dungeon's SHAPE is the actual job — that must not depend on where you are
    // standing. Both a flat-band and a ramped version of the relative scheme
    // were built and both failed the same way; the problem was the anchor, not
    // the curve.
    //
    // WHY NOT ANCHORED TO THE TRAIL'S START: breaks whenever the player starts
    // at the top of a dungeon — everything then reads "below" and the ramp is
    // one-sided. min/max has no such orientation.
    //
    // kPathElevMinSpan: guard against auto-scaling a FLAT cell. Same-floor Z
    // noise measures ~8 units, and stretching that across the full ramp would
    // paint a fake rainbow on level ground. Below this span the trail draws in
    // its normal flat colour. Sized just under the smallest real floor gap
    // measured in-game (inn mezzanine, 253) so one genuine step still triggers
    // colouring. Full data: memory reference_interior_floor_z_gaps.md.
    //
    // TRADE-OFF, deliberate: the ramp is relative to THIS cell's verticality, so
    // a 250-unit step is a bold colour change in a shallow cell and a subtle one
    // in a 4100-unit stack like Bleak Falls. Reads "which parts are high vs low"
    // well; does not read "how many units apart are these two points" at all.
    constexpr float kPathElevMinSpan = 250.0f;

    // -------------------------------------------------------------------------
    // Vanilla-marker badges (favorite star / has-note dot), world map only.
    // Static, no animation. Blue ring = destination, gold = favorite,
    // teal = note — these three are the whole visual vocabulary.
    // -------------------------------------------------------------------------
    constexpr ImU32 kFavBadgeFill    = IM_COL32(255, 205,  70, 230);  // gold
    constexpr ImU32 kFavBadgeOutline = IM_COL32( 30,  25,  10, 230);
    constexpr ImU32 kNoteBadgeFill   = IM_COL32( 70, 200, 190, 230);  // teal
    constexpr ImU32 kNoteBadgeOutline= IM_COL32( 10,  40,  38, 230);
    constexpr ImU32 kClearedBadgeFill    = IM_COL32( 90, 200, 110, 235);  // green
    constexpr ImU32 kClearedBadgeOutline = IM_COL32( 15,  40,  20, 235);

    // -------------------------------------------------------------------------
    // Marker settings defaults (Settings::marker*). RGBA ints (0..255) mirror
    // the PathTrail pattern above so ColorEdit4Hex can bind directly. Fav/Note
    // fill defaults are the kFavBadgeFill/kNoteBadgeFill colors above split
    // into channels; outline stays fixed (not user-configurable, matches the
    // dark-outline convention every other badge/pin uses).
    // -------------------------------------------------------------------------
    constexpr float        kMarkerIconScale = 1.0f;      // 0.5..2.0, custom pin PNG/vector size
    constexpr float        kFavBadgeSize    = 9.0f;      // star radius, px (was kStarRadius local const)
    constexpr std::int32_t kFavBadgeR = 255, kFavBadgeG = 205, kFavBadgeB = 70, kFavBadgeA = 230;
    constexpr float        kNoteBadgeSize   = 6.0f;      // dot radius, px (was kDotRadius local const)
    constexpr std::int32_t kNoteBadgeR = 70, kNoteBadgeG = 200, kNoteBadgeB = 190, kNoteBadgeA = 230;
    constexpr float        kClearedBadgeSize = 7.0f;     // disc radius, px
    constexpr std::int32_t kClearedBadgeR = 90, kClearedBadgeG = 200, kClearedBadgeB = 110, kClearedBadgeA = 235;

    // -------------------------------------------------------------------------
    // Inspect overlay entity-layer glyphs (MinimapRenderer::DrawInspect).
    // Vector-only in v1, no PNGs. Colors read the MiniMap-Menu convention:
    // corpses stark white, doors tan (match the door pins), loot/containers
    // amber-orange. Restyle here, never at the draw site (project rule).
    // -------------------------------------------------------------------------
    constexpr ImU32 kEntityCorpse    = IM_COL32(235, 235, 235, 235);  // white X
    constexpr ImU32 kEntityDoor      = IM_COL32(200, 170, 110, 235);  // tan rounded rect
    constexpr ImU32 kEntityContainer = IM_COL32(230, 150,  60, 235);  // orange square
    constexpr ImU32 kEntityLoot      = IM_COL32(230, 150,  60, 235);  // orange dot
    constexpr ImU32 kEntityIngredient = IM_COL32( 90, 200, 110, 255); // green, ingredient/plant glyph fallback

    // -------------------------------------------------------------------------
    // Navigation overlay player arrow (MinimapRenderer, Glance + Inspect).
    // -------------------------------------------------------------------------
    inline constexpr ImU32 kRetraceArrow = IM_COL32(235, 200, 60, 255); // player arrow tint while retracing an already-tracked path
    inline constexpr ImU32 kCombatBorder = IM_COL32(200,  55,  45, 255); // Glance rim tint while player is in combat (pulsed)

    // -------------------------------------------------------------------------
    // Actor radar layer (MinimapRenderer, Glance + Inspect). Transient dots,
    // vector-only (no PNGs, task decision). Ally = cool blue, Hostile = warning
    // red, Neutral = green so it reads as "present, no threat". Animal took the
    // grey (swapped with Neutral 2026-08-27): wildlife is background scenery, so
    // it should recede, while an unflagged NPC is worth actually spotting.
    // -------------------------------------------------------------------------
    constexpr ImU32 kActorAlly     = IM_COL32( 70, 140, 230, 235);  // blue
    constexpr ImU32 kActorHostile  = IM_COL32(220,  60,  55, 235);  // red
    constexpr ImU32 kActorNeutral  = IM_COL32( 90, 200, 120, 235);  // green; unflagged NPCs (townsfolk, guards not in combat)
    constexpr ImU32 kActorAnimal   = IM_COL32(210, 210, 215, 230);  // grey; non-hostile wildlife (fox/deer). Hostile creatures still red.
                                                                    // 150/160 was near-invisible on the backdrop -- keep it bright.

} // namespace MapTheme
