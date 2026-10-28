#pragma once

#include <d3d11.h>
#include <imgui.h>
#include <cstdint>

namespace LootBeamDesigns {
    enum class Design { Modern, Wisp };

    struct DrawContext {
        ImDrawList* drawList;
        ImVec2 base;
        ImVec2 top;
        float segmentStart;
        float segmentEnd;
        float iconHalf;
        float alphaScale;
        ImU32 core;
        ImU32 glow;
        ImU32 halo;
        std::uint32_t formID;
        ID3D11ShaderResourceView* icon;
        bool preserveIconColor;  // Precolored museum artwork bypasses RGB tint and duotone shading.
        // Full pixel widths before interface scaling; supplied for the selected design.
        float bottomWidth;
        float topWidth;
        float groundGlow;
    };

    constexpr float LengthScale(Design a_design) {
        switch (a_design) {
            case Design::Modern: return 1.0f;
            case Design::Wisp: return 0.70f;
        }
        return 1.0f;
    }
    void DrawModern(const DrawContext& a_context);
    void DrawWisp(const DrawContext& a_context);
}
