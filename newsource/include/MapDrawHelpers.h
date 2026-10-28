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

    // Scale an existing color's alpha by 0..1, keeping its RGB. For fading a
    // themed color in/out without re-deriving it from the settings channels.
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

    // RGBA color picker with a hex input + alpha, bridging ImGui's 0..1 floats
    // to the 0..255 int32 fields we store in INI. Pass the four channel refs;
    // returns true if any changed this frame (caller persists on Save). Used by
    // every tool that exposes a configurable color, so the float<->int bridge
    // and the picker flags live in one place.
    inline bool ColorEdit4Hex(const char* label,
                              std::int32_t& r, std::int32_t& g,
                              std::int32_t& b, std::int32_t& a) {
        float c[4] = { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
        // NoInputs: hide the inline hex text field. It does nothing on its own
        // (the picker popup has its own hex field) and just eats row width.
        // Row collapses to swatch + label; click the swatch to open the picker.
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

    // RGB-only color picker (no alpha channel/bar), same hex-bridge pattern as
    // ColorEdit4Hex. Used for the global Title/Body text colors, which have no
    // alpha field.
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
    inline Rect PinBox(float px, float py) {
        return { ImVec2(px - 17.0f, py - 17.0f),
                 ImVec2(px + 17.0f, py + 17.0f) };
    }

    // Wrap width for a hover-panel note, at title scale 1.0. Scaled by the
    // caller's titleScale (see ComputeInfoPanel) -- a raw 300 would hand a
    // 2x-scaled title the same room as a 1x one, so the panel stayed narrow
    // and the note wrapped into two or three tall lines instead of growing.
    constexpr float kNoteMaxTextW  = 300.0f;
    constexpr float kHeaderPadX    = 10.0f;
    constexpr float kHeaderPadY    = 10.0f;
    constexpr float kMinPanelW     = 180.0f;

    // Label tone, as a fraction of the text it labels. Every hover-card label
    // (stat-card row labels, the "Notes" heading, the "Custom Marker" subtitle)
    // is drawn as its OWN text colour faded to this, never as a hardcoded grey:
    // a user-set body colour then tints label and value together instead of the
    // label staying stubbornly neutral.
    constexpr float kLabelAlpha = 0.70f;

    // Scale an IM_COL32's alpha byte in place. The colour tokens are already
    // packed ImU32, so this avoids a round trip through ImVec4.
    inline ImU32 FadeAlpha(ImU32 a_col, float a_frac) {
        const float a = static_cast<float>((a_col >> IM_COL32_A_SHIFT) & 0xFF) * a_frac;
        return (a_col & ~IM_COL32_A_MASK) |
               (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
    }

    // Layout for the info-only custom-pin hover panel: name + "Custom Marker"
    // subtitle, no action rows. The actions live in the right-click context
    // menu (MapMarkerRenderer::DrawCustomContextMenu) instead.
    struct InfoPanelLayout {
        Rect  panel;
        float nameH;      // wrapped note height at the scaled title size (subtitle Y offset)
        // Wrap width the note was MEASURED at, in screen pixels. Draw sites must
        // pass this straight to DrawTextShadowed rather than kNoteMaxTextW: the
        // wrap scales with titleScale now, so a draw site using the raw constant
        // would break lines somewhere other than where the panel was sized for.
        float noteWrapW;
    };

    // offsetX/offsetY: user-configurable nudge on top of the default anchor
    // (Settings::markerHoverOffsetX/Y, [Markers] HoverOffsetX/Y). Default 0,0
    // reproduces the original hardcoded anchor (+14, -panelH-8) exactly.
    // Applied before the on-screen clamp so a large offset still can't push
    // the panel off-screen.
    inline InfoPanelLayout ComputeInfoPanel(float pinX, float pinY, const char* note, ImVec2 disp,
                                            float titleScale = 1.0f, float bodyScale = 1.0f,
                                            float offsetX = 0.0f, float offsetY = 0.0f) {
        // Wrap width scales WITH the title, so a larger title scale gets
        // proportionally more room and the note keeps roughly the same line
        // count at every scale. Previously the wrap was a flat kNoteMaxTextW in
        // screen pixels (measured at kNoteMaxTextW / titleScale, then scaled
        // back up by titleScale, which cancels out): a 2x title had the same
        // 300px to work with as a 1x one, so a note that fit on one line at 1x
        // broke into three tall lines in a narrow box.
        //
        // CalcTextSize measures at the BASE font size, so the wrap passed to it
        // is de-scaled and the result scaled back -- the measured line breaks
        // then match a draw at (fs * titleScale, wrap = wrapW).
        const float wrapW = kNoteMaxTextW * titleScale;
        ImVec2 nameSize = ImGui::CalcTextSize(note, nullptr, false, wrapW / titleScale);
        nameSize.x *= titleScale;
        nameSize.y *= titleScale;
        const std::string subText = LocalizationManager::Get("Labels.CustomMarker");
        ImVec2 subSize = ImGui::CalcTextSize(subText.c_str());
        subSize.x *= bodyScale;
        subSize.y *= bodyScale;

        float contentW = std::max({ nameSize.x, subSize.x, kMinPanelW - kHeaderPadX * 2.0f });
        float panelW = contentW + kHeaderPadX * 2.0f;
        float panelH = kHeaderPadY + nameSize.y + 2.0f + subSize.y + kHeaderPadY;

        float ox = pinX + 14.0f + offsetX;
        float oy = pinY - panelH - 8.0f + offsetY;
        ox = std::min(ox, disp.x - panelW - 4.0f);
        oy = std::max(oy, 4.0f);

        InfoPanelLayout l;
        l.panel.mn  = ImVec2(ox, oy);
        l.panel.mx  = ImVec2(ox + panelW, oy + panelH);
        l.nameH     = nameSize.y;
        l.noteWrapW = wrapW;
        return l;
    }

    // Interior waypoint glyph types. Drawn as ImGui vector primitives, no PNG,
    // never sent to the compass or vanilla map (CompassHook skips >= 100).
    //   100..104 = manual picker numbered badges 1..5 (note dialog)
    //   110..113 = color dots red / green / blue / yellow
    //   120      = star
    //   200+     = auto-numbered quick-mark breadcrumbs (MinimapRenderer),
    //              unbounded, kept clear of the fixed 100-120 picker range
    inline bool IsWaypointType(std::uint32_t t) { return t >= 100; }
    inline constexpr std::uint32_t kQuickMarkBase = 200;

    // Defined after DrawStar (needs it); DrawPin dispatches here for
    // waypoint types before its PNG/vector paths.
    inline void DrawWaypointGlyph(ImDrawList* dl, float cx, float cy, float r,
                                  std::uint32_t iconType, float hoverT);

    // ===== Icon 3D treatment (shared) =====
    //
    // ONE draw site for every PNG icon we render, so pins (world/local/Glance/
    // Inspect) and the Glance/Inspect ENTITY icons get an identical look and a
    // future on/off setting only has to be threaded here. Ported from PartySheet
    // HUDDraw::DrawIconStyled, deliberately MINUS its outline pass: our icon PNGs
    // already carry a strong baked stroke, and a contour ring on top reads as a
    // smudge rather than an edge.
    //
    // Two passes:
    //  1. Shadow — a white rim copy up-left plus two black copies down-right. That
    //     asymmetry (light from top-left) is what actually sells the lift; a
    //     symmetric drop shadow just looks blurry. Offsets scale with the drawn
    //     size so a 2x-scaled icon doesn't keep a 1px pin-prick shadow.
    //  2. Duotone — draw the art tinted WHITE (white carries alpha, so the source
    //     art passes through untouched), then re-shade the quad's verts with a
    //     vertical gradient derived from a_tint: lit top, sunk bottom.
    //
    // KeepAlpha is MANDATORY on the shade call. The plain
    // ShadeVertsLinearColorGradient overwrites vertex alpha, which turns a
    // transparent icon into a solid block — the gradient endpoints carry alpha 255.
    //
    // a_tint drives the duotone, so any caller-side tint (category color, hover
    // yellow, crossfade alpha) still works exactly as it did before.
    // a_shadow/a_duotone default to the "3D Icons" setting (Settings::iconFx3D).
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
        const bool fxOn = Settings::GetSingleton()->iconFx3D;
        const bool a_shadow  = (a_shadowOpt  < 0) ? fxOn : (a_shadowOpt  != 0);
        const bool a_duotone = (a_duotoneOpt < 0) ? fxOn : (a_duotoneOpt != 0);
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

    // Vector-dot sibling of DrawIconFx, for the actor/loot/ingredient dots on the
    // Glance radar and Inspect overlay. Those are AddCircleFilled calls, not
    // textures, so DrawIconFx cannot touch them (it offsets an alpha silhouette,
    // and a flat circle has none). Ported from PartySheet HUDDraw::DrawPipRow,
    // which solved the same "flat disc next to a lifted icon" problem.
    //
    // Three passes, matching DrawIconFx's light direction so dots and icons agree:
    //  1. Socket — a dark disc offset down-right, seating the dot on the surface.
    //  2. Duotone fill — white circle re-shaded lit-top/sunk-bottom. Same
    //     KeepAlpha rule as DrawIconFx: the plain gradient would overwrite vertex
    //     alpha and break the crossfade.
    //  3. Specular — a small light catch up-left, plus a dark rim to define the
    //     edge against the snapshot backdrop.
    //
    // The specular is skipped below ~3px radius: at Glance's default dot size it
    // just smears the dot into a lighter blob instead of reading as a highlight.
    // Spread text shadow for text drawn over the configurable panel background.
    // Opaque light panels fade the black shadow when the text is already darker;
    // light text over dark panels retains it. As the panel becomes transparent,
    // restore the shadow because the map underneath is unknown.
    //
    // Offsets scale with the drawn size: panels here draw the same string at
    // anything from 0.85x to 3x via TitleScale/BodyScale, and a fixed 1.5px offset
    // is a big fraction of a small glyph and a pin-prick under a large one.
    //
    // Shadow alpha scales with the TEXT's alpha so a fading caller (the Glance /
    // Inspect overlays fade whole panels) doesn't leave an opaque shadow behind
    // after the glyphs are gone. Fully transparent text draws nothing at all.
    //
    // EVERY position here is pixel-SNAPPED, and that is the whole reason rows of
    // identical strings used to carry visibly different shadow weight. The glyph
    // atlas is rasterized at fixed pixel size, so a draw at a fractional position
    // resamples: the 5 shadow copies land on nearly the same pixel on one row (the
    // alphas stack, shadow reads heavy) and straddle two pixels on the next (each
    // reads light). With whole-pixel offsets every row gets the identical 5-copy
    // footprint. Snapping the text copy too is what stops the glyphs themselves
    // from looking soft/uneven on a black background where no shadow is visible.
    // Callers still pass fractional layout positions; the snap happens once, here,
    // so no draw site has to know about it.
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
        // Offsets still scale with drawn size (a fixed 1px is a lot of a small
        // glyph and nothing under a large one), but round to >= 1px so the spread
        // never collapses onto the center copy.
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

    // -------------------------------------------------------------------------
    // Layered panel card background (ported from PartySheet HUDDraw::DrawChip).
    // Vector-only draw stack with 5 depth layers:
    //  1. Soft drop shadow (2-pass stacked offset rects).
    //  2. Vertical gradient fill (ShadeVertsLinearColorGradientKeepAlpha) derived
    //     from the base color, keeping vertex opacity intact.
    //  3. Inner vignette (darkened inset perimeter / inner shadow).
    //  4. Top inner highlight catch-line (1px light rim inside upper edge).
    //  5. Double border (outer dark contour + inner metallic/accent border).
    // -------------------------------------------------------------------------
    inline void DrawPanelCard(ImDrawList* dl, const ImVec2& bMin, const ImVec2& bMax,
                              ImU32 bgCol, ImU32 borderCol, float alpha = 1.0f) {
        if (!dl || alpha <= 0.0f) return;
        const auto* s = Settings::GetSingleton();
        const float r = MapTheme::kPanelRounding;
        const float borderW = MapTheme::kPanelBorderW;

        // 1) Soft drop shadow: stacked offset rects fake blur and anchor the card.
        if (s->bgShadow) {
            for (int i = 2; i >= 1; --i) {
                const float grow = static_cast<float>(i);
                dl->AddRectFilled(
                    ImVec2(bMin.x - grow + 0.5f, bMin.y - grow + 1.0f),
                    ImVec2(bMax.x + grow + 0.5f, bMax.y + grow + 1.0f),
                    IM_COL32(0, 0, 0, static_cast<int>(28.0f * alpha)),
                    r + grow);
            }
        }

        // 2) Base fill with vertical gradient. KeepAlpha retains vertex alpha so the
        //    fill carries the panel opacity without clipping or alpha clobbering.
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
                ImVec2(bMin.x + 1.5f, bMin.y + 1.5f),
                ImVec2(bMax.x - 1.5f, bMax.y - 1.5f),
                IM_COL32(0, 0, 0, static_cast<int>(45.0f * alpha)),
                (std::max)(r - 1.0f, 0.0f), 0, 3.0f);
        }

        // 4) Top inner highlight: 1px light catch line just inside the top edge.
        if (s->bgHighlight && bgA > 0) {
            const float hPad = (r > 0.0f) ? r : 1.0f;
            dl->AddRectFilled(
                ImVec2(bMin.x + hPad, bMin.y + 1.0f),
                ImVec2(bMax.x - hPad, bMin.y + 2.0f),
                IM_COL32(255, 255, 255, static_cast<int>(26.0f * alpha)));
        }

        // 5) Borders: dark outer contour + metallic/accent inner border.
        if (s->bgBorder) {
            const auto bA = static_cast<std::uint8_t>(((borderCol >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
            dl->AddRect(bMin, bMax, IM_COL32(6, 6, 8, static_cast<int>(220.0f * alpha)), r, 0, 1.2f);
            if (bA > 0) {
                dl->AddRect(
                    ImVec2(bMin.x + 1.0f, bMin.y + 1.0f),
                    ImVec2(bMax.x - 1.0f, bMax.y - 1.0f),
                    WithAlpha(borderCol, alpha),
                    (std::max)(r - 1.0f, 0.0f), 0, borderW);
            }
        }
    }

    // hoverT: 0 = unhovered, 1 = fully hovered. Callers animate this per-pin
    // (see MapMarkerRenderer/LocalMapRenderer _hoverAnim) so the yellow tint,
    // the scale pop and the halo all ease in/out instead of snapping. Pass the
    // LINEAR ramp — the pop curve is applied here, easing it twice distorts the
    // timing. The hit box (PinBox) deliberately does NOT grow with the pop:
    // a hit box that tracks the animation feeds back into its own input and
    // flickers at the pin's edge.
    // scale: Settings::markerIconScale (0.5-2.0), shared by world+local map so
    // both renderers stay in sync. Default 1.0 keeps any other caller as-is.
    // overallAlpha: multiplies every drawn color (PNG tint and vector fallback),
    // used by the Glance/Inspect crossfade so pins fade with the rest of the overlay.
    inline void DrawPin(ImDrawList* dl, float px, float py, float hoverT,
                        std::uint32_t iconType = 0,
                        ID3D11ShaderResourceView* iconSrv = nullptr,
                        float scale = 1.0f,
                        ImU32 tintCol = IM_COL32(255, 255, 255, 0),
                        float overallAlpha = 1.0f) {
        // Hover pop (2026-08-21): vanilla map markers grow on hover, ours only
        // changed tint, so a hovered custom pin read as dead. hoverT arrives
        // LINEAR from the caller's _hoverAnim (see the delta-time easing rule:
        // state stays linear, the curve is applied at draw time only, so
        // reversing mid-animation stays symmetric).
        //
        // Curve: soft back-out. The standard back constant (1.70158) overshoots
        // ~10% of the travel and, stacked on the caller's fast rise ramp, read
        // as a SNAP on the way in (the zoom-out was fine because the fall ramp
        // is slower). Halving the constant keeps a hint of pop while the ramp
        // does the smoothing; the rise time itself was also lengthened at both
        // call sites. Applied both directions — with the gentler constant the
        // downward overshoot is not visible, and a one-sided curve made the
        // in/out timing asymmetric.
        const float hoverLin = std::clamp(hoverT, 0.0f, 1.0f);
        constexpr float kBackC1 = 0.85f;         // half the standard 1.70158
        constexpr float kBackC3 = kBackC1 + 1.0f;
        const float inv = hoverLin - 1.0f;
        const float popT = 1.0f + kBackC3 * inv * inv * inv + kBackC1 * inv * inv;
        // 30% bigger at full hover.
        const float hoverScale = 1.0f + 0.30f * popT;

        const float r  = kPinRadius * scale * hoverScale;
        const float cx = px;
        // Centered on the anchor point. The old tail-and-lift layout (icon at
        // py - 2r, triangle pointing down at the point) made pins read as
        // landing above the click; centering IS the accuracy fix, no tail
        // needed. Keep in sync with PinBox().
        const float cy = py;

        // Category tint used by the vector fallback circle and hover override.
        // Default is golden yellow so an icon-less custom marker stands out
        // from every category color and from vanilla's white icons.
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

        // Hover override: bright opaque yellow so the highlight reads cleanly
        // (a shade above the golden default). Eased via hoverT instead of a
        // hard snap.
        const float fadeA = std::clamp(overallAlpha, 0.0f, 1.0f);
        auto fadeAlpha = [fadeA](ImU32 c) {
            const auto ca = static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * fadeA);
            return (c & ~IM_COL32_A_MASK) | (ca << IM_COL32_A_SHIFT);
        };

        ImU32 pinColor = fadeAlpha(LerpColor(tint, IM_COL32(255, 230, 110, 255), hoverT));

        const ImU32 outline = fadeAlpha(IM_COL32(0, 0, 0, 200));

        // Soft halo behind the pin, fading in with the same hoverT. Stacked
        // translucent rings (a draw list can't blur) so the edge falls off
        // instead of ending at a hard rim.
        if (hoverLin > 0.01f) {
            const float g = fadeA * hoverLin;
            const float gr = r + 4.0f;
            dl->AddCircleFilled(ImVec2(cx, cy), gr + 5.0f,
                                IM_COL32(255, 225, 120, static_cast<int>(26 * g)));
            dl->AddCircleFilled(ImVec2(cx, cy), gr + 2.5f,
                                IM_COL32(255, 225, 120, static_cast<int>(34 * g)));
            dl->AddCircleFilled(ImVec2(cx, cy), gr,
                                IM_COL32(255, 235, 150, static_cast<int>(42 * g)));
        }

        if (IsWaypointType(iconType)) {
            // Vector glyphs; callers may still pass a (fallback) iconSrv for
            // these types, ignore it — waypoints never draw PNG art. DrawWaypointGlyph
            // doesn't take an alpha param (no crossfade caller uses waypoint types yet).
            DrawWaypointGlyph(dl, cx, cy, r + 2.0f, iconType, hoverT);
            return;
        }

        if (iconSrv) {
            // PNG path: fit the icon into a scaled box while preserving the
            // source aspect ratio (longer axis = kMaxAxis, shorter axis
            // scaled). Box is centered on the anchor point. Base 32px times
            // the user's icon-size setting.
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
            // Base tint: lerp white -> user tint color by the tint's alpha, so
            // alpha 0 leaves the art true (white multiply) and alpha 255 fully
            // recolors. RGB is applied at full opacity — alpha is a STRENGTH
            // knob here, not an image-alpha, so the icon never fades out.
            // Setting: Settings::pinTint* / PinTintCol(). Hover still overrides.
            const float tintStr = static_cast<float>((tintCol >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
            const ImU32 tintOpaque = (tintCol & ~IM_COL32_A_MASK) | (0xFFu << IM_COL32_A_SHIFT);
            const ImU32 baseTint = LerpColor(IM_COL32(255, 255, 255, 255), tintOpaque, tintStr);
            ImU32 imgTint = fadeAlpha(LerpColor(baseTint, IM_COL32(255, 220, 80, 255), hoverT));

            DrawIconFx(dl, iconSrv, imgMin, imgMax, imgTint);
        } else {
            // Vector fallback: colored circle centered on the point, with a
            // small dark center dot marking the exact anchor.
            dl->AddCircleFilled(ImVec2(cx, cy), r, pinColor);
            dl->AddCircle(ImVec2(cx, cy), r + 1.0f, outline, 0, 1.5f);
            dl->AddCircleFilled(ImVec2(cx, cy), 2.0f, outline);
        }
    }

    // Continuous animated halo for the active destination marker. Two pulsing
    // concentric rings in cyan-blue with sin-wave alpha and radius. Drawn under
    // the pin so it reads as a glow.
    // centerOnPoint is retained for call-site compatibility but no longer
    // changes anything: pins draw centered on the anchor point now, so both
    // pin-backed and pinless (vanilla-icon redirect) rings center on the point.
    inline void DrawDestinationRing(ImDrawList* dl, float px, float py, float timeSec,
                                    bool centerOnPoint = false) {
        (void)centerOnPoint;
        const float r  = kPinRadius;
        const float cx = px;
        const float cy = py;
        // Calm breathing: ~2.4s cycle (2*pi/2.6), gentler radius/alpha swing
        // than the original 3.0f rate so the destination reads as a steady
        // presence rather than a flashing beacon.
        const float n  = 0.5f + 0.5f * std::sin(timeSec * 2.6f);
        const float r0 = r + 8.0f + n * 4.0f;
        const float r1 = r + 14.0f + n * 5.0f;
        const ImU32 c0 = IM_COL32(80,  170, 255, static_cast<int>(130 + n * 60));
        const ImU32 c1 = IM_COL32(140, 200, 255, static_cast<int>(70  + n * 40));
        // Static anchor ring under the breathing rings so the destination
        // never visually vanishes at the bottom of the breath cycle.
        dl->AddCircle(ImVec2(cx, cy), r + 5.0f, IM_COL32(80, 170, 255, 90), 0, 1.5f);
        dl->AddCircle(ImVec2(cx, cy), r0, c0, 0, 2.5f);
        dl->AddCircle(ImVec2(cx, cy), r1, c1, 0, 1.5f);
    }

    // Per-vertex-coloured thick polyline, built as a quad strip by hand.
    //
    // WHY THIS EXISTS instead of ImDrawList::AddPolyline: AddPolyline takes ONE
    // ImU32 for the whole call, so any gradient along a path has to be faked by
    // splitting it into many short calls, and the colour then steps per-call
    // instead of per-vertex. That is fine for a two-tone fade and wrong for
    // elevation banding, where the colour must change exactly where the trail
    // crosses a floor boundary, not at whatever point the run happened to split.
    //
    // Geometry: for each point, offset +-halfWidth along the miter direction
    // (average of the incoming and outgoing segment normals) to get two rim
    // vertices, then stitch consecutive pairs into quads. Endpoints use the
    // single adjacent segment's normal.
    //
    // Miter clamp: on a hairpin turn the miter length blows up (1/sin(theta/2)),
    // which would fling a vertex off-screen. Clamped to 2x halfWidth; past that
    // the joint visibly pinches, which is far better than a spike.
    //
    // No anti-aliasing: AddPolyline's AA fringe needs its own vertex ring that
    // would double the vertex count and the colour bookkeeping. At the widths
    // the trail uses (2-12px) over a noisy map backdrop the aliasing does not
    // read. The bright core pass hides it further.
    //
    // colours[] is parallel to pts[] — one ImU32 per point, interpolated across
    // each quad by the renderer.
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

    // ---- World-anchored beam drawing (shared: destination beacons + loot beams) ----
    //
    // Extracted from WorldBeaconRenderer 2026-08-15 when loot beams became a
    // second caller. Duplicating the projection or the gradient body is how the
    // two drift apart, and the destination beam's look was tuned over several
    // in-game rounds (commit c0c0853), so it lives here now rather than being
    // copied.

    // Project a gameplay world point through the world camera.
    //
    // sz <= 0 means BEHIND the camera: the returned sx/sy are mirrored garbage in
    // that case, so callers MUST check a_outInFront before trusting the screen
    // position. a_outScreen is written on every path that returns true, and the
    // false path leaves it untouched, so callers must not read it unless this
    // returned true.
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

    // Liang-Barsky clip: walk the four edges narrowing the parametric range the
    // segment occupies inside the rect. Empty range = no overlap. Also reports
    // the surviving range so the caller can place a pin at the highest point of
    // the beam that is actually on screen.
    //
    // A beam is a SEGMENT, so this is the visibility test. Point tests on either
    // endpoint were both tried and both failed: gating on the top hid tall
    // beacons until the player looked up, gating on the base hid them when the
    // player looked up too far with the beam still crossing the whole screen.
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

    // Shared five-rail beam mesh. Callers own the alpha profile: loot beams
    // pass a constant profile; tall world beacons fold in top/foot fading and
    // per-height occlusion. Keeping those policies outside this primitive lets
    // both surfaces share geometry without forcing them into the same design.
    template <typename AlphaAtFn>
    inline void DrawFeatheredBeam(
        ImDrawList* dl,
        const ImVec2& a_baseScr, const ImVec2& a_topScr,
        int a_steps,
        float a_wBase, float a_wTop,
        ImU32 a_coreCol, ImU32 a_glowCol, ImU32 a_haloCol,
        float a_alphaScale,
        AlphaAtFn a_alphaAt)
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
            dl->AddLine(spinePts[s], spinePts[s + 1],
                        WithAlpha(IM_COL32(255, 255, 255, 220), a), 1.2f);
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

    // ---- Trail style presets (PROTOTYPE, hardcoded 2026-08-15) ------------
    // Multiple trail looks so we can eyeball them in-game before committing to
    // a settings knob. Flip kTrailStyle, rebuild, look. Dots is the shipped
    // look and stays the default; nothing about it changed.
    // TODO: promote to Settings::pathTrailStyle + a SettingsMenu combo once a
    // preset is picked. Reference art: reference/path_tracking_concepts_*.jpg.
    enum class TrailStyle {
        Dots,          // shipped: breadcrumb dots + periodic direction chevrons
        LuminousFlow,  // concept 1: layered glow polyline + chevrons on top
    };
    inline constexpr TrailStyle kTrailStyle = TrailStyle::LuminousFlow;

    // Smart, turn-aware breadcrumb trail. Per-dot age/turn attributes survive
    // Catmull-Rom smoothing through a fractional source index tracked against
    // walked arc length. All callers pass source points in screen space.
    // Visibility is NOT owned here — each caller gates its own call
    // (LocalMapRenderer: pathTrailVisible; Glance/Inspect: navGlanceShowTrail/
    // navInspectShowTrail). This function only owns shared trail STYLE
    // (color, thickness, fade, etc).
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
                              // Parallel to segmentsIn: the REAL world Z of each
                              // screen-space point, for elevation colouring. Null
                              // (default) keeps the flat single-colour trail, so
                              // callers opt in one at a time.
                              // MUST be built in the same loop that builds the
                              // ImVec2s and skip in the same branch: callers drop
                              // points that fail WorldPtToScreenPt3, and a Z array
                              // built separately would silently desync.
                              const std::vector<std::vector<float>>* worldZ = nullptr,
                              // Unused by the colour ramp (which is anchored to
                              // the cell's own Z range, not the player). Kept
                              // because the DIAG logging in LocalMapRenderer
                              // reports deltas against it, and a future
                              // "you are here" band may want it back.
                              [[maybe_unused]] float playerZ = 0.0f,
                              const RoundedRectBoundary* clipBoundary = nullptr) {
        auto* trailSet = Settings::GetSingleton();

        // Elevation colouring is only meaningful when the caller supplied Z AND
        // the shape matches. A mismatch means a desync bug upstream; drop to
        // flat colour rather than index out of bounds.
        bool  useElevation = worldZ && worldZ->size() == segmentsIn.size();
        float elevMinZ = 0.0f, elevMaxZ = 0.0f, elevSpan = 0.0f;
        if (useElevation) {
            // The ramp is the trail's OWN range in this cell. Computed fresh each
            // frame from the same data being drawn, so exploring a new lower
            // level rescales it — a slow, occasional shift, unlike the constant
            // repaint the player-relative version caused.
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
        // alphaMul lets Glance/Inspect crossfade the whole trail without touching
        // Settings; every downstream alpha (segmentAlpha, a01, originAlpha, glow)
        // derives from trailAlpha, so scaling it here fades the entire trail.
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
            // Stub filter: a segment under 4 points is ~300 units of walking, a
            // useless breadcrumb clump (e.g. step into a cell, turn around, leave).
            // Recording keeps every point; this only skips DRAWING the stub. If the
            // player returns and the segment grows past 4, it draws in full.
            // Safe now that every caller passes tailWindow 0 (segments == source);
            // if a tail window is ever reintroduced, filter on source size instead.
            if (segPts.size() < 4) {
                globalSrcBase += segPts.size();
                continue;
            }
            const bool isNewestVisitedSegment = (s + 1 == segments.size());
            // All segments now carry full trailAlpha. Age dimming is handled ONLY by
            // the global pathTrailFadeByAge fade (a01 below), which already fades
            // older segments via the cross-segment globalSrcBase age. The old
            // hardcoded 0.55 older-segment dim (removed 2026-07-19) stacked a second
            // always-on dim on top: fine interior, but muddy on radar/inspect where
            // the tail-window clip already trims history. One fade, global setting.
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

            // Connecting polyline removed 2026-07-19: read as a moving "snake" on
            // the newest segment. Trail is now dots + periodic direction chevrons only.

            auto ageAt = [&](float srcU) {
                return std::clamp((static_cast<float>(globalSrcBase) + srcU) / globalDen,
                                  0.0f, 1.0f);
            };
            auto turnAt = [&](float srcU) {
                int k = static_cast<int>(srcU + 0.5f);
                k = std::clamp(k, 0, static_cast<int>(turn.size()) - 1);
                return turn[k];
            };

            // ---- LuminousFlow: layered glow polyline ----------------------
            // Concept 1 from the reference art. Three passes over the SAME
            // smoothPath the dots walk, widest+faintest first, so the stroke
            // reads as a glow instead of a flat stroke. Drawn before the emit
            // loop so chevrons land on top of it.
            //
            // Clipping: AddPolyline can't be clipped per-vertex, so the path is
            // cut into runs of consecutive inside-clip points and each run is
            // stroked separately. Without this the Glance radar would draw
            // chords straight across the circle between two outside points.
            //
            // Age fade is now PER-VERTEX via PolylineVertexColored: each point
            // carries its own alpha, so a long segment fades smoothly along its
            // length instead of taking one alpha per clip run. (The AddPolyline
            // version banded at run boundaries; that also made elevation colour
            // impossible, which is why the quad-strip helper exists.)
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

                // Per-point RGB from elevation relative to the LIVE player Z.
                // smoothSrcU maps a smoothed vertex back to a fractional source
                // index, so the Z is lerped between the two source points the
                // vertex sits between — that is what makes the colour change
                // land exactly where the trail crosses the band boundary
                // (partway up a staircase) instead of snapping at a source point.
                // Point's height mapped onto the cell's own Z range: lowest ->
                // orange, highest -> blue. No player anchor, so this is stable
                // as the player moves. See MapTheme.h for why the relative
                // version was rejected.
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

                    // Widest/faintest to tightest/brightest: outer haze, mid
                    // body, bright core. Alphas are deliberately low; they
                    // stack where the passes overlap. Each pass rebuilds the
                    // colour array at its own alpha scale.
                    std::vector<ImU32> pass(run.size());

                    // rgbOverride == 0 means "use each point's own elevation
                    // colour" (the haze and body passes). The core pass forces
                    // white so the line keeps a bright centre regardless of band.
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

                // Clip runs still exist: a quad strip is still one primitive
                // batch, so a point outside the radar circle must break the
                // strip or it draws a chord across the circle.
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
                    // Dashed-gap rhythm is a DOT-trail device (breaks the
                    // breadcrumb run so it reads as steps). In LuminousFlow the
                    // polyline is continuous and chevrons are the only emitted
                    // glyph, so honouring the gap would drop a chevron at random.
                    const bool inGap = (kTrailStyle != TrailStyle::LuminousFlow) &&
                        (kGapEvery > 0) &&
                        (gapCounter % kGapEvery) >= (kGapEvery - kGapSkip);

                    if (!inGap && insideClip(p)) {
                        // Direction chevron every kChevronEvery emitted glyphs,
                        // pointing along the local trail direction so you can read
                        // which way you walked. Replaces the 2026-07-17 turn-diamonds
                        // (which only marked hard turns, told you nothing the bend
                        // didn't). Chevron uses the TRAIL color, NOT the retrace yellow
                        // that made the old chevrons read as a second cursor.
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
                            // Dots are the trail body in Dots style. In
                            // LuminousFlow the polyline is the body, so the
                            // emit loop runs only to place chevrons.
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
                // Head arrow/cone removed 2026-07-17: read as a second player
                // cursor next to the real arrow. drawHeadCone/headingAngle params
                // kept so callers stay untouched.
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

    // 5-point star badge. Concave shape, so AddConvexPolyFilled doesn't work —
    // fan-fill from the center over the 10 alternating outer/inner vertices,
    // then stroke the outline as a closed polyline. Dependency-free (no
    // PathFillConcave, which isn't in the pinned imgui version).
    inline void DrawStar(ImDrawList* dl, ImVec2 center, float r, ImU32 fill, ImU32 outline) {
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
        dl->AddPolyline(v, kPoints * 2, outline, ImDrawFlags_Closed, 1.0f);
    }

    inline void DrawWaypointGlyph(ImDrawList* dl, float cx, float cy, float r,
                                  std::uint32_t iconType, float hoverT) {
        const ImU32 dark  = IM_COL32(0, 0, 0, 200);
        const ImU32 white = IM_COL32(255, 255, 255, 235);
        const ImVec2 c(cx, cy);

        if (iconType >= 100 && iconType <= 104) {
            // Manual picker badges (note dialog "1"-"5" cells): single digit,
            // dark disc, white ring. Hover brightens the ring the same way
            // pins tint.
            const ImU32 ring = LerpColor(white, IM_COL32(255, 230, 110, 255), hoverT);
            dl->AddCircleFilled(c, r, IM_COL32(35, 38, 48, 235));
            dl->AddCircle(c, r, ring, 0, 1.8f);
            char digit[2] = { static_cast<char>('1' + (iconType - 100)), '\0' };
            ImFont* font = ImGui::GetFont();
            const float fs = r * 1.5f;
            ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, digit);
            dl->AddText(font, fs, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), white, digit);
            return;
        }

        if (iconType >= kQuickMarkBase) {
            // Auto-numbered breadcrumb (quick-mark, see MinimapRenderer): same
            // disc/ring styling as the manual 1-5 picker above but unbounded,
            // so multi-digit numbers shrink the font to keep fitting the disc.
            const ImU32 ring = LerpColor(white, IM_COL32(255, 230, 110, 255), hoverT);
            dl->AddCircleFilled(c, r, IM_COL32(35, 38, 48, 235));
            dl->AddCircle(c, r, ring, 0, 1.8f);
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
            dl->AddCircle(c, r * 0.85f, dark, 0, 1.5f);
            return;
        }

        // 120 (and any future >= 100 fallback): gold star.
        const ImU32 fill = LerpColor(IM_COL32(240, 195, 70, 240),
                                     IM_COL32(255, 230, 110, 255), hoverT);
        DrawStar(dl, c, r + 1.0f, fill, dark);
    }

    // Category picker grid: shared by the note dialog's world-map and
    // interior-map category pickers (previously interior-only; world used a
    // text combo). Waypoint types (>= 100) draw as vector glyphs via
    // DrawWaypointGlyph; everything else draws its PNG icon (TextureManager)
    // or falls back to a text button if no PNG exists yet. selected is
    // in/out: clicking a cell updates it and the call returns true that
    // frame. showTooltip controls the hover name label — interior glyph rows
    // skip it (names are exterior-flavored and just confuse an interior
    // pick), world-map icons want it since the grid replaces a text combo.
    inline bool DrawCategoryGrid(const char* const* names, const std::uint32_t* values,
                                 int count, int& selected,
                                 int perRow = 10, float cellSz = 30.0f,
                                 bool showTooltip = true) {
        bool changed = false;
        int col = 0;
        for (int i = 0; i < count; ++i) {
            // Separator between the waypoint glyph rows and the
            // compass-compatible icon set below them (interior table only;
            // world table has no waypoint types so this never fires there).
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
                const ImVec2 cell(cellSz + 8.0f, cellSz + 8.0f);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                clicked = ImGui::InvisibleButton("##cat", cell);
                ImDrawList* wdl = ImGui::GetWindowDrawList();
                if (ImGui::IsItemHovered() && !sel)
                    wdl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                       ImGui::GetColorU32(ImGuiCol_ButtonHovered, 0.4f), 4.0f);
                DrawWaypointGlyph(wdl,
                    pos.x + cell.x * 0.5f, pos.y + cell.y * 0.5f,
                    cellSz * 0.38f, values[i], 0.0f);
            } else if (auto* srv = TextureManager::GetCategoryIcon(values[i])) {
                clicked = ImGui::ImageButton("##cat",
                    reinterpret_cast<ImTextureID>(srv), ImVec2(cellSz, cellSz));
            } else {
                clicked = ImGui::Button(names[i], ImVec2(cellSz + 8.0f, cellSz + 8.0f));
            }
            ImGui::PopStyleColor();
            if (clicked) { selected = i; changed = true; }
            if (sel) {
                // Border ring so the active pick reads even on image cells.
                ImDrawList* gridDl = ImGui::GetWindowDrawList();
                gridDl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                ImGui::GetColorU32(ImGuiCol_CheckMark), 4.0f, 0, 2.0f);
            }
            if (showTooltip && !IsWaypointType(values[i]) && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", names[i]);
            }
            ImGui::PopID();
        }
        return changed;
    }

    // Deferred map cursor.
    //
    // WHY: every map surface appends to ONE shared ImGui::GetForegroundDrawList(),
    // so within it z-order is just who appended first. MapMarkerRenderer::Draw()
    // runs before HoverImageRenderer::Draw() (see ImGui/Renderer.cpp), so a cursor
    // drawn inside the former was painted over by the vanilla stat card drawn in
    // the latter -- very visible on a city marker, whose hover radius is wide
    // enough that the card pops while the pointer is still sitting on top of it.
    //
    // The renderers now REQUEST the cursor at their existing guard points and one
    // call at the end of the frame draws it, after every panel has appended. Keeps
    // the old visibility rules exactly: no request, no cursor.
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
        // Custom cursor PNG. The hotspot is the texture's top-left (0,0), so
        // the image is drawn with its corner at the cursor point, matching the
        // vector-arrow fallback below. Vanilla cursor is suppressed because it
        // renders under our ImGui layer; we draw our own here instead.
        static ID3D11ShaderResourceView* s_cursorSrv =
            TextureManager::GetTexture("Data\\Interface\\CustomMarkers\\Icons\\cursor.png");

        const float dispH = ImGui::GetIO().DisplaySize.y;
        const float rawScale = dispH > 0.0f ? (dispH / 1080.0f) : 1.0f;
        // Dampened resolution scaling (50% slope) so high-DPI displays (1440p: 44px, 4K: 57px)
        // scale comfortably without blowing the cursor up to 76px on 4K.
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
    // --- World-map projection Z ------------------------------------------
    // Flat paper maps use a straight-down camera and need the tested high map
    // plane. Vanilla maps use a tilted camera and need the supplied world Z.
    // Both wrong choices can still project successfully, so select from the
    // map configuration and camera geometry before calling WorldPtToScreenPt3.
    inline constexpr float kMapPlaneZ = 180000.0f;

    inline bool UsesFlatMapProjection(const float (&a_worldToCam)[4][4],
                                      const RE::TESWorldSpace* a_mapWorld) noexcept {
        // FWMF's camera tilts during movement while its paper plane stays high.
        // The 2026-09-09 log shows pitch 90 / offset 184073 throughout those
        // tilts. Keep that authored mode stable instead of dropping to ground Z
        // whenever the live look direction crosses the 0.05 threshold.
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

    // --- Projection depth test -------------------------------------------
    // WorldPtToScreenPt3 writes sz = (row2 . pt + m[11]) * (1/W). The engine
    // (SE FUN_140c66580 / AE equivalent) only rejects a point when |W| is below
    // the zero tolerance; it takes fabs(W) and never tests the SIGN of anything.
    //
    // Our old guard was `sz > 0.0f`, which silently assumed a positive-W camera.
    // The AE world-map camera happens to produce positive W, so it passed there.
    // The SE world-map camera produces NEGATIVE W (measured 2026-08-19: row3 =
    // [0, 0.6013, -0.7990, 76610.73] against a map-plane Z of 180000 gives
    // W = -65062), so 1/W is negative, every sz came out negative, and EVERY
    // marker was culled on SE while the projection itself had succeeded.
    //
    // Sign is a camera convention, not a validity signal. Test magnitude only:
    // a point exactly on the camera plane is degenerate, and WorldPtToScreenPt3
    // has already returned false for that case. Keeping a magnitude epsilon
    // preserves the behind-camera/degenerate cull on BOTH runtimes without
    // baking in either sign convention.
    inline bool ProjectionDepthValid(float a_sz) noexcept {
        return std::fabs(a_sz) > 1e-6f;
    }

} // namespace MapDraw
