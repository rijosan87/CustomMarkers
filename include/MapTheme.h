#pragma once

// Theme color palettes, badge styling, and icon definitions.

#include <imgui.h>
#include <cstdint>

namespace MapTheme {
    constexpr ImU32 kPanelBg         = IM_COL32( 18,  16,  12, 245);
    constexpr ImU32 kPanelBorder     = IM_COL32(140, 115,  65, 255);
    constexpr ImU32 kPanelBorderHover= IM_COL32(200, 170,  90, 255);

    constexpr float kPanelRounding   = 0.0f;
    constexpr float kPanelBorderW    = 1.5f;

    constexpr ImU32 kHeaderText      = IM_COL32(255, 220, 140, 255);
    constexpr ImU32 kSeparator       = IM_COL32(140, 115,  65, 180);

    constexpr ImU32 kBodyText        = IM_COL32(230, 220, 200, 255);
    constexpr ImU32 kBodyTextHover   = IM_COL32(255, 245, 210, 255);

    constexpr ImU32 kMutedText       = IM_COL32(160, 150, 130, 255);

    constexpr ImU32 kRowHoverBg      = IM_COL32(140, 115,  65,  55);

    constexpr ImU32 kTabTrayBg       = IM_COL32(  0,   0,   0,  60);
    constexpr ImU32 kTabDivider      = IM_COL32(140, 115,  65,  70);
    constexpr ImU32 kTabHoverBg      = IM_COL32(140, 115,  65,  40);
    constexpr ImU32 kTabActiveBg     = IM_COL32(140, 115,  65,  90);

    constexpr ImU32 kRemoveBg        = IM_COL32( 80,  30,  30, 220);
    constexpr ImU32 kRemoveBgHover   = IM_COL32(170,  45,  45, 255);
    constexpr ImU32 kRemoveBorder    = IM_COL32(  0,   0,   0, 180);
    constexpr ImU32 kRemoveText      = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kRemoveRowText   = IM_COL32(220,  80,  80, 255);
    constexpr ImU32 kRemoveRowIcon   = IM_COL32(200,  70,  70, 255);
    constexpr float kBtnRounding     = 3.0f;
    constexpr float kBtnBorderW      = 1.0f;

    constexpr ImU32 kDestBg          = IM_COL32( 35,  55,  90, 220);
    constexpr ImU32 kDestBgHover     = IM_COL32( 60, 110, 175, 255);
    constexpr ImU32 kDestBorder      = IM_COL32(  0,   0,   0, 180);
    constexpr ImU32 kDestText        = IM_COL32(255, 255, 255, 255);
    constexpr ImU32 kDestRowBg       = IM_COL32( 30,  60, 105,  55);
    constexpr ImU32 kDestRowBgHover  = IM_COL32( 45,  85, 145,  95);
    constexpr ImU32 kDestBadgeTint   = IM_COL32(255, 255, 255, 255);

    constexpr ImU32 kEditBg          = IM_COL32( 30,  65,  55, 220);
    constexpr ImU32 kEditBgHover     = IM_COL32( 50, 120,  90, 255);
    constexpr ImU32 kEditBorder      = IM_COL32(  0,   0,   0, 180);
    constexpr ImU32 kEditText        = IM_COL32(255, 255, 255, 255);

    inline ImVec4 kPopupBg()         { return ImVec4(0.07f, 0.06f, 0.05f, 0.97f); }
    inline ImVec4 kPopupBorder()     { return ImVec4(0.55f, 0.45f, 0.25f, 1.0f);  }
    inline ImVec4 kPopupHeaderText() { return ImVec4(1.0f,  0.86f, 0.55f, 1.0f);  }
    inline ImVec4 kPopupText()       { return ImVec4(0.90f, 0.86f, 0.78f, 1.0f);  }
    inline ImVec4 kPopupHoverBg()    { return ImVec4(0.55f, 0.45f, 0.25f, 0.40f); }
    inline ImVec4 kPopupHoverText()  { return ImVec4(1.0f,  1.0f,  0.90f, 1.0f);  }

    inline ImVec4 kDlgBg()           { return ImVec4(0.07f, 0.06f, 0.05f, 1.0f);  }
    inline ImVec4 kDlgBorder()       { return ImVec4(0.55f, 0.45f, 0.25f, 1.0f);  }
    inline ImVec4 kDlgFrameBg()      { return ImVec4(0.14f, 0.12f, 0.09f, 1.0f);  }
    inline ImVec4 kDlgBtnBg()        { return ImVec4(0.27f, 0.22f, 0.14f, 1.0f);  }
    inline ImVec4 kDlgBtnHover()     { return ImVec4(0.55f, 0.45f, 0.25f, 1.0f);  }
    inline ImVec4 kDlgBtnActive()    { return ImVec4(0.70f, 0.58f, 0.32f, 1.0f);  }
    inline ImVec4 kDlgText()         { return ImVec4(0.90f, 0.86f, 0.78f, 1.0f);  }
    inline ImVec4 kDlgTitleText()    { return ImVec4(1.0f,  0.86f, 0.55f, 1.0f);  }
    inline ImVec4 kDlgCheckMark()    { return ImVec4(0.70f, 0.58f, 0.32f, 1.0f);  }

    inline ImVec4 kDlgGrab()         { return ImVec4(0.62f, 0.50f, 0.28f, 1.0f);  }
    inline ImVec4 kDlgGrabActive()   { return ImVec4(0.85f, 0.70f, 0.38f, 1.0f);  }
    inline ImVec4 kDlgHeaderBg()     { return ImVec4(0.20f, 0.16f, 0.10f, 1.0f);  }
    inline ImVec4 kDlgHeaderHover()  { return ImVec4(0.34f, 0.27f, 0.15f, 1.0f);  }
    inline ImVec4 kDlgHeaderActive() { return ImVec4(0.44f, 0.35f, 0.19f, 1.0f);  }
    inline ImVec4 kDlgScrollBg()     { return ImVec4(0.10f, 0.08f, 0.06f, 0.6f);  }
    inline ImVec4 kDlgScrollGrab()   { return ImVec4(0.40f, 0.33f, 0.19f, 1.0f);  }
    inline ImVec4 kMutedTextV()      { return ImVec4(0.63f, 0.59f, 0.51f, 1.0f);  }

    constexpr ImU32 kBeaconBg        = IM_COL32( 18,  16,  12, 245);
    constexpr ImU32 kBeaconBorder    = IM_COL32( 80, 130, 200, 255);
    constexpr ImU32 kBeaconText      = IM_COL32(220, 235, 255, 255);

    constexpr std::int32_t kPathTrailR = 255, kPathTrailG = 200, kPathTrailB = 100;
    constexpr std::int32_t kPathTrailA = 230;
    constexpr float        kPathTrailThickness = 2.5f;
    constexpr std::int32_t kPathPulseR = 255, kPathPulseG = 225, kPathPulseB = 140;
    constexpr ImU32 kPathTurnDiamond = IM_COL32(255, 200, 90, 255);

    constexpr ImU32 kPathElevHigh = IM_COL32(120, 190, 255, 255);
    constexpr ImU32 kPathElevLow  = IM_COL32(255, 130,  90, 255);

    constexpr float kPathElevMinSpan = 250.0f;

    constexpr ImU32 kFavBadgeFill    = IM_COL32(255, 205,  70, 230);
    constexpr ImU32 kFavBadgeOutline = IM_COL32( 30,  25,  10, 230);
    constexpr ImU32 kNoteBadgeFill   = IM_COL32( 70, 200, 190, 230);
    constexpr ImU32 kNoteBadgeOutline= IM_COL32( 10,  40,  38, 230);
    constexpr ImU32 kClearedBadgeFill    = IM_COL32( 90, 200, 110, 235);
    constexpr ImU32 kClearedBadgeOutline = IM_COL32( 15,  40,  20, 235);

    constexpr float        kMarkerIconScale = 1.0f;
    constexpr float        kFavBadgeSize    = 9.0f;
    constexpr std::int32_t kFavBadgeR = 255, kFavBadgeG = 205, kFavBadgeB = 70, kFavBadgeA = 230;
    constexpr float        kNoteBadgeSize   = 6.0f;
    constexpr std::int32_t kNoteBadgeR = 70, kNoteBadgeG = 200, kNoteBadgeB = 190, kNoteBadgeA = 230;
    constexpr float        kClearedBadgeSize = 7.0f;
    constexpr std::int32_t kClearedBadgeR = 90, kClearedBadgeG = 200, kClearedBadgeB = 110, kClearedBadgeA = 235;

    constexpr ImU32 kEntityCorpse    = IM_COL32(235, 235, 235, 235);
    constexpr ImU32 kEntityDoor      = IM_COL32(200, 170, 110, 235);
    constexpr ImU32 kEntityContainer = IM_COL32(230, 150,  60, 235);
    constexpr ImU32 kEntityLoot      = IM_COL32(230, 150,  60, 235);
    constexpr ImU32 kEntityIngredient = IM_COL32( 90, 200, 110, 255);

    inline constexpr ImU32 kRetraceArrow = IM_COL32(235, 200, 60, 255);
    inline constexpr ImU32 kCombatBorder = IM_COL32(200,  55,  45, 255);

    constexpr ImU32 kActorAlly     = IM_COL32( 70, 140, 230, 235);
    constexpr ImU32 kActorHostile  = IM_COL32(220,  60,  55, 235);
    constexpr ImU32 kActorNeutral  = IM_COL32( 90, 200, 120, 235);
    constexpr ImU32 kActorAnimal   = IM_COL32(210, 210, 215, 230);

}
