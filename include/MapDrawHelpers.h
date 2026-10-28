// include/MapDrawHelpers.h
#pragma once
#include "LocalizationManager.h"
#include <imgui.h>
#include <imgui_internal.h>  // ShadeVertsLinearColorGradientKeepAlpha (icon duotone)
#include <RE/Skyrim.h>       // NiCamera/NiPoint3 for ProjectWorldPoint (beam helpers)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <d3d11.h>
#include <vector>
#include "TextureManager.h"
#include "MapTheme.h"
#include "Settings.h"
#include "PathTracker.h"

namespace MapDraw {
    constexpr float kPinRadius = 9.0f;

    constexpr float kTurnThresholdRad = 0.61f;  // smart-spacing turn sensitivity
    constexpr float kDotSpacingMin    = 6.0f;
    constexpr float kDotSpacingMax    = 13.0f;
    constexpr int   kGapEvery         = 34;
    constexpr int   kGapSkip           = 2;
    constexpr float kRevisitGlowSec   = 2.0f;

    inline ImU32 WithAlpha(ImU32 col, float mul) {
        const int a = static_cast<int>(((col >> IM_COL32_A_SHIFT) & 0xFF) * mul);
        return (col & ~(0xFFu << IM_COL32_A_SHIFT)) |
               (static_cast<ImU32>(a < 0 ? 0 : (a > 255 ? 255 : a)) << IM_COL32_A_SHIFT);
    }

    // Keep the settled Glance snapshot fully opaque like Inspect. The shared
    // crossfade still scales this alpha during transitions.
    constexpr ImU32 kGlanceBackdropTint = IM_COL32(255, 255, 255, 255);

    struct Rect { ImVec2 mn, mx; };

    struct RoundedRectBoundary {
        ImVec2 center{};
        float halfSize{0.0f};
        float rounding{0.0f};

        ImVec2 Min() const { return ImVec2(center.x - halfSize, center.y - halfSize); }
        ImVec2 Max() const { return ImVec2(center.x + halfSize, center.y + halfSize); }

        bool Contains(const ImVec2& a_point, float a_extent = 0.0f) const {
            const float innerHalf = std::max(0.0f, halfSize - a_extent);
            const float innerRound = std::clamp(rounding - a_extent, 0.0f, innerHalf);
            const ImVec2 delta(std::abs(a_point.x - center.x),
                               std::abs(a_point.y - center.y));
            const ImVec2 q(delta.x - (innerHalf - innerRound),
                           delta.y - (innerHalf - innerRound));
            const float outsideX = std::max(q.x, 0.0f);
            const float outsideY = std::max(q.y, 0.0f);
            return std::sqrt(outsideX * outsideX + outsideY * outsideY) <= innerRound;
        }

        ImVec2 ClampFromCenter(const ImVec2& a_point, float a_extent = 0.0f) const {
            if (Contains(a_point, a_extent)) {
                return a_point;
            }
            const float dx = a_point.x - center.x;
            const float dy = a_point.y - center.y;
            const float len = std::hypot(dx, dy);
            if (len < 1e-4f) {
                return center;
            }
            const float dirX = dx / len;
            const float dirY = dy / len;
            const float farDist = 2.0f * halfSize;
            float low = 0.0f;
            float high = farDist;
            for (int i = 0; i < 16; ++i) {
                const float mid = (low + high) * 0.5f;
                const ImVec2 candidate(center.x + dirX * mid, center.y + dirY * mid);
                if (Contains(candidate, a_extent)) {
                    low = mid;
                } else {
                    high = mid;
                }
            }
            return ImVec2(center.x + dirX * low, center.y + dirY * low);
        }

        ImVec2 RayIntersection(float a_angle, float a_extent = 0.0f) const {
            const ImVec2 direction(std::cos(a_angle), std::sin(a_angle));
            return ClampFromCenter(ImVec2(center.x + direction.x * (2.0f * halfSize),
                                          center.y + direction.y * (2.0f * halfSize)),
                                   a_extent);
        }
    };

    inline bool ColorEdit4Hex(const char* label,
                              std::int32_t& r, std::int32_t& g,
                              std::int32_t& b, std::int32_t& a) {
        float c[4] = { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
        const ImGuiColorEditFlags flags =
            ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_DisplayHex |
            ImGuiColorEditFlags_NoInputs;
        if (ImGui::ColorEdit4(label, c, flags)) {
            r = static_cast<std::int32_t>(c[0] * 255.0f + 0.5f);
            g = static_cast<std::int32_t>(c[1] * 255.0f + 0.5f);
            b = static_cast<std::int32_t>(c[2] * 255.0f + 0.5f);
            a = static_cast<std::int32_t>(c[3] * 255.0f + 0.5f);
            return true;
        }
        return false;
    }

    inline bool ColorEdit3Hex(const char* label,
                              std::int32_t& r, std::int32_t& g, std::int32_t& b) {
        float c[3] = { r / 255.0f, g / 255.0f, b / 255.0f };
        // NoInputs: hide the inline hex field (picker popup has its own). See
        // ColorEdit4Hex note.
        const ImGuiColorEditFlags flags =
            ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoInputs;
        if (ImGui::ColorEdit3(label, c, flags)) {
            r = static_cast<std::int32_t>(c[0] * 255.0f + 0.5f);
            g = static_cast<std::int32_t>(c[1] * 255.0f + 0.5f);
            b = static_cast<std::int32_t>(c[2] * 255.0f + 0.5f);
            return true;
        }
        return false;
    }

    // Component-wise integer color lerp (0..255 channels packed in ImU32).
    inline ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        auto lerp8 = [t](int ca, int cb) {
            return static_cast<int>(ca + (cb - ca) * t);
        };
        int ar = (a >> IM_COL32_R_SHIFT) & 0xFF, ag = (a >> IM_COL32_G_SHIFT) & 0xFF,
            ab = (a >> IM_COL32_B_SHIFT) & 0xFF, aa = (a >> IM_COL32_A_SHIFT) & 0xFF;
        int br = (b >> IM_COL32_R_SHIFT) & 0xFF, bg = (b >> IM_COL32_G_SHIFT) & 0xFF,
            bb = (b >> IM_COL32_B_SHIFT) & 0xFF, ba = (b >> IM_COL32_A_SHIFT) & 0xFF;
        return IM_COL32(lerp8(ar, br), lerp8(ag, bg), lerp8(ab, bb), lerp8(aa, ba));
    }

    inline bool PointInRect(ImVec2 p, const Rect& r) {
        return p.x >= r.mn.x && p.x <= r.mx.x &&
               p.y >= r.mn.y && p.y <= r.mx.y;
    }

    inline Rect Combine(const Rect& a, const Rect& b, float pad) {
        return { ImVec2(std::min(a.mn.x, b.mn.x) - pad, std::min(a.mn.y, b.mn.y) - pad),
                 ImVec2(std::max(a.mx.x, b.mx.x) + pad, std::max(a.mx.y, b.mx.y) + pad) };
    }

    // Pin icon is centered ON the anchor point (no tail lift), so the hover
    // box is symmetric around it. Keep in sync with DrawPin's geometry.
    inline Rect PinBox(float px, float py, float uiScale = 1.0f) {
        const float half = 17.0f * uiScale;
        return { ImVec2(px - half, py - half),
                 ImVec2(px + half, py + half) };
    }

    constexpr float kNoteMaxTextW  = 300.0f;
    constexpr float kHeaderPadX    = 10.0f;
    constexpr float kHeaderPadY    = 10.0f;
    constexpr float kMinPanelW     = 180.0f;

    constexpr float kLabelAlpha = 0.70f;

    // Scale an IM_COL32's alpha byte in place. The colour tokens are already
    // packed ImU32, so this avoids a round trip through ImVec4.
    inline ImU32 FadeAlpha(ImU32 a_col, float a_frac) {
        const float a = static_cast<float>((a_col >> IM_COL32_A_SHIFT) & 0xFF) * a_frac;
        return (a_col & ~IM_COL32_A_MASK) |
               (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
    }

    struct InfoPanelLayout {
        Rect  panel;
        float nameH;      // wrapped note height at the scaled title size (subtitle Y offset)
        // Wrap width the note was MEASURED at, in screen pixels. Draw sites must
        // pass this straight to DrawTextShadowed so measurement and drawing agree.
        float noteWrapW;
        float padX;
        float padY;
    };

    inline InfoPanelLayout ComputeInfoPanel(float pinX, float pinY, const char* note, ImVec2 disp,
                                            float titleScale = 1.0f, float bodyScale = 1.0f,
                                            float offsetX = 0.0f, float offsetY = 0.0f,
                                            float geometryScale = 1.0f,
                                            float titleWidthScale = 1.0f) {
        geometryScale = (std::max)(geometryScale, 0.01f);
        const float padX = kHeaderPadX * geometryScale;
        const float padY = kHeaderPadY * geometryScale;
        const float minW = kMinPanelW * geometryScale;
        const float gapY = 2.0f * geometryScale;

        const float wrapW = kNoteMaxTextW * titleWidthScale * geometryScale;
        ImVec2 nameSize = ImGui::CalcTextSize(note, nullptr, false, wrapW / titleScale);
        nameSize.x *= titleScale;
        nameSize.y *= titleScale;
        const std::string subText = LocalizationManager::Get("Labels.CustomMarker");
        ImVec2 subSize = ImGui::CalcTextSize(subText.c_str());
        subSize.x *= bodyScale;
        subSize.y *= bodyScale;

        float contentW = std::max({ nameSize.x, subSize.x, minW - padX * 2.0f });
        float panelW = contentW + padX * 2.0f;
        float panelH = padY + nameSize.y + gapY + subSize.y + padY;

        float ox = pinX + (14.0f + offsetX) * geometryScale;
        float oy = pinY - panelH - (8.0f - offsetY) * geometryScale;
        const float clampMargin = 4.0f * geometryScale;
        const float maxOx = (std::max)(clampMargin, disp.x - panelW - clampMargin);
        const float maxOy = (std::max)(clampMargin, disp.y - panelH - clampMargin);
        ox = std::clamp(ox, clampMargin, maxOx);
        oy = std::clamp(oy, clampMargin, maxOy);

        InfoPanelLayout l;
        l.panel.mn  = ImVec2(ox, oy);
        l.panel.mx  = ImVec2(ox + panelW, oy + panelH);
        l.nameH     = nameSize.y;
        l.noteWrapW = wrapW;
        l.padX      = padX;
        l.padY      = padY;
        return l;
    }

    inline bool IsWaypointType(std::uint32_t t) { return t >= 100; }
    inline constexpr std::uint32_t kQuickMarkBase = 200;

    // Defined after DrawStar (needs it); DrawPin dispatches here for
    // waypoint types before its PNG/vector paths.
    inline void DrawWaypointGlyph(ImDrawList* dl, float cx, float cy, float r,
                                  std::uint32_t iconType, float hoverT, float detailScale = 1.0f);

    inline float IconFxBoundaryExtent(float a_w, float a_h) {
        const float sc = (std::max)(1.0f, (std::max)(a_w, a_h) / 24.0f);
        return 2.0f * sc;
    }

    // Pass an explicit false to force a pass off at one call site, e.g. if a
    // future colored icon must keep its own RGB and only wants the shadow.
    inline void DrawIconFx(ImDrawList* dl, ID3D11ShaderResourceView* a_tex,
                           const ImVec2& a_min, const ImVec2& a_max,
                           ImU32 a_tint,
                           int a_shadowOpt = -1, int a_duotoneOpt = -1) {
        if (!dl || !a_tex) return;
        const auto* settings = Settings::GetSingleton();
        const bool a_shadow  = (a_shadowOpt  < 0) ? settings->iconShadow : (a_shadowOpt  != 0);
        const bool a_duotone = (a_duotoneOpt < 0) ? settings->iconFx3D : (a_duotoneOpt != 0);
        const auto tex = reinterpret_cast<ImTextureID>(a_tex);
        const ImVec2 uv0(0, 0), uv1(1, 1);

        const float w = a_max.x - a_min.x;
        const float h = a_max.y - a_min.y;

        // Alpha of the caller's tint gates the whole effect: a fully faded icon
        // must not leave its shadow behind during a crossfade.
        const auto tA = static_cast<std::uint8_t>((a_tint >> IM_COL32_A_SHIFT) & 0xFF);
        if (tA == 0) return;
        const float aScale = tA / 255.0f;
        auto fx = [aScale](int base) {
            return static_cast<int>(base * aScale);
        };

        if (a_shadow) {
            const float sc  = (std::max)(1.0f, (std::max)(w, h) / 24.0f);
            const float rim = 1.0f * sc;
            dl->AddImage(tex, ImVec2(a_min.x - rim, a_min.y - rim),
                         ImVec2(a_max.x - rim, a_max.y - rim),
                         uv0, uv1, IM_COL32(255, 255, 255, fx(55)));
            const float shOff = 1.5f * sc;
            dl->AddImage(tex, ImVec2(a_min.x + shOff, a_min.y + shOff + 0.5f),
                         ImVec2(a_max.x + shOff, a_max.y + shOff + 0.5f),
                         uv0, uv1, IM_COL32(0, 0, 0, fx(70)));
            dl->AddImage(tex, ImVec2(a_min.x + shOff * 0.6f, a_min.y + shOff * 0.6f),
                         ImVec2(a_max.x + shOff * 0.6f, a_max.y + shOff * 0.6f),
                         uv0, uv1, IM_COL32(0, 0, 0, fx(90)));
        }

        if (!a_duotone) {
            dl->AddImage(tex, a_min, a_max, uv0, uv1, a_tint);
            return;
        }

        const auto tR = static_cast<std::uint8_t>((a_tint >> IM_COL32_R_SHIFT) & 0xFF);
        const auto tG = static_cast<std::uint8_t>((a_tint >> IM_COL32_G_SHIFT) & 0xFF);
        const auto tB = static_cast<std::uint8_t>((a_tint >> IM_COL32_B_SHIFT) & 0xFF);
        auto lift = [](std::uint8_t c) {
            return static_cast<std::uint8_t>((std::min)(255.0f, c * 1.15f + 30.0f));
        };
        const ImU32 topCol = IM_COL32(lift(tR), lift(tG), lift(tB), 255);
        const ImU32 botCol = IM_COL32(static_cast<std::uint8_t>(tR * 0.55f),
                                      static_cast<std::uint8_t>(tG * 0.55f),
                                      static_cast<std::uint8_t>(tB * 0.55f), 255);
        const int v0 = dl->VtxBuffer.Size;
        dl->AddImage(tex, a_min, a_max, uv0, uv1, IM_COL32(255, 255, 255, tA));
        const int v1 = dl->VtxBuffer.Size;
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, v1,
            a_min, ImVec2(a_min.x, a_max.y), topCol, botCol);
    }

    inline void DrawTextShadowed(ImDrawList* dl, ImFont* font, float fontSize,
                                 const ImVec2& pos, ImU32 col, const char* txt,
                                 const char* txtEnd = nullptr, float wrapWidth = 0.0f) {
        if (!dl || !txt) return;
        if (!font) font = ImGui::GetFont();
        if (fontSize <= 0.0f) fontSize = ImGui::GetFontSize();

        const auto a = static_cast<std::uint8_t>((col >> IM_COL32_A_SHIFT) & 0xFF);
        if (a == 0) return;
        const float aScale = a / 255.0f;

        auto luma = [](ImU32 color) {
            const float r = static_cast<float>((color >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
            const float g = static_cast<float>((color >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
            const float b = static_cast<float>((color >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        };

        const ImU32 bg = Settings::GetSingleton()->BgCol();
        const float bgOpacity = static_cast<float>((bg >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
        const float contrastDirection = luma(col) - luma(bg);
        // Fully suppress at 0.20 darker, retain at 0.20 lighter, and ease the
        // middle so small color-picker changes do not make the shadow pop.
        const float contrastT = std::clamp((contrastDirection + 0.20f) / 0.40f, 0.0f, 1.0f);
        const float opaqueStrength = contrastT * contrastT * (3.0f - 2.0f * contrastT);
        const float shadowScale = aScale * ((1.0f - bgOpacity) + bgOpacity * opaqueStrength);

        // Snap the ANCHOR, then build every copy off it with whole-pixel deltas.
        const ImVec2 tp(std::floor(pos.x), std::floor(pos.y));
        const float sScale = std::clamp(fontSize / font->FontSize, 0.35f, 1.0f);
        const float off    = std::max(1.0f, std::floor(2.0f * sScale));
        const ImVec2 sc(tp.x + off, tp.y + off);

        if (shadowScale > 0.01f) {
            dl->AddText(font, fontSize, sc, IM_COL32(0, 0, 0, static_cast<int>(150 * shadowScale)),
                        txt, txtEnd, wrapWidth);
            static constexpr float kSpread[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            for (const auto& d : kSpread) {
                dl->AddText(font, fontSize, ImVec2(sc.x + d[0] * off, sc.y + d[1] * off),
                            IM_COL32(0, 0, 0, static_cast<int>(55 * shadowScale)), txt, txtEnd, wrapWidth);
            }
        }

        dl->AddText(font, fontSize, tp, col, txt, txtEnd, wrapWidth);
    }

    inline void DrawDotFx(ImDrawList* dl, const ImVec2& c, float r, ImU32 col) {
        if (!dl || r <= 0.0f) return;
        const auto a = static_cast<std::uint8_t>((col >> IM_COL32_A_SHIFT) & 0xFF);
        if (a == 0) return;                 // fully faded: no shadow left behind
        const float aScale = a / 255.0f;
        auto A = [aScale](int base) { return static_cast<int>(base * aScale); };

        const auto cR = static_cast<std::uint8_t>((col >> IM_COL32_R_SHIFT) & 0xFF);
        const auto cG = static_cast<std::uint8_t>((col >> IM_COL32_G_SHIFT) & 0xFF);
        const auto cB = static_cast<std::uint8_t>((col >> IM_COL32_B_SHIFT) & 0xFF);

        // 1) Socket
        dl->AddCircleFilled(ImVec2(c.x + 0.5f, c.y + 1.0f), r + 0.5f, IM_COL32(0, 0, 0, A(70)));

        // 2) Duotone fill
        auto lift = [](std::uint8_t v) {
            return static_cast<std::uint8_t>((std::min)(255.0f, v * 1.22f + 18.0f));
        };
        auto sink = [](std::uint8_t v) { return static_cast<std::uint8_t>(v * 0.62f); };
        const ImU32 top = IM_COL32(lift(cR), lift(cG), lift(cB), 255);
        const ImU32 bot = IM_COL32(sink(cR), sink(cG), sink(cB), 255);
        const int v0 = dl->VtxBuffer.Size;
        dl->AddCircleFilled(c, r, IM_COL32(255, 255, 255, a));
        const int v1 = dl->VtxBuffer.Size;
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, v1,
            ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), top, bot);

        // 3) Specular + rim
        if (r >= 3.0f) {
            dl->AddCircleFilled(ImVec2(c.x - r * 0.30f, c.y - r * 0.35f),
                                (std::max)(1.0f, r * 0.28f), IM_COL32(255, 255, 255, A(110)));
        }
        dl->AddCircle(c, r, IM_COL32(0, 0, 0, A(140)), 0, 1.0f);
    }

    inline void DrawPanelCard(ImDrawList* dl, const ImVec2& bMin, const ImVec2& bMax,
                              ImU32 bgCol, ImU32 borderCol, float alpha = 1.0f,
                              float geometryScale = 1.0f) {
        if (!dl || alpha <= 0.0f) return;
        const auto* s = Settings::GetSingleton();
        geometryScale = (std::max)(geometryScale, 0.01f);
        const float r = MapTheme::kPanelRounding * geometryScale;
        const float borderW = MapTheme::kPanelBorderW * geometryScale;

        // 1) Soft drop shadow: stacked offset rects fake blur and anchor the card.
        if (s->bgShadow) {
            for (int i = 2; i >= 1; --i) {
                const float grow = static_cast<float>(i) * geometryScale;
                dl->AddRectFilled(
                    ImVec2(bMin.x - grow + 0.5f * geometryScale,
                           bMin.y - grow + 1.0f * geometryScale),
                    ImVec2(bMax.x + grow + 0.5f * geometryScale,
                           bMax.y + grow + 1.0f * geometryScale),
                    IM_COL32(0, 0, 0, static_cast<int>(28.0f * alpha)),
                    r + grow);
            }
        }

        const auto bgA = static_cast<std::uint8_t>(((bgCol >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        if (bgA > 0) {
            if (s->bgGradient) {
                const auto bgR = static_cast<std::uint8_t>((bgCol >> IM_COL32_R_SHIFT) & 0xFF);
                const auto bgG = static_cast<std::uint8_t>((bgCol >> IM_COL32_G_SHIFT) & 0xFF);
                const auto bgB = static_cast<std::uint8_t>((bgCol >> IM_COL32_B_SHIFT) & 0xFF);
                auto lift = [](std::uint8_t c) {
                    return static_cast<std::uint8_t>((std::min)(255.0f, c * 1.20f + 14.0f));
                };
                const ImU32 topCol = IM_COL32(lift(bgR), lift(bgG), lift(bgB), 255);
                const ImU32 botCol = IM_COL32(static_cast<std::uint8_t>(bgR * 0.55f),
                                              static_cast<std::uint8_t>(bgG * 0.55f),
                                              static_cast<std::uint8_t>(bgB * 0.55f), 255);
                const int v0 = dl->VtxBuffer.Size;
                dl->AddRectFilled(bMin, bMax, IM_COL32(255, 255, 255, bgA), r);
                const int v1 = dl->VtxBuffer.Size;
                ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, v1,
                    bMin, ImVec2(bMin.x, bMax.y), topCol, botCol);
            } else {
                dl->AddRectFilled(bMin, bMax, WithAlpha(bgCol, alpha), r);
            }
        }

        // 3) Inner vignette: dark inset perimeter stroke darkens the fill toward edges.
        if (s->bgVignette && bgA > 0) {
            dl->AddRect(
                ImVec2(bMin.x + 1.5f * geometryScale, bMin.y + 1.5f * geometryScale),
                ImVec2(bMax.x - 1.5f * geometryScale, bMax.y - 1.5f * geometryScale),
                IM_COL32(0, 0, 0, static_cast<int>(45.0f * alpha)),
                (std::max)(r - 1.0f * geometryScale, 0.0f), 0, 3.0f * geometryScale);
        }

        // 4) Top inner highlight: 1px light catch line just inside the top edge.
        if (s->bgHighlight && bgA > 0) {
            const float hPad = (r > 0.0f) ? r : 1.0f * geometryScale;
            dl->AddRectFilled(
                ImVec2(bMin.x + hPad, bMin.y + 1.0f * geometryScale),
                ImVec2(bMax.x - hPad, bMin.y + 2.0f * geometryScale),
                IM_COL32(255, 255, 255, static_cast<int>(26.0f * alpha)));
        }

        // 5) Borders: dark outer contour + metallic/accent inner border.
        if (s->bgBorder) {
            const auto bA = static_cast<std::uint8_t>(((borderCol >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
            dl->AddRect(bMin, bMax, IM_COL32(6, 6, 8, static_cast<int>(220.0f * alpha)),
                        r, 0, 1.2f * geometryScale);
            if (bA > 0) {
                dl->AddRect(
                    ImVec2(bMin.x + 1.0f * geometryScale, bMin.y + 1.0f * geometryScale),
                    ImVec2(bMax.x - 1.0f * geometryScale, bMax.y - 1.0f * geometryScale),
                    WithAlpha(borderCol, alpha),
                    (std::max)(r - 1.0f * geometryScale, 0.0f), 0, borderW);
            }
        }
    }

    inline void DrawPin(ImDrawList* dl, float px, float py, float hoverT,
                        std::uint32_t iconType = 0,
                        ID3D11ShaderResourceView* iconSrv = nullptr,
                        float scale = 1.0f,
                        ImU32 tintCol = IM_COL32(255, 255, 255, 0),
                        float overallAlpha = 1.0f,
                        float detailScale = 1.0f) {
        const float hoverLin = std::clamp(hoverT, 0.0f, 1.0f);
        constexpr float kBackC1 = 0.85f;         // half the standard 1.70158
        constexpr float kBackC3 = kBackC1 + 1.0f;
        const float inv = hoverLin - 1.0f;
        const float popT = 1.0f + kBackC3 * inv * inv * inv + kBackC1 * inv * inv;
        // 30% bigger at full hover.
        const float hoverScale = 1.0f + 0.30f * popT;

        const float r  = kPinRadius * scale * hoverScale;
        const float cx = px;
        const float cy = py;

        ImU32 tint = IM_COL32(235, 190, 60, 230);
        switch (iconType) {
            case 1:  // City
            case 2:  // Town
            case 3:  // Settlement
                tint = IM_COL32(220, 220, 240, 220); break;
            case 4:  // Cave
            case 15: // Mine
                tint = IM_COL32(160, 160, 160, 220); break;
            case 5:  // Camp
            case 16: // Imperial Camp
            case 17: // Stormcloak Camp
                tint = IM_COL32(210, 160, 110, 220); break;
            case 13: // Farm
            case 14: // WoodMill
            case 19: // WheatMill
                tint = IM_COL32(220, 200, 120, 220); break;
            case 7:  // Nordic Ruin
            case 8:  // Dwemer Ruin
            case 30: // Temple
                tint = IM_COL32(180, 180, 220, 220); break;
            case 12: // Dragon Lair
                tint = IM_COL32(220, 100, 100, 220); break;
        }

        const float fadeA = std::clamp(overallAlpha, 0.0f, 1.0f);
        auto fadeAlpha = [fadeA](ImU32 c) {
            const auto ca = static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * fadeA);
            return (c & ~IM_COL32_A_MASK) | (ca << IM_COL32_A_SHIFT);
        };

        ImU32 pinColor = fadeAlpha(LerpColor(tint, IM_COL32(255, 230, 110, 255), hoverT));

        const ImU32 outline = fadeAlpha(IM_COL32(0, 0, 0, 200));

        if (hoverLin > 0.01f) {
            const float g = fadeA * hoverLin;
            const float gr = r + 4.0f * detailScale;
            dl->AddCircleFilled(ImVec2(cx, cy), gr + 5.0f * detailScale,
                                IM_COL32(255, 225, 120, static_cast<int>(26 * g)));
            dl->AddCircleFilled(ImVec2(cx, cy), gr + 2.5f * detailScale,
                                IM_COL32(255, 225, 120, static_cast<int>(34 * g)));
            dl->AddCircleFilled(ImVec2(cx, cy), gr,
                                IM_COL32(255, 235, 150, static_cast<int>(42 * g)));
        }

        if (IsWaypointType(iconType)) {
            DrawWaypointGlyph(dl, cx, cy, r + 2.0f * detailScale, iconType, hoverT, detailScale);
            return;
        }

        if (iconSrv) {
            const float kMaxAxis = 32.0f * scale * hoverScale;
            auto sz = TextureManager::GetTextureSize(iconSrv);
            float drawW = kMaxAxis;
            float drawH = kMaxAxis;
            if (sz.width > 0 && sz.height > 0) {
                if (sz.width >= sz.height) {
                    drawW = kMaxAxis;
                    drawH = kMaxAxis * static_cast<float>(sz.height) / static_cast<float>(sz.width);
                } else {
                    drawH = kMaxAxis;
                    drawW = kMaxAxis * static_cast<float>(sz.width) / static_cast<float>(sz.height);
                }
            }
            const float halfW = drawW * 0.5f;
            const float halfH = drawH * 0.5f;
            ImVec2 imgMin(cx - halfW, cy - halfH);
            ImVec2 imgMax(cx + halfW, cy + halfH);
            const float tintStr = static_cast<float>((tintCol >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
            const ImU32 tintOpaque = (tintCol & ~IM_COL32_A_MASK) | (0xFFu << IM_COL32_A_SHIFT);
            const ImU32 baseTint = LerpColor(IM_COL32(255, 255, 255, 255), tintOpaque, tintStr);
            ImU32 imgTint = fadeAlpha(LerpColor(baseTint, IM_COL32(255, 220, 80, 255), hoverT));

            DrawIconFx(dl, iconSrv, imgMin, imgMax, imgTint);
        } else {
            // Vector fallback: colored circle centered on the point, with a
            // small dark center dot marking the exact anchor.
            dl->AddCircleFilled(ImVec2(cx, cy), r, pinColor);
            dl->AddCircle(ImVec2(cx, cy), r + 1.0f * detailScale, outline, 0, 1.5f * detailScale);
            dl->AddCircleFilled(ImVec2(cx, cy), 2.0f * detailScale, outline);
        }
    }

    inline void DrawDestinationRing(ImDrawList* dl, float px, float py, float timeSec,
                                    bool centerOnPoint = false, float scale = 1.0f) {
        (void)centerOnPoint;
        const float r  = kPinRadius * scale;
        const float cx = px;
        const float cy = py;
        const float n  = 0.5f + 0.5f * std::sin(timeSec * 2.6f);
        const float r0 = r + (8.0f + n * 4.0f) * scale;
        const float r1 = r + (14.0f + n * 5.0f) * scale;
        const ImU32 c0 = IM_COL32(80,  170, 255, static_cast<int>(130 + n * 60));
        const ImU32 c1 = IM_COL32(140, 200, 255, static_cast<int>(70  + n * 40));
        // Static anchor ring under the breathing rings so the destination
        // never visually vanishes at the bottom of the breath cycle.
        dl->AddCircle(ImVec2(cx, cy), r + 5.0f * scale, IM_COL32(80, 170, 255, 90), 0, 1.5f * scale);
        dl->AddCircle(ImVec2(cx, cy), r0, c0, 0, 2.5f * scale);
        dl->AddCircle(ImVec2(cx, cy), r1, c1, 0, 1.5f * scale);
    }

    inline void PolylineVertexColored(ImDrawList* dl,
                                      const ImVec2* pts,
                                      const ImU32* colours,
                                      int count,
                                      float thickness) {
        if (count < 2 || thickness <= 0.0f) return;

        const float half = thickness * 0.5f;
        const ImVec2 uv  = ImGui::GetDrawListSharedData()->TexUvWhitePixel;

        // Two rim vertices per point, six indices per quad between them.
        const int quadCount = count - 1;
        dl->PrimReserve(quadCount * 6, count * 2);

        const unsigned int base = dl->_VtxCurrentIdx;

        for (int i = 0; i < count; ++i) {
            // Direction of the segments touching this point.
            float dxIn = 0.0f, dyIn = 0.0f, dxOut = 0.0f, dyOut = 0.0f;
            if (i > 0) {
                dxIn = pts[i].x - pts[i - 1].x;
                dyIn = pts[i].y - pts[i - 1].y;
                const float l = std::sqrt(dxIn * dxIn + dyIn * dyIn);
                if (l > 1e-6f) { dxIn /= l; dyIn /= l; } else { dxIn = dyIn = 0.0f; }
            }
            if (i + 1 < count) {
                dxOut = pts[i + 1].x - pts[i].x;
                dyOut = pts[i + 1].y - pts[i].y;
                const float l = std::sqrt(dxOut * dxOut + dyOut * dyOut);
                if (l > 1e-6f) { dxOut /= l; dyOut /= l; } else { dxOut = dyOut = 0.0f; }
            }
            // Interior points miter between both segments; endpoints use the
            // one they have.
            float dx = dxIn + dxOut;
            float dy = dyIn + dyOut;
            const float dl2 = std::sqrt(dx * dx + dy * dy);
            if (dl2 > 1e-6f) {
                dx /= dl2; dy /= dl2;
            } else {
                // Exact reversal (180 degree fold) or a degenerate pair: fall
                // back to whichever direction exists so the rim stays finite.
                dx = (i > 0) ? dxIn : dxOut;
                dy = (i > 0) ? dyIn : dyOut;
            }

            // Perpendicular, scaled by the miter factor so the strip keeps a
            // constant visual width through bends.
            float nx = -dy, ny = dx;
            float scale = half;
            if (i > 0 && i + 1 < count) {
                const float cosHalf = dx * dxOut + dy * dyOut;  // both unit
                if (cosHalf > 1e-3f) {
                    scale = std::min(half / cosHalf, half * 2.0f);
                } else {
                    scale = half * 2.0f;  // hairpin: clamp instead of spike
                }
            }

            dl->PrimWriteVtx(ImVec2(pts[i].x + nx * scale, pts[i].y + ny * scale),
                             uv, colours[i]);
            dl->PrimWriteVtx(ImVec2(pts[i].x - nx * scale, pts[i].y - ny * scale),
                             uv, colours[i]);
        }

        for (int i = 0; i < quadCount; ++i) {
            const unsigned int a = base + i * 2;
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 3));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 3));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(a + 2));
        }
    }

    inline bool ProjectWorldPoint(RE::NiCamera* a_cam, const RE::NiPoint3& a_world,
                                  ImVec2& a_outScreen, bool& a_outInFront) {
        if (!a_cam) return false;
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        if (!RE::NiCamera::WorldPtToScreenPt3(
                a_cam->GetRuntimeData().worldToCam,
                a_cam->GetRuntimeData2().port,
                a_world, sx, sy, sz, 1e-5f)) {
            return false;
        }
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        // Y flip: sx/sy are 0..1 with a bottom-left origin. Same convention as
        // every map pin (MapMarkerRenderer.cpp:109).
        a_outScreen  = ImVec2(sx * disp.x, (1.0f - sy) * disp.y);
        a_outInFront = sz > 0.0f;
        return true;
    }

    inline bool SegmentInRect(const ImVec2& a, const ImVec2& b,
                              float minX, float minY, float maxX, float maxY,
                              float& a_outT0, float& a_outT1) {
        float t0 = 0.0f, t1 = 1.0f;
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;

        const float p[4] = { -dx, dx, -dy, dy };
        const float q[4] = { a.x - minX, maxX - a.x, a.y - minY, maxY - a.y };

        for (int i = 0; i < 4; ++i) {
            if (std::fabs(p[i]) < 1e-6f) {
                // Parallel to this edge: outside it means no overlap at all.
                if (q[i] < 0.0f) return false;
                continue;
            }
            const float r = q[i] / p[i];
            if (p[i] < 0.0f) {
                if (r > t1) return false;
                if (r > t0) t0 = r;
            } else {
                if (r < t0) return false;
                if (r < t1) t1 = r;
            }
        }
        a_outT0 = t0;
        a_outT1 = t1;
        return true;
    }

    // Fixed ceiling for beam vertex arrays. Stack-allocated per call because
    // these primitives run every frame for every visible beam.
    constexpr int kMaxBeamSteps = 32;

    template <typename AlphaAtFn>
    inline void DrawFeatheredBeam(
        ImDrawList* dl,
        const ImVec2& a_baseScr, const ImVec2& a_topScr,
        int a_steps,
        float a_wBase, float a_wTop,
        ImU32 a_coreCol, ImU32 a_glowCol, ImU32 a_haloCol,
        float a_alphaScale,
        AlphaAtFn a_alphaAt,
        float a_filamentThickness = -1.0f)
    {
        if (!dl || a_steps < 2) return;
        a_steps = std::clamp(a_steps, 2, kMaxBeamSteps);

        const float dx = a_topScr.x - a_baseScr.x;
        const float dy = a_topScr.y - a_baseScr.y;
        const float fullLen = std::sqrt(dx * dx + dy * dy);
        if (fullLen < 1.0f) return;

        // Normal perpendicular to beam axis
        const float nx = -dy / fullLen;
        const float ny =  dx / fullLen;

        constexpr float kRailOffsets[5] = { -1.0f, -0.32f, 0.0f, 0.32f, 1.0f };
        const ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
        ImVec2 spinePts[kMaxBeamSteps];
        float  spineAlpha[kMaxBeamSteps];

        // Reserve vertices: a_steps rows * 5 rails
        const int totalVtx = a_steps * 5;
        const int totalIdx = (a_steps - 1) * 4 * 6; // (steps - 1) slices * 4 quads * 6 indices
        dl->PrimReserve(totalIdx, totalVtx);
        const unsigned int vBase = dl->_VtxCurrentIdx;

        for (int s = 0; s < a_steps; ++s) {
            const float f = static_cast<float>(s) / static_cast<float>(a_steps - 1);
            const ImVec2 spinePt(a_baseScr.x + dx * f, a_baseScr.y + dy * f);
            const float currentW = a_wBase + (a_wTop - a_wBase) * f;
            const float sliceAlpha = std::clamp(a_alphaScale * a_alphaAt(f), 0.0f, 1.0f);
            spinePts[s] = spinePt;
            spineAlpha[s] = sliceAlpha;

            // 5 rail colors for this slice
            const ImU32 colOuter = MapDraw::WithAlpha(a_haloCol, 0.0f);
            const ImU32 colInner = MapDraw::WithAlpha(a_glowCol, 0.65f * sliceAlpha);
            const ImU32 colCore  = MapDraw::WithAlpha(a_coreCol, 0.95f * sliceAlpha);
            const ImU32 sliceCols[5] = { colOuter, colInner, colCore, colInner, colOuter };

            for (int r = 0; r < 5; ++r) {
                const float offset = kRailOffsets[r] * currentW;
                dl->PrimWriteVtx(ImVec2(spinePt.x + nx * offset, spinePt.y + ny * offset), uv, sliceCols[r]);
            }
        }

        // Write quad indices across slices
        for (int s = 0; s < a_steps - 1; ++s) {
            const unsigned int row0 = vBase + s * 5;
            const unsigned int row1 = row0 + 5;
            for (int r = 0; r < 4; ++r) {
                const auto i0 = static_cast<ImDrawIdx>(row0 + r);
                const auto i1 = static_cast<ImDrawIdx>(row0 + r + 1);
                const auto i2 = static_cast<ImDrawIdx>(row1 + r + 1);
                const auto i3 = static_cast<ImDrawIdx>(row1 + r);

                dl->PrimWriteIdx(i0); dl->PrimWriteIdx(i1); dl->PrimWriteIdx(i2);
                dl->PrimWriteIdx(i0); dl->PrimWriteIdx(i2); dl->PrimWriteIdx(i3);
            }
        }

        // Segment the filament so it follows the same fade and occlusion profile
        // as the mesh instead of shining uniformly through blocked geometry.
        for (int s = 0; s < a_steps - 1; ++s) {
            const float a = 0.5f * (spineAlpha[s] + spineAlpha[s + 1]);
            // Modern supplies a scaled cap for narrow shafts; other callers keep 1.2 px.
            const float f0 = static_cast<float>(s) / static_cast<float>(a_steps - 1);
            const float f1 = static_cast<float>(s + 1) / static_cast<float>(a_steps - 1);
            const float width0 = a_wBase + (a_wTop - a_wBase) * f0;
            const float width1 = a_wBase + (a_wTop - a_wBase) * f1;
            const float thickness = a_filamentThickness < 0.0f
                ? 1.2f : (std::min)(a_filamentThickness, width0 + width1);
            if (thickness > 0.0f) {
                dl->AddLine(spinePts[s], spinePts[s + 1],
                            WithAlpha(IM_COL32(255, 255, 255, 220), a), thickness);
            }
        }
    }

    // Shared frame used by loot and destination beacons. Icon sizing and
    // content remain caller-owned because the two surfaces use different art.
    inline void DrawDiamondBadgeFrame(
        ImDrawList* dl, const ImVec2& a_center, float a_radius,
        ImU32 a_coreCol, ImU32 a_glowCol, ImU32 a_haloCol, float a_alphaScale,
        float a_glowPadding = 2.5f, float a_innerAlpha = 0.45f,
        float a_innerMinRadius = 4.0f)
    {
        if (!dl || a_radius <= 0.0f) return;

        const ImVec2 top(a_center.x, a_center.y - a_radius);
        const ImVec2 right(a_center.x + a_radius, a_center.y);
        const ImVec2 bottom(a_center.x, a_center.y + a_radius);
        const ImVec2 left(a_center.x - a_radius, a_center.y);

        const float glowR = a_radius + a_glowPadding;
        dl->AddQuad(
            ImVec2(a_center.x, a_center.y - glowR),
            ImVec2(a_center.x + glowR, a_center.y),
            ImVec2(a_center.x, a_center.y + glowR),
            ImVec2(a_center.x - glowR, a_center.y),
            WithAlpha(a_haloCol, 0.40f * a_alphaScale), 3.5f);
        dl->AddQuadFilled(top, right, bottom, left,
                          WithAlpha(IM_COL32(14, 16, 22, 235), a_alphaScale));
        dl->AddQuad(top, right, bottom, left,
                    WithAlpha(a_glowCol, a_alphaScale), 2.0f);

        const float innerR = a_radius - 3.5f;
        if (innerR > a_innerMinRadius) {
            dl->AddQuad(
                ImVec2(a_center.x, a_center.y - innerR),
                ImVec2(a_center.x + innerR, a_center.y),
                ImVec2(a_center.x, a_center.y + innerR),
                ImVec2(a_center.x - innerR, a_center.y),
                WithAlpha(a_coreCol, a_innerAlpha * a_alphaScale), 1.0f);
        }
    }

    enum class TrailStyle {
        Dots,          // shipped: breadcrumb dots + periodic direction chevrons
        LuminousFlow,  // concept 1: layered glow polyline + chevrons on top
    };
    inline constexpr TrailStyle kTrailStyle = TrailStyle::LuminousFlow;

    inline void DrawPathTrail(ImDrawList* dl,
                              const std::vector<std::vector<ImVec2>>& segmentsIn,
                              bool headNearPlayer,
                              bool isTracking,
                              ImVec2 clipCenter = ImVec2(0.0f, 0.0f),
                              float clipRadius = 0.0f,
                              float headingAngle = 0.0f,
                              bool drawHeadCone = false,
                              const std::vector<RevisitHit>* revisits = nullptr,
                              std::size_t tailWindow = 0,
                              float alphaMul = 1.0f,
                              const std::vector<std::vector<float>>* worldZ = nullptr,
                              [[maybe_unused]] float playerZ = 0.0f,
                              const RoundedRectBoundary* clipBoundary = nullptr) {
        auto* trailSet = Settings::GetSingleton();

        bool  useElevation = worldZ && worldZ->size() == segmentsIn.size();
        float elevMinZ = 0.0f, elevMaxZ = 0.0f, elevSpan = 0.0f;
        if (useElevation) {
            elevMinZ =  1e30f;
            elevMaxZ = -1e30f;
            for (const auto& zs : *worldZ) {
                for (float z : zs) {
                    elevMinZ = std::min(elevMinZ, z);
                    elevMaxZ = std::max(elevMaxZ, z);
                }
            }
            elevSpan = elevMaxZ - elevMinZ;
            // Flat cell: do NOT auto-scale ~8 units of same-floor noise across
            // the whole ramp, that paints a fake rainbow on level ground.
            if (elevSpan < MapTheme::kPathElevMinSpan) useElevation = false;
        }

        constexpr float kTrailGlyphExtent = 9.0f;
        const bool hasCircularClip = clipRadius > 0.0f;
        const float safeRadius = std::max(0.0f, clipRadius - kTrailGlyphExtent);
        const float safeRadiusSq = safeRadius * safeRadius;
        auto insideClip = [&](const ImVec2& p) {
            if (clipBoundary) {
                return clipBoundary->Contains(p, kTrailGlyphExtent);
            }
            if (!hasCircularClip) return true;
            const float dx = p.x - clipCenter.x;
            const float dy = p.y - clipCenter.y;
            return dx * dx + dy * dy <= safeRadiusSq;
        };

        const float timeSec = std::chrono::duration<float>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const ImU32 trailColorBase = IM_COL32(trailSet->pathTrailColorR, trailSet->pathTrailColorG,
                                              trailSet->pathTrailColorB, 255);
        const float trailAlpha = std::clamp(trailSet->pathTrailColorA / 255.0f, 0.0f, 1.0f) *
                                  std::clamp(alphaMul, 0.0f, 1.0f);
        const float dotRadius = std::clamp(trailSet->pathTrailThickness, 1.0f, 8.0f);
        const bool fade = trailSet->pathTrailFadeByAge;
        const bool smartSpace = trailSet->pathTrailSmartSpacing;

        auto withAlpha = [](ImU32 col, float alpha) {
            return (col & 0x00FFFFFF) |
                   (static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f) << 24);
        };

        // Keep the tail window on source points so smoothing and arc-length
        // phasing do not change as the radar scrolls.
        std::vector<std::vector<ImVec2>> trimmed;
        const std::vector<std::vector<ImVec2>>* segsP = &segmentsIn;
        bool droppedOrigin = false;
        if (tailWindow > 0) {
            std::size_t remaining = tailWindow;
            std::vector<std::vector<ImVec2>> rev;
            for (std::size_t s = segmentsIn.size(); s-- > 0 && remaining > 0; ) {
                const auto& seg = segmentsIn[s];
                if (seg.size() <= remaining) {
                    rev.push_back(seg);
                    remaining -= seg.size();
                } else {
                    rev.emplace_back(seg.end() - remaining, seg.end());
                    remaining = 0;
                    if (s != 0) droppedOrigin = true;
                }
            }
            if (segmentsIn.size() > rev.size()) droppedOrigin = true;
            std::reverse(rev.begin(), rev.end());
            trimmed = std::move(rev);
            segsP = &trimmed;
        }
        const auto& segments = *segsP;

        std::size_t globalSrcCount = 0;
        for (const auto& seg : segments) globalSrcCount += seg.size();
        const float globalDen = (globalSrcCount > 1)
            ? static_cast<float>(globalSrcCount - 1)
            : 1.0f;
        std::size_t globalSrcBase = 0;

        for (std::size_t s = 0; s < segments.size(); ++s) {
            const auto& segPts = segments[s];
            if (segPts.size() < 4) {
                globalSrcBase += segPts.size();
                continue;
            }
            const bool isNewestVisitedSegment = (s + 1 == segments.size());
            const float segmentAlpha = trailAlpha;

            std::vector<float> turn(segPts.size(), 0.0f);
            for (std::size_t k = 1; k + 1 < segPts.size(); ++k) {
                const ImVec2 a(segPts[k].x - segPts[k - 1].x,
                               segPts[k].y - segPts[k - 1].y);
                const ImVec2 b(segPts[k + 1].x - segPts[k].x,
                               segPts[k + 1].y - segPts[k].y);
                const float la = std::sqrt(a.x * a.x + a.y * a.y);
                const float lb = std::sqrt(b.x * b.x + b.y * b.y);
                if (la < 1e-4f || lb < 1e-4f) continue;
                const float d = (a.x * b.x + a.y * b.y) / (la * lb);
                turn[k] = std::acos(std::clamp(d, -1.0f, 1.0f));
            }

            std::vector<ImVec2> smoothPath;
            std::vector<float> smoothSrcU;
            if (segPts.size() < 3) {
                smoothPath = segPts;
                smoothSrcU.reserve(segPts.size());
                for (std::size_t i = 0; i < segPts.size(); ++i) {
                    smoothSrcU.push_back(static_cast<float>(i));
                }
            } else {
                smoothPath.reserve((segPts.size() - 1) * 8 + 1);
                smoothSrcU.reserve(smoothPath.capacity());
                for (std::size_t i = 0; i < segPts.size() - 1; ++i) {
                    const ImVec2 p0 = (i == 0) ? segPts[i] : segPts[i - 1];
                    const ImVec2 p1 = segPts[i];
                    const ImVec2 p2 = segPts[i + 1];
                    const ImVec2 p3 = (i + 2 < segPts.size()) ? segPts[i + 2] : p2;

                    for (int j = 0; j < 8; ++j) {
                        const float t = static_cast<float>(j) / 8.0f;
                        const float t2 = t * t;
                        const float t3 = t2 * t;
                        smoothPath.emplace_back(
                            0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t +
                                     (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
                                     (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3),
                            0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t +
                                     (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                                     (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3));
                        smoothSrcU.push_back(static_cast<float>(i) + t);
                    }
                }
                smoothPath.push_back(segPts.back());
                smoothSrcU.push_back(static_cast<float>(segPts.size() - 1));
            }

            auto ageAt = [&](float srcU) {
                return std::clamp((static_cast<float>(globalSrcBase) + srcU) / globalDen,
                                  0.0f, 1.0f);
            };
            auto turnAt = [&](float srcU) {
                int k = static_cast<int>(srcU + 0.5f);
                k = std::clamp(k, 0, static_cast<int>(turn.size()) - 1);
                return turn[k];
            };

            if (kTrailStyle == TrailStyle::LuminousFlow && smoothPath.size() >= 2) {
                std::vector<ImVec2> run;
                std::vector<ImU32>  runCol;
                run.reserve(smoothPath.size());
                runCol.reserve(smoothPath.size());

                // Per-point alpha, same curve the dots use.
                auto alphaAtIdx = [&](std::size_t i) {
                    return fade ? segmentAlpha * (0.30f + 0.70f * ageAt(smoothSrcU[i]))
                                : segmentAlpha;
                };

                auto elevAtIdx = [&](std::size_t i) -> ImU32 {
                    if (!useElevation) return trailColorBase;
                    const auto& zseg = (*worldZ)[s];
                    if (zseg.size() != segPts.size() || zseg.empty()) return trailColorBase;

                    const float u  = std::clamp(smoothSrcU[i], 0.0f,
                                                static_cast<float>(zseg.size() - 1));
                    const auto  i0 = static_cast<std::size_t>(u);
                    const std::size_t i1 = std::min(i0 + 1, zseg.size() - 1);
                    const float t  = u - static_cast<float>(i0);
                    const float z  = zseg[i0] + (zseg[i1] - zseg[i0]) * t;

                    // 0 at the cell's floor, 1 at its ceiling.
                    const float k = std::clamp((z - elevMinZ) / std::max(1.0f, elevSpan),
                                               0.0f, 1.0f);
                    auto lerpCh = [&](int shift) {
                        const float a = static_cast<float>((MapTheme::kPathElevLow  >> shift) & 0xFF);
                        const float b = static_cast<float>((MapTheme::kPathElevHigh >> shift) & 0xFF);
                        return static_cast<ImU32>(a + (b - a) * k + 0.5f) & 0xFF;
                    };
                    return IM_COL32(lerpCh(IM_COL32_R_SHIFT),
                                    lerpCh(IM_COL32_G_SHIFT),
                                    lerpCh(IM_COL32_B_SHIFT), 255);
                };

                auto strokeRun = [&]() {
                    if (run.size() < 2) return;
                    const float w = std::max(1.5f, dotRadius * 1.5f);
                    const int   n = static_cast<int>(run.size());

                    std::vector<ImU32> pass(run.size());

                    auto fill = [&](ImU32 rgbOverride, float mul) {
                        for (std::size_t k = 0; k < run.size(); ++k) {
                            const ImU32 rgb = rgbOverride ? rgbOverride : runCol[k];
                            pass[k] = (rgb & 0x00FFFFFF) |
                                (static_cast<ImU32>(std::clamp(
                                    ((runCol[k] >> 24) & 0xFF) / 255.0f * mul,
                                    0.0f, 1.0f) * 255.0f + 0.5f) << 24);
                        }
                    };

                    fill(0, 0.18f);
                    PolylineVertexColored(dl, run.data(), pass.data(), n, w * 2.6f);
                    fill(0, 0.45f);
                    PolylineVertexColored(dl, run.data(), pass.data(), n, w * 1.5f);
                    fill(IM_COL32(255, 255, 255, 255), 0.75f);
                    PolylineVertexColored(dl, run.data(), pass.data(), n,
                                          std::max(1.0f, w * 0.5f));
                };

                for (std::size_t i = 0; i < smoothPath.size(); ++i) {
                    if (insideClip(smoothPath[i])) {
                        run.push_back(smoothPath[i]);
                        // RGB = elevation tint, A = age fade. fill() splits them
                        // back apart per pass.
                        runCol.push_back(withAlpha(elevAtIdx(i), alphaAtIdx(i)));
                    } else if (!run.empty()) {
                        strokeRun();
                        run.clear();
                        runCol.clear();
                    }
                }
                if (!run.empty()) strokeRun();
            }

            float emitAt = 0.0f;
            float walked = 0.0f;
            int gapCounter = 0;
            // Counts emitted glyphs (dot OR chevron slot) along this segment; every
            // kChevronEvery-th slot draws a direction chevron instead of a dot.
            int emitIndex = 0;
            constexpr int kChevronEvery = 8;
            for (std::size_t i = 1; i < smoothPath.size(); ++i) {
                const ImVec2 a = smoothPath[i - 1];
                const ImVec2 b = smoothPath[i];
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                const float segLen = std::sqrt(dx * dx + dy * dy);
                if (segLen <= 1e-4f) continue;

                while (emitAt <= walked + segLen) {
                    const float t = (emitAt - walked) / segLen;
                    const ImVec2 p(a.x + dx * t, a.y + dy * t);
                    const float srcU = smoothSrcU[i - 1] +
                                       (smoothSrcU[i] - smoothSrcU[i - 1]) * t;
                    const float age = ageAt(srcU);
                    const float curv = turnAt(srcU);
                    const bool inGap = (kTrailStyle != TrailStyle::LuminousFlow) &&
                        (kGapEvery > 0) &&
                        (gapCounter % kGapEvery) >= (kGapEvery - kGapSkip);

                    if (!inGap && insideClip(p)) {
                        const bool isChevron = (emitIndex % kChevronEvery) == 0 && emitIndex > 0;
                        const float a01 = fade
                            ? segmentAlpha * (0.30f + 0.70f * age)
                            : segmentAlpha;
                        if (isChevron && (dx * dx + dy * dy) > 1e-4f) {
                            // Solid filled triangle pointing along the local trail
                            // heading. Forward unit vector = trail direction here.
                            const float inv = 1.0f / segLen;
                            const float fx = dx * inv, fy = dy * inv;
                            const float px = -fy, py = fx;  // perpendicular
                            const float len = dotRadius * 2.4f;  // tip distance
                            const float wid = dotRadius * 1.6f;  // half base width
                            // Tip ahead, two base corners behind and to the sides.
                            const ImVec2 tip(p.x + fx * len, p.y + fy * len);
                            const ImVec2 baseL(p.x - fx * len * 0.4f + px * wid,
                                               p.y - fy * len * 0.4f + py * wid);
                            const ImVec2 baseR(p.x - fx * len * 0.4f - px * wid,
                                               p.y - fy * len * 0.4f - py * wid);
                            const ImU32 chevOutline = withAlpha(IM_COL32(0, 0, 0, 255), a01);
                            const ImU32 chevColor = withAlpha(trailColorBase, a01);
                            dl->AddTriangleFilled(tip, baseL, baseR, chevColor);
                            dl->AddTriangle(tip, baseL, baseR, chevOutline, 1.2f);
                        } else if constexpr (kTrailStyle != TrailStyle::LuminousFlow) {
                            const float radius = fade
                                ? dotRadius * (0.65f + 0.35f * age)
                                : dotRadius;
                            dl->AddCircleFilled(p, radius + 1.0f,
                                                withAlpha(IM_COL32(0, 0, 0, 255),
                                                          a01 * (110.0f / 255.0f)));
                            dl->AddCircleFilled(p, radius, withAlpha(trailColorBase, a01));
                        }
                    }

                    ++gapCounter;
                    ++emitIndex;
                    float spacing = kDotSpacingMax;
                    if (smartSpace) {
                        const float turnFactor = std::clamp(curv / kTurnThresholdRad, 0.0f, 1.0f);
                        spacing = kDotSpacingMax + (kDotSpacingMin - kDotSpacingMax) * turnFactor;
                    }
                    emitAt += spacing;
                }
                walked += segLen;
            }

            if (s == 0 && !droppedOrigin && insideClip(smoothPath.front())) {
                const float originAlpha = fade ? trailAlpha * 0.30f : trailAlpha;
                dl->AddCircleFilled(smoothPath.front(), 5.0f,
                                    withAlpha(IM_COL32(80, 190, 90, 255),
                                              std::max(originAlpha, 0.5f)));
                dl->AddCircle(smoothPath.front(), 5.0f,
                              withAlpha(IM_COL32(0, 0, 0, 255),
                                        std::max(originAlpha, 0.5f)), 0, 1.5f);
            } else if (s != 0 && !droppedOrigin && insideClip(smoothPath.front())) {
                dl->AddCircle(smoothPath.front(), 5.0f,
                              withAlpha(IM_COL32(0, 0, 0, 255), segmentAlpha * 0.65f), 0, 3.0f);
                dl->AddCircle(smoothPath.front(), 5.0f,
                              withAlpha(trailColorBase, segmentAlpha * 0.85f), 0, 1.8f);
            }

            if (isNewestVisitedSegment && insideClip(smoothPath.back())) {
                const ImVec2 head = smoothPath.back();
                const float fadeMul = std::clamp(alphaMul, 0.0f, 1.0f);
                const ImU32 pulseCol = withAlpha(IM_COL32(trailSet->pathPulseColorR,
                                                trailSet->pathPulseColorG,
                                                trailSet->pathPulseColorB, 255), fadeMul);
                (void)drawHeadCone;
                (void)headingAngle;

                const bool doPulse = trailSet->pathPulseEnabled && isTracking && headNearPlayer;
                const float headR = doPulse ? 4.0f + 1.5f * std::sin(timeSec * 4.0f) : 4.0f;
                dl->AddCircleFilled(head, headR, pulseCol);
                dl->AddCircle(head, headR, withAlpha(IM_COL32(0, 0, 0, 220), fadeMul), 0, 1.5f);
            }

            globalSrcBase += segPts.size();
        }

        // Pulse revisit feedback over existing dots. The caller supplies copied
        // screen-space positions, so no tracker lock is held during drawing.
        if (revisits) {
            for (const auto& hit : *revisits) {
                const ImVec2 p(hit.pos.x, hit.pos.y);
                if (!insideClip(p)) continue;
                const float ageSec = std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - hit.when).count();
                const float glow = 1.0f - std::clamp(ageSec / kRevisitGlowSec, 0.0f, 1.0f);
                if (glow <= 0.0f) continue;
                const float pulse = 0.5f + 0.5f * std::sin(ageSec * 8.0f);
                const float coreRadius = 3.0f + 1.0f * pulse;
                const float ringRadius = 5.0f + 4.0f * pulse;
                const float revisitFade = std::clamp(alphaMul, 0.0f, 1.0f);
                dl->AddCircleFilled(p, coreRadius,
                                    withAlpha(trailColorBase, 0.35f * glow * revisitFade));
                dl->AddCircle(p, ringRadius,
                              withAlpha(trailColorBase, 0.65f * glow * revisitFade), 0, 1.5f);
            }
        }
    }

    inline void DrawStar(ImDrawList* dl, ImVec2 center, float r, ImU32 fill, ImU32 outline, float stroke = 1.0f) {
        constexpr int kPoints = 5;
        constexpr float kInnerRatio = 0.45f;
        // IM_PI lives in imgui_internal.h, which we don't include; local pi.
        constexpr float kPi = 3.14159265f;
        ImVec2 v[kPoints * 2];
        for (int i = 0; i < kPoints * 2; ++i) {
            const float ang = -kPi / 2.0f + i * (kPi / kPoints);
            const float rad = (i % 2 == 0) ? r : r * kInnerRatio;
            v[i] = ImVec2(center.x + std::cos(ang) * rad, center.y + std::sin(ang) * rad);
        }
        for (int i = 0; i < kPoints * 2; ++i) {
            int next = (i + 1) % (kPoints * 2);
            dl->AddTriangleFilled(center, v[i], v[next], fill);
        }
        dl->AddPolyline(v, kPoints * 2, outline, ImDrawFlags_Closed, stroke);
    }

    inline void DrawWaypointGlyph(ImDrawList* dl, float cx, float cy, float r,
                                  std::uint32_t iconType, float hoverT, float detailScale) {
        const ImU32 dark  = IM_COL32(0, 0, 0, 200);
        const ImU32 white = IM_COL32(255, 255, 255, 235);
        const ImVec2 c(cx, cy);

        if (iconType >= 100 && iconType <= 104) {
            const ImU32 ring = LerpColor(white, IM_COL32(255, 230, 110, 255), hoverT);
            dl->AddCircleFilled(c, r, IM_COL32(35, 38, 48, 235));
            dl->AddCircle(c, r, ring, 0, 1.8f * detailScale);
            char digit[2] = { static_cast<char>('1' + (iconType - 100)), '\0' };
            ImFont* font = ImGui::GetFont();
            const float fs = r * 1.5f;
            ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, digit);
            dl->AddText(font, fs, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), white, digit);
            return;
        }

        if (iconType >= kQuickMarkBase) {
            const ImU32 ring = LerpColor(white, IM_COL32(255, 230, 110, 255), hoverT);
            dl->AddCircleFilled(c, r, IM_COL32(35, 38, 48, 235));
            dl->AddCircle(c, r, ring, 0, 1.8f * detailScale);
            char label[8];
            const int len = std::snprintf(label, sizeof(label), "%u", iconType - kQuickMarkBase + 1);
            ImFont* font = ImGui::GetFont();
            // One digit gets the same size as the manual picker; each extra
            // digit shrinks the font so 2-3 digit numbers still fit the disc.
            const float fs = r * 1.5f * (len <= 1 ? 1.0f : (len == 2 ? 0.75f : 0.6f));
            ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
            dl->AddText(font, fs, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), white, label);
            return;
        }

        if (iconType >= 110 && iconType <= 113) {
            // Color dot. Order matches the note-dialog grid: R, G, B, Y.
            static constexpr ImU32 kDot[4] = {
                IM_COL32(220,  70,  60, 235),   // red
                IM_COL32( 80, 190,  90, 235),   // green
                IM_COL32( 70, 140, 230, 235),   // blue
                IM_COL32(235, 200,  70, 235),   // yellow
            };
            const ImU32 fill = LerpColor(kDot[iconType - 110],
                                         IM_COL32(255, 230, 110, 255), hoverT);
            dl->AddCircleFilled(c, r * 0.85f, fill);
            dl->AddCircle(c, r * 0.85f, dark, 0, 1.5f * detailScale);
            return;
        }

        // 120 (and any future >= 100 fallback): gold star.
        const ImU32 fill = LerpColor(IM_COL32(240, 195, 70, 240),
                                     IM_COL32(255, 230, 110, 255), hoverT);
        DrawStar(dl, c, r + 1.0f * detailScale, fill, dark, 1.0f * detailScale);
    }

    inline bool DrawCategoryGrid(const char* const* names, const std::uint32_t* values,
                                 int count, int& selected,
                                 int perRow = 10, float cellSz = 30.0f,
                                 bool showTooltip = true,
                                 float geometryScale = 1.0f,
                                 const ImVec2* controllerPos = nullptr) {
        bool changed = false;
        int col = 0;
        geometryScale = (std::max)(geometryScale, 0.01f);
        const float cellExtra = 8.0f * geometryScale;
        const float rounding  = 4.0f * geometryScale;
        const float borderW   = 2.0f * geometryScale;
        for (int i = 0; i < count; ++i) {
            if (i > 0 && IsWaypointType(values[i - 1]) && !IsWaypointType(values[i])) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                col = 0;
            }
            if (col) ImGui::SameLine();
            col = (col + 1) % perRow;

            const bool sel = (selected == i);
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Button,
                sel ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                    : ImVec4(0, 0, 0, 0));
            bool clicked = false;
            if (IsWaypointType(values[i])) {
                // Vector glyph cell: invisible hit box, glyph drawn on
                // the window list at the cell center.
                const ImVec2 cell(cellSz + cellExtra, cellSz + cellExtra);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                clicked = ImGui::InvisibleButton("##cat", cell);
                ImDrawList* wdl = ImGui::GetWindowDrawList();
                if (ImGui::IsItemHovered() && !sel)
                    wdl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                       ImGui::GetColorU32(ImGuiCol_ButtonHovered, 0.4f), rounding);
                DrawWaypointGlyph(wdl,
                    pos.x + cell.x * 0.5f, pos.y + cell.y * 0.5f,
                    cellSz * 0.38f, values[i], 0.0f, geometryScale);
            } else if (auto* srv = TextureManager::GetCategoryIcon(values[i])) {
                clicked = ImGui::ImageButton("##cat",
                    reinterpret_cast<ImTextureID>(srv), ImVec2(cellSz, cellSz));
            } else {
                clicked = ImGui::Button(names[i], ImVec2(cellSz + cellExtra, cellSz + cellExtra));
            }
            ImGui::PopStyleColor();
            if (controllerPos) {
                const ImVec2 mn = ImGui::GetItemRectMin();
                const ImVec2 mx = ImGui::GetItemRectMax();
                clicked = clicked || (controllerPos->x >= mn.x && controllerPos->x <= mx.x &&
                                      controllerPos->y >= mn.y && controllerPos->y <= mx.y);
            }
            if (clicked) { selected = i; changed = true; }
            if (sel) {
                // Border ring so the active pick reads even on image cells.
                ImDrawList* gridDl = ImGui::GetWindowDrawList();
                gridDl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                ImGui::GetColorU32(ImGuiCol_CheckMark), rounding, 0, borderW);
            }
            if (showTooltip && !IsWaypointType(values[i]) && ImGui::IsItemHovered()) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * geometryScale, 5.0f * geometryScale));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * geometryScale);
                if (ImGui::BeginTooltip()) {
                    ImGui::TextUnformatted(names[i]);
                    ImGui::EndTooltip();
                }
                ImGui::PopStyleVar(2);
            }
            ImGui::PopID();
        }
        return changed;
    }

    inline ImVec2 g_cursorReq{ -1.0f, -1.0f };

    inline void RequestCursor(float mx, float my) { g_cursorReq = ImVec2(mx, my); }

    inline void DrawCursor(ImDrawList* dl, float mx, float my);

    // Call once per frame after every renderer has drawn. Clears the request, so
    // a frame where no map surface asked for a cursor draws none.
    inline void DrawRequestedCursor() {
        const ImVec2 c = g_cursorReq;
        g_cursorReq = ImVec2(-1.0f, -1.0f);
        if (c.x >= 0.0f && c.y >= 0.0f) {
            DrawCursor(ImGui::GetForegroundDrawList(), c.x, c.y);
        }
    }

    inline void DrawCursor(ImDrawList* dl, float mx, float my) {
        static ID3D11ShaderResourceView* s_cursorSrv =
            TextureManager::GetTexture("Data\\Interface\\CustomMarkers\\Icons\\cursor.png");

        const float dispH = ImGui::GetIO().DisplaySize.y;
        const float rawScale = dispH > 0.0f ? (dispH / 1080.0f) : 1.0f;
        const float resScale = 1.0f + (rawScale - 1.0f) * 0.5f;

        if (s_cursorSrv) {
            const auto sz = TextureManager::GetTextureSize(s_cursorSrv);
            if (sz.width > 0 && sz.height > 0) {
                // Matches Music Conductor native 38px height at 1080p.
                constexpr float kBaseCursorHeightPx = 38.0f;
                const float aspect = static_cast<float>(sz.width) /
                                     static_cast<float>(sz.height);
                const float h = std::round(kBaseCursorHeightPx * resScale);
                const float w = std::round(h * aspect);
                dl->AddImage(reinterpret_cast<ImTextureID>(s_cursorSrv),
                             ImVec2(mx, my), ImVec2(mx + w, my + h));
                return;
            }
        }

        // Fallback: hand-drawn arrow when cursor.png is missing or failed load.
        const float s = 1.7f * resScale;
        const ImVec2 base[7] = {
            {  0.0f,  0.0f }, {  0.0f, 16.0f }, {  3.7f, 12.3f },
            {  6.0f, 17.6f }, {  8.4f, 16.5f }, {  5.8f, 11.5f }, { 11.0f, 11.0f },
        };
        ImVec2 p[7];
        for (int i = 0; i < 7; ++i)
            p[i] = ImVec2(mx + base[i].x * s, my + base[i].y * s);
        for (int i = 1; i <= 5; ++i)
            dl->AddTriangleFilled(p[0], p[i], p[i + 1], IM_COL32(255, 255, 255, 255));
        dl->AddPolyline(p, 7, IM_COL32(0, 0, 0, 255), ImDrawFlags_Closed, 1.4f * resScale);
    }
    inline constexpr float kMapPlaneZ = 180000.0f;

    inline bool UsesFlatMapProjection(const float (&a_worldToCam)[4][4],
                                      const RE::TESWorldSpace* a_mapWorld) noexcept {
        if (a_mapWorld) {
            const float pitch = a_mapWorld->worldMapData.cameraData.initialPitch;
            const float offsetZ = a_mapWorld->worldMapOffsetData.mapOffsetZ;
            if (std::isfinite(pitch) && std::isfinite(offsetZ) &&
                std::fabs(pitch - 90.0f) <= 1.0f && offsetZ > 100000.0f) {
                return true;
            }
        }

        const float lookX = a_worldToCam[3][0];
        const float lookY = a_worldToCam[3][1];
        const float lookZ = a_worldToCam[3][2];

        if (!std::isfinite(lookX) || !std::isfinite(lookY) || !std::isfinite(lookZ)) {
            return false;
        }

        constexpr float kMaxHorizontalLookSq = 0.05f * 0.05f;
        constexpr float kMinVerticalLook = 0.95f;
        constexpr float kMaxVerticalLook = 1.05f;

        const float horizontalLookSq = lookX * lookX + lookY * lookY;
        const float verticalLook = std::fabs(lookZ);
        return horizontalLookSq <= kMaxHorizontalLookSq &&
               verticalLook >= kMinVerticalLook &&
               verticalLook <= kMaxVerticalLook;
    }

    inline RE::NiPoint3 MapProjectionPoint(const RE::NiPoint3& a_worldPos,
                                           bool a_useFlatMapPlane) noexcept {
        RE::NiPoint3 p = a_worldPos;
        if (a_useFlatMapPlane) {
            p.z = kMapPlaneZ;
        }
        return p;
    }

    inline bool ProjectionDepthValid(float a_sz) noexcept {
        return std::fabs(a_sz) > 1e-6f;
    }

} // namespace MapDraw
