#include "PCH.h"
#include "LootBeamDesigns.h"
#include "MapDrawHelpers.h"
#include "UIScale.h"

#include <algorithm>
#include <cmath>

namespace LootBeamDesigns {
    namespace {
        void DrawRibbon(ImDrawList* a_dl, const ImVec2& a_base, const ImVec2& a_top,
                        float a_baseWidth, float a_topWidth, float a_bend,
                        ImU32 a_color, float a_alphaScale) {
            constexpr int kRows = 7;
            constexpr float kRails[5] = { -1.0f, -0.32f, 0.0f, 0.32f, 1.0f };
            const float dx = a_top.x - a_base.x;
            const float dy = a_top.y - a_base.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length < 1.0f) return;

            const float nx = -dy / length;
            const float ny = dx / length;
            const ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
            a_dl->PrimReserve((kRows - 1) * 4 * 6, kRows * 5);
            const unsigned int first = a_dl->_VtxCurrentIdx;

            for (int row = 0; row < kRows; ++row) {
                const float f = static_cast<float>(row) / static_cast<float>(kRows - 1);
                const float bend = std::sin(f * 3.1415927f) * UIScale::Px(a_bend);
                const float width = UIScale::Px(a_baseWidth + (a_topWidth - a_baseWidth) * f);
                const float topFade = std::clamp((1.0f - f) / 0.30f, 0.0f, 1.0f);
                const float uneven = 0.88f + 0.12f * std::sin(f * 14.0f + a_bend);
                const float alpha = a_alphaScale * topFade * uneven;
                const ImVec2 center(a_base.x + dx * f + nx * bend,
                                    a_base.y + dy * f + ny * bend);
                const ImU32 outer = MapDraw::WithAlpha(a_color, 0.0f);
                const ImU32 inner = MapDraw::WithAlpha(a_color, alpha * 0.65f);
                const ImU32 middle = MapDraw::WithAlpha(a_color, alpha);
                const ImU32 colors[5] = { outer, inner, middle, inner, outer };
                for (int rail = 0; rail < 5; ++rail) {
                    a_dl->PrimWriteVtx(ImVec2(center.x + nx * kRails[rail] * width,
                                               center.y + ny * kRails[rail] * width),
                                       uv, colors[rail]);
                }
            }

            for (int row = 0; row < kRows - 1; ++row) {
                const unsigned int lower = first + row * 5;
                const unsigned int upper = lower + 5;
                for (int rail = 0; rail < 4; ++rail) {
                    a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(lower + rail));
                    a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(lower + rail + 1));
                    a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(upper + rail + 1));
                    a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(lower + rail));
                    a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(upper + rail + 1));
                    a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(upper + rail));
                }
            }
        }

        void DrawAnchor(ImDrawList* a_dl, const ImVec2& a_base,
                        std::uint32_t a_formID, ImU32 a_color, float a_alphaScale) {
            constexpr int kPoints = 12;
            for (int layer = 0; layer < 2; ++layer) {
                ImVec2 points[kPoints];
                const float rx = UIScale::Px(layer == 0 ? 16.0f : 10.0f);
                const float ry = UIScale::Px(layer == 0 ? 5.0f : 3.0f);
                const float offset = layer == 0 ? 0.0f : UIScale::Px(1.0f);
                for (int i = 0; i < kPoints; ++i) {
                    const float angle = static_cast<float>(i) * (6.2831853f / kPoints);
                    const unsigned int seed = a_formID ^
                        (static_cast<unsigned int>(i + layer * kPoints) * 0x9E3779B9u);
                    const float irregularity = 0.90f +
                        0.10f * static_cast<float>((seed >> 12) & 0xFFu) / 255.0f;
                    points[i] = ImVec2(a_base.x + offset + std::cos(angle) * rx * irregularity,
                                       a_base.y + std::sin(angle) * ry * irregularity);
                }
                a_dl->AddConvexPolyFilled(points, kPoints,
                    MapDraw::WithAlpha(a_color, a_alphaScale * (layer == 0 ? 0.10f : 0.16f)));
            }
        }
    }

    void DrawWisp(const DrawContext& a_context) {
        const auto& base = a_context.base;
        const auto& top = a_context.top;
        const float axisX = top.x - base.x;
        const float axisY = top.y - base.y;
        const float axisLength = std::sqrt(axisX * axisX + axisY * axisY);
        if (axisLength < 1.0f) return;

        const float iconGap = a_context.iconHalf + UIScale::Px(8.0f);
        const ImVec2 iconAt(top.x + axisX / axisLength * iconGap,
                            top.y + axisY / axisLength * iconGap);

        // The anchor stays at the real projected base, not the clipped edge.
        if (a_context.segmentStart <= 0.01f && a_context.groundGlow > 0.0f) {
            DrawAnchor(a_context.drawList, base, a_context.formID,
                       a_context.glow, a_context.alphaScale * a_context.groundGlow);
        }
        // Controls use full widths; ribbon rails extend by a half-width on each side.
        DrawRibbon(a_context.drawList, base, top,
                   a_context.bottomWidth * 0.5f, a_context.topWidth * 0.5f, 1.2f,
                   a_context.preserveIconColor ? a_context.core : IM_COL32(142, 174, 181, 255),
                   a_context.alphaScale * 0.50f);
        DrawRibbon(a_context.drawList, base, top,
                   a_context.bottomWidth * 0.3f, a_context.topWidth * 0.2f, -1.0f,
                   a_context.glow, a_context.alphaScale * 0.28f);

        if (a_context.icon) {
            // Keep the icon in the beam's category palette without using the
            // saturated glow color alone as a texture multiplier.
            const ImVec4 core = ImGui::ColorConvertU32ToFloat4(a_context.core);
            const ImVec4 glow = ImGui::ColorConvertU32ToFloat4(a_context.glow);
            const ImU32 iconTint = ImGui::ColorConvertFloat4ToU32(ImVec4(
                (core.x + glow.x) * 0.5f,
                (core.y + glow.y) * 0.5f,
                (core.z + glow.z) * 0.5f,
                a_context.alphaScale));
            MapDraw::DrawIconFx(a_context.drawList, a_context.icon,
                                ImVec2(iconAt.x - a_context.iconHalf,
                                       iconAt.y - a_context.iconHalf),
                                ImVec2(iconAt.x + a_context.iconHalf,
                                       iconAt.y + a_context.iconHalf),
                                a_context.preserveIconColor
                                    ? MapDraw::WithAlpha(IM_COL32_WHITE, a_context.alphaScale)
                                    : iconTint, 1, a_context.preserveIconColor ? 0 : 1);
        }
    }
}
