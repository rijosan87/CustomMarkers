#include "PCH.h"
#include "LootBeamDesigns.h"
#include "MapDrawHelpers.h"
#include "UIScale.h"

#include <cmath>

namespace LootBeamDesigns {
    void DrawModern(const DrawContext& a_context) {
        auto* dl = a_context.drawList;
        const auto& base = a_context.base;
        const auto& top = a_context.top;
        const float alpha = a_context.alphaScale;

        const float tTop = (top.y <= base.y) ? a_context.segmentEnd : a_context.segmentStart;
        const ImVec2 iconAt(base.x + (top.x - base.x) * tTop,
                            base.y + (top.y - base.y) * tTop);
        const float badgeR = a_context.iconHalf * 1.55f;
        const ImVec2 beamEnd = (iconAt.y < base.y)
            ? ImVec2(iconAt.x, iconAt.y + badgeR)
            : iconAt;

        if (a_context.segmentStart <= 0.01f && a_context.groundGlow > 0.0f) {
            constexpr int kRingPts = 16;
            ImVec2 ringPts[kRingPts];
            auto drawStrokedRing = [&](float rx, float ry, ImU32 col, float thickness) {
                for (int i = 0; i < kRingPts; ++i) {
                    const float angle = static_cast<float>(i) * (6.2831853f / kRingPts);
                    ringPts[i] = ImVec2(base.x + std::cos(angle) * rx,
                                         base.y + std::sin(angle) * ry);
                }
                dl->AddPolyline(ringPts, kRingPts, MapDraw::WithAlpha(col, alpha * a_context.groundGlow),
                                ImDrawFlags_Closed, thickness);
            };
            drawStrokedRing(UIScale::Px(8.0f), UIScale::Px(3.0f),
                            MapDraw::WithAlpha(a_context.core, 0.85f), UIScale::Px(1.5f));
            drawStrokedRing(UIScale::Px(14.0f), UIScale::Px(5.0f),
                            MapDraw::WithAlpha(a_context.glow, 0.60f), UIScale::Px(1.3f));
            drawStrokedRing(UIScale::Px(22.0f), UIScale::Px(8.0f),
                            MapDraw::WithAlpha(a_context.halo, 0.35f), UIScale::Px(1.0f));
        }

        MapDraw::DrawFeatheredBeam(
            dl, base, beamEnd, 2,
            UIScale::Px(a_context.bottomWidth * 0.5f), UIScale::Px(a_context.topWidth * 0.5f),
            a_context.core, a_context.glow, a_context.halo, alpha,
            [](float) { return 1.0f; }, UIScale::Px(1.2f));

        const float glowR = badgeR + UIScale::Px(2.5f);
        dl->AddQuad(
            ImVec2(iconAt.x, iconAt.y - glowR),
            ImVec2(iconAt.x + glowR, iconAt.y),
            ImVec2(iconAt.x, iconAt.y + glowR),
            ImVec2(iconAt.x - glowR, iconAt.y),
            MapDraw::WithAlpha(a_context.halo, 0.40f * alpha), 3.5f);
        dl->AddQuad(
            ImVec2(iconAt.x, iconAt.y - badgeR),
            ImVec2(iconAt.x + badgeR, iconAt.y),
            ImVec2(iconAt.x, iconAt.y + badgeR),
            ImVec2(iconAt.x - badgeR, iconAt.y),
            MapDraw::WithAlpha(a_context.glow, alpha), 2.0f);
        const float innerR = badgeR - 3.5f;
        if (innerR > UIScale::Px(4.0f)) {
            dl->AddQuad(
                ImVec2(iconAt.x, iconAt.y - innerR),
                ImVec2(iconAt.x + innerR, iconAt.y),
                ImVec2(iconAt.x, iconAt.y + innerR),
                ImVec2(iconAt.x - innerR, iconAt.y),
                MapDraw::WithAlpha(a_context.core, 0.45f * alpha), 1.0f);
        }

        // Small inscribed diamonds break up the frame's clean corners without
        // putting detail behind the icon or changing the beam geometry.
        const ImVec2 kDirections[4] = {
            ImVec2(0.0f, -1.0f), ImVec2(1.0f, 0.0f),
            ImVec2(0.0f, 1.0f), ImVec2(-1.0f, 0.0f)
        };
        const float runeR = UIScale::Px(2.5f);
        for (const ImVec2& dir : kDirections) {
            const ImVec2 point(iconAt.x + dir.x * badgeR,
                               iconAt.y + dir.y * badgeR);
            dl->AddQuad(
                ImVec2(point.x, point.y - runeR),
                ImVec2(point.x + runeR, point.y),
                ImVec2(point.x, point.y + runeR),
                ImVec2(point.x - runeR, point.y),
                MapDraw::WithAlpha(a_context.glow, alpha), UIScale::Px(1.0f));
            dl->AddLine(
                ImVec2(point.x - dir.x * runeR, point.y - dir.y * runeR),
                ImVec2(point.x + dir.x * runeR, point.y + dir.y * runeR),
                MapDraw::WithAlpha(a_context.core, 0.75f * alpha), UIScale::Px(1.0f));
        }

        if (a_context.icon) {
            const float iconHalf = a_context.iconHalf * 0.82f;
            const ImVec4 core = ImGui::ColorConvertU32ToFloat4(a_context.core);
            const ImVec4 glow = ImGui::ColorConvertU32ToFloat4(a_context.glow);
            const ImU32 iconTint = ImGui::ColorConvertFloat4ToU32(ImVec4(
                (core.x + glow.x) * 0.5f,
                (core.y + glow.y) * 0.5f,
                (core.z + glow.z) * 0.5f,
                alpha));
            MapDraw::DrawIconFx(dl, a_context.icon,
                                ImVec2(iconAt.x - iconHalf,
                                       iconAt.y - iconHalf),
                                ImVec2(iconAt.x + iconHalf,
                                       iconAt.y + iconHalf),
                                a_context.preserveIconColor
                                    ? MapDraw::WithAlpha(IM_COL32_WHITE, alpha) : iconTint,
                                -1, a_context.preserveIconColor ? 0 : -1);
        }
    }
}
