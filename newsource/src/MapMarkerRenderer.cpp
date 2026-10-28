#include "MapMarkerRenderer.h"
#include "MarkerManager.h"
#include "MapDrawHelpers.h"
#include "TextureManager.h"
#include "MarkerFilterHook.h"
#include "Settings.h"
#include "PlaceMarkerHook.h"
#include "LocalMapClickHook.h"
#include "FontManager.h"
#include "BackgroundManager.h"
#include "LocalizationManager.h"
#include "MapStats.h"
#include "QuestTracker.h"
#include "SettingsMenu.h"
#include "MenuUtils.h"
#include "UISound.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>

// ============================================================================
// MapMarkerRenderer - world map ImGui pins, hover panel, list panel, click-pan
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working:
//     - World-map pins project through MapDraw::MapProjectionPoint, which owns
//       the camera-mode Z rule: flat maps use 180000, tilted maps retain the
//       supplied world Z. WorldspaceID 0 markers stay local-only.
//       Authored high-plane maps retain flat mode during camera tilt. Confirmed
//       in-game on FWMF 1.6.1170, 2026-09-09; MapPinProjectionLog is retained.
//     - The custom-pin hover panel is info-only. Set/Clear Destination, Rename,
//       and Remove live in one right-click popup shared by world and local pins.
//     - Entering a custom pin plays vanilla's UIMapRolloverFlyout cue once per
//       hover transition. Moving directly between pins plays it for the new pin.
//     - List panel (hotkey C): Overview / Custom Markers / Favorites / Notes.
//       The active tab persists as Settings::listActiveTab; ListTab is
//       APPEND-ONLY before Count because the persisted value is its enum index.
//       Each tab has an independent scroll offset. Fixed title/tab chrome sits
//       outside the body clip, and _listScrollMax measures only the active tab.
//       Inactive tab labels use TitleColor at 70% alpha; hover restores full
//       alpha, so no fixed muted hue leaks into user themes.
//       Active-quest names use full BodyColor; objective text uses the same
//       color at 70% alpha instead of a fixed muted hue.
//       Height is capped by listPanelMaxHeightPct; claimed wheel ticks do not
//       leak into map zoom. New markers/favorites/notes select their owning tab.
//       The explicit C-key visibility choice persists as [ListPanel] Visible.
//       SMF/modal/local-map suppression and automatic ShowListTab reveals do
//       not overwrite that preference. Awaiting build and in-game verification.
//     - Click swallowing: TryHandlePanelClick blocks clicks under the panel
//       from reaching the placement hook OR the vanilla marker click hook.
//     - Auto-hides on local map or modal dialogs.
//     - Click-to-pan uses MapCamera::GlidePan (SE 52137 / AE 53022), preserving
//       zoom. A camera-state busy check coalesces back-to-back row clicks and
//       retries after the active transition finishes.
//     - Click-to-pan clamps the destination to the active worldspace's map
//       cell rectangle (TESWorldSpace+0x188 MapData, nwCellX/Y..seCellX/Y * 4096)
//       so markers placed by the terrain ray near the map edge still pan
//       reliably. Works for Tamriel, Solstheim, modded worlds.
//     - Per-row delete button ("X") on each list panel row. Click fires
//       RemoveMarker immediately; click is swallowed by TryHandlePanelClick.
//     - List panel is draggable by its header strip (the band above the first
//       row). Drag tracks the engine MenuCursor + GetAsyncKeyState(VK_LBUTTON);
//       position is held in Settings::listPanelPosX/Y and written to INI on
//       drag-release via SavePositions(). Width + bg alpha read from Settings.
//     - Map Filters window position persists to INI on drag-release too
//       (Settings::filterPosX/Y). It's a real ImGui window, so ImGui owns the
//       drag; we mirror GetWindowPos() back and edge-detect the release.
//     - List panel and pin projection both filter to worldSpaceID != 0, so
//       interior markers placed on the local map never appear on the world map.
//     - The note dialog is shared by world-map and interior local-map placement.
//       InputVerboseLog records field changes and wrap callback before/after
//       byte lengths and cursor state. Diagnostics await in-game verification.
//       Creation consumes the allocated ID returned by AddMarker/AddAutoNamedMarker.
//       Local map skips world pins/list-panels but still renders this dialog.
//     - Note dialog OK/Cancel buttons also have direct MenuCursor +
//       GetAsyncKeyState release hit-tests as a fallback when ImGui's normal
//       Button() activation misses clicks on the map render/input split.
//     - IsMapFading() hides the entire ImGui layer while FaderMenu is open.
//     - A blocking SKSE Menu Framework window temporarily hides the complete
//       map layer and clears transient hit targets. Panel open state and saved
//       positions are preserved, so they return when SMF closes.
//     - Cleared badge: a green disc + checkmark drawn top-center on every
//       cleared dungeon marker. Unlike favorite/note badges, this enumerates the
//       vanilla marker array (MapMenu+0x30470) each frame, gated on type==0,
//       a dungeon-type allowlist (Cave/Fort/NordicRuin/DwemerRuin/Shipwreck/
//       Grove/DragonLair/Mine), and BGSLocation::IsCleared() on the ref's own
//       editor location. All three badge passes share _badgesSeeded.
//     - Focus-mode gate on all three badges (filteredByMode lambda). Badges are
//       drawn from OUR lists, so the MarkerFilterHook IsMarkerVisible detour
//       that hides a vanilla marker never reaches them; without the gate a
//       filtered-out marker keeps its star/dot/check floating over bare
//       terrain. Cleared reads the type it already holds; note reuses its
//       existing ref lookup; fav needs a LookupByID it otherwise avoids
//       (favorites carry position + worldspace but no ref). A ref that won't
//       resolve, or carries no ExtraMapMarker, counts as unfiltered — same
//       posture as the hook, which only forces false on markers it classifies.
//   Cleanup debt:
//     - GetRemoveButtonTarget/GetDestButtonTarget/GetEditButtonTarget and their
//       hook reads remain from the old hover-panel action rows. They stay zero;
//       current actions route through the right-click popup.
//   Evidence: findings-World-Map-Glide-Pan*.md,
//             findings/spec-findings-ListPanel-Scroll-Collapse.md,
//             findings/spec-findings-Cleared-Badge.md,
//             findings/spec-findings-Map-Projection-Mode-Detection.md.
// ============================================================================
//
// Renders saved markers as ImGui pins on the world map each frame.
//
// Camera path: MapMenu::GetRuntimeData2()->camera.cameraRoot->GetChildren()[0] -> NiCamera*
//
// Projection: RE::NiCamera::WorldPtToScreenPt3(worldToCam, port, pos, sx, sy, sz, 1e-5f)
//   sx, sy are normalized 0..1. Y is flipped: py = (1 - sy) * displaySize.y
//   ProjectionDepthValid tests magnitude because depth sign varies by camera.
//
// Cursor: the map uses Skyrim's Scaleform cursor (RE::MenuCursor), NOT the OS
// cursor. GetCursorPos / ImGui::GetMousePos do not match the visible map
// cursor and must not be used here. The game draws its cursor before Present,
// so our pins/panel cover it; we draw our own at the same menu cursor position.
// It is REQUESTED here (MapDraw::RequestCursor) and drawn at the very end of
// the frame by MapDraw::DrawRequestedCursor in ImGui/Renderer.cpp. Drawing it
// inline used to bury it under the vanilla stat card, which HoverImageRenderer
// appends to the same shared foreground list after this renderer returns.
//
// Everything visible is drawn with ImGui draw lists. We deliberately do NOT
// use ImGui windows/buttons: those widgets need mouse input the backend never
// receives on the map. The remove button is hit-tested here; the actual delete
// is performed by PlaceMarkerHook, which already intercepts every map click.
//
// Hover: once a marker is "active" it stays active while the cursor is inside
// one padded box enclosing the whole pin AND the whole panel. A gap between
// the two would let a fast mouse move drop the hover before reaching the
// button, so the box is a single rectangle with no gap.

namespace {
    // Shared row-text cap for the list panel (marker names AND quest objective
    // lines). Both are author-written strings of arbitrary length -- a quest
    // objective ("Meet up with the mercenaries on the road to Falkreath") or a
    // player's own note -- and both overrun the panel, which then runs the text
    // under the X button.
    //
    // Capped by CHARACTER COUNT, not measured width: the panel width is a user
    // setting and the body font is swappable per language, so a width-fit would
    // have to re-measure per row every frame for no visible gain at this size.
    // 40 fits the default panel with room to spare ("Find the owner of the Golden
    // Claw" is 33).
    //
    // Cuts on the last space before the limit so a word is not split mid-letter,
    // falling back to a hard cut when that space lands in the first half (one very
    // long word would otherwise be chopped back to almost nothing).
    //
    // Counts BYTES, not glyphs. Fine for Latin and Cyrillic; a CJK translation
    // truncates shorter than 40 visible characters. Revisit if that ships.
    // Trims a row label to the pixel width actually available to it.
    //
    // Replaced a fixed 40-character cut, which was a proxy that only held at
    // the one panel width and body scale it was eyeballed at. The panel width
    // is a user slider and the body size is another, so at a wide panel the old
    // rule cut names that fit with room to spare, and at a narrow panel with a
    // large font 40 characters still ran under the delete button. It also said
    // nothing about GLYPH width: 40 "W"s and 40 "i"s are wildly different runs,
    // and a CJK label is roughly triple a Latin one at the same count.
    //
    // a_drawFS is the size the caller actually DRAWS at; CalcTextSize measures
    // at the base font size, so every measurement is scaled by drawFS/baseFS --
    // the same correction the delete button's X centring does. That is what
    // makes the cut point follow the body-size slider on its own.
    //
    // Binary search over the cut, so a long label costs ~log2(n) CalcTextSize
    // calls instead of one per character, and a label that already fits costs
    // exactly one.
    std::string TruncateRowTextToWidth(const std::string& a_text, float a_availW, float a_drawFS) {
        if (a_text.empty()) return a_text;
        const float baseFS = ImGui::GetFontSize();
        if (baseFS <= 0.0f) return a_text;
        const float scale = a_drawFS / baseFS;

        auto widthOf = [&](const char* a_begin, const char* a_end) {
            return ImGui::CalcTextSize(a_begin, a_end).x * scale;
        };

        const char* first = a_text.c_str();
        const char* last  = first + a_text.size();
        if (widthOf(first, last) <= a_availW) return a_text;

        // Budget for the body minus the ellipsis, which is always appended once
        // we know the label does not fit.
        static constexpr char kEllipsis[] = "...";
        const float ellipsisW = widthOf(kEllipsis, kEllipsis + 3);
        const float bodyW     = a_availW - ellipsisW;
        // Not even "..." fits. Returning it anyway is better than an empty row:
        // it still says "there is a name here, the panel is too narrow".
        if (bodyW <= 0.0f) return kEllipsis;

        // Largest prefix whose width fits bodyW. Bytes, not codepoints: a cut
        // landing mid-sequence is fixed by the UTF-8 walk-back below.
        std::size_t lo = 0, hi = a_text.size();
        while (lo < hi) {
            const std::size_t mid = (lo + hi + 1) / 2;
            if (widthOf(first, first + mid) <= bodyW) lo = mid; else hi = mid - 1;
        }
        std::size_t cut = lo;

        // Never split a UTF-8 sequence: continuation bytes are 10xxxxxx, so back
        // up off them to the lead byte. Without this a truncated Cyrillic or CJK
        // label ends in a half character and renders as a replacement glyph.
        while (cut > 0 && (static_cast<unsigned char>(a_text[cut]) & 0xC0) == 0x80) --cut;

        // Prefer a word boundary when one sits reasonably close to the cut, so
        // "Wreck of the Pride of Tel..." beats "Wreck of the Pride of Tel Vo...".
        // Only when it keeps at least half the room we had, otherwise a label
        // with one early space would collapse to almost nothing.
        if (const std::size_t sp = a_text.rfind(' ', cut); sp != std::string::npos && sp >= cut / 2) {
            cut = sp;
        }
        if (cut == 0) return kEllipsis;

        return a_text.substr(0, cut) + kEllipsis;
    }

    // ---- Note input soft wrap ------------------------------------------------
    //
    // ImGui's InputTextMultiline does NOT word-wrap: there is no flag for it
    // (ImGuiInputTextFlags_NoHorizontalScroll only stops the box scrolling, it
    // does not reflow), so a long note ran off to the right and the start of the
    // line scrolled out of view while typing.
    //
    // The upstream answer is to insert real newlines from a CallbackEdit and
    // that is what this does, using ImGui's OWN CalcWordWrapPositionA -- the
    // same break-point finder AddText uses, so the input box breaks lines
    // exactly where the rest of our UI would.
    //
    // The inserted newlines are SOFT: StripSoftWraps collapses them back to
    // spaces when the note is committed, so the stored note stays one clean
    // string. That matters because the note's only consumer is the hover panel,
    // which wraps to its own width -- hard breaks baked in at dialog width
    // would fight that wrap at every other size. The cost is that a paragraph
    // break the user typed deliberately is flattened too; the note is a one-
    // line annotation, so that trade is worth it.
    struct NoteWrapCtx {
        ImFont* font      = nullptr;
        float   fontSize  = 0.0f;
        float   wrapWidth = 0.0f;
    };

    int NoteWrapCallback(ImGuiInputTextCallbackData* a_data) {
        const bool verbose = Settings::GetSingleton()->inputVerboseLog;
        const int cursorBefore = a_data->CursorPos;
        const int selectionStartBefore = a_data->SelectionStart;
        const int selectionEndBefore = a_data->SelectionEnd;
        auto* ctx = static_cast<NoteWrapCtx*>(a_data->UserData);
        if (!ctx || !ctx->font || ctx->wrapWidth <= 0.0f || ctx->fontSize <= 0.0f) {
            if (verbose) logger::info("[InputVerbose] note_wrap frame={} skipped=invalid_wrap_context", ImGui::GetFrameCount());
            return 0;
        }

        // CalcWordWrapPositionA takes a SCALE on the font's own FontSize, not a
        // pixel size.
        const float scale = ctx->fontSize / ctx->font->FontSize;

        // Rewrap from scratch: drop every existing newline, then re-break. A
        // purely incremental pass cannot handle deletions or edits in the middle
        // of the text, which move every later break point.
        std::string text(a_data->Buf, a_data->Buf + a_data->BufTextLen);

        // Cursor is a byte offset into the buffer. Track how many newlines sit
        // before it so it can be restored after the rewrite, otherwise the caret
        // jumps every time a break is added or removed.
        int cursor = a_data->CursorPos;
        int removedBeforeCursor = 0;
        std::string flat;
        flat.reserve(text.size());
        for (int i = 0; i < static_cast<int>(text.size()); ++i) {
            if (text[i] == '\n') {
                if (i < cursor) ++removedBeforeCursor;
                flat.push_back(' ');
            } else {
                flat.push_back(text[i]);
            }
        }
        cursor -= removedBeforeCursor;

        // Re-break. Each pass asks ImGui where this line should end, converts
        // that to an offset, and replaces the space at the break with a newline
        // (or inserts one when the break lands mid-word, which happens only for
        // a single word longer than the box).
        std::string wrapped;
        wrapped.reserve(flat.size() + 8);
        int addedBeforeCursor = 0;
        const char* begin = flat.c_str();
        const char* end   = begin + flat.size();
        const char* line  = begin;
        while (line < end) {
            const char* brk = ctx->font->CalcWordWrapPositionA(scale, line, end, ctx->wrapWidth);
            if (brk >= end) { wrapped.append(line, end); break; }
            // CalcWordWrapPositionA can return the line start when even one
            // glyph does not fit (a box narrower than a character). Force one
            // byte of progress or this loops forever.
            if (brk <= line) brk = line + 1;

            wrapped.append(line, brk);
            const int brkOff = static_cast<int>(brk - begin);
            // Swallow the space AT the break so it does not start the next line.
            const char* next = brk;
            if (next < end && *next == ' ') ++next;
            if (next < end) {
                wrapped.push_back('\n');
                if (brkOff < cursor) ++addedBeforeCursor;
            }
            line = next;
        }

        // Only rewrite when the text actually changed. Rewriting unconditionally
        // would fight the caret every keystroke.
        if (wrapped != text) {
            if (static_cast<int>(wrapped.size()) < a_data->BufSize) {
                a_data->DeleteChars(0, a_data->BufTextLen);
                a_data->InsertChars(0, wrapped.c_str(), wrapped.c_str() + wrapped.size());
                a_data->CursorPos = std::clamp(cursor + addedBeforeCursor,
                                               0, static_cast<int>(wrapped.size()));
                a_data->BufDirty = true;
            }
        }
        if (verbose) {
            logger::info(
                "[InputVerbose] note_wrap frame={} callback={} beforeBytes={} proposedBytes={} afterBytes={} capacity={} changed={} capacityRejected={} cursorBefore={} cursorAfter={} selectionBefore={}:{} selectionAfter={}:{} offsets=bytes",
                ImGui::GetFrameCount(), static_cast<int>(a_data->EventFlag), text.size(), wrapped.size(),
                a_data->BufTextLen, a_data->BufSize, wrapped != text,
                wrapped != text && static_cast<int>(wrapped.size()) >= a_data->BufSize,
                cursorBefore, a_data->CursorPos, selectionStartBefore, selectionEndBefore,
                a_data->SelectionStart, a_data->SelectionEnd);
        }
        return 0;
    }

    // Collapse the soft wraps back out before the note is stored. Runs on every
    // commit path, including one typed before this wrapping existed (a note
    // saved with literal newlines flattens the same way).
    std::string StripSoftWraps(const char* a_text) {
        std::string out(a_text);
        std::replace(out.begin(), out.end(), '\n', ' ');
        // A break swallowed its space, so re-inserting one can double up where
        // the user had typed a space of their own right before it.
        const auto tail = std::unique(out.begin(), out.end(),
                                      [](char l, char r) { return l == ' ' && r == ' '; });
        out.erase(tail, out.end());
        // Trim: a trailing break becomes a trailing space.
        const auto notSpace = out.find_last_not_of(' ');
        if (notSpace == std::string::npos) return {};
        out.erase(notSpace + 1);
        out.erase(0, out.find_first_not_of(' '));
        return out;
    }

    // Mirror of the guard inside MapCamera_GlidePan (RVA 0x97FA80, AE 1.6.1170):
    //     camera+0x28  ==  camera+0x68[camera+0x60]
    // True (busy) when current state is the Transition state at camera+0x80,
    // i.e. a prior glide-pan is still running. The helper itself silently
    // no-ops in that case, so we re-queue and retry next frame.
    // See findings/findings-World-Map-Glide-Pan-Backtoback-Stall.md.
    inline bool MapCameraIsBusyTransitioning(RE::TESCamera* camera) {
        auto base = reinterpret_cast<std::uintptr_t>(camera);
        const auto current   = *reinterpret_cast<std::uintptr_t*>(base + 0x28);
        const auto slot      = *reinterpret_cast<std::uint32_t*>(base + 0x60);
        const auto destWorld = *reinterpret_cast<std::uintptr_t*>(
            base + 0x68 + static_cast<std::uintptr_t>(slot) * 8);
        return current != destWorld;
    }

    RE::NiCamera* FindMapCamera(RE::MapMenu* a_menu) {
        if (!a_menu) return nullptr;
        auto root = a_menu->GetRuntimeData2()->camera.cameraRoot;
        if (!root) return nullptr;
        auto& children = root->GetChildren();
        if (children.empty() || !children[0]) return nullptr;

        // [MapPinProjectionLog] Child-array probe for the 1.7.99 invisible-pin bug.
        // child[0] is ASSUMED to be the map NiCamera below; on 1.7.99 the pins
        // project with an inverted W (sz -1.0 vs AE's +0.99) and sy ~2.0 instead
        // of ~0.5, which is what reading the WRONG camera would look like. A
        // netimmerse_cast to the wrong node returns null (we would draw nothing),
        // but a cast to a DIFFERENT NiCamera succeeds and silently projects
        // garbage, so the only way to tell is to print what is actually there.
        // Throttled to one burst per map session; this sits in a per-frame path.
        if (Settings::GetSingleton()->mapPinProjectionLog) {
            static const RE::NiAVObject* s_lastRoot = nullptr;
            if (s_lastRoot != root.get()) {
                s_lastRoot = root.get();
                SKSE::log::info("[pinproj] cameraRoot={} name='{}' childCount={}",
                                static_cast<const void*>(root.get()),
                                root->name.c_str(), children.size());
                for (std::uint16_t i = 0; i < children.size(); ++i) {
                    auto* c = children[i].get();
                    if (!c) {
                        SKSE::log::info("[pinproj]   child[{}] = null", i);
                        continue;
                    }
                    auto* asCam = netimmerse_cast<RE::NiCamera*>(c);
                    SKSE::log::info("[pinproj]   child[{}] name='{}' rtti='{}' isNiCamera={}",
                                    i, c->name.c_str(),
                                    c->GetRTTI() ? c->GetRTTI()->GetName() : "?",
                                    asCam != nullptr);
                    if (asCam) {
                        // Row 3 of worldToCam is the translation; the AE map camera
                        // reads (0,-0,-1,~257844) and 1.7.99 read (0,0.6,-0.8,~21450),
                        // so printing every candidate shows whether a DIFFERENT child
                        // carries the AE-shaped matrix.
                        // Full matrix + an orthonormality check. NiCamera's NG
                        // accessor is RUNTIME_DATA_ACCESSOR(0x110, 0) with NO 1.7.99
                        // slot, so 1.7.99 reads the AE offset. If that offset moved,
                        // worldToCam is simply the wrong 16 floats — and the tell is
                        // that its basis rows stop being unit length / perpendicular.
                        // A real view matrix has |row| == 1 and row0.row1 == 0.
                        const auto& m = asCam->GetRuntimeData().worldToCam;
                        for (int r = 0; r < 4; ++r) {
                            SKSE::log::info("[pinproj]     -> row{}=({:.4f},{:.4f},{:.4f},{:.1f})",
                                            r, m[r][0], m[r][1], m[r][2], m[r][3]);
                        }
                        auto len3 = [](const float* v) {
                            return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
                        };
                        auto dot3 = [](const float* a, const float* b) {
                            return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
                        };
                        SKSE::log::info("[pinproj]     -> |r0|={:.4f} |r1|={:.4f} |r2|={:.4f} "
                                        "| r0.r1={:.4f} r0.r2={:.4f} r1.r2={:.4f} (unit+zero = matrix is real)",
                                        len3(m[0]), len3(m[1]), len3(m[2]),
                                        dot3(m[0], m[1]), dot3(m[0], m[2]), dot3(m[1], m[2]));
                    }
                }
            }
        }

        return netimmerse_cast<RE::NiCamera*>(children[0].get());
    }

    // --- Cleared-dungeon badge: vanilla marker array walk -------------------
    // Mirror of the HoverImageRenderer array access (kept local on purpose;
    // see findings/spec-findings-Cleared-Badge.md — two consumers don't
    // justify a shared header). Stride is the ENGINE stride 0x38, not
    // sizeof(the minimal view struct below).
    constexpr std::uintptr_t kMarkerArrayOffset = 0x30470;
    constexpr std::size_t    kMarkerStride      = 0x38;
    inline std::ptrdiff_t MarkerCountOffset() { return REL::Relocate<std::ptrdiff_t>(0x30480, 0x30478); }

    // Minimal view of the engine's marker-array entry: only the fields this
    // badge pass needs (handle to resolve the ref, type to skip stacked
    // aliases). Full layout with namePtr/nameStr/nextIndex/questObj lives in
    // HoverImageRenderer.cpp; duplicated here deliberately, see finding.
    struct MarkerEntry {
        void*         namePtr;  // +0x00  unused here
        RE::RefHandle handle;   // +0x08
        std::uint32_t pad0C;    // +0x0C
        RE::BSFixedString nameStr; // +0x10  unused here
        std::uint32_t type;     // +0x18  0 = base MapMarker, 1 = stacked alias
    };

    // Clearable dungeon marker types (ExtraMapMarker::mapData->type). Same
    // numbers MarkerTypeKey (HoverImageRenderer.cpp) uses. IsCleared() is the
    // primary gate; this keeps the badge off non-dungeon cleared LCTNs
    // (cities/farms/camps).
    constexpr bool IsDungeonMarkerType(std::uint16_t t) {
        switch (t) {
            case 4: case 6: case 7: case 8: case 9: case 10: case 12: case 15:
                return true;   // Cave, Fort, NordicRuin, DwemerRuin, Shipwreck, Grove, DragonLair, Mine
            default: return false;
        }
    }

    // Filled disc + dark checkmark. Disc = the color knob (Settings), check
    // stroke is a fixed dark outline like every other badge.
    inline void DrawClearedCheck(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 outline) {
        dl->AddCircleFilled(c, r, fill);
        dl->AddCircle(c, r, outline, 0, 1.2f);
        const ImVec2 p0{ c.x - r * 0.45f, c.y + r * 0.05f };
        const ImVec2 p1{ c.x - r * 0.10f, c.y + r * 0.40f };
        const ImVec2 p2{ c.x + r * 0.50f, c.y - r * 0.40f };
        dl->AddLine(p0, p1, outline, std::max(1.5f, r * 0.22f));
        dl->AddLine(p1, p2, outline, std::max(1.5f, r * 0.22f));
    }

    // Note-dialog category combo. Two lists, picked by WHERE THE PIN LANDS,
    // not where the player stands: world-map placements (worldSpaceID != 0)
    // get the full set of every vanilla type we ship art for, including from
    // inside a dungeon; local-map placements get a curated dungeon-relevant
    // set plus the waypoint glyphs. Values are vanilla RE::MARKER_TYPE ids, so the
    // compass and vanilla map render them natively (hudmenu.swf has a frame
    // per type). Index in the active list is the _selectedCategory state;
    // the parallel *Values[] is the iconType actually stored on the marker.
    // Shared by DrawNoteDialog (build the combo) and BeginEditMarker
    // (reverse-map a marker's iconType back to its combo index).
    constexpr const char* kWorldCategoryNames[] = {
        "Custom Marker", "City", "Town", "Settlement", "Cave", "Camp", "Fort",
        "Nordic Ruin", "Dwemer Ruin", "Shipwreck", "Grove", "Landmark",
        "Dragon Lair", "Farm", "Wood Mill", "Mine", "Imperial Camp",
        "Stormcloak Camp", "Doomstone", "Wheat Mill", "Smelter", "Temple",
        // Were interior-only until 2026-08-15. All are outdoor features (a
        // stable, docks, a lighthouse), so limiting them to the local map was
        // an accident of how the interior set was curated, not a design call.
        // Every one already has a PNG, so this is an array edit with no new art.
        // APPEND ONLY: index into this array is _selectedCategory, and
        // BeginEditMarker reverse-maps a saved iconType through it. Reordering
        // or inserting mid-list would repoint existing markers' icons.
        "Stable", "Clearing", "Altar", "Rock", "Lighthouse", "Dwelling",
        "Docks", "Shrine",
        // New art added 2026-08-15. Vanilla compass glyph matches each name.
        "Imperial Tower", "Pass", "Orc Stronghold", "Giant Camp", "Nordic Tower",
        // Solstheim types. Frames ship in hudmenu.swf regardless of whether
        // Dragonborn is installed, so these are safe on any load order.
        "Miraak Temple", "Raven Rock", "Standing Stone", "Telvanni Tower",
        "Castle Karstaag"
    };
    constexpr std::uint32_t kWorldCategoryValues[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 30,
        21, 23, 25, 26, 27, 32, 33, 34,
        22, 24, 28, 29, 31,
        53, 54, 55, 56, 59
    };
    static_assert(std::size(kWorldCategoryNames) == std::size(kWorldCategoryValues),
                  "world category name/value arrays out of sync");

    // Interior set: vanilla types that read sensibly inside a dungeon/house.
    // kDoor (61) and kQuestTarget (62) deliberately excluded: those frames are
    // engine special-cases (quest/door compass slots), plain markers of those
    // types never reach the compass. See RE/E/ExtraMapMarker.h for the enum.
    // Waypoint glyph rows (>= 100, see MapDraw::DrawWaypointGlyph) come first:
    // numbered badges, color dots, star. Vector-drawn, interior-only, skipped
    // by CompassHook. The vanilla compass-compatible set follows below them.
    constexpr const char* kInteriorCategoryNames[] = {
        "1", "2", "3", "4", "5",
        "Red", "Green", "Blue", "Yellow", "Star",
        "Custom Marker", "Shrine", "Altar", "Landmark", "Rock",
        "Clearing", "Shack", "Docks", "Stable", "Lighthouse", "Dwelling",
        "Animal Den"  // vanilla Stormcloak Camp frame: reads as a paw/bear icon
    };
    constexpr std::uint32_t kInteriorCategoryValues[] = {
        100, 101, 102, 103, 104,
        110, 111, 112, 113, 120,
        0, 34, 25, 11, 26, 23, 30, 33, 21, 27, 32, 17
    };
    static_assert(std::size(kInteriorCategoryNames) == std::size(kInteriorCategoryValues),
                  "interior category name/value arrays out of sync");

    struct CategoryTable {
        const char* const*   names;
        const std::uint32_t* values;
        int                  count;
    };
    constexpr CategoryTable kWorldCategories{
        kWorldCategoryNames, kWorldCategoryValues,
        static_cast<int>(std::size(kWorldCategoryNames)) };
    constexpr CategoryTable kInteriorCategories{
        kInteriorCategoryNames, kInteriorCategoryValues,
        static_cast<int>(std::size(kInteriorCategoryNames)) };

    const CategoryTable& CategoriesFor(bool interior) {
        return interior ? kInteriorCategories : kWorldCategories;
    }

    // iconType -> combo index in the given table. Falls back to 0
    // (Custom Marker) if the type isn't in that table.
    int CategoryIndexForIcon(std::uint32_t iconType, bool interior) {
        const auto& tbl = CategoriesFor(interior);
        for (int i = 0; i < tbl.count; ++i) {
            if (tbl.values[i] == iconType) return i;
        }
        return 0;
    }
}

void MapMarkerRenderer::Install() {}

void MapMarkerRenderer::OpenNoteDialog(const RE::NiPoint3& pos,
                                        RE::FormID worldSpaceID,
                                        RE::FormID cellID) {
    _pendingPos        = pos;
    _pendingWorldSpace = worldSpaceID;
    _pendingCell       = cellID;
    _noteDialogInterior = (worldSpaceID == 0);  // interior cells have no worldspace
    _noteBuffer[0]  = '\0';
    _selectedCategory = 0;
    _pendingIsDestination = false;
    _editingMarkerId = 0;   // fresh placement, not an edit
    _editingVanillaFormID = 0;  // not a vanilla-note edit
    _noteDialogOpen = true;
    _noteFocusRequested = true;
    _noteWrapPrimeFrames = 2;
    _notePrevLeftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    _noteOkArmed = false;
    _noteCancelArmed = false;
    _resetImGuiMouseOnNextDraw = true;

    // Remember the cursor screen position so the dialog opens next to the
    // clicked spot, not screen-center. screenWidthX/Y are the cursor's own
    // coordinate-space extents; cursorPosX/Y are in that space.
    _noteDialogX = -1.0f;
    _noteDialogY = -1.0f;
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        const float cw = cursor->screenWidthX;
        const float ch = cursor->screenWidthY;
        if (cw > 0.0f && ch > 0.0f) {
            _noteDialogX = (cursor->cursorPosX / cw);
            _noteDialogY = (cursor->cursorPosY / ch);
        }
    }

    logger::debug("[NoteDialog] opened for pos ({:.1f},{:.1f},{:.1f}) | screen=({:.2f},{:.2f})",
                  pos.x, pos.y, pos.z, _noteDialogX, _noteDialogY);
}

void MapMarkerRenderer::BeginEditMarker(std::uint32_t id) {
    // Find the marker and seed the dialog from it. Reuses the same dialog as a
    // fresh placement; the only difference is _editingMarkerId is non-zero, so
    // confirm routes to UpdateMarker instead of AddMarker.
        // Edit target: only a real marker can be edited.
    auto markers = MarkerManager::GetSingleton()->GetUserMarkers();
    const MapMarker* target = nullptr;
    for (const auto& m : markers) {
        if (m.id == id) { target = &m; break; }
    }
    if (!target) {
        logger::warn("[NoteDialog] BeginEditMarker: no marker with id {}", id);
        return;
    }

    // Seed the buffer with the current note (truncate to buffer size, NUL-term).
    std::snprintf(_noteBuffer, sizeof(_noteBuffer), "%s", target->note.c_str());
    _noteDialogInterior   = (target->worldSpaceID == 0);
    _selectedCategory     = CategoryIndexForIcon(target->iconType, _noteDialogInterior);
    _pendingIsDestination = target->isDestination;
    _editingMarkerId      = id;          // edit mode
    _editingVanillaFormID = 0;           // not a vanilla-note edit
    _noteDialogOpen       = true;
    _noteFocusRequested   = true;
    _noteWrapPrimeFrames  = 2;
    _notePrevLeftDown     = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    _noteOkArmed          = false;
    _noteCancelArmed      = false;
    _resetImGuiMouseOnNextDraw = true;

    // Open the dialog at the current cursor (the click that hit Edit Note).
    _noteDialogX = -1.0f;
    _noteDialogY = -1.0f;
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        const float cw = cursor->screenWidthX;
        const float ch = cursor->screenWidthY;
        if (cw > 0.0f && ch > 0.0f) {
            _noteDialogX = (cursor->cursorPosX / cw);
            _noteDialogY = (cursor->cursorPosY / ch);
        }
    }

}

void MapMarkerRenderer::BeginEditVanillaNote(RE::FormID formID, const std::string& markerName) {
    if (formID == 0) {
        logger::warn("[NoteDialog] BeginEditVanillaNote: formID 0, ignoring");
        return;
    }
    // Seed the buffer with any existing note for this vanilla marker.
    std::string existing = MarkerManager::GetSingleton()->GetVanillaNote(formID);
    std::snprintf(_noteBuffer, sizeof(_noteBuffer), "%s", existing.c_str());

    _editingMarkerId      = 0;          // not editing a custom pin
    _editingVanillaFormID = formID;     // vanilla-note mode
    _vanillaNoteName      = markerName;
    _selectedCategory     = 0;
    _pendingIsDestination = false;
    _noteDialogOpen       = true;
    _noteFocusRequested   = true;
    _noteWrapPrimeFrames  = 2;
    _notePrevLeftDown     = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    _noteOkArmed          = false;
    _noteCancelArmed      = false;
    _resetImGuiMouseOnNextDraw = true;

    // Open at the current cursor (the click that hit the menu item).
    _noteDialogX = -1.0f;
    _noteDialogY = -1.0f;
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        const float cw = cursor->screenWidthX;
        const float ch = cursor->screenWidthY;
        if (cw > 0.0f && ch > 0.0f) {
            _noteDialogX = (cursor->cursorPosX / cw);
            _noteDialogY = (cursor->cursorPosY / ch);
        }
    }
    logger::debug("[NoteDialog] opened for vanilla marker {:08X} ({})", formID, markerName);
}

void MapMarkerRenderer::ToggleListPanel() {
    const bool visible = !_showListPanel.load();
    _showListPanel.store(visible);

    auto* settings = Settings::GetSingleton();
    settings->listPanelVisible = visible;
    // SavePositions already owns lightweight ListPanel view-state persistence;
    // avoid rewriting unrelated settings for a single hotkey press.
    settings->SavePositions();
    logger::debug("List panel visibility preference saved: visible={}", visible);
}

void MapMarkerRenderer::Draw() {
    auto ui = RE::UI::GetSingleton();

    // Never paint over the black fade. The engine raises FaderMenu during both
    // the map open and close transitions; our ImGui pins/panels used to draw on
    // top of that black screen, so they "popped" before and after the map was
    // actually visible. Bailing while the fader is up hides our layer for the
    // whole transition, in lockstep with vanilla, and has no effect during
    // normal interactive map use (fader down). Confirmed in-game; see plugin.cpp.
    if (IsMapFading()) {
        _activeMarkerId = 0;
        _pinHovered.store(false);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        // Don't touch _showListPanel: the fader also runs during map OPEN, so
        // clearing it here would defeat the default-open state every time.
        if (_noteDialogOpen) {
            _noteDialogOpen = false;
        }
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
        }
        return;
    }

    if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
        _activeMarkerId = 0;
        _pinHovered.store(false);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        // Guardrail: if the map closes while the note dialog is open (e.g. user
        // pressed M/Tab to bail because something external like a "typing mode"
        // mod was blocking InputText), we must reset _noteDialogOpen here.
        // Otherwise InputHook stays in blockKeys=true forever and starves the
        // entire game of keyboard input.
        if (_noteDialogOpen) {
            logger::debug("[NoteDialog] map closed while dialog open - force-resetting state");
            _noteDialogOpen = false;
        }
        // Restore the user's explicit C-key preference for the next map session.
        // Temporary SMF/modal/local-map suppression never writes this value.
        _showListPanel.store(Settings::GetSingleton()->listPanelVisible);
        // Every tab's offset, not just the active one: a tab left deep-scrolled
        // would still be there on the next map session.
        for (auto& sy : _listScrollY) sy = 0.0f;
        _listScrollMax = 0.0f;
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
            _rowIds.clear();
            _rowDeleteRects.clear();
            _destRowValid = false;
        }
        _pendingPanTarget.store(0);
        _pendingPanDestPos.store(false);
        // Badge scale-in: clear so the next map open re-seeds already-badged
        // markers at scale=1 instantly and only new additions animate.
        _favBadgeBorn.clear();
        _noteBadgeBorn.clear();
        _clearedBadgeBorn.clear();
        _badgesSeeded = false;
        return;
    }

    const bool smfBlocksMap = MenuUtils::ShouldBlockMapOverlay();
    static bool wasSmfBlocked = false;
    if (smfBlocksMap != wasSmfBlocked) {
        if (Settings::GetSingleton()->inputVerboseLog) {
            logger::info("[InputVerbose] map_overlay_smf_blocked={}", smfBlocksMap);
        }
        wasSmfBlocked = smfBlocksMap;
    }
    if (smfBlocksMap) {
        _activeMarkerId = 0;
        _pinHovered.store(false);
        _hoveredMarkerId.store(0);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        _editButtonTarget.store(0);
        _openTerrainMenu.store(false);
        _pendingTerrainPlace.store(false);
        _openCustomContextMenu.store(false);
        _customMenuVisible = false;
        _noteDialogOpen = false;
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
            _rowIds.clear();
            _rowDeleteRects.clear();
            _destRowValid = false;
        }
        _pendingPanTarget.store(0);
        _pendingPanDestPos.store(false);
        return;
    }

    auto mapMenu = ui->GetMenu<RE::MapMenu>();
    if (!mapMenu) return;

    // Detect local map active state.
    // RE findings (findings-localmap.md): MapMenu+0x3045c is a byte flag,
    // 1 = local map showing, 0 = world map. Raw RVA, AE 1.6.1170 only.
    // Equivalently LocalMapMenu+0x303fc; written by FUN_14097bf40.
    bool isLocalMap = false;
    {
        const auto base = reinterpret_cast<std::uintptr_t>(mapMenu.get());
        isLocalMap = *reinterpret_cast<const bool*>(base + 0x3045c);
    }

    ImDrawList* dl       = ImGui::GetBackgroundDrawList();
    ImVec2      dispSize = ImGui::GetIO().DisplaySize;

    // Engine menu cursor, normalised into our ImGui display space.
    ImVec2 mouse{ -1.0f, -1.0f };
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        float w = cursor->screenWidthX;
        float h = cursor->screenWidthY;
        if (w > 0.0f && h > 0.0f) {
            mouse = ImVec2((cursor->cursorPosX / w) * dispSize.x,
                           (cursor->cursorPosY / h) * dispSize.y);
        }
    }

    // Check if modal dialogs or menus are open on top of the map
    bool isDialogOpen = ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME) ||
                        ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                        ui->IsMenuOpen(RE::JournalMenu::MENU_NAME);

    if (isDialogOpen) {
        _activeMarkerId = 0;
        _pinHovered.store(false);
        _hoveredMarkerId.store(0);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        // _showListPanel intentionally kept: this branch early-returns so the
        // panel isn't drawn under the modal, and it comes back when it closes.

        // Note: Ensure the note dialog state is reset if a modal menu (like the console) forces the UI to hide.
        // Failing to do this leaves the InputHook permanently active, breaking text input globally.
        _noteDialogOpen = false;
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
            _rowIds.clear();
            _rowDeleteRects.clear();
            _destRowValid = false;
        }
        _pendingPanTarget.store(0);
        _pendingPanDestPos.store(false);
        if (mouse.x >= 0.0f && mouse.y >= 0.0f) {
            MapDraw::RequestCursor(mouse.x, mouse.y);
        }
        return;
    }

    if (isLocalMap) {
        _activeMarkerId = 0;
        _pinHovered.store(false);
        // Also clear the id InputHook's right-click router reads. Without this
        // the last WORLD-map hover survives the switch to the local map and
        // hijacks every right-click into the custom-pin menu (quick-marks land
        // at the player, i.e. dead center where the map opens, so they were
        // reliably hovered the frame before pressing L). Bug 2026-07-17.
        _hoveredMarkerId.store(0);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        // _showListPanel kept: list panel is world-map only (this branch
        // early-returns before it draws), but the state survives the local
        // detour so it's still open when the user tabs back to the world map.
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
            _rowIds.clear();
            _rowDeleteRects.clear();
            _destRowValid = false;
        }
        _pendingPanTarget.store(0);
        _pendingPanDestPos.store(false);

        // LocalMapRenderer owns local pins/hover panels. This renderer still
        // owns the shared note dialog, so keep drawing it while the local map
        // is active.
        if (_noteDialogOpen) {
            DrawNoteDialog();
        }
        // Right-click "Add Marker Here" menu must run here too - Draw() returns
        // early for the local map, before the world-map call below. Without
        // this the local-map menu never opens (its placement-fire block never
        // executes), which is why right-click placement silently no-op'd.
        DrawTerrainMenu(true);
        // Same reasoning for the custom-pin right-click menu: LocalMapRenderer
        // detects the pin hover and calls OpenCustomContextMenu, but the popup
        // itself is drawn here since this class owns MarkerManager/BeginEditMarker.
        DrawCustomContextMenu();
        if (mouse.x >= 0.0f && mouse.y >= 0.0f) {
            MapDraw::RequestCursor(mouse.x, mouse.y);
        }
        return;
    }

    auto camera = FindMapCamera(mapMenu.get());
    if (!camera) return;

    auto& camData  = camera->GetRuntimeData();
    auto& camData2 = camera->GetRuntimeData2();
    RE::TESWorldSpace* currentMapWorld = mapMenu->GetRuntimeData2()->worldSpace;
    const bool useFlatMapPlane = MapDraw::UsesFlatMapProjection(camData.worldToCam, currentMapWorld);

    // [MapPinProjectionLog] One header line per map frame, ahead of the per-pin
    // rows. Worldspace configuration keeps authored flat maps stable during
    // camera tilt; other maps use the live camera geometry.
    if (Settings::GetSingleton()->mapPinProjectionLog) {
        const auto& t = camData.worldToCam[3];
        if (currentMapWorld) {
            SKSE::log::info(
                "[pinproj] --- frame | projectionMode={} mapPlaneZ={:.0f} "
                "| camLook=({:.4f},{:.4f},{:.4f}) camTranslation={:.1f} "
                "| mapWorld={:08X} mapInitialPitch={:.1f} mapOffsetZ={:.1f} "
                "| portWxH=({:.0f}x{:.0f})",
                useFlatMapPlane ? "flat-plane" : "terrain-z", MapDraw::kMapPlaneZ,
                t[0], t[1], t[2], t[3], currentMapWorld->GetFormID(),
                currentMapWorld->worldMapData.cameraData.initialPitch,
                currentMapWorld->worldMapOffsetData.mapOffsetZ,
                camData2.port.GetWidth(), camData2.port.GetHeight());
        } else {
            SKSE::log::info(
                "[pinproj] --- frame | projectionMode={} mapPlaneZ={:.0f} "
                "| camLook=({:.4f},{:.4f},{:.4f}) camTranslation={:.1f} "
                "| mapWorld=null mapInitialPitch=null mapOffsetZ=null "
                "| portWxH=({:.0f}x{:.0f})",
                useFlatMapPlane ? "flat-plane" : "terrain-z", MapDraw::kMapPlaneZ,
                t[0], t[1], t[2], t[3],
                camData2.port.GetWidth(), camData2.port.GetHeight());
        }
    }

    // Apply any pan requested by a panel click on the previous frame, before
    // the engine's own tick bakes the camera transform later this frame.
    ApplyPendingPan(mapMenu.get());

    // Project every marker that is in front of the camera.
    struct Projected {
        std::uint32_t id;
        float         px, py;
        std::string   note;
        std::uint32_t iconType;
        bool          isDestination;
    };
    std::vector<Projected> visible;
    std::vector<Projected> destRings;  // destination markers: ring-only, no pin
    auto markers = MarkerManager::GetSingleton()->GetMarkers();
    visible.reserve(markers.size());
    
    for (const auto& m : markers) {
        if (m.worldSpaceID == 0) continue;  // interior marker, not visible on world map
        
        // Ensure the marker actually belongs to the map currently on screen.
        // E.g., if the map is Tamriel, show Tamriel and Whiterun (child).
        // Hide Solstheim or FWMF custom worldspaces.
        bool worldMatch = false;
        if (currentMapWorld) {
            auto* markerWorld = RE::TESForm::LookupByID<RE::TESWorldSpace>(m.worldSpaceID);
            for (auto* w = markerWorld; w; w = w->parentWorld) {
                if (w == currentMapWorld) {
                    worldMatch = true;
                    break;
                }
            }
        }
        if (!worldMatch) continue;

        // Hide vanilla destinations from the world map renderer completely.
        // They are already represented by their vanilla Scaleform icons. Drawing our
        // custom UI over them causes visual drifting when using flat map mods (FWMF)
        // and creates conflicting hover panels. They remain visible in the Compass.
        // Still project them into destRings so the pulse halo draws.
        // Keyed on the reserved synthesized id, not a note prefix.
        if (m.id == kVanillaDestId) {
            // A door destination has no world-map icon of its own, so its ring
            // would pulse over bare terrain with nothing under it. Its surface is
            // the EXTERIOR local map, where the door icon actually is. A vanilla
            // world marker set as destination is the other case and keeps its
            // ring here (there is an icon beneath it).
            if (MarkerManager::GetSingleton()->IsVanillaDestFromLocalDoor()) continue;
            // Destination markers store ground Z. Flat maps project them on the
            // tested high plane; tilted maps retain that ground Z.
            RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(m.position, useFlatMapPlane);
            float sx, sy, sz;
            if (RE::NiCamera::WorldPtToScreenPt3(
                    camData.worldToCam, camData2.port, mapPos, sx, sy, sz, 1e-5f)
                && MapDraw::ProjectionDepthValid(sz)) {
                destRings.push_back({ m.id, sx * dispSize.x, (1.0f - sy) * dispSize.y,
                                      m.note, m.iconType, true });
            }
            continue;
        }

        // Stored Z varies by placement path and map setup. Select projection Z
        // from the live camera instead: normalize every flat-map pin to the
        // tested plane and leave tilted-map positions untouched.
        RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(m.position, useFlatMapPlane);
        float sx, sy, sz;
        bool inFront = RE::NiCamera::WorldPtToScreenPt3(
            camData.worldToCam, camData2.port, mapPos, sx, sy, sz, 1e-5f);
        // [MapPinProjectionLog] Printed BEFORE the reject below: a culled pin is
        // exactly the case we need numbers for, so logging after the continue
        // would show nothing on the broken runtime.
        if (Settings::GetSingleton()->mapPinProjectionLog) {
            const bool depthOk = MapDraw::ProjectionDepthValid(sz);
            SKSE::log::info(
                "[pinproj] id={} mode={} storedZ={:.1f} zAdjusted={} usedZ={:.1f} | sx={:.4f} sy={:.4f} sz={:.4f} "
                "| inFront={} depthOk={} -> px={:.1f} py={:.1f} | verdict={}",
                m.id, useFlatMapPlane ? "flat-plane" : "terrain-z", m.position.z,
                (mapPos.z != m.position.z) ? "YES" : "no", mapPos.z,
                sx, sy, sz, inFront, depthOk,
                sx * dispSize.x, (1.0f - sy) * dispSize.y,
                (!inFront)          ? "CULL_BEHIND_CAMERA"
                : (!depthOk)        ? "CULL_DEPTH_INVALID"
                                    : "DRAWN");
        }
        if (!inFront || !MapDraw::ProjectionDepthValid(sz)) continue;
        visible.push_back({ m.id,
                            sx * dispSize.x,
                            (1.0f - sy) * dispSize.y,
                            ResolveMarkerName(m),
                            m.iconType,
                            m.isDestination });
    }

    // HUD-settings scales, resolved once for BOTH the keep-rect check and the
    // draw below — they must feed ComputeInfoPanel identically or the keep-
    // rect drifts from the drawn panel. Heading font (INI HeadingFontSize)
    // folds into the title scale; same TTF, so base-font measurement times
    // the size ratio stays exact.
    ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
    if (!titleFont) titleFont = ImGui::GetFont();
    const float fontRatio  = titleFont->FontSize / ImGui::GetFontSize();
    const float titleScale = std::clamp(Settings::GetSingleton()->titleScale, 0.5f, 3.0f) * fontRatio;
    const float bodyScale  = std::clamp(Settings::GetSingleton()->bodyScale,  0.5f, 3.0f);

    // Resolve the active (panel-showing) marker.
    //  - Already active: stays active while the cursor is inside the combined
    //    padded box of pin + panel (one rectangle, no gap to fall through).
    //  - Otherwise: the first marker whose pin box contains the cursor.
    // Skip entirely while the Note dialog is open: the dialog already sits
    // over the map and swallows right-click, but without this guard hovering
    // a custom pin behind it still pops the hover panel + Remove/Set
    // Destination buttons underneath. Pins themselves keep drawing below.
    const Projected* active = nullptr;
    // IsCustomMenuActive: the instant a pin is right-clicked the info panel
    // hides, same as the vanilla hover card when its context menu opens.
    if (!_noteDialogOpen && !IsCustomMenuActive()) {
        if (_activeMarkerId != 0) {
            for (const auto& v : visible) {
                if (v.id != _activeMarkerId) continue;
                // Info-only panel (no action rows), so the keep-rect is sized
                // from the header alone.
                MapDraw::InfoPanelLayout pl = MapDraw::ComputeInfoPanel(v.px, v.py, v.note.c_str(), dispSize,
                                                                        titleScale, bodyScale,
                                                                        static_cast<float>(Settings::GetSingleton()->markerHoverOffsetX),
                                                                        static_cast<float>(Settings::GetSingleton()->markerHoverOffsetY));
                MapDraw::Rect keep = MapDraw::Combine(MapDraw::PinBox(v.px, v.py), pl.panel, 8.0f);
                if (MapDraw::PointInRect(mouse, keep)) active = &v;
                break;
            }
        }
        if (!active) {
            for (const auto& v : visible) {
                if (MapDraw::PointInRect(mouse, MapDraw::PinBox(v.px, v.py))) { active = &v; break; }
            }
        }
    }
    const std::uint32_t previousMarkerId = _activeMarkerId;
    _activeMarkerId = active ? active->id : 0;
    if (_activeMarkerId != 0 && _activeMarkerId != previousMarkerId) {
        UISound::PlayMapMarkerHover();
    }
    _pinHovered.store(_activeMarkerId != 0);
    _hoveredMarkerId.store(_activeMarkerId);

    // Destination halo: drawn under the pin so it reads as a glow. Continuous
    // pulsing sine animation, sampled off steady_clock for monotonic time.
    const float timeSec = std::chrono::duration<float>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const auto& v : visible) {
        if (v.isDestination) MapDraw::DrawDestinationRing(dl, v.px, v.py, timeSec);
    }
    for (const auto& v : destRings) {
        MapDraw::DrawDestinationRing(dl, v.px, v.py, timeSec, /*centerOnPoint=*/true);
    }

    // Badge pass on vanilla map markers: star (favorited) top-right, dot
    // (has a vanilla note) top-left. Drawn BEFORE our own pins so custom pins
    // stay on top; world map only (this whole function returns early above
    // for isLocalMap). Snapshot both lists once per frame, not per badge.
    if (Settings::GetSingleton()->enableBadges) {
        constexpr float kBadgeOffsetX = 12.0f;
        constexpr float kBadgeOffsetY = 12.0f;
        constexpr float kCullMargin   = 24.0f;
        constexpr float kBadgePopSec  = 0.2f;
        // Size + color are user-configurable (Settings -> Markers tab),
        // outline stays the fixed dark ring every other badge/pin uses.
        auto* markerSet = Settings::GetSingleton();
        const float kStarRadius = markerSet->favBadgeSize;
        const float kDotRadius  = markerSet->noteBadgeSize;
        const ImU32 favFill  = markerSet->FavBadgeCol();
        const ImU32 noteFill = markerSet->NoteBadgeCol();

        auto onScreen = [&](float x, float y) {
            return x >= -kCullMargin && x <= dispSize.x + kCullMargin &&
                   y >= -kCullMargin && y <= dispSize.y + kCullMargin;
        };

        // Focus-mode gate. Badges hang off vanilla markers, but they're drawn
        // from OUR lists, so the MarkerFilterHook IsMarkerVisible detour that
        // hides the marker never touches them — without this a filtered-out
        // dungeon keeps its star/dot/check floating over empty terrain.
        // The marker type lives on the ref's ExtraMapMarker, so a ref we can't
        // resolve (or that carries no map data) is treated as unfiltered:
        // matches the hook, which only forces false on markers it can classify.
        auto filteredByMode = [](RE::TESObjectREFR* a_ref) {
            if (!a_ref) return false;
            auto* extra = a_ref->extraList.GetByType<RE::ExtraMapMarker>();
            if (!extra || !extra->mapData) return false;
            // Ref-aware form, NOT IsTypeFiltered: a CoMAP-retyped marker's type
            // alone resolves to kModded, so the type-only call would leave a
            // badge floating over a marker that Dungeons/Shops/Loot just hid.
            return MarkerFilterHook::IsMarkerFiltered(a_ref, extra->mapData->type.underlying());
        };

        auto favsAll  = MarkerManager::GetSingleton()->GetFavorites();
        auto notesAll = MarkerManager::GetSingleton()->GetVanillaNotes();

        // Seed-on-open: the first badge pass after the map (re)opens must not
        // animate already-favorited/noted/cleared markers, only additions made
        // while the map stays open. Birth times are backdated so scale=1
        // instantly. One gate (_badgesSeeded) covers all three badge types;
        // it flips true only after the cleared-badge loop below has also had
        // a chance to backdate (that loop has no stored list to seed from
        // up front, so it backdates lazily as it enumerates).
        const bool seedingThisFrame = !_badgesSeeded;
        if (seedingThisFrame) {
            const auto farPast = std::chrono::steady_clock::now() - std::chrono::hours(24);
            for (const auto& f : favsAll) _favBadgeBorn.emplace(f.formID, farPast);
            for (const auto& [formID, note] : notesAll) { (void)note; _noteBadgeBorn.emplace(formID, farPast); }
        }

        auto easeScale = [](std::chrono::steady_clock::time_point born) {
            const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - born).count();
            const float w = std::clamp(age / kBadgePopSec, 0.0f, 1.0f);
            return 1.0f - std::pow(1.0f - w, 3.0f); // cubic ease-out
        };

        for (const auto& f : favsAll) {
            if (f.worldSpaceID == 0) continue;

            // Favorites store position/worldspace but no ref, so the type read
            // needs a lookup this loop otherwise avoids. Favorite lists are a
            // handful of entries, so the per-frame cost is noise; revisit only
            // if it ever shows up in a profile.
            if (filteredByMode(RE::TESForm::LookupByID<RE::TESObjectREFR>(f.formID))) continue;

            bool worldMatch = false;
            if (currentMapWorld) {
                auto* markerWorld = RE::TESForm::LookupByID<RE::TESWorldSpace>(f.worldSpaceID);
                for (auto* w = markerWorld; w; w = w->parentWorld) {
                    if (w == currentMapWorld) { worldMatch = true; break; }
                }
            }
            if (!worldMatch) continue;

            RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(f.position, useFlatMapPlane);
            float sx, sy, sz;
            if (!RE::NiCamera::WorldPtToScreenPt3(
                    camData.worldToCam, camData2.port, mapPos, sx, sy, sz, 1e-5f) || !MapDraw::ProjectionDepthValid(sz))
                continue;
            float px = sx * dispSize.x, py = (1.0f - sy) * dispSize.y;
            if (!onScreen(px, py)) continue;

            auto [it, inserted] = _favBadgeBorn.try_emplace(f.formID, std::chrono::steady_clock::now());
            const float scale = easeScale(it->second);

            MapDraw::DrawStar(dl, ImVec2(px + kBadgeOffsetX, py - kBadgeOffsetY), kStarRadius * scale,
                              favFill, MapTheme::kFavBadgeOutline);
        }

        for (const auto& [formID, note] : notesAll) {
            (void)note;
            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
            if (!ref) continue;
            if (filteredByMode(ref)) continue;
            auto* ws = ref->GetWorldspace();
            if (!ws) continue;
            bool worldMatch = false;
            for (auto* w = ws; w; w = w->parentWorld) {
                if (w == currentMapWorld) { worldMatch = true; break; }
            }
            if (!worldMatch) continue;

            RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(ref->GetPosition(), useFlatMapPlane);
            float sx, sy, sz;
            if (!RE::NiCamera::WorldPtToScreenPt3(
                    camData.worldToCam, camData2.port, mapPos, sx, sy, sz, 1e-5f) || !MapDraw::ProjectionDepthValid(sz))
                continue;
            float px = sx * dispSize.x, py = (1.0f - sy) * dispSize.y;
            if (!onScreen(px, py)) continue;

            auto [it, inserted] = _noteBadgeBorn.try_emplace(formID, std::chrono::steady_clock::now());
            const float scale = easeScale(it->second);

            ImVec2 dotCenter(px - kBadgeOffsetX, py - kBadgeOffsetY);
            dl->AddCircleFilled(dotCenter, kDotRadius * scale, noteFill);
            dl->AddCircle(dotCenter, kDotRadius * scale, MapTheme::kNoteBadgeOutline, 0, 1.2f);
        }

        // Cleared-dungeon badge: green disc + checkmark, top-center of the
        // pin (the one free corner: fav star is top-right, note dot is
        // top-left). No stored list to iterate (unlike fav/note) — the set
        // of cleared dungeons lives in the game, so this enumerates the
        // vanilla marker array (MapMenu+0x30470) each frame and gates on
        // type==0, dungeon-type, and BGSLocation::IsCleared(). See
        // findings/spec-findings-Cleared-Badge.md.
        {
            const float kClearedRadius = markerSet->clearedBadgeSize;
            const ImU32 clearedFill    = markerSet->ClearedBadgeCol();
            // Top-center: no X offset, lifted a touch more than the side badges.
            const float kClearedLiftY = kBadgeOffsetY + 4.0f;

            auto base   = reinterpret_cast<std::uintptr_t>(mapMenu.get());
            auto arrPtr = *reinterpret_cast<std::uintptr_t*>(base + kMarkerArrayOffset);
            const auto arrSize = arrPtr
                ? *reinterpret_cast<std::uint32_t*>(base + MarkerCountOffset()) : 0u;

            for (std::uint32_t i = 0; i < arrSize; ++i) {
                const auto* e = reinterpret_cast<const MarkerEntry*>(
                    arrPtr + static_cast<std::uintptr_t>(i) * kMarkerStride);
                // type==1 entries are stacked aliases; the type==0 base entry
                // for the same dungeon is elsewhere in the array and will
                // badge it. Dispatching type-1 here would double-draw.
                if (e->type != 0 || e->handle == 0) continue;

                RE::NiPointer<RE::TESObjectREFR> refr;
                if (!RE::LookupReferenceByHandle(e->handle, refr) || !refr) continue;

                // Dungeon-type gate.
                auto* extra = refr->extraList.GetByType<RE::ExtraMapMarker>();
                if (!extra || !extra->mapData) continue;
                const auto markerType = extra->mapData->type.underlying();
                if (!IsDungeonMarkerType(markerType)) continue;
                // Ref-aware: a CoMAP-retyped dungeon needs its icon name to
                // land in kDungeons rather than kModded.
                if (MarkerFilterHook::IsMarkerFiltered(refr.get(), markerType)) continue;

                // Cleared gate: the ref's OWN editor location, not the parent
                // Hold (the Hold is never "cleared"; the dungeon is).
                auto* loc = refr->GetEditorLocation();
                if (!loc || !loc->IsCleared()) continue;

                // Worldspace match (same pattern as the note-dot loop).
                auto* ws = refr->GetWorldspace();
                if (!ws) continue;
                bool worldMatch = false;
                for (auto* w = ws; w; w = w->parentWorld) {
                    if (w == currentMapWorld) { worldMatch = true; break; }
                }
                if (!worldMatch) continue;

                // The ref supplies ground Z; the live camera selects whether
                // projection retains it or uses the flat-map plane.
                RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(
                    refr->GetPosition(), useFlatMapPlane);
                float sx, sy, sz;
                if (!RE::NiCamera::WorldPtToScreenPt3(
                        camData.worldToCam, camData2.port, mapPos, sx, sy, sz, 1e-5f) || !MapDraw::ProjectionDepthValid(sz))
                    continue;
                float px = sx * dispSize.x, py = (1.0f - sy) * dispSize.y;
                if (!onScreen(px, py)) continue;

                const RE::FormID fid = refr->GetFormID();
                const auto birth = seedingThisFrame
                    ? std::chrono::steady_clock::now() - std::chrono::hours(24)
                    : std::chrono::steady_clock::now();
                auto [it, inserted] = _clearedBadgeBorn.try_emplace(fid, birth);
                const float scale = easeScale(it->second);

                DrawClearedCheck(dl, ImVec2(px, py - kClearedLiftY),
                                  kClearedRadius * scale, clearedFill, MapTheme::kClearedBadgeOutline);
            }
        }

        _badgesSeeded = true;
    }

    // Draw all pins. Hover tint + scale pop ease in/out per marker id instead
    // of snapping. Rise was 0.15s and read as a snap once DrawPin added the
    // scale pop on top of it (the ramp finished before the eye tracked it);
    // 0.26s in / 0.30s out is slow enough to see the grow, still responsive.
    {
        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
        for (const auto& v : visible) {
            float& anim = _hoverAnim[v.id];
            const float target = (v.id == _activeMarkerId) ? 1.0f : 0.0f;
            const float rate = (target > anim) ? (dt / 0.26f) : (dt / 0.30f);
            anim = std::clamp(anim + (target - anim > 0.0f ? rate : -rate), 0.0f, 1.0f);
        }
        // Keep the map small: drop entries that have faded out and aren't hovered.
        for (auto it = _hoverAnim.begin(); it != _hoverAnim.end(); ) {
            if (it->second <= 0.0f && it->first != _activeMarkerId) it = _hoverAnim.erase(it);
            else ++it;
        }
    }
    for (const auto& v : visible) {
        auto* pinSrv = TextureManager::GetCategoryIcon(v.iconType);
        MapDraw::DrawPin(dl, v.px, v.py, _hoverAnim[v.id], v.iconType, pinSrv,
                         Settings::GetSingleton()->markerIconScale,
                         Settings::GetSingleton()->PinTintCol());
    }

    // Pulse ring on the most recently panned-to marker. Two staggered waves
    // (B starts 0.4s after A), each a cubic ease-out expansion that holds
    // near-full alpha for its first third then fades over the rest, so the
    // ring reads as a quick double-tap instead of one slow linear ripple.
    if (_pulseMarkerId != 0 || _pulseFavId != 0 || _pulseNoteId != 0 || _pulseQuestId != 0) {
        constexpr float kPulseSeconds = 1.8f;
        const float t = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - _pulseStart).count();
        if (t >= kPulseSeconds) {
            _pulseMarkerId = 0;
            _pulseFavId = 0;
            _pulseNoteId = 0;
            _pulseQuestId = 0;
        } else {
            float px = 0.0f, py = 0.0f;
            bool foundPos = false;

            if (_pulseMarkerId != 0) {
                for (const auto& v : visible) {
                    if (v.id == _pulseMarkerId) { 
                        px = v.px; py = v.py; 
                        foundPos = true; 
                        break; 
                    }
                }
            } else if (_pulseFavId != 0) {
                auto favs = MarkerManager::GetSingleton()->GetFavorites();
                for (const auto& f : favs) {
                    if (f.formID == _pulseFavId) {
                        // Favorites store the marker ref's ground position.
                        RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(
                            f.position, useFlatMapPlane);
                        float sx, sy, sz;
                        if (RE::NiCamera::WorldPtToScreenPt3(
                                camData.worldToCam, camData2.port, mapPos, sx, sy, sz, 1e-5f)
                            && MapDraw::ProjectionDepthValid(sz)) {
                            px = sx * dispSize.x;
                            py = (1.0f - sy) * dispSize.y;
                            foundPos = true;
                        }
                        break;
                    }
                }
            } else if (_pulseNoteId != 0) {
                // Vanilla-note pan: no cached position, resolve the ref fresh
                // and use the shared camera-mode projection rule.
                if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(_pulseNoteId)) {
                    RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(
                        ref->GetPosition(), useFlatMapPlane);
                    float sx, sy, sz;
                    if (RE::NiCamera::WorldPtToScreenPt3(
                            camData.worldToCam, camData2.port, mapPos, sx, sy, sz, 1e-5f)
                        && MapDraw::ProjectionDepthValid(sz)) {
                        px = sx * dispSize.x;
                        py = (1.0f - sy) * dispSize.y;
                        foundPos = true;
                    }
                }
            } else if (_pulseQuestId != 0) {
                // Quest rows carry the exact resolved world position directly in
                // _pulseQuestPos, so no row lookup or quest ID matching is needed.
                // These positions are ground-level; the live camera selects their projection Z.
                RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(_pulseQuestPos, useFlatMapPlane);
                float sx, sy, sz;
                if (RE::NiCamera::WorldPtToScreenPt3(
                        camData.worldToCam, camData2.port, mapPos, sx, sy, sz, 1e-5f)
                    && MapDraw::ProjectionDepthValid(sz)) {
                    px = sx * dispSize.x;
                    py = (1.0f - sy) * dispSize.y;
                    foundPos = true;
                }
            }

            if (foundPos) {
                constexpr float kWaveLife = 1.4f;
                constexpr float kStagger  = 0.4f;
                // wave: 0 = A (starts t=0), 1 = B (starts t=kStagger).
                auto drawWave = [&](float startT, float r0Base, float r0Travel,
                                    float r1Base, float r1Travel,
                                    int peakA0, int peakA1) {
                    const float localT = t - startT;
                    if (localT < 0.0f || localT > kWaveLife) return; // not yet started / already finished
                    const float w = localT / kWaveLife;              // 0..1 raw progress
                    const float e = 1.0f - std::pow(1.0f - w, 3.0f); // cubic ease-out
                    // Hold full alpha for the first third of the wave's life,
                    // then fade linearly to 0 over the remaining two thirds.
                    const float alpha = (w < (1.0f / 3.0f))
                        ? 1.0f
                        : 1.0f - (w - (1.0f / 3.0f)) / (2.0f / 3.0f);
                    const float r0 = r0Base + e * r0Travel;
                    const float r1 = r1Base + e * r1Travel;
                    const ImU32 col0 = IM_COL32(255, 220, 120, static_cast<int>(alpha * peakA0));
                    const ImU32 col1 = IM_COL32(255, 240, 180, static_cast<int>(alpha * peakA1));
                    dl->AddCircle(ImVec2(px, py), r0, col0, 0, 2.5f);
                    dl->AddCircle(ImVec2(px, py), r1, col1, 0, 1.5f);
                };
                // Same start/travel values as before (outer 18->44, inner 12->28).
                drawWave(0.0f,     18.0f, 26.0f, 12.0f, 16.0f, 220, 180);
                drawWave(kStagger, 18.0f, 26.0f, 12.0f, 16.0f, 220, 180);
            }
        }
    }

    // Draw the info-only hover panel for the active marker: name + "Custom
    // Marker" subtitle, no action rows. Actions (Set/Clear Destination,
    // Rename, Remove) moved to the right-click menu (DrawCustomContextMenu);
    // this panel is purely a "what is this pin" readout now, same visual
    // header style as before. Left-click on a custom pin is still swallowed
    // by MarkerHoverHook's IsCustomPinHovered check (no re-placement), it just
    // has nothing to click on the panel anymore.
    if (active) {
        // Scales + titleFont resolved above, shared with the keep-rect check.
        auto* hoverSet = Settings::GetSingleton();
        MapDraw::InfoPanelLayout pl = MapDraw::ComputeInfoPanel(active->px, active->py, active->note.c_str(),
                                                                 dispSize, titleScale, bodyScale,
                                                                 static_cast<float>(hoverSet->markerHoverOffsetX),
                                                                 static_cast<float>(hoverSet->markerHoverOffsetY));

        // Panel background + border. Bg comes from the GLOBAL appearance setting,
        // same as the border beside it and the vanilla stat card: MapTheme is the
        // source of the DEFAULT (Settings seeds from it), not of the live colour.
        // This panel and the local-map one were the last two still painting the
        // hardcoded token, so a user-set background skipped them.
        MapDraw::DrawPanelCard(dl, pl.panel.mn, pl.panel.mx, hoverSet->BgCol(), hoverSet->BorderCol());

        // Header: note name + "Custom Marker" subtitle
        const float fs   = ImGui::GetFontSize();
        const float hPadX = MapDraw::kHeaderPadX;
        const float hPadY = MapDraw::kHeaderPadY;
        MapDraw::DrawTextShadowed(dl, titleFont, fs * titleScale,
                                  ImVec2(pl.panel.mn.x + hPadX, pl.panel.mn.y + hPadY),
                                  hoverSet->TitleCol(), active->note.c_str(), nullptr,
                                  pl.noteWrapW);
        // "Custom Marker" is a LABEL, so it takes the same tone as the vanilla
        // stat card's row labels and Notes heading: the body colour faded to
        // kLabelAlpha, not the hardcoded kMutedText grey it used to be. A
        // user-set body colour now moves this subtitle with everything else.
        dl->AddText(ImGui::GetFont(), fs * bodyScale * 0.85f,
                    ImVec2(pl.panel.mn.x + hPadX, pl.panel.mn.y + hPadY + pl.nameH + 2.0f),
                    MapDraw::FadeAlpha(hoverSet->BodyCol(), MapDraw::kLabelAlpha),
                    LocalizationManager::Get("Labels.CustomMarker").c_str());
    }
    // Button-target getters are now always 0 for custom pins (no more panel
    // rows to hit-test); left in place since MarkerHoverHook/LocalMapClickHook
    // still read them harmlessly (they just never fire for custom pins now).
    _removeButtonTarget.store(0);
    _destButtonTarget.store(0);
    _editButtonTarget.store(0);

    // Custom marker list panel (hotkey C).
    if (_showListPanel.load()) {
        DrawListPanel();
    } else {
        std::lock_guard<std::mutex> lk(_panelMutex);
        _panelVisible = false;
        _rowIds.clear();
        _rowDeleteRects.clear();
    }

    // Focus-mode filter rows (world map only).
    if (!isLocalMap && Settings::GetSingleton()->showFilterPanel) {
        DrawFilterPanel();
    }

    // Right-click "Add Marker Here" menu runs on both world and local maps;
    // the placement path differs (engine PlaceMarker vs local-map raycast).
    DrawTerrainMenu(isLocalMap);

    // Right-click menu for our own custom pins (world map path; local map's
    // early-return branch above calls this too).
    DrawCustomContextMenu();

    // Note entry dialog.
    if (_noteDialogOpen) {
        DrawNoteDialog();
    }

    // The cursor is REQUESTED here and drawn at the end of the frame
    // (MapDraw::DrawRequestedCursor, called from ImGui/Renderer.cpp). It must
    // land on the ForegroundDrawList -- on the background list any ImGui window
    // such as the Note Dialog covers it -- and it must be appended AFTER
    // HoverImageRenderer's stat card, which draws to that same shared list after
    // this function returns and used to bury the cursor.
    if (mouse.x >= 0.0f && mouse.y >= 0.0f) {
        MapDraw::RequestCursor(mouse.x, mouse.y);
    }
}

void MapMarkerRenderer::DrawListPanel() {
    ImDrawList* dl       = ImGui::GetBackgroundDrawList();
    ImVec2      dispSize = ImGui::GetIO().DisplaySize;

    // Fetched before the section refreshes below: the list-panel section
    // toggles gate whether those walks run at all.
    auto* settings = Settings::GetSingleton();

    // Rows are what the PLAYER placed: the synthesized vanilla
    // destination must never appear as a removable/renamable row.
    auto allMarkers = MarkerManager::GetSingleton()->GetUserMarkers();

    auto ui = RE::UI::GetSingleton();
    auto mapMenu = ui ? ui->GetMenu<RE::MapMenu>() : nullptr;
    RE::TESWorldSpace* currentMapWorld = mapMenu ? mapMenu->GetRuntimeData2()->worldSpace : nullptr;

    // Map Progress stats (dungeons/locations/stones cleared or discovered,
    // total bounty). Refresh is a no-op unless the worldspace changed since
    // the last call (guard is the first thing it does), so calling it every
    // frame here is cheap. See findings/spec-findings-Map-Stats.md.
    // Hidden section costs nothing: skip the walk, not just the draw.
    MapStatsData mapStats{};
    if (settings->listShowOverview) {
        MapStats::GetSingleton()->Refresh(mapMenu.get(), currentMapWorld);
        mapStats = MapStats::GetSingleton()->Get();
    }

    auto belongsToMap = [&](RE::FormID markerWorldID) -> bool {
        if (markerWorldID == 0) return false;
        if (!currentMapWorld) return true; // fallback if map isn't found
        
        auto* markerWorld = RE::TESForm::LookupByID<RE::TESWorldSpace>(markerWorldID);
        for (auto* w = markerWorld; w; w = w->parentWorld) {
            if (w == currentMapWorld) return true;
        }
        return false;
    };

    // Active destination row (Overview tab). ONE row at most: the two kinds are
    // mutually exclusive by construction in MarkerManager's setters.
    //
    // This was lost in 2fbc0f2, which moved the vanilla destination out of
    // _markers into its own record so no UI could offer Rename/Remove on it.
    // The list panel switched to GetUserMarkers() as part of that and stopped
    // showing the destination at all -- a side effect of removing the buttons,
    // not a decision that the panel shouldn't show where you're headed.
    //
    // Read-only by construction: the vanilla half comes from a COPY of the
    // record, the custom half only reads the flagged marker. Neither path can
    // rename or remove, so 2fbc0f2's constraint still holds.
    struct DestRow {
        bool          active = false;
        std::string   name;
        RE::NiPoint3  position{};
        RE::FormID    worldSpaceID = 0;
        bool          isCustom = false;   // custom marker vs vanilla marker/door
        bool          canPan   = false;   // false for an interior door: no world pos
        std::uint32_t iconType = 0;
    };
    DestRow destRow;
    if (settings->listShowOverview) {
        const auto vd = MarkerManager::GetSingleton()->GetVanillaDestination();
        if (vd.active) {
            destRow.active       = true;
            destRow.name         = vd.name;
            destRow.position     = vd.position;
            destRow.worldSpaceID = vd.worldSpaceID;
            destRow.isCustom     = false;
            destRow.iconType     = vd.fromLocalDoor ? 61u : 0u;
            // A local-map door has no worldspace (interiors carry none), so
            // there is no world position to glide to. Same rule the beacon
            // code uses to decide a destination is unreachable from the map.
            destRow.canPan = (vd.worldSpaceID != 0) && belongsToMap(vd.worldSpaceID);
        } else {
            // Custom marker holding the destination flag. Walked from the same
            // list the Custom tab draws, so the two can never disagree.
            for (const auto& m : allMarkers) {
                if (!m.isDestination) continue;
                destRow.active       = true;
                destRow.name         = ResolveMarkerName(m);
                destRow.position     = m.position;
                destRow.worldSpaceID = m.worldSpaceID;
                destRow.isCustom     = true;
                destRow.canPan       = belongsToMap(m.worldSpaceID);
                destRow.iconType     = m.iconType;
                break;
            }
        }
    }

    // Journal-tracked quests, worldspace-filtered like the marker sections
    // below. A quest whose target sits in another worldspace has no honest
    // place on this map: its position is in the OTHER world's coordinate space,
    // so panning to it lands on unrelated terrain (a Wyrmstooth quest carrying
    // Tamriel coords glides to nowhere on the Wyrmstooth map). A row with no
    // resolved position at all (worldSpaceID 0, e.g. "Find the owner of the
    // Golden Claw", authored with no target) still shows, unclickable — it is
    // tracked everywhere because it points nowhere.
    std::vector<ActiveQuestRow> activeQuests;
    if (settings->listShowActiveQuests) {
        QuestTracker::GetSingleton()->Refresh();
        const bool qlog = settings->questRowLog;
        for (const auto& q : QuestTracker::GetSingleton()->Get()) {
            const bool kept = !q.hasPosition || belongsToMap(q.worldSpaceID);
            if (qlog) {
                const char* kind = q.isMiscellaneous ? "misc" : "regular";
                const std::string& text = q.isMiscellaneous ? q.objectiveText : q.name;
                logger::info("[QuestFilter] {:08X} \"{}\" kind={} instance={} objective={} hasPos={} qworld={:08X} mapworld={:08X} src={} kept={}",
                             q.questID, text, kind, q.instanceID, q.objectiveIndex,
                             q.hasPosition, q.worldSpaceID,
                             currentMapWorld ? currentMapWorld->GetFormID() : 0,
                             q.posSource, kept);
            }
            if (!kept) continue;
            activeQuests.push_back(q);
        }
    }

    // List panel is only shown on the world map — filter out interior markers and markers from other maps.
    std::vector<MapMarker> markers;
    markers.reserve(allMarkers.size());
    for (const auto& m : allMarkers) {
        if (belongsToMap(m.worldSpaceID)) markers.push_back(m);
    }

    auto favsAll = MarkerManager::GetSingleton()->GetFavorites();
    std::vector<FavoriteMarker> favs;
    favs.reserve(favsAll.size());
    for (const auto& f : favsAll) {
        if (belongsToMap(f.worldSpaceID)) favs.push_back(f);
    }

    // Notes section: resolve each vanilla-note FormID live every frame (no
    // cached name/worldspace like the other two sections have). A note
    // outlives the ref only in the save file; if the ref no longer resolves
    // (uninstalled mod, edge case) the row is silently skipped.
    auto notesAll = MarkerManager::GetSingleton()->GetVanillaNotes();
    std::vector<NoteRow> notes;
    notes.reserve(notesAll.size());
    for (const auto& [formID, note] : notesAll) {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);
        if (!ref) {
            logger::debug("[ListPanel] Notes: FormID {:08X} no longer resolves, skipping row", formID);
            continue;
        }
        auto* ws = ref->GetWorldspace();
        if (!belongsToMap(ws ? ws->GetFormID() : 0)) continue;

        std::string name;
        if (auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>()) {
            if (extra->mapData) name = extra->mapData->locationName.GetFullName();
        }
        if (name.empty()) name = ref->GetName();
        if (name.empty()) name = "Marker";

        // Row shows the location name only. The note text itself stays off the
        // panel: long notes overflow the fixed panel width, and the section's
        // job is "where have I annotated", not reading the annotation (hover
        // the marker for that).
        (void)note;
        notes.push_back({ formID, name, ref->GetPosition() });
    }

    const float baseFS    = ImGui::GetFontSize();
    // Section headers use the heading font when the INI loads one
    // (HeadingFontSize > FontSize); TitleScale multiplies on top.
    ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
    if (!titleFont) titleFont = ImGui::GetFont();
    const float titleFS   = titleFont->FontSize * std::clamp(settings->titleScale, 0.5f, 3.0f);
    const float bodyFS    = baseFS * std::clamp(settings->bodyScale,  0.5f, 3.0f);
    const float pad       = 10.0f;
    // Row/header advances are SNAPPED to whole pixels. Both derive from the
    // font-scale sliders, so unsnapped they carry a fraction that compounds down
    // the panel: each row starts at a different subpixel offset, the glyph atlas
    // resamples differently per row, and rows of the same string end up looking
    // unevenly weighted (the shadow copies stack on some rows and straddle two
    // pixels on others). Snapping the ADVANCE keeps every row on an integer y as
    // long as the panel's own origin is integral, which the drag-persist code
    // stores as ints. DrawTextShadowed snaps its own draw position too, so a
    // fractional caller can't reintroduce it.
    // +5 over the old bodyFS + 6: marker rows now draw a category icon, and at the
    // tighter advance the art nearly touched the row above/below.
    const float rowHeight = std::floor(bodyFS + 11.0f);
    const float headerH   = std::floor(titleFS + 12.0f);
    // Drag-grip strip above the tabs. Replaced a full "Map Markers" title line:
    // the tabs already name what the panel holds, so the title was a generic
    // label eating titleFS + 12 of vertical space. The strip stays because it is
    // the DRAG HANDLE -- the tab strip below it consumes clicks for tab
    // switching, so deleting the band outright would leave the panel unmovable.
    // Fixed height, not font-relative: it carries no text, and a grip that grew
    // with the title-size slider would just be a thicker bar.
    //
    // 18 = 12 for the dots plus 6 of clear space beneath them, so the grip does
    // not sit flush on the tab strip. The gap is folded into kGripH rather than
    // added at the tab strip because kGripH IS the drag band: splitting them
    // would make the 6px read as draggable-looking but inert.
    constexpr float kGripH = 18.0f;
    // Row band offset. Every row draws its hover band from y - kBandLift to
    // y + rowHeight - kBandLift, so the band's centre is NOT y + bodyFS * 0.5.
    // Text drawn at plain y therefore sat high by a constant ~2.5px, which the
    // delete button never showed because it derives its own top/bottom from the
    // band. Constant, not proportional, so raising the body scale did not hide
    // it -- the text just got bigger and stayed off-centre.
    //
    // RowTextY gives the top-left y that centres a glyph run of height a_fs
    // inside the band, so text and icons sit where the button already does.
    constexpr float kBandLift = 3.0f;
    const auto RowTextY = [rowHeight](float a_y, float a_fs) {
        return std::floor(a_y - kBandLift + (rowHeight - a_fs) * 0.5f);
    };
    const float panelW    = static_cast<float>(settings->listPanelWidth);

    // Section headers. GetLabel returns the key itself when the translation
    // file doesn't have it yet, so fall back to the English string. Cached
    // once per frame here rather than per-row.
    auto* loc = LocalizationManager::GetSingleton();
    std::string customHeader = loc->GetLabel("ListPanel.CustomMarkers");
    if (customHeader == "ListPanel.CustomMarkers") customHeader = "Custom Markers";
    std::string favHeader = loc->GetLabel("ListPanel.Favorites");
    if (favHeader == "ListPanel.Favorites") favHeader = "Favorites";
    std::string notesHeader = loc->GetLabel("ListPanel.Notes");
    if (notesHeader == "ListPanel.Notes") notesHeader = "Notes";
    std::string statsHeader = loc->GetLabel("ListPanel.Overview");
    if (statsHeader == "ListPanel.Overview") statsHeader = "Overview";
    std::string destHeader = loc->GetLabel("ListPanel.Destination");
    if (destHeader == "ListPanel.Destination") destHeader = "Destination";
    std::string questHeader = loc->GetLabel("ListPanel.ActiveQuests");
    std::string miscHeader = loc->GetLabel("ListPanel.Miscellaneous");
    if (miscHeader == "ListPanel.Miscellaneous") miscHeader = "Miscellaneous";

    // Map Progress stat rows. All rows always show; bounty at 0 is meaningful
    // ("you owe nothing" is a completionist fact, same as 0 / N). Bounty is
    // save-wide Skyrim crime gold, NOT worldspace-scoped, so it reads the same
    // on Solstheim (no holds there) as on Tamriel.
    std::vector<std::string> statRows;
    if (settings->listShowOverview) {
    statRows.reserve(4);
    statRows.push_back(LocalizationManager::Format("Overview.Dungeons", mapStats.dungeonsCleared, mapStats.dungeonsTotal));
    statRows.push_back(LocalizationManager::Format("Overview.Locations", mapStats.locationsDiscovered, mapStats.locationsTotal));
    statRows.push_back(LocalizationManager::Format("Overview.StandingStones", mapStats.stonesFound, mapStats.stonesTotal));
    statRows.push_back(LocalizationManager::Format("Overview.TotalBounty", mapStats.totalBounty));
    }

    // Height calculation: preserve every regular row height. Add one rowHeight for
    // the synthetic header when at least one miscellaneous row exists. Each misc
    // objective is a single primary-text row using q.objectiveText.
    const bool hasMisc = std::ranges::any_of(activeQuests,
        [](const ActiveQuestRow& q) { return q.isMiscellaneous; });
    float questSectionH = 0.0f;
    if (hasMisc) {
        questSectionH += rowHeight;
    }
    for (const auto& q : activeQuests) {
        questSectionH += q.isMiscellaneous
            ? rowHeight
            : (q.objectiveText.empty() ? rowHeight : std::floor(rowHeight * 1.8f));
    }

    // Active tab. Clamped every frame rather than trusted: the INI is
    // hand-editable and a stale index from a build with more tabs would index
    // past the tab table below.
    const int tabCount = static_cast<int>(ListTab::Count);
    settings->listActiveTab = std::clamp(settings->listActiveTab, 0, tabCount - 1);
    const ListTab activeTab = static_cast<ListTab>(settings->listActiveTab);
    float& scrollY = _listScrollY[settings->listActiveTab];

    // Tab strip sits between the drag/title strip and the rows. Its labels
    // carry the row count, which is the whole reason the old collapsed-header
    // count readout is not missed: every section's size is visible at once
    // without opening it.
    struct TabDef {
        ListTab     tab;
        std::string label;
        std::size_t count;
    };
    std::vector<TabDef> tabs;
    tabs.reserve(static_cast<std::size_t>(tabCount));
    // Overview's count is the quest count, not the stat-row count: the stat
    // rows are a fixed set, so "4" would say nothing.
    tabs.push_back({ ListTab::Overview,  statsHeader,  activeQuests.size() });
    tabs.push_back({ ListTab::Custom,    customHeader, markers.size() });
    tabs.push_back({ ListTab::Favorites, favHeader,    favs.size() });
    tabs.push_back({ ListTab::Notes,     notesHeader,  notes.size() });

    // Tab strip height tracks the body font so the strip grows with the
    // text-size sliders like every other band in the panel.
    const float tabStripH = std::floor(bodyFS + 14.0f);

    // Content height = ONLY the active tab's rows. That is the fix for the
    // full-height scroll: with one section on screen the scroll range can
    // never exceed one list, however many favorites or notes are stored.
    float contentH = 0.0f;
    switch (activeTab) {
    case ListTab::Overview:
        if (settings->listShowOverview) {
            contentH += rowHeight * static_cast<float>(statRows.size());
            // Destination sub-heading + separator gap + its single row. The row
            // is counted whether or not a destination is set: the placeholder
            // takes the same height, which is what stops the Overview tab
            // reflowing every time one is set or cleared.
            contentH += pad + headerH + pad + rowHeight;
        }
        if (settings->listShowActiveQuests) {
            // Leading gap + the quest sub-heading + the gap under its separator,
            // only when the stat block above it is actually drawn (it is what the
            // heading separates from). Must match the draw pass's y advance
            // exactly, or the tab scrolls short by the difference.
            if (settings->listShowOverview) contentH += pad + headerH + pad;
            contentH += activeQuests.empty() ? rowHeight : questSectionH;
        }
        // Both sub-blocks off: the tab still needs a body for its placeholder row.
        if (!settings->listShowOverview && !settings->listShowActiveQuests) {
            contentH += rowHeight;
        }
        break;
    case ListTab::Custom:
        contentH += headerH + pad;
        contentH += markers.empty() ? rowHeight
                                    : (rowHeight * static_cast<float>(markers.size()));
        break;
    case ListTab::Favorites:
        contentH += favs.empty() ? rowHeight
                                 : (rowHeight * static_cast<float>(favs.size()));
        break;
    case ListTab::Notes:
        contentH += notes.empty() ? rowHeight
                                  : (rowHeight * static_cast<float>(notes.size()));
        break;
    default:
        break;
    }
    contentH += pad * 2.0f;

    // Chrome (drag grip + tab strip) is OUTSIDE the scrolling body: it is
    // fixed, so it must not enter contentH or the scroll range would include
    // height that never moves.
    const float chromeH = kGripH + tabStripH;

    const float maxH = dispSize.y *
        (std::clamp(settings->listPanelMaxHeightPct, 30, 95) / 100.0f);
    // Min is clamped against the CURRENT max, not its own 0..95 range, so a Min
    // slider dragged above Max cannot invert the band -- the two sliders are
    // independent and nothing else stops the user crossing them.
    const float minH = dispSize.y *
        (std::clamp(settings->listPanelMinHeightPct, 0,
                    std::clamp(settings->listPanelMaxHeightPct, 30, 95)) / 100.0f);

    const float bodyMaxH = std::max(rowHeight * 3.0f, maxH - chromeH);
    // Floor the BODY, not the panel: chrome is fixed, so flooring the panel
    // would let a tall chrome eat the whole minimum and still leave a one-row
    // body. Without this a tab holding a single row shrank the panel to that
    // row, and switching tabs made it jump on every click.
    const float bodyMinH = std::clamp(minH - chromeH, 0.0f, bodyMaxH);
    const float bodyH    = std::clamp(contentH, bodyMinH, bodyMaxH);
    const float panelH   = chromeH + bodyH;

    // Engine menu cursor. Needed before positioning so the header-strip drag
    // can read it. Same projection used below for row hover hit-testing.
    ImVec2 mouse{ -1.0f, -1.0f };
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        const float cw = cursor->screenWidthX;
        const float ch = cursor->screenWidthY;
        if (cw > 0.0f && ch > 0.0f) {
            mouse = ImVec2((cursor->cursorPosX / cw) * dispSize.x,
                           (cursor->cursorPosY / ch) * dispSize.y);
        }
    }

    // Resolve top-left position. Saved pos wins; -1 means unset, so fall back
    // to the old top-right anchor and seed the saved pos from it so the first
    // drag has a sane starting point.
    const float margin = 24.0f;
    if (settings->listPanelPosX < 0 || settings->listPanelPosY < 0) {
        settings->listPanelPosX = static_cast<std::int32_t>(dispSize.x - panelW - margin);
        settings->listPanelPosY = static_cast<std::int32_t>(margin);
    }

    ImVec2 mn(static_cast<float>(settings->listPanelPosX),
              static_cast<float>(settings->listPanelPosY));
    ImVec2 mx(mn.x + panelW, mn.y + panelH);

    // Recompute scroll ceiling BEFORE clamping scroll offset. Measured against
    // the BODY height, not the panel: the title and tab strips are fixed chrome
    // and never scroll.
    _listScrollMax = std::max(0.0f, contentH - bodyH);
    const bool cursorInPanel = (mouse.x >= mn.x && mouse.x <= mx.x &&
                                mouse.y >= mn.y && mouse.y <= mx.y);
    if (_listScrollMax > 0.0f && cursorInPanel) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            scrollY -= wheel * rowHeight * 3.0f;
        }
    }
    scrollY = std::clamp(scrollY, 0.0f, _listScrollMax);

    // Grip strip is the band above the tabs. Dragging it moves the panel;
    // tabs, rows and delete buttons stay clickable.
    const float headerStripBottom = mn.y + kGripH;
    const bool  leftDown = (mouse.x >= 0.0f) &&
                           (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool  leftPressed = (mouse.x >= 0.0f) && leftDown && !_panelPrevLeftDown;

    const bool overHeader =
        mouse.x >= mn.x && mouse.x <= mx.x &&
        mouse.y >= mn.y && mouse.y <= headerStripBottom;

    if (!_draggingPanel && leftPressed && overHeader) {
        _draggingPanel = true;
        _panelGrabDX = mouse.x - mn.x;
        _panelGrabDY = mouse.y - mn.y;
    }
    if (_draggingPanel) {
        if (leftDown) {
            settings->listPanelPosX = static_cast<std::int32_t>(mouse.x - _panelGrabDX);
            settings->listPanelPosY = static_cast<std::int32_t>(mouse.y - _panelGrabDY);
            mn.x = static_cast<float>(settings->listPanelPosX);
            mn.y = static_cast<float>(settings->listPanelPosY);
            mx.x = mn.x + panelW;
            mx.y = mn.y + panelH;
        } else {
            _draggingPanel = false;
            settings->SavePositions();
        }
    }
    _panelPrevLeftDown = leftDown;

    // Panel background: a user image when one is picked, otherwise the solid
    // Appearance color. The image REPLACES both the fill and the border —
    // drawing either behind or around custom art fights whatever frame the art
    // already has, and a border is the most obvious thing to clash.
    ID3D11ShaderResourceView* panelBg = nullptr;
    if (settings->listPanelUseCustomBg) {
        panelBg = BackgroundManager::GetBackground(BackgroundManager::Type::ListPanel,
                                                   settings->listPanelBgIndex);
    }

    // The background bleeds OUTWARD past the panel rect so a frame or border
    // baked into custom art sits outside the text rather than on top of it.
    // BOTH paths use it: the solid fill used to stop exactly at mn/mx, so
    // toggling a custom background visibly changed the apparent outer padding
    // (26px of visual edge-to-text with art, 12px without) even though the
    // content never moved. Only this rect grows -- mn/mx still drive every row
    // position and the hit test below, so nothing gets compressed either way.
    constexpr float kBgBleed = 14.0f;
    const ImVec2 bgMin(mn.x - kBgBleed, mn.y - kBgBleed);
    const ImVec2 bgMax(mx.x + kBgBleed, mx.y + kBgBleed);

    if (panelBg) {
        // Stretched to that rect, which grows with the marker count, so art
        // should be authored tall (portrait) rather than square.
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(panelBg), bgMin, bgMax,
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, MapTheme::kPanelRounding);
    } else {
        // Unified panel background + border (Settings > Appearance).
        MapDraw::DrawPanelCard(dl, bgMin, bgMax, settings->BgCol(), settings->BorderCol());
    }

    // ------------------------------------------------------------------------
    // Title strip + tab strip. Both are FIXED chrome drawn outside the body
    // clip: the tab strip must stay reachable no matter how far the rows have
    // scrolled, which is exactly what the old parked-header machinery was
    // trying to fake with one sticky header at a time.
    //
    // Tabs replaced per-section collapsing headers. With one section on screen
    // the scroll range is bounded by a single list, so a long Favorites list
    // can no longer push Custom Markers off the panel (and vice versa) -- the
    // problem the collapse flags never actually solved.
    // ------------------------------------------------------------------------
    // Grip: two dotted rows spanning the panel, the conventional "drag me"
    // affordance. Dotted rather than solid because two solid full-width lines
    // read as a separator or a border, not as a handle. Drawn rather than
    // textured so it follows the theme colour and scales with the panel width
    // for free. Brighter while actually dragging, so the panel confirms the
    // grab even when the cursor leaves the strip mid-drag.
    //
    // The dots are hand-stepped rects: ImDrawList has no dash/stipple support,
    // and AddLine takes one colour and one solid run per call.
    {
        // Centred on the dot band (the first 12 of kGripH), NOT on kGripH: the
        // remaining 6 is deliberate clearance above the tabs, so centring on the
        // full height would push the dots down into it and close the gap again.
        constexpr float kDotBandH = 12.0f;
        const float gripMidY = mn.y + std::floor(kDotBandH * 0.5f);
        // Full width, inset by pad so the dots stop clear of the panel border.
        // A short centred grip read as a tiny target; the whole strip is the
        // drag handle, so the affordance should say so.
        const float gripX0   = mn.x + pad;
        const float gripX1   = mx.x - pad;
        const ImU32 gripCol  = _draggingPanel ? settings->TitleCol() : MapTheme::kSeparator;

        constexpr float kDotSz   = 1.0f;   // square dot
        constexpr float kDotStep = 4.0f;   // dot pitch along the row
        constexpr float kRowGap  = 4.0f;   // vertical gap between the two rows
        for (int r = 0; r < 2; ++r) {
            const float ly = std::floor(gripMidY + (static_cast<float>(r) - 0.5f) * kRowGap);
            // Offset every other row by half a step so the two read as a
            // texture rather than a grid.
            const float x0 = gripX0 + static_cast<float>(r) * kDotStep * 0.5f;
            for (float x = x0; x <= gripX1 - kDotSz; x += kDotStep) {
                dl->AddRectFilled(ImVec2(x, ly), ImVec2(x + kDotSz, ly + kDotSz), gripCol);
            }
        }
    }

    // Row band inset. 1.0f, NOT a text pad: this is the hover/selection BAND,
    // and it is aligned to the tab strip's tray, which spans mn.x + 1 .. mx.x - 1.
    // At the old 12.0f the band was inset 11px inside the tray, so hovering a row
    // showed a band visibly narrower than the active tab cell directly above it
    // -- the invisible padding the panel appeared to have below the tabs. The
    // band is the only thing that moves here; row text keeps its absolute x via
    // kRowTextInset below, so nothing reflows and no name wraps sooner.
    constexpr float kRowPadX = 1.0f;
    const float rowsLeft  = mn.x + kRowPadX;
    const float rowsRight = mx.x - kRowPadX;
    const bool  cursorInRowsX = mouse.x >= rowsLeft && mouse.x <= rowsRight;

    // Left inset for row CONTENT (icon, name) INSIDE the band, and via
    // rowContentRight the mirror on the right. Absolute rather than derived from
    // rowsLeft: the band is aligned to the tab tray at mn.x + 1, and following it
    // would put the first glyph flush against the band edge, reading as clipped.
    //
    // Headers, separators and the unbanded stat rows use it too. They used to sit
    // at mn.x + pad, 8px LEFT of the banded row text, so the Overview tab drew
    // its stat lines and its Destination row on two different columns.
    //
    // 10, down from 18: at 18 the content column sat well right of the tab
    // labels above it and the delete button was pulled deep inside the band.
    // This is the ONLY knob for the panel's inner text margin -- both edges move
    // together, so a name truncates and the X shifts in step. Do not raise it to
    // fix a wrap; that is what the listPanelWidth slider is for.
    constexpr float kRowTextInset = 10.0f;
    const float rowContentX = mn.x + kRowTextInset;

    // Tab strip. Equal-width cells across the panel: the labels are localized
    // and vary in length, so measuring per label would make the strip reflow
    // between languages. The click is hit-tested off MenuCursor like every
    // other row here, and swallowed by the existing panel-rect check in
    // TryHandlePanelClick -- the strip is inside mn..mx, so no new swallow
    // rect is needed.
    const float tabTop    = mn.y + kGripH;
    const float tabBottom = tabTop + tabStripH;
    const float tabW      = (mx.x - mn.x) / static_cast<float>(tabs.size());

    // Tray behind the whole strip. Drawn BEFORE the cells so the active fill and
    // hover tint composite on top of it. This is what makes the row read as a
    // control surface rather than a line of text on the panel background --
    // painting only the selected cell left the other three as bare captions.
    // Inset 1px from the panel edges so it never sits on the panel border.
    dl->AddRectFilled(ImVec2(mn.x + 1.0f, tabTop),
                      ImVec2(mx.x - 1.0f, tabBottom),
                      MapTheme::kTabTrayBg);

    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const float tx0 = mn.x + tabW * static_cast<float>(i);
        const float tx1 = tx0 + tabW;
        const bool  isActive  = (tabs[i].tab == activeTab);
        const bool  tabHover  = mouse.x >= tx0 && mouse.x <= tx1 &&
                                mouse.y >= tabTop && mouse.y <= tabBottom;

        // Drag suppresses the switch: the title strip above starts the drag and
        // the cursor can slide onto the strip mid-drag.
        if (tabHover && leftPressed && !_draggingPanel) {
            settings->listActiveTab = static_cast<std::int32_t>(tabs[i].tab);
            settings->SavePositions();
        }

        if (isActive) {
            dl->AddRectFilled(ImVec2(tx0, tabTop), ImVec2(tx1, tabBottom),
                              MapTheme::kTabActiveBg);
            // Underline is the primary active marking. The fill alone is close
            // enough to the hover tint to be missed, which is exactly how the
            // strip read before the tray existed.
            dl->AddLine(ImVec2(tx0 + 4.0f, tabBottom - 1.0f),
                        ImVec2(tx1 - 4.0f, tabBottom - 1.0f),
                        settings->TitleCol(), 2.0f);
        } else if (tabHover) {
            // Deliberately weaker than kTabActiveBg. Both used kRowHoverBg
            // before, so hovering an inactive tab looked like selecting it.
            dl->AddRectFilled(ImVec2(tx0, tabTop), ImVec2(tx1, tabBottom),
                              MapTheme::kTabHoverBg);
        }

        std::string label = tabs[i].label;
        // Row count in the label, PARKED. It carried over from the collapsed
        // headers, where it told you what was hidden behind a folded section --
        // with tabs nothing is hidden, so on a normal save it just prints " 1"
        // after every name and reads as part of the title. TabDef still carries
        // the count so this can come back (a badge, or only when > 1) once the
        // tab strip moves to icons + short titles.
        // if (tabs[i].count > 0) {
        //     label += " ";
        //     label += std::to_string(tabs[i].count);
        // }
        // Labels are localized and the panel width is a slider, so a long label
        // in a narrow panel WILL overrun its cell. Shrink the text to fit rather
        // than clipping mid-word: a truncated tab name is unreadable at 4 cells.
        const float availW = tabW - 8.0f;
        float tabFS = bodyFS * 0.92f;
        ImVec2 lblSz = ImGui::CalcTextSize(label.c_str());
        float  lblW  = lblSz.x * (tabFS / baseFS);
        if (lblW > availW && lblW > 0.0f) {
            tabFS *= availW / lblW;
            lblW = availW;
        }
        // Inactive tabs belong to the selected theme too. The old kMutedText
        // gold fought light and recolored panels; 70% title alpha keeps the
        // hierarchy without introducing a fixed hue. Hover restores full alpha.
        const ImU32 tabCol = (isActive || tabHover)
                                 ? settings->TitleCol()
                                 : MapDraw::WithAlpha(settings->TitleCol(), 0.70f);
        MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), tabFS,
                    ImVec2(tx0 + (tabW - lblW) * 0.5f,
                           tabTop + (tabStripH - tabFS) * 0.5f),
                    tabCol, label.c_str());
    }

    // Dividers, in their own pass AFTER every cell is filled. Drawn inside the
    // cell loop they would be painted over by the next cell's active/hover
    // fill, so a divider would vanish exactly when its neighbour lit up.
    // Leading edge only, skipped for i == 0: neighbours SHARE one line, which
    // is what stops adjacent borders doubling into a 2px seam, and nothing is
    // drawn on the strip's outer ends.
    for (std::size_t i = 1; i < tabs.size(); ++i) {
        const float dx = mn.x + tabW * static_cast<float>(i);
        dl->AddLine(ImVec2(dx, tabTop + 3.0f), ImVec2(dx, tabBottom - 3.0f),
                    MapTheme::kTabDivider, 1.0f);
    }

    // Right-hand mirror of kRowTextInset. Row CONTENT stops here, not at
    // rowsRight: the band was widened out to the tab tray, and following it
    // would shove the delete button and every truncated label hard against the
    // panel edge. mn.x + panelW - 18 keeps both at the inset they had before
    // the band moved.
    const float rowContentRight = mx.x - kRowTextInset;

    // Delete button tracks the body font so the X and its box grow together.
    const float delBtnW   = std::max(20.0f, bodyFS + 6.0f);
    const float delBtnPad = 4.0f;
    const float delBtnX0  = rowContentRight - delBtnW - delBtnPad;
    const float delBtnX1  = rowContentRight - delBtnPad;

    // Breathing room between a truncated label and whatever bounds it on the
    // right (the delete button, or the panel's row edge). Without it a name cut
    // to the exact budget ends flush against the X and reads as collided.
    constexpr float kRowTextSlack = 6.0f;
    // Right edge available to a row label. Rows WITH a delete button stop short
    // of it; rows without run to the row edge.
    const float textRightWithBtn = delBtnX0 - kRowTextSlack;
    const float textRightNoBtn   = rowContentRight - kRowTextSlack;

    // Index gutter for the Favorites and Notes tabs. Those rows are bare text:
    // the Markers tab fills the same column with a category icon, and without
    // one the rows read as floating with dead space on the left. A number gives
    // the column a rail to sit on.
    //
    // The index is NOT stored anywhere. It is the draw loop's position in the
    // vector, recomputed every frame, so deleting a row from the middle
    // renumbers the rest on the next frame with no reindex pass and nothing in
    // the cosave to fall out of sync.
    //
    // Zero-padded to two digits so every row is the same width through 99 and
    // the numbers form a straight edge; measured off "00" rather than the real
    // max index for the same reason. A list past 99 still measures correctly,
    // it just gets a wider gutter. Right-aligned: left-aligning would start
    // different digit counts at different x, which is the exact raggedness the
    // padding is there to avoid.
    const float idxGutterW = ImGui::CalcTextSize("00").x * (bodyFS / baseFS);
    // Gap between the number and the name. Scaled off the body font, not a
    // constant, so the rail keeps its proportions at every text-size setting.
    const float idxGapW    = std::floor(bodyFS * 0.5f);
    const float idxTextX   = rowContentX + idxGutterW + idxGapW;

    // Right-aligns one index inside the gutter and returns its draw x.
    const auto IndexX = [rowContentX, idxGutterW, baseFS, bodyFS](const char* a_txt) {
        const float w = ImGui::CalcTextSize(a_txt).x * (bodyFS / baseFS);
        return rowContentX + (idxGutterW - w);
    };

    std::vector<RE::FormID>     questIds;
    std::vector<Rect2D>         questRects;
    std::vector<RE::NiPoint3>   questPositions;

    std::vector<std::uint32_t>  rowIds;
    std::vector<Rect2D>         rowRects;
    std::vector<Rect2D>         rowDeleteRects;
    rowIds.reserve(markers.size());
    rowRects.reserve(markers.size());
    rowDeleteRects.reserve(markers.size());

    std::vector<RE::FormID> favIds;
    std::vector<Rect2D>     favRects;
    std::vector<Rect2D>     favDeleteRects;
    favIds.reserve(favs.size());
    favRects.reserve(favs.size());
    favDeleteRects.reserve(favs.size());

    std::vector<RE::FormID> noteIds;
    std::vector<Rect2D>     noteRects;
    std::vector<Rect2D>     noteDeleteRects;
    noteIds.reserve(notes.size());
    noteRects.reserve(notes.size());
    noteDeleteRects.reserve(notes.size());

    // Body clip starts BELOW the chrome, so a scrolled row slides under the tab
    // strip instead of over it. This replaces the old rowsTop floor: the clip
    // rect does the hiding, and rows are laid out from bodyTop so nothing can
    // be positioned inside the chrome band in the first place.
    const ImVec2 bodyMin(mn.x, tabBottom);
    const ImVec2 bodyMax(mx.x, mx.y);
    dl->PushClipRect(bodyMin, bodyMax, true);

    // Row visibility is tested against the body band, not the panel: a row
    // scrolled under the tab strip is invisible, and its rect must not be
    // published or it would keep eating clicks aimed at a tab.
    const float rowsTop = bodyMin.y;

    float y = bodyMin.y + pad - scrollY;

    // ------------------------------------------------------------------------
    // Map Progress Section (worldspace-scoped completionist stats). Drawn
    // above the marker sections so it reads as the panel's summary line, not
    // buried under the lists. See findings/spec-findings-Map-Stats.md.
    // ------------------------------------------------------------------------
    if (activeTab == ListTab::Overview && settings->listShowOverview) {
        for (const auto& row : statRows) {
            const bool rowVisible = (y + rowHeight > rowsTop) && (y < mx.y);
            if (rowVisible) {
                // Same baseline as the banded rows below. These draw no hover
                // band of their own, but sitting 2.5px higher than the
                // Destination row under them made the Overview column read
                // unevenly.
                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                            ImVec2(rowContentX, RowTextY(y, bodyFS)),
                            settings->BodyCol(), row.c_str());
            }
            y += rowHeight;
        }
    }

    // ------------------------------------------------------------------------
    // Destination Section (Overview tab). At most one row: a vanilla marker /
    // local-map door, or a custom marker carrying isDestination. Read-only --
    // no Remove, no Rename. See the DestRow resolve above for why.
    //
    // A CUSTOM destination shows here AND keeps its row in the Custom tab.
    // That is intentional, not duplication: "where am I headed" and "markers I
    // placed" are different questions, and the Custom row is the one that owns
    // the destination.png icon and the right-click menu.
    // ------------------------------------------------------------------------
    // Locals, published under _panelMutex with the other hit rects at the end:
    // written on the render thread, read on the game thread by
    // TryHandlePanelClick.
    bool         destRowValid = false;
    Rect2D       destRowRect{};
    RE::NiPoint3 destRowPos{};
    if (activeTab == ListTab::Overview && settings->listShowOverview) {
        y += pad;
        if ((y + headerH > rowsTop) && (y < mx.y)) {
            MapDraw::DrawTextShadowed(dl, titleFont, titleFS,
                        ImVec2(rowContentX, y),
                        settings->TitleCol(), destHeader.c_str());
            const float sepLineY = y + headerH;
            dl->AddLine(ImVec2(rowContentX, sepLineY), ImVec2(rowContentRight, sepLineY),
                        MapTheme::kSeparator, 1.0f);
        }
        y += headerH + pad;

        const float bandTop    = y - 3.0f;
        const float bandBottom = y + rowHeight - 3.0f;
        const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
        if (rowVisible) {
            if (!destRow.active) {
                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                            ImVec2(rowContentX, RowTextY(y, bodyFS)),
                            MapTheme::kMutedText,
                            LocalizationManager::Get("ListPanel.NoDestination").c_str());
            } else {
                // Blue-tinted background so destination is clearly identified, brighter on hover
                const bool rowHovered = destRow.canPan && cursorInRowsX &&
                                        mouse.y >= bandTop && mouse.y <= bandBottom;
                const ImU32 bgFill = rowHovered ? MapTheme::kDestRowBgHover : MapTheme::kDestRowBg;
                dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                  ImVec2(rowsRight, bandBottom),
                                  bgFill, 2.0f);

                // Use the marker's own icon on the left
                const float iconSz = std::floor(bodyFS * 0.9f);
                ID3D11ShaderResourceView* rowSrv = nullptr;
                if (destRow.isCustom) {
                    rowSrv = TextureManager::GetCategoryIcon(destRow.iconType);
                } else {
                    rowSrv = destRow.iconType == 61u ? TextureManager::GetIconByName("door")
                                                     : TextureManager::GetIconByName("destination");
                }
                MapDraw::DrawPin(dl, rowContentX + iconSz * 0.5f,
                                 RowTextY(y, iconSz) + iconSz * 0.5f,
                                 0.0f, destRow.iconType, rowSrv,
                                 iconSz / (2.0f * MapDraw::kPinRadius),
                                 settings->PinTintCol());

                // Muted destination badge on the right (+4px over old 15px)
                const float badgeSz = std::floor(bodyFS * 1.05f);
                const float badgeX1 = rowContentRight - delBtnPad;
                const float badgeX0 = badgeX1 - badgeSz;
                const float badgeY  = RowTextY(y, badgeSz);
                if (auto* destIcon = TextureManager::GetIconByName("destination")) {
                    dl->AddImage(reinterpret_cast<ImTextureID>(destIcon),
                                 ImVec2(badgeX0, badgeY), ImVec2(badgeX1, badgeY + badgeSz),
                                 ImVec2(0, 0), ImVec2(1, 1), MapTheme::kDestBadgeTint);
                }

                const float nameX = rowContentX + iconSz + std::floor(bodyFS * 0.55f);
                const ImU32 textCol = rowHovered ? settings->BodyColHover() : settings->BodyCol();
                const float textRightEdge = badgeX0 - kRowTextSlack;
                const std::string rowName =
                    TruncateRowTextToWidth(destRow.name, textRightEdge - nameX, bodyFS);
                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                            ImVec2(nameX, RowTextY(y, bodyFS)), textCol, rowName.c_str());

                if (destRow.canPan) {
                    destRowValid = true;
                    destRowRect  = { rowsLeft, bandTop, rowsRight, bandBottom };
                    destRowPos   = destRow.position;
                }
            }
        }
        y += rowHeight;
    }

    // ------------------------------------------------------------------------
    // Active Quests Section (journal-tracked quests). Rows are name + current
    // objective; no delete button, a tracked quest is not ours to remove.
    // See findings/spec-findings-Active-Quests-Section.md.
    // ------------------------------------------------------------------------
    if (activeTab == ListTab::Overview && settings->listShowActiveQuests) {
        {
            // Gap between the stat block and the quest rows. Only when the stat
            // block actually drew, or the tab would open with dead space.
            // The Overview tab is the one tab carrying TWO blocks, so the quest
            // half gets an in-flow sub-heading. It scrolls with the rows -- it
            // is not a pinned header, and there is no collapse here: the tab
            // itself is the show/hide.
            if (settings->listShowOverview) {
                y += pad;
                if ((y + headerH > rowsTop) && (y < mx.y)) {
                    MapDraw::DrawTextShadowed(dl, titleFont, titleFS,
                                ImVec2(rowContentX, y),
                                settings->TitleCol(), questHeader.c_str());
                    // Separator under the heading, same geometry the old
                    // per-section headers used (baseline at headerTop +
                    // headerH). It is what gave the panel its banded look, and
                    // dropping the header flush pass took it with it.
                    const float sepLineY = y + headerH;
                    dl->AddLine(ImVec2(rowContentX, sepLineY),
                                ImVec2(rowContentRight, sepLineY),
                                MapTheme::kSeparator, 1.0f);
                }
                // headerH clears the heading, pad clears the separator. The old
                // drawHeader ended on `y = sepLineY + pad` for the same reason:
                // without it the first row's hover band sits ON the line.
                y += headerH + pad;
            }
            if (activeQuests.empty()) {
                const bool rowVisible = (y + rowHeight > rowsTop) && (y < mx.y);
                if (rowVisible) {
                    MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                ImVec2(rowContentX, RowTextY(y, bodyFS)),
                                MapTheme::kMutedText, LocalizationManager::Get("ListPanel.NoTrackedQuests").c_str());
                }
                y += rowHeight;
            } else {
                bool drewMiscHeader = false;
                for (const auto& q : activeQuests) {
                    if (q.isMiscellaneous && !drewMiscHeader) {
                        // Draw one body-scale subgroup label, then advance by rowHeight.
                        // It has no hover band and contributes no click rect.
                        const bool headerVisible = (y + rowHeight > rowsTop) && (y < mx.y);
                        if (headerVisible) {
                            MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                                      ImVec2(rowContentX, RowTextY(y, bodyFS)),
                                                      MapDraw::WithAlpha(settings->BodyCol(), 0.75f), miscHeader.c_str());
                        }
                        y += rowHeight;
                        drewMiscHeader = true;
                    }

                    const float rowH       = q.isMiscellaneous
                        ? rowHeight
                        : (q.objectiveText.empty() ? rowHeight : std::floor(rowHeight * 1.8f));
                    const float bandTop    = y - 3.0f;
                    const float bandBottom = y + rowH - 3.0f;
                    const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
                    if (rowVisible) {
                        // Only rows with a live marker highlight: a row with no position has
                        // nowhere to pan, so a hover affordance there would promise a click
                        // that does nothing.
                        const bool rowHovered = q.hasPosition && cursorInRowsX &&
                                                mouse.y >= bandTop && mouse.y <= bandBottom;
                        if (rowHovered) {
                            dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                              ImVec2(rowsRight, bandBottom),
                                              MapTheme::kRowHoverBg, 2.0f);
                        }

                        const ImU32 textCol = rowHovered ? settings->BodyColHover() : settings->BodyCol();
                        const std::string& primary = q.isMiscellaneous ? q.objectiveText : q.name;
                        const std::string primaryTrunc = TruncateRowTextToWidth(
                            primary, textRightNoBtn - rowContentX, bodyFS);

                        if (q.isMiscellaneous) {
                            MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                        ImVec2(rowContentX, RowTextY(y, bodyFS)), textCol, primaryTrunc.c_str());
                        } else {
                            MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                        ImVec2(rowContentX, y), textCol, primaryTrunc.c_str());

                            if (!q.objectiveText.empty()) {
                                const float objX = rowContentX + 10.0f;
                                const std::string obj = TruncateRowTextToWidth(
                                    q.objectiveText, textRightNoBtn - objX, bodyFS * 0.85f);
                                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS * 0.85f,
                                            ImVec2(objX, y + rowHeight * 0.9f),
                                            MapDraw::WithAlpha(settings->BodyCol(), 0.70f), obj.c_str());
                            }
                        }

                        if (q.hasPosition) {
                            // All three push together, in the same branch: a parallel array
                            // filled anywhere else desyncs the moment one row is skipped.
                            questIds.push_back(q.questID);
                            questRects.push_back({ rowsLeft, bandTop, rowsRight, bandBottom });
                            questPositions.push_back(q.position);
                        }
                    }
                    y += rowH;
                }
            }
        }
    }

    // Both Overview sub-blocks off: the tab would otherwise be an empty box with
    // no explanation of why. The toggles live in Settings > Map > List Panel.
    if (activeTab == ListTab::Overview && !settings->listShowOverview &&
        !settings->listShowActiveQuests) {
        const bool rowVisible = (y + rowHeight > rowsTop) && (y < mx.y);
        if (rowVisible) {
            MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                        ImVec2(rowContentX, RowTextY(y, bodyFS)),
                        MapTheme::kMutedText, LocalizationManager::Get("ListPanel.NoOverview").c_str());
        }
        y += rowHeight;
    }

    // ------------------------------------------------------------------------
    // Custom Markers Section
    // ------------------------------------------------------------------------
    if (activeTab == ListTab::Custom) {
        if ((y + headerH > rowsTop) && (y < mx.y)) {
            std::string sectionTitle = loc->GetLabel("Labels.CustomMarkers");
            if (sectionTitle == "Labels.CustomMarkers" || sectionTitle == "CustomMarkers") {
                std::string single = loc->GetLabel("Labels.CustomMarker");
                if (single != "Labels.CustomMarker" && single != "CustomMarker") {
                    sectionTitle = single + "s";
                } else {
                    sectionTitle = "Custom Markers";
                }
            }
            MapDraw::DrawTextShadowed(dl, titleFont, titleFS,
                        ImVec2(rowContentX, y),
                        settings->TitleCol(), sectionTitle.c_str());

            const std::string countStr = std::to_string(markers.size());
            ImVec2 cntSz = ImGui::CalcTextSize(countStr.c_str());
            cntSz.x *= (titleFS / baseFS);
            const float countX = rowContentRight - cntSz.x;
            MapDraw::DrawTextShadowed(dl, titleFont, titleFS,
                        ImVec2(countX, y),
                        settings->TitleCol(), countStr.c_str());

            const float sepLineY = y + headerH;
            dl->AddLine(ImVec2(rowContentX, sepLineY), ImVec2(rowContentRight, sepLineY),
                        MapTheme::kSeparator, 1.0f);
        }
        y += headerH + pad;

        if (markers.empty()) {
            const bool rowVisible = (y + rowHeight > rowsTop) && (y < mx.y);
            if (rowVisible) {
                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                            ImVec2(rowContentX, RowTextY(y, bodyFS)),
                            MapTheme::kMutedText, LocalizationManager::Get("ListPanel.NoMarkers").c_str());
            }
            y += rowHeight;
        } else {
            for (const auto& m : markers) {
                const float bandTop    = y - 3.0f;
                const float bandBottom = y + rowHeight - 3.0f;
                const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
                if (rowVisible) {
                    const bool  rowHovered = cursorInRowsX && mouse.y >= bandTop && mouse.y <= bandBottom;
                    const float delY0      = bandTop + 2.0f;
                    const float delY1      = bandBottom - 2.0f;
                    const bool  delHovered = rowHovered && mouse.x >= delBtnX0 && mouse.x <= delBtnX1;

                    if (m.isDestination) {
                        const ImU32 bgFill = rowHovered ? MapTheme::kDestRowBgHover : MapTheme::kDestRowBg;
                        dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                          ImVec2(rowsRight, bandBottom),
                                          bgFill, 2.0f);
                    } else if (rowHovered) {
                        dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                          ImVec2(rowsRight, bandBottom),
                                          MapTheme::kRowHoverBg, 2.0f);
                    }

                    const ImU32 textCol = rowHovered ? settings->BodyColHover() : settings->BodyCol();
                    // Auto-named markers carry an empty note and resolve their label
                    // from the stored scheme + number, so the row follows the language.
                    // Rows come from GetUserMarkers(), so every one is a marker the
                    // player placed. The vanilla destination is no longer a marker at
                    // all, which is why the old "[Destination] " prefix test and its
                    // hardcoded 14-byte pointer bump are gone.
                    const std::string resolvedName = ResolveMarkerName(m);
                    const char* displayName = resolvedName.c_str();

                    // Category icon ahead of the name, so a list of markers reads like the
                    // Legend rows rather than a wall of text. DrawPin, not a raw AddImage:
                    // it is the one path that already handles all three cases a custom
                    // marker can be -- a category PNG, a waypoint glyph (iconType >= 100,
                    // vector-drawn, no PNG exists), and an icon-less marker (the golden
                    // fallback disc). It also carries the "3D Icons" shadow/duotone
                    // treatment, same as the Legend icons and the map pins.
                    //
                    // Every row draws the marker's OWN assigned iconType. If it is the
                    // destination, a muted destination badge sits on the right.
                    const float iconSz = std::floor(bodyFS * 0.9f);
                    auto* rowSrv = TextureManager::GetCategoryIcon(m.iconType);
                    MapDraw::DrawPin(dl, rowContentX + iconSz * 0.5f,
                                     RowTextY(y, iconSz) + iconSz * 0.5f,
                                     0.0f, m.iconType, rowSrv,
                                     iconSz / (2.0f * MapDraw::kPinRadius),
                                     settings->PinTintCol());

                    float textRightBudget = textRightWithBtn;
                    if (m.isDestination) {
                        const float badgeSz  = std::floor(bodyFS * 1.05f);
                        const float badgeGap = 6.0f;
                        const float badgeX1  = delBtnX0 - badgeGap;
                        const float badgeX0  = badgeX1 - badgeSz;
                        const float badgeY   = RowTextY(y, badgeSz);
                        if (auto* destIcon = TextureManager::GetIconByName("destination")) {
                            dl->AddImage(reinterpret_cast<ImTextureID>(destIcon),
                                         ImVec2(badgeX0, badgeY), ImVec2(badgeX1, badgeY + badgeSz),
                                         ImVec2(0, 0), ImVec2(1, 1), MapTheme::kDestBadgeTint);
                        }
                        textRightBudget = badgeX0 - kRowTextSlack;
                    }

                    // Gap scales with the text, not a fixed 6px: at a large body size a
                    // fixed gap reads as the icon crowding the name.
                    const float nameX = rowContentX + iconSz + std::floor(bodyFS * 0.55f);
                    const std::string rowName =
                        TruncateRowTextToWidth(displayName, textRightBudget - nameX, bodyFS);
                    MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                ImVec2(nameX, RowTextY(y, bodyFS)), textCol, rowName.c_str());

                    // CalcTextSize measures at the base font size; scale to the drawn
                    // bodyFS or the X sits off-center at non-default font sizes.
                    ImVec2 xSz = ImGui::CalcTextSize("X");
                    xSz.x *= bodyFS / baseFS;
                    xSz.y *= bodyFS / baseFS;
                    const ImVec2 xPos(delBtnX0 + (delBtnW - xSz.x) * 0.5f,
                                      delY0   + (delY1 - delY0 - xSz.y) * 0.5f);

                    if (rowHovered) {
                        const ImU32 delFill = delHovered ? MapTheme::kRemoveBgHover : MapTheme::kRemoveBg;
                        dl->AddRectFilled(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), delFill, 2.0f);
                        dl->AddRect(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), MapTheme::kRemoveBorder, 2.0f, 0, MapTheme::kBtnBorderW);
                        dl->AddText(ImGui::GetFont(), bodyFS, xPos, MapTheme::kRemoveText, "X");
                    } else {
                        dl->AddText(ImGui::GetFont(), bodyFS, xPos, MapTheme::kMutedText, "X");
                    }

                    rowIds.push_back(m.id);
                    rowRects.push_back({ rowsLeft, bandTop, rowsRight, bandBottom });
                    rowDeleteRects.push_back({ delBtnX0, delY0, delBtnX1, delY1 });
                }
                y += rowHeight;
            }
        }
    }

    // ------------------------------------------------------------------------
    // Favorites Section
    // ------------------------------------------------------------------------
    if (activeTab == ListTab::Favorites) {
        if (favs.empty()) {
            const bool rowVisible = (y + rowHeight > rowsTop) && (y < mx.y);
            if (rowVisible) {
                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                            ImVec2(rowContentX, RowTextY(y, bodyFS)),
                            MapTheme::kMutedText, LocalizationManager::Get("ListPanel.NoFavorites").c_str());
            }
            y += rowHeight;
        } else {
            std::size_t favIdx = 0;
            for (const auto& f : favs) {
                ++favIdx;
                const float bandTop    = y - 3.0f;
                const float bandBottom = y + rowHeight - 3.0f;
                const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
                if (rowVisible) {
                    const bool  rowHovered = cursorInRowsX && mouse.y >= bandTop && mouse.y <= bandBottom;
                    const float delY0      = bandTop + 2.0f;
                    const float delY1      = bandBottom - 2.0f;
                    const bool  delHovered = rowHovered && mouse.x >= delBtnX0 && mouse.x <= delBtnX1;

                    if (rowHovered) {
                        dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                          ImVec2(rowsRight, bandBottom),
                                          MapTheme::kRowHoverBg, 2.0f);
                    }

                    const ImU32 textCol = rowHovered ? settings->BodyColHover() : settings->BodyCol();

                    // Muted so the rail reads as structure and the name stays
                    // the thing you actually scan.
                    char idxBuf[8];
                    std::snprintf(idxBuf, sizeof(idxBuf), "%02zu", favIdx);
                    MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                ImVec2(IndexX(idxBuf), RowTextY(y, bodyFS)),
                                MapTheme::kMutedText, idxBuf);

                    const std::string favName = TruncateRowTextToWidth(
                        f.name, textRightWithBtn - idxTextX, bodyFS);
                    MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                ImVec2(idxTextX, RowTextY(y, bodyFS)), textCol, favName.c_str());

                    ImVec2 xSz = ImGui::CalcTextSize("X");
                    xSz.x *= bodyFS / baseFS;
                    xSz.y *= bodyFS / baseFS;
                    const ImVec2 xPos(delBtnX0 + (delBtnW - xSz.x) * 0.5f,
                                      delY0   + (delY1 - delY0 - xSz.y) * 0.5f);

                    if (rowHovered) {
                        const ImU32 delFill = delHovered ? MapTheme::kRemoveBgHover : MapTheme::kRemoveBg;
                        dl->AddRectFilled(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), delFill, 2.0f);
                        dl->AddRect(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), MapTheme::kRemoveBorder, 2.0f, 0, MapTheme::kBtnBorderW);
                        dl->AddText(ImGui::GetFont(), bodyFS, xPos, MapTheme::kRemoveText, "X");
                    } else {
                        dl->AddText(ImGui::GetFont(), bodyFS, xPos, MapTheme::kMutedText, "X");
                    }

                    favIds.push_back(f.formID);
                    favRects.push_back({ rowsLeft, bandTop, rowsRight, bandBottom });
                    favDeleteRects.push_back({ delBtnX0, delY0, delBtnX1, delY1 });
                }
                y += rowHeight;
            }
        }
    }

    // ------------------------------------------------------------------------
    // Notes Section
    // ------------------------------------------------------------------------
    if (activeTab == ListTab::Notes) {
        if (notes.empty()) {
            const bool rowVisible = (y + rowHeight > rowsTop) && (y < mx.y);
            if (rowVisible) {
                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                            ImVec2(rowContentX, RowTextY(y, bodyFS)),
                            MapTheme::kMutedText, LocalizationManager::Get("ListPanel.NoNotes").c_str());
            }
            y += rowHeight;
        } else {
            std::size_t noteIdx = 0;
            for (const auto& n : notes) {
                ++noteIdx;
                const float bandTop    = y - 3.0f;
                const float bandBottom = y + rowHeight - 3.0f;
                const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
                if (rowVisible) {
                    const bool  rowHovered = cursorInRowsX && mouse.y >= bandTop && mouse.y <= bandBottom;
                    const float delY0      = bandTop + 2.0f;
                    const float delY1      = bandBottom - 2.0f;
                    const bool  delHovered = rowHovered && mouse.x >= delBtnX0 && mouse.x <= delBtnX1;

                    if (rowHovered) {
                        dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                          ImVec2(rowsRight, bandBottom),
                                          MapTheme::kRowHoverBg, 2.0f);
                    }

                    const ImU32 textCol = rowHovered ? settings->BodyColHover() : settings->BodyCol();

                    char idxBuf[8];
                    std::snprintf(idxBuf, sizeof(idxBuf), "%02zu", noteIdx);
                    MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                ImVec2(IndexX(idxBuf), RowTextY(y, bodyFS)),
                                MapTheme::kMutedText, idxBuf);

                    const std::string noteName = TruncateRowTextToWidth(
                        n.displayName, textRightWithBtn - idxTextX, bodyFS);
                    MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                ImVec2(idxTextX, RowTextY(y, bodyFS)), textCol, noteName.c_str());

                    ImVec2 xSz = ImGui::CalcTextSize("X");
                    xSz.x *= bodyFS / baseFS;
                    xSz.y *= bodyFS / baseFS;
                    const ImVec2 xPos(delBtnX0 + (delBtnW - xSz.x) * 0.5f,
                                      delY0   + (delY1 - delY0 - xSz.y) * 0.5f);

                    if (rowHovered) {
                        const ImU32 delFill = delHovered ? MapTheme::kRemoveBgHover : MapTheme::kRemoveBg;
                        dl->AddRectFilled(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), delFill, 2.0f);
                        dl->AddRect(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), MapTheme::kRemoveBorder, 2.0f, 0, MapTheme::kBtnBorderW);
                        dl->AddText(ImGui::GetFont(), bodyFS, xPos, MapTheme::kRemoveText, "X");
                    } else {
                        dl->AddText(ImGui::GetFont(), bodyFS, xPos, MapTheme::kMutedText, "X");
                    }

                    noteIds.push_back(n.formID);
                    noteRects.push_back({ rowsLeft, bandTop, rowsRight, bandBottom });
                    noteDeleteRects.push_back({ delBtnX0, delY0, delBtnX1, delY1 });
                }
                y += rowHeight;
            }
        }
    }

    dl->PopClipRect();

    // Scrollbar indicator. Spans the BODY only -- it reports the active tab's
    // scroll, and the chrome above it never moves.
    if (_listScrollMax > 0.0f) {
        const float trackH = bodyH - pad * 2.0f;
        const float thumbH = std::max(24.0f, trackH * (bodyH / contentH));
        const float t      = scrollY / _listScrollMax;
        const float thumbY = bodyMin.y + pad + t * (trackH - thumbH);
        dl->AddRectFilled(ImVec2(mx.x - 5.0f, thumbY),
                          ImVec2(mx.x - 2.0f, thumbY + thumbH),
                          MapTheme::kSeparator, 2.0f);
    }

    std::lock_guard<std::mutex> lk(_panelMutex);
    _panelVisible = true;
    _panelMinX = mn.x; _panelMinY = mn.y;
    _panelMaxX = mx.x; _panelMaxY = mx.y;
    _rowIds          = std::move(rowIds);
    _rowRects        = std::move(rowRects);
    _rowDeleteRects  = std::move(rowDeleteRects);
    _rowFavIds       = std::move(favIds);
    _rowFavRects     = std::move(favRects);
    _rowFavDeleteRects = std::move(favDeleteRects);
    _rowNoteIds        = std::move(noteIds);
    _rowNoteRects      = std::move(noteRects);
    _rowNoteDeleteRects = std::move(noteDeleteRects);
    _rowQuestIds       = std::move(questIds);
    _rowQuestRects     = std::move(questRects);
    _rowQuestPositions = std::move(questPositions);
    _destRowValid      = destRowValid;
    _destRowRect       = destRowRect;
    _destRowPos        = destRowPos;
}

// Jumps the list panel to the tab a newly added row landed in. Without this a
// marker/favorite/note added while another tab is showing appears to do
// nothing: the row exists, but in a section the player is not looking at.
// Also resets that tab's scroll -- an add lands at the section's own order, and
// a stale offset from a previous browse would open the tab mid-list.
void MapMarkerRenderer::ShowListTab(ListTab tab) {
    const auto idx = static_cast<std::int32_t>(tab);
    if (idx < 0 || idx >= static_cast<std::int32_t>(ListTab::Count)) return;

    auto* settings = Settings::GetSingleton();
    settings->listActiveTab = idx;
    _listScrollY[idx] = 0.0f;
    // Persist immediately, same as a tab click: the panel is usable without
    // ever opening the settings menu.
    settings->SavePositions();

    // The panel is the point of the jump -- an add while it is hidden should
    // reveal the result, not silently switch a tab nobody can see.
    _showListPanel.store(true);
}

bool MapMarkerRenderer::CursorOverPanel() const {
    auto* cursor = RE::MenuCursor::GetSingleton();
    if (!cursor || cursor->screenWidthX <= 0.0f || cursor->screenWidthY <= 0.0f) {
        return false;
    }
    auto& io = ImGui::GetIO();
    const float dispW = io.DisplaySize.x;
    const float dispH = io.DisplaySize.y;
    const float mx = (cursor->cursorPosX / cursor->screenWidthX) * dispW;
    const float my = (cursor->cursorPosY / cursor->screenWidthY) * dispH;

    std::lock_guard<std::mutex> lk(_panelMutex);
    if (!_panelVisible) return false;
    return mx >= _panelMinX && mx <= _panelMaxX
        && my >= _panelMinY && my <= _panelMaxY;
}

bool MapMarkerRenderer::TryHandlePanelClick() {
    auto* cursor = RE::MenuCursor::GetSingleton();
    if (!cursor) return false;
    if (cursor->screenWidthX <= 0.0f || cursor->screenWidthY <= 0.0f) return false;

    auto& io = ImGui::GetIO();
    const float mx = (cursor->cursorPosX / cursor->screenWidthX) * io.DisplaySize.x;
    const float my = (cursor->cursorPosY / cursor->screenWidthY) * io.DisplaySize.y;

    std::lock_guard<std::mutex> lk(_panelMutex);
    if (!_panelVisible) return false;
    if (mx < _panelMinX || mx > _panelMaxX || my < _panelMinY || my > _panelMaxY) {
        return false;
    }

    // Check delete buttons for custom markers.
    for (int i = 0; i < static_cast<int>(_rowDeleteRects.size()); ++i) {
        const auto& r = _rowDeleteRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            const std::uint32_t id = _rowIds[i];
            logger::debug("[PanelClick] delete btn custom row {} -> marker id {}", i, id);
            MarkerManager::GetSingleton()->RemoveMarker(id);
            return true;
        }
    }
    
    // Check delete buttons for favorites.
    for (int i = 0; i < static_cast<int>(_rowFavDeleteRects.size()); ++i) {
        const auto& r = _rowFavDeleteRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            const RE::FormID id = _rowFavIds[i];
            logger::debug("[PanelClick] delete btn fav row {} -> form id {}", i, id);
            // Toggle to false removes it
            MarkerManager::GetSingleton()->ToggleFavorite(id, 0, 0, {}, "");
            return true;
        }
    }

    // Check delete buttons for notes.
    for (int i = 0; i < static_cast<int>(_rowNoteDeleteRects.size()); ++i) {
        const auto& r = _rowNoteDeleteRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            const RE::FormID id = _rowNoteIds[i];
            logger::debug("[PanelClick] delete btn note row {} -> form id {}", i, id);
            MarkerManager::GetSingleton()->SetVanillaNote(id, ""); // empty erases
            return true;
        }
    }

    // Check custom marker row pan clicks.
    for (int i = 0; i < static_cast<int>(_rowRects.size()); ++i) {
        const auto& r = _rowRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            _pendingPanTarget.store(_rowIds[i]);
            logger::debug("[PanelClick] hit custom row {} -> marker id {}", i, _rowIds[i]);
            return true;
        }
    }
    
    // Check favorite row pan clicks.
    for (int i = 0; i < static_cast<int>(_rowFavRects.size()); ++i) {
        const auto& r = _rowFavRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            _pendingPanFavTarget.store(_rowFavIds[i]);
            logger::debug("[PanelClick] hit fav row {} -> form id {}", i, _rowFavIds[i]);
            return true;
        }
    }

    // Check note row pan clicks.
    for (int i = 0; i < static_cast<int>(_rowNoteRects.size()); ++i) {
        const auto& r = _rowNoteRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            _pendingPanNoteTarget.store(_rowNoteIds[i]);
            logger::debug("[PanelClick] hit note row {} -> form id {}", i, _rowNoteIds[i]);
            return true;
        }
    }

    // Destination row pan click. Publishes the POSITION, not an id: a vanilla
    // destination on a local-map door has no marker id, and the record is
    // deliberately unreachable from the UI. _destRowValid is only set for a
    // pannable destination, so an interior door's row is inert here.
    if (_destRowValid) {
        const auto& r = _destRowRect;
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            _pendingPanDestPos.store(true);
            logger::debug("[PanelClick] hit destination row");
            return true;
        }
    }

    // Check active-quest row pan clicks. Only quests with a resolved position
    // have a rect at all, so a click on an unpannable row falls through to the
    // panel-swallow below and does nothing, which is the intent.
    for (int i = 0; i < static_cast<int>(_rowQuestRects.size()); ++i) {
        const auto& r = _rowQuestRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            _pendingPanQuestPos = _rowQuestPositions[i];
            _pendingPanQuestID  = _rowQuestIds[i];
            _pendingPanQuestPosValid.store(true);
            logger::debug("[PanelClick] hit quest row {} -> quest id {:08X} pos=({:.1f},{:.1f},{:.1f})",
                          i, _rowQuestIds[i], _pendingPanQuestPos.x, _pendingPanQuestPos.y, _pendingPanQuestPos.z);
            return true;
        }
    }

    return true;
}

void MapMarkerRenderer::ApplyPendingPan(RE::MapMenu* a_menu) {
    std::uint32_t target = _pendingPanTarget.exchange(0);
    RE::FormID favTarget = _pendingPanFavTarget.exchange(0);
    RE::FormID noteTarget = _pendingPanNoteTarget.exchange(0);
    const bool questTarget = _pendingPanQuestPosValid.exchange(false);
    const bool destTarget = _pendingPanDestPos.exchange(false);

    if ((target == 0 && favTarget == 0 && noteTarget == 0 && !questTarget &&
         !destTarget) || !a_menu) return;

    // Resolve target marker position.
    RE::NiPoint3 pos{};
    RE::NiPoint3 questPos{};
    RE::FormID   questID = 0;
    bool found = false;

    if (destTarget) {
        // Position was published with the row; nothing to look up. Read under
        // the panel mutex, same as every other cross-thread panel field.
        std::lock_guard<std::mutex> lk(_panelMutex);
        if (_destRowValid) {
            pos   = _destRowPos;
            found = true;
        }
    }

    if (!found && target != 0) {
        // Pan target: a real marker the user picked from the list.
        auto markers = MarkerManager::GetSingleton()->GetUserMarkers();
        for (const auto& m : markers) {
            if (m.id == target) { pos = m.position; found = true; break; }
        }
    } else if (favTarget != 0) {
        auto favs = MarkerManager::GetSingleton()->GetFavorites();
        for (const auto& f : favs) {
            if (f.formID == favTarget) { pos = f.position; found = true; break; }
        }
    } else if (noteTarget != 0) {
        // Notes have no cached position; resolve fresh from the ref, same as
        // the display name in DrawListPanel.
        if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(noteTarget)) {
            pos = ref->GetPosition();
            found = true;
        }
    } else if (questTarget) {
        // Position comes directly from the clicked row's cached position,
        // rather than re-looking up by quest FormID (which collides when multiple
        // miscellaneous objectives share one quest).
        std::lock_guard<std::mutex> lk(_panelMutex);
        pos = _pendingPanQuestPos;
        questPos = _pendingPanQuestPos;
        questID = _pendingPanQuestID;
        found = true;
    }

    if (!found) return;

    if (auto* ws = a_menu->GetRuntimeData2()->worldSpace) {
        const auto& md = ws->worldMapData;
        const float x_min = md.nwCellX * 4096.0f;
        const float x_max = md.seCellX * 4096.0f;
        const float y_min = md.seCellY * 4096.0f;
        const float y_max = md.nwCellY * 4096.0f;
        pos.x = std::clamp(pos.x, x_min, x_max);
        pos.y = std::clamp(pos.y, y_min, y_max);
    }

    const auto base   = reinterpret_cast<std::uintptr_t>(a_menu);
    auto*      camera = reinterpret_cast<RE::TESCamera*>(base + 0x304A0);
    if (!camera || !camera->cameraRoot) return;

    if (MapCameraIsBusyTransitioning(camera)) {
        if (target != 0) _pendingPanTarget.store(target);
        if (favTarget != 0) _pendingPanFavTarget.store(favTarget);
        if (noteTarget != 0) _pendingPanNoteTarget.store(noteTarget);
        if (questTarget) {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _pendingPanQuestPos = questPos;
            _pendingPanQuestID  = questID;
            _pendingPanQuestPosValid.store(true);
        }
        if (destTarget) _pendingPanDestPos.store(true);
        return;
    }

    using GlidePan_t = void(*)(RE::TESCamera*, const RE::NiPoint3*, float);
    // MapCamera::GlidePan: SE 52137 / AE 53022 (findings-SE-Hook-IDs.md row 10).
    REL::Relocation<GlidePan_t> glidePan{ REL::RelocationID(52137, 53022) };
    glidePan(camera, &pos, 0.5f);
    logger::debug("[Pan] target custom {} fav {} quest {:08X} glidePan called", target, favTarget, questID);

    // Mutually exclusive by construction: each id only sets when every earlier
    // one is 0, matching the if/else-if chain in the pulse draw block.
    _pulseMarkerId = target;
    _pulseFavId    = (target == 0) ? favTarget : 0;
    _pulseNoteId   = (target == 0 && favTarget == 0) ? noteTarget : 0;
    _pulseQuestId  = (target == 0 && favTarget == 0 && noteTarget == 0 && questTarget) ? (questID != 0 ? questID : 1) : 0;
    _pulseQuestPos = questPos;
    _pulseStart = std::chrono::steady_clock::now();
}

void MapMarkerRenderer::DrawFilterPanel() {
    // The "Legend" panel. Called "Map Filters" throughout the code, the INI
    // ([Positions] FilterPosX/Y), and the ImGui window ID; the user-facing
    // name is whatever the MapFilters translation key says, which is now
    // "Legend". Same panel, two names — don't add a second one.
    //
    // Focus-mode rows: [category icon] Label (count) ....... [eye].
    // Show/hide directly, no checkboxes — the eye IS the state readout (full
    // alpha = shown, dimmed = hidden), so a hidden row reads as "I hid this"
    // rather than "this is disabled". Each toggle drives MarkerFilterHook,
    // which skips the disabled categories in AddMarkerByHandle and rebuilds
    // the map live. In-memory only for now; not persisted to INI.
    using MarkerFilterHook::FocusMode;

    auto* settings = Settings::GetSingleton();

    // Feed the saved position only the first time; after that ImGui owns the
    // live drag and we mirror it back. -1 means unset -> let ImGui place it.
    if (settings->filterPosX >= 0 && settings->filterPosY >= 0) {
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(settings->filterPosX),
                                       static_cast<float>(settings->filterPosY)),
                                ImGuiCond_FirstUseEver);
    }

    // Background + border + body text pull from the global Appearance settings
    // (user-themeable); everything else stays on the fixed kDlg* theme.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImVec4(settings->bodyColorR / 255.0f, settings->bodyColorG / 255.0f,
                                 settings->bodyColorB / 255.0f, 1.0f));
    // Header underline. Without this the panel takes ImGui's default grey-blue
    // separator, which was the one thing visually disowning it from the list
    // panel -- every section underline there is kSeparator (the map gold).
    ImGui::PushStyleColor(ImGuiCol_Separator,     MapTheme::kSeparator);
    // No checkbox/FrameBg colors here anymore: rows are icon + label + eye
    // drawn by hand on the window draw list, with an InvisibleButton for the
    // hit test. Nothing in this window uses an ImGui frame widget.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(8.0f,  6.0f));
    // No title bar: kills the default collapse triangle. The header is drawn
    // by hand below (localized text, heading font) as its own design instead.
    // NoScrollbar because AlwaysAutoResize lags SetWindowFontScale by a frame,
    // which flashed a permanent scrollbar; content always fits, never scrolls.
    if (ImGui::Begin("Map Filters", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 wPos  = ImGui::GetWindowPos();
        const ImVec2 wSize = ImGui::GetWindowSize();
        if (wSize.x > 1.0f && wSize.y > 1.0f) {
            ImDrawList* wdl = ImGui::GetWindowDrawList();
            wdl->PushClipRectFullScreen();
            MapDraw::DrawPanelCard(wdl, wPos, ImVec2(wPos.x + wSize.x, wPos.y + wSize.y),
                                   settings->BgCol(), settings->BorderCol());
            wdl->PopClipRect();
        }

        const float bodyScale  = std::clamp(settings->bodyScale,  0.5f, 3.0f);
        const float titleScale = std::clamp(settings->titleScale, 0.5f, 3.0f);

        // ---- Collapse animation ------------------------------------------
        // Discrete state (a click toggle), continuous transition. `_filterAnim`
        // is 1 = fully expanded, 0 = icon strip. It lives on the renderer, NOT
        // as a local: this function re-runs every present, so a local float
        // would be pinned at its initializer and nothing would ever move.
        //
        // Frame-rate independent easing: `powf(smoothing, dt)` composes over
        // time, so two short frames land exactly where one long frame of the
        // same duration would. The naive `x += (t-x)*0.1f` is a per-FRAME
        // fraction and would run ~2.4x faster at 144fps than at 60.
        {
            const float target    = settings->filterCollapsed ? 0.0f : 1.0f;
            // First draw of the session: snap to the persisted state. Without
            // this a panel saved as collapsed would animate shut in front of
            // the player every time the map opens.
            if (!_filterAnimInit) {
                _filterAnim     = target;
                _filterAnimInit = true;
            }
            // Clamp dt: a loading screen or stutter spike would otherwise snap
            // the panel open in one frame instead of animating it.
            const float dt        = std::min(ImGui::GetIO().DeltaTime, 0.1f);
            const float smoothing = 0.0005f;   // gap remaining after 1s: fast, ~0.2s settle
            _filterAnim = target + (_filterAnim - target) * std::pow(smoothing, dt);
            // powf approaches but never reaches the target, and float error can
            // push it a hair outside 0..1; clamp before the curve.
            _filterAnim = std::clamp(_filterAnim, 0.0f, 1.0f);
            // Snap the last sliver so the strip settles on an exact width
            // rather than creeping for the rest of the session.
            if (std::abs(_filterAnim - target) < 0.001f) _filterAnim = target;
        }
        // Keep the state linear, ease only at draw time (quadratic ease-out).
        const float easedOpen = 1.0f - (1.0f - _filterAnim) * (1.0f - _filterAnim);

        ImDrawList* hdrDl = ImGui::GetWindowDrawList();

        // ---- Collapse toggle ---------------------------------------------
        // Sits at the top-right when expanded and becomes the whole header when
        // collapsed (the title is gone, so this button is the only way back).
        // Drawn as a chevron with AddTriangleFilled rather than an icon: there
        // is no arrow PNG in the set, and a vector glyph scales with the font
        // and picks up the theme colors for free.
        const float btnSz = std::floor(ImGui::GetFontSize() * 1.1f);

        // Title fades and its width collapses together, so the button slides
        // left into the strip instead of jumping. Measured on the heading font.
        std::string title = LocalizationManager::GetSingleton()->GetLabel("MapFilters");
        if (title == "MapFilters") title = "Map Filters";
        float titleW = 0.0f;
        ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
        if (titleFont) ImGui::PushFont(titleFont);
        ImGui::SetWindowFontScale(titleScale);
        const float titleFs = ImGui::GetFontSize();
        titleW = ImGui::CalcTextSize(title.c_str()).x;
        if (titleFont) ImGui::PopFont();
        ImGui::SetWindowFontScale(bodyScale);

        // Per-mode category icon, indexed by FocusMode. Empty string = no
        // honest match in the icon set yet; the column is still reserved so
        // labels stay aligned.
        static constexpr const char* kModeIcons[] = {
            "city",       // kCities
            "stable",     // kTravel     - carriage travel, the dominant Travel marker
            "cave",       // kDungeons
            "camp",       // kCamps
            "smelter",    // kIndustry   - generic industry; mills/mines are subtypes
            "landmark",   // kLandmarks
            "dwelling",   // kDwellings
            "quest",      // kQuests
            "shop",       // kShops
            "loot",       // kLoot
            "custom",     // kModded
        };
        static_assert(std::size(kModeIcons) == static_cast<std::size_t>(FocusMode::kCount),
                      "kModeIcons must stay in sync with FocusMode");

        ImDrawList* dl = ImGui::GetWindowDrawList();

        const float fs      = ImGui::GetFontSize();
        // Legend rows are icon-led, so the art carries the row here and runs LARGER
        // than the list panel's (fs * 0.9). Font-relative on purpose: the body-size
        // slider scales both panels' icons together.
        const float iconSz  = std::floor(fs * 1.35f);  // category icon, left column
        const float eyeSz   = fs * 1.05f;   // state icon, right column
        const float gap     = 8.0f;         // icon -> label
        const float rowPadY = 3.0f;
        const float rowH    = std::max(fs, iconSz) + rowPadY * 2.0f;

        // Widest label decides the panel width; the eye is right-aligned to
        // that same edge so the column doesn't jitter as counts change.
        float maxLabelW = 0.0f;
        std::vector<std::string> labels(static_cast<std::size_t>(FocusMode::kCount));
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const auto  mode     = static_cast<FocusMode>(i);
            const char* modeName = MarkerFilterHook::GetModeName(mode);
            // Localized: FilterMode.<Name> in the translation file; GetLabel
            // returns the key when it's missing, so fall back to the English name.
            std::string label = LocalizationManager::GetSingleton()->GetLabel(
                std::string{ "FilterMode." } + modeName);
            if (label.starts_with("FilterMode.")) label = modeName;
            // Marker count for this bucket. Counts every marker in the bucket,
            // including ones currently filtered out, so the number stays put as
            // you toggle. Zero is hidden rather than shown as "(0)": on any
            // given worldspace most buckets are legitimately empty and a column
            // of "(0)" is noise. Reads the loaded MapMenu array, so it will not
            // match the List panel's Overview stats (those walk locations).
            if (const auto n = MarkerFilterHook::GetModeCount(mode); n > 0) {
                label += " (" + std::to_string(n) + ")";
            }
            labels[i]  = std::move(label);
            maxLabelW  = std::max(maxLabelW, ImGui::CalcTextSize(labels[i].c_str()).x);
        }
        // Expanded width vs collapsed width, interpolated by the animation.
        // Collapsed reserves only the icon column, so the panel really does
        // shrink — the point of the feature is giving the map back its space.
        // The settings row below the categories carries a label too, and it is
        // not in labels[], so measure it here or a long translation would run
        // past the panel edge.
        {
            std::string setLbl = LocalizationManager::GetSingleton()->GetLabel("ListPanel.Settings");
            if (setLbl == "ListPanel.Settings") setLbl = "Settings";
            maxLabelW = std::max(maxLabelW, ImGui::CalcTextSize(setLbl.c_str()).x);
        }
        const float rowWFull = iconSz + gap + maxLabelW + gap * 2.0f + eyeSz;
        const float rowWIcon = std::max(iconSz, btnSz);
        const float rowW     = rowWIcon + (rowWFull - rowWIcon) * easedOpen;

        // ---- Header row: chevron button, plus the title when expanded ------
        {
            const ImVec2 hMin = ImGui::GetCursorScreenPos();
            const float  hH   = std::max(btnSz, titleFs);

            // Two anchors, blended by the animation:
            //   expanded  -> right-aligned to the panel edge, so it lines up
            //                with the eye column that ends at the same x.
            //   collapsed -> centred on the ICON column, so it sits on the same
            //                vertical axis as the category icons below it.
            // Right-aligning in BOTH states was the bug: collapsed, rowW is
            // max(iconSz, btnSz) = iconSz (icons run fs*1.35, the button
            // fs*1.1), so the chevron was shoved (iconSz - btnSz) to the right
            // while the icons stayed flush left, and the strip read crooked.
            const float bxExpanded  = hMin.x + rowW - btnSz;
            const float bxCollapsed = hMin.x + (iconSz - btnSz) * 0.5f;
            const float bx = bxCollapsed + (bxExpanded - bxCollapsed) * easedOpen;
            const float by = hMin.y + (hH - btnSz) * 0.5f;

            ImGui::PushID("filterCollapse");
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            const bool btnClicked = ImGui::InvisibleButton("##collapse", ImVec2(btnSz, btnSz));
            const bool btnHover   = ImGui::IsItemHovered();
            ImGui::PopID();

            if (btnClicked) {
                settings->filterCollapsed = !settings->filterCollapsed;
                // Persist immediately. SavePositions writes the whole position
                // block including [Filters] Collapsed; the panel's own drag
                // handler only fires on a drag, which this is not.
                settings->SavePositions();
            }

            if (btnHover) {
                hdrDl->AddRectFilled(ImVec2(bx - 3.0f, by - 3.0f),
                                     ImVec2(bx + btnSz + 3.0f, by + btnSz + 3.0f),
                                     MapTheme::kRowHoverBg, 3.0f);
            }

            // Chevron: points LEFT when expanded (click to collapse), RIGHT
            // when collapsed (click to open). Rotates through the animation by
            // interpolating the tip's x, so it reads as one continuous motion.
            {
                const float cx   = bx + btnSz * 0.5f;
                const float cy   = by + btnSz * 0.5f;
                const float armX = btnSz * 0.22f;
                const float armY = btnSz * 0.28f;
                // easedOpen 1 = expanded = tip on the left.
                const float tipX = cx + armX * (1.0f - 2.0f * easedOpen);
                const float bkX  = cx - armX * (1.0f - 2.0f * easedOpen);
                const ImU32 chevCol = btnHover ? settings->BodyColHover() : settings->TitleCol();
                hdrDl->AddTriangleFilled(ImVec2(bkX, cy - armY),
                                         ImVec2(tipX, cy),
                                         ImVec2(bkX, cy + armY), chevCol);
            }

            // Title: fades out as it collapses. Skipped entirely below ~2%
            // alpha so a fully collapsed strip does no text work at all.
            if (easedOpen > 0.02f) {
                if (titleFont) ImGui::PushFont(titleFont);
                ImGui::SetWindowFontScale(titleScale);
                const ImU32 tc = MapDraw::WithAlpha(settings->TitleCol(), easedOpen);
                MapDraw::DrawTextShadowed(hdrDl, ImGui::GetFont(), ImGui::GetFontSize(),
                                          ImVec2(hMin.x, hMin.y + (hH - titleFs) * 0.5f),
                                          tc, title.c_str());
                if (titleFont) ImGui::PopFont();
                ImGui::SetWindowFontScale(bodyScale);
            }

            // Restore layout flow: the button used SetCursorScreenPos, so the
            // cursor is wherever that left it. Put it back under the header.
            ImGui::SetCursorScreenPos(ImVec2(hMin.x, hMin.y + hH));
            ImGui::Dummy(ImVec2(rowW, 2.0f));
        }
        ImGui::Separator();
        ImGui::Spacing();

        ID3D11ShaderResourceView* eyeSrv = TextureManager::GetIconByName("eye");

        // Collapsed-strip tooltip. Recorded during the row loop and drawn AFTER
        // it: drawn inline, a later row would paint over a tooltip that extends
        // past its own row band. Only meaningful while collapsed -- expanded,
        // the label is right there.
        std::string  tipText;
        ImVec2       tipAnchor{};

        for (std::size_t i = 0; i < labels.size(); ++i) {
            const auto  mode     = static_cast<FocusMode>(i);
            const bool  on       = MarkerFilterHook::IsModeEnabled(mode);
            const char* modeName = MarkerFilterHook::GetModeName(mode);

            const ImVec2 rowMin = ImGui::GetCursorScreenPos();

            // Whole row is the hit target, not just the eye: a 18px glyph is a
            // poor click target with the engine cursor, and the row reads as
            // one control.
            ImGui::PushID(modeName);
            const bool clicked = ImGui::InvisibleButton("##row", ImVec2(rowW, rowH));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clicked) {
                MarkerFilterHook::SetModeEnabled(mode, !on);
            }

            if (hovered) {
                dl->AddRectFilled(ImVec2(rowMin.x - 4.0f, rowMin.y),
                                  ImVec2(rowMin.x + rowW + 4.0f, rowMin.y + rowH),
                                  MapTheme::kRowHoverBg, 3.0f);
                // Collapsed: the label is not drawn, so the icon alone has to
                // say what the row is. Threshold matches the one the label and
                // eye fade out on, so the tooltip takes over exactly when the
                // text stops being readable.
                if (easedOpen <= 0.02f) {
                    tipText   = labels[i];
                    tipAnchor = ImVec2(rowMin.x + rowW + 8.0f, rowMin.y);
                }
            }

            // Off state is dim, never a second asset: one PNG tinted by alpha
            // keeps the eye and the category art reading as the same set, and
            // a slash overlay fights the art at this size.
            const float    dim      = on ? 1.0f : 0.35f;
            const ImU32    iconTint = IM_COL32(255, 255, 255, static_cast<int>(255 * dim));
            // Legend rows are body text, so they follow Appearance > Body Color
            // like the list panel's rows. Only the dim "off" state stays a fixed
            // muted token: it reads as disabled, not as themed text.
            const ImU32    textCol  = on ? (hovered ? settings->BodyColHover() : settings->BodyCol())
                                         : MapTheme::kMutedText;

            // Category icon. Blank entries reserve the column so every label
            // starts on the same x.
            if (kModeIcons[i][0] != '\0') {
                if (auto* srv = TextureManager::GetIconByName(kModeIcons[i])) {
                    const float iy = rowMin.y + (rowH - iconSz) * 0.5f;
                    // Same "3D Icons" treatment as the map pins and radar icons.
                    // The eye below stays a raw AddImage on purpose: its alpha is
                    // the shown/hidden readout, and shading would fight that.
                    MapDraw::DrawIconFx(dl, srv,
                                        ImVec2(rowMin.x, iy),
                                        ImVec2(rowMin.x + iconSz, iy + iconSz),
                                        iconTint);
                }
            }

            // Label, count and eye all belong to the expanded state: they fade
            // together and are skipped outright once collapsed. The category
            // icon alone carries the on/off readout in the strip (dimmed =
            // hidden), which is why the eye can go without losing information.
            if (easedOpen > 0.02f) {
                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), fs,
                                          ImVec2(rowMin.x + iconSz + gap, rowMin.y + (rowH - fs) * 0.5f),
                                          MapDraw::WithAlpha(textCol, easedOpen), labels[i].c_str());

                if (eyeSrv) {
                    const float ex = rowMin.x + rowW - eyeSz;
                    const float ey = rowMin.y + (rowH - eyeSz) * 0.5f;
                    dl->AddImage(reinterpret_cast<ImTextureID>(eyeSrv),
                                 ImVec2(ex, ey), ImVec2(ex + eyeSz, ey + eyeSz),
                                 ImVec2(0, 0), ImVec2(1, 1),
                                 MapDraw::WithAlpha(iconTint, easedOpen));
                }
            }
        }

        // ---- Settings button -------------------------------------------
        // Present in BOTH states, laid out like one more row so it lines up
        // with the category icons in the collapsed strip. Opens the same HUD
        // Settings window the X hotkey does; the Legend is where a user looks
        // when they want to change what the map shows, so it is the natural
        // place to reach the rest of the options from.
        {
            ImGui::Spacing();
            const ImVec2 setMin = ImGui::GetCursorScreenPos();

            ImGui::PushID("filterSettings");
            const bool setClicked = ImGui::InvisibleButton("##settings", ImVec2(rowW, rowH));
            const bool setHovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (setClicked) SettingsMenu::Toggle();

            if (setHovered) {
                dl->AddRectFilled(ImVec2(setMin.x - 4.0f, setMin.y),
                                  ImVec2(setMin.x + rowW + 4.0f, setMin.y + rowH),
                                  MapTheme::kRowHoverBg, 3.0f);
            }

            // Full tint always: unlike a category row this has no on/off state
            // to read, so dimming it would imply one.
            if (auto* setSrv = TextureManager::GetIconByName("settings")) {
                const float iy = setMin.y + (rowH - iconSz) * 0.5f;
                MapDraw::DrawIconFx(dl, setSrv,
                                    ImVec2(setMin.x, iy),
                                    ImVec2(setMin.x + iconSz, iy + iconSz),
                                    IM_COL32_WHITE);
            }

            std::string setLabel = LocalizationManager::GetSingleton()->GetLabel("ListPanel.Settings");
            if (setLabel == "ListPanel.Settings") setLabel = "Settings";

            if (easedOpen > 0.02f) {
                const ImU32 setCol = setHovered ? settings->BodyColHover() : settings->BodyCol();
                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), fs,
                                          ImVec2(setMin.x + iconSz + gap,
                                                 setMin.y + (rowH - fs) * 0.5f),
                                          MapDraw::WithAlpha(setCol, easedOpen),
                                          setLabel.c_str());
            } else if (setHovered) {
                // Collapsed: same tooltip treatment as the category rows.
                tipText   = setLabel;
                tipAnchor = ImVec2(setMin.x + rowW + 8.0f, setMin.y);
            }
        }

        // Collapsed tooltip. Hand-drawn rather than ImGui::SetTooltip: a tooltip
        // is its own window, and this panel sits on the map where an extra
        // window would land in a different z-slot than the strip it belongs to.
        // Colours come from Appearance > Global Styles, the same ones the Legend
        // panel itself uses -- not the fixed kPopup* tokens the right-click
        // menus take. The tooltip is part of THIS panel (it is the collapsed
        // strip's label, standing in for text the panel would otherwise draw
        // inline), so it follows the panel's theme rather than reading as
        // separate chrome.
        //
        // FOREGROUND draw list, not the window one. GetWindowDrawList clips to
        // the window rect, and the tooltip anchors past the panel's right edge
        // by design -- on the window list it was drawn and then clipped away
        // every frame, which looked like the hover never firing.
        if (!tipText.empty()) {
            ImDrawList* tipDl = ImGui::GetForegroundDrawList();
            const std::string& tip  = tipText;
            const ImVec2       tsz  = ImGui::CalcTextSize(tip.c_str());
            constexpr float    tpad = 6.0f;
            const ImVec2 tmn = tipAnchor;
            const ImVec2 tmx = ImVec2(tmn.x + tsz.x + tpad * 2.0f,
                                      tmn.y + tsz.y + tpad * 2.0f);
            // The kPopup* tokens are ImVec4 (they exist for PushStyleColor);
            // draw-list calls take packed ImU32.
            // Square corners: the tooltip is a small transient chip, and the
            // rounding read as a bubble against the panel's own square edges.
            tipDl->AddRectFilled(tmn, tmx, ImGui::GetColorU32(settings->BgColVec4()));
            tipDl->AddRect(tmn, tmx, ImGui::GetColorU32(settings->BorderColVec4()), 0.0f, 0, 1.0f);
            tipDl->AddText(ImVec2(tmn.x + tpad, tmn.y + tpad),
                           settings->BodyCol(), tip.c_str());
        }

        ImVec2 pos = ImGui::GetWindowPos();
        settings->filterPosX = static_cast<std::int32_t>(pos.x);
        settings->filterPosY = static_cast<std::int32_t>(pos.y);

        const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (_wasDraggingFilter && !down) {
            settings->SavePositions();
        }
        _wasDraggingFilter = down && ImGui::IsWindowFocused();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
}

void MapMarkerRenderer::OpenTerrainMenu() {
    // Game-thread entry from InputHook (right-button press on empty terrain).
    // Capture the cursor at the right-click so we can restore it before calling
    // PlaceMarker if the live cursor has drifted onto the menu by click-time.
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        _terrainMenuCursorX = cursor->cursorPosX;
        _terrainMenuCursorY = cursor->cursorPosY;
    }
    // Defer the actual OpenPopup to the render thread; see DrawTerrainMenu.
    _openTerrainMenu.store(true);
}

void MapMarkerRenderer::DrawTerrainMenu(bool isLocalMap) {
    // Empty-terrain right-click menu, shared by world and local maps.
    //   "Add Marker Here"      - world: engine PlaceMarker; local: raycast.
    //   "Set Destination Here" - greyed; bare-terrain destinations don't exist.
    // Position the popup at the engine cursor so it appears where the user
    // right-clicked (ImGui's auto-position uses io.MousePos, which we don't
    // drive on the map).
    auto* settings = Settings::GetSingleton();
    if (_openTerrainMenu.exchange(false)) {
        ImVec2 dispSize = ImGui::GetIO().DisplaySize;
        if (auto* cursor = RE::MenuCursor::GetSingleton()) {
            const float cw = cursor->screenWidthX;
            const float ch = cursor->screenWidthY;
            if (cw > 0.0f && ch > 0.0f) {
                ImGui::SetNextWindowPos(ImVec2((cursor->cursorPosX / cw) * dispSize.x,
                                               (cursor->cursorPosY / ch) * dispSize.y));
            }
        }
        ImGui::OpenPopup("TerrainContextMenu");
    }

    // CHROME, NOT CONTENT (2026-08-25). Context menus are pinned to the fixed
    // kPopup* theme on EVERY channel, background and border included. They used
    // to take bg/border from Appearance > Global Styles like the panels do, and
    // that was wrong: a menu is a transient interaction surface, not a surface
    // the user is decorating. With a red card set in the Visuals tile the menu
    // came up red too and read as part of the card. The hover band and disabled
    // rows are also tuned against a dark ground, so a bright user bg costs
    // contrast the card never loses. Deliberately NOT user-settable.
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       MapTheme::kPopupBg());
    ImGui::PushStyleColor(ImGuiCol_Border,        MapTheme::kPopupBorder());
    ImGui::PushStyleColor(ImGuiCol_Text,          MapTheme::kPopupText());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MapTheme::kPopupHoverBg());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  MapTheme::kPopupHoverBg());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(8.0f,  6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    if (ImGui::BeginPopup("TerrainContextMenu")) {
        ImGui::SetWindowFontScale(std::clamp(settings->bodyScale, 0.5f, 3.0f));
        if (ImGui::MenuItem(LocalizationManager::Get("Menu.AddMarkerHere").c_str())) {
            // Reuse vanilla's exact placement path. MapMenu::PlaceMarker
            // (RELOCATION_ID 53113) reads the current cursor from the engine's
            // screen-data singleton, unprojects to a ray, ray-picks the map
            // terrain, and calls the SetDestination helper (SE 39458 / AE 40535)
            // with the precise hit position — which is the function PlaceMarkerHook
            // already detours, so the note dialog opens for that point. No projection math of our own.
            // The pick runs over a few frames (PlaceMarker arms a countdown),
            // matching vanilla timing; the cursor is still parked here because
            // the right-click froze it (MapLookRemapHook). See
            // findings/findings-placemarker.md.
            _pendingTerrainPlace.store(true);
        }
        // Always disabled: bare terrain has no location to travel to. Same
        // wording as the other two menus -- one destination verb everywhere.
        ImGui::BeginDisabled();
        ImGui::MenuItem(LocalizationManager::Get("Menu.SetDestination").c_str());
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);

    // Fire the queued placement after the popup closed this frame. Deferring by
    // one flag-hop keeps the PlaceMarker call out of the BeginPopup scope and
    // ensures the popup's own click isn't still being processed by the engine.
    if (_pendingTerrainPlace.exchange(false)) {
        if (auto* ui = RE::UI::GetSingleton()) {
            if (auto mm = ui->GetMenu<RE::MapMenu>()) {
                // Both placement paths read the LIVE cursor, not a passed
                // position. By click-time the cursor has drifted onto the menu
                // item (~24px off), so restore it to the right-click point
                // captured in OpenTerrainMenu, place, then put it back.
                auto* cursor = RE::MenuCursor::GetSingleton();
                const bool restore = cursor && _terrainMenuCursorX >= 0.0f;
                float liveX = 0.0f, liveY = 0.0f;
                if (restore) {
                    liveX = cursor->cursorPosX;
                    liveY = cursor->cursorPosY;
                    cursor->cursorPosX = _terrainMenuCursorX;
                    cursor->cursorPosY = _terrainMenuCursorY;
                }

                if (isLocalMap) {
                    // Local map runs its own cursor raycast (interior only).
                    LocalMapClickHook::PlaceAtCursor();
                } else {
                    // World map reuses the engine's PlaceMarker, which lands in
                    // PlaceMarkerHook. Left-click placement is off, so allow
                    // exactly this one through.
                    PlaceMarkerHook::AllowNextPlacement();
                    using PlaceMarkerFn = void(*)(RE::MapMenu*);
                    REL::Relocation<PlaceMarkerFn> placeMarker{ RELOCATION_ID(52226, 53113) };
                    placeMarker(mm.get());
                }

                if (restore) {
                    cursor->cursorPosX = liveX;
                    cursor->cursorPosY = liveY;
                }
            }
        }
    }
}

void MapMarkerRenderer::OpenCustomContextMenu(std::uint32_t id) {
    // Game-thread entry from InputHook (right-button press over one of our own
    // custom pins, world or local map). Cache the id now; the popup itself
    // opens on the render thread in DrawCustomContextMenu (OpenPopup must run
    // inside the ImGui frame, same pattern as OpenTerrainMenu).
    _customContextMarkerId = id;
    _openCustomContextMenu.store(true);
}

void MapMarkerRenderer::DrawCustomContextMenu() {
    // Right-click menu for our own custom pins (world + local map). Same three
    // actions the old hover-panel buttons drove (Set/Clear Destination, Rename,
    // Remove), now behind a right-click menu instead of always-visible rows —
    // mirrors HoverImageRenderer's "VanillaContextMenu" styling/structure so
    // both menus read as one family. Position at the engine cursor like
    // DrawTerrainMenu; the pin that was right-clicked is under the cursor.
    if (_openCustomContextMenu.exchange(false)) {
        ImVec2 dispSize = ImGui::GetIO().DisplaySize;
        if (auto* cursor = RE::MenuCursor::GetSingleton()) {
            const float cw = cursor->screenWidthX;
            const float ch = cursor->screenWidthY;
            if (cw > 0.0f && ch > 0.0f) {
                ImGui::SetNextWindowPos(ImVec2((cursor->cursorPosX / cw) * dispSize.x,
                                               (cursor->cursorPosY / ch) * dispSize.y));
            }
        }
        ImGui::OpenPopup("CustomContextMenu");
    }

    // CHROME, NOT CONTENT (2026-08-25). Context menus are pinned to the fixed
    // kPopup* theme on EVERY channel, background and border included. They used
    // to take bg/border from Appearance > Global Styles like the panels do, and
    // that was wrong: a menu is a transient interaction surface, not a surface
    // the user is decorating. With a red card set in the Visuals tile the menu
    // came up red too and read as part of the card. The hover band and disabled
    // rows are also tuned against a dark ground, so a bright user bg costs
    // contrast the card never loses. Deliberately NOT user-settable.
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       MapTheme::kPopupBg());
    ImGui::PushStyleColor(ImGuiCol_Border,        MapTheme::kPopupBorder());
    ImGui::PushStyleColor(ImGuiCol_Text,          MapTheme::kPopupText());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MapTheme::kPopupHoverBg());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  MapTheme::kPopupHoverBg());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(8.0f,  6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    const bool popupOpen = ImGui::BeginPopup("CustomContextMenu");
    // Feeds IsCustomMenuActive() so the hover panels stay hidden while the
    // menu is up, and reappear the frame after it closes.
    _customMenuVisible = popupOpen;
    if (popupOpen) {
        ImGui::SetWindowFontScale(std::clamp(Settings::GetSingleton()->bodyScale, 0.5f, 3.0f));
        const std::uint32_t id = _customContextMarkerId;

        // Note (header label) and isDestination are both read live rather than
        // cached at open time, so a Rename/destination change elsewhere is
        // reflected immediately if the popup stays open across frames — same
        // reasoning as the vanilla menu's live IsVanillaDestination/IsFavorite
        // reads. If the marker was removed since the menu opened, hasMarker
        // is false and the actions below just no-op harmlessly.
        std::string note;
        bool isDest = false;
        bool hasMarker = false;
        // Context-menu target: never the synthesized destination entry.
        for (const auto& m : MarkerManager::GetSingleton()->GetUserMarkers()) {
            if (m.id != id) continue;
            note = ResolveMarkerName(m);
            isDest = m.isDestination;
            hasMarker = true;
            break;
        }

        ImGui::TextColored(MapTheme::kPopupHeaderText(), "%s", hasMarker ? note.c_str() : "");
        ImGui::TextColored(MapTheme::kMutedTextV(), "%s",
                           LocalizationManager::Get("Labels.CustomMarker").c_str());
        ImGui::Separator();

        if (!hasMarker) ImGui::BeginDisabled();
        if (ImGui::MenuItem(LocalizationManager::Get(isDest ? "Menu.RemoveDestination"
                                                             : "Menu.SetDestination").c_str())) {
            logger::debug("[CustomContextMenu] Toggle Destination clicked for id {}", id);
            MarkerManager::GetSingleton()->SetDestination(id, !isDest);
        }

        // Localized "Rename" row label (ListPanel.Rename).
        if (ImGui::MenuItem(LocalizationManager::Get("ListPanel.Rename").c_str())) {
            logger::debug("[CustomContextMenu] Rename clicked for id {}", id);
            BeginEditMarker(id);
        }

        if (ImGui::MenuItem(LocalizationManager::Get("Menu.RemoveMarker").c_str())) {
            logger::debug("[CustomContextMenu] Remove clicked for id {}", id);
            MarkerManager::GetSingleton()->RemoveMarker(id);
        }
        if (!hasMarker) ImGui::EndDisabled();

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}

void MapMarkerRenderer::DrawNoteDialog() {
    // First frame after OpenNoteDialog: force ImGui's left-button state to
    // released. The user is still physically holding the click that opened the
    // dialog, so without this the next press on Combo/Checkbox doesn't register
    // (ImGui sees "still down" and treats the in-widget press as a continuation).
    if (_resetImGuiMouseOnNextDraw) {
        auto& io = ImGui::GetIO();
        io.MouseDown[0] = false;
        io.AddMouseButtonEvent(0, false);
        _resetImGuiMouseOnNextDraw = false;
    }

    ImVec2 dispSize = ImGui::GetIO().DisplaySize;

    // Width stays fixed (drives inputW/grid layout below); height is now
    // AlwaysAutoResize (see flags below) so it fits whatever content mode
    // draws instead of a hand-tuned constant that drifted stale (combo vs.
    // icon grid, single-line vs. multiline note, heading font on/off).
    // The 340 base was tuned at ImGui's 13px default font; since FontSize is
    // INI-driven now, fold the actual base size in or the grid clips off the
    // right edge.
    auto* settings = Settings::GetSingleton();
    const float bodyScale  = std::clamp(settings->bodyScale,  0.5f, 3.0f);
    const float titleScale = std::clamp(settings->titleScale, 0.5f, 3.0f);
    const float fontScale  = ImGui::GetFontSize() / 13.0f;
    float dlgW = 340.0f * bodyScale * fontScale;
    // Both world and interior modes now use the icon grid picker (previously
    // world-map used a one-line text combo). Make the window wide enough for
    // a 10-cell row. Keep in sync with kPerRow/kCellSz passed to
    // DrawCategoryGrid below.
    if (_editingVanillaFormID == 0) {
        const auto& style = ImGui::GetStyle();
        const float cellW = 38.0f + style.ItemSpacing.x;
        dlgW = std::max(dlgW, cellW * 10.0f + style.WindowPadding.x * 2.0f);
    }
    // Title width. The width above knows the grid and the body scale but never
    // measured the TITLE, and the window is pinned to dlgW by the size
    // constraints below (AlwaysAutoResize can only grow the height), so a long
    // marker name in "Note: %s" had nowhere to go and clipped at the border.
    // titleScale did not enter dlgW at all either, so raising it clipped sooner.
    //
    // Measured on the font and size the title is ACTUALLY drawn with a few lines
    // into the Begin() block below. CalcTextSizeA takes both, unlike
    // CalcTextSize which is stuck on the current font at the base size.
    //
    // The size is the MEASURE FONT'S OWN FontSize * titleScale, not
    // GetFontSize() * titleScale. The heading font is loaded at
    // [Localization] HeadingFontSize, which is larger than FontSize by
    // definition (FontManager only builds it when headingSize > baseFontSize),
    // and PushFont + SetWindowFontScale(titleScale) draws at that larger size.
    // Measuring against the base size under-measures by the heading/base ratio,
    // which at the default 18/24 is a third short -- enough that widening the
    // dialog by it looks like it did nothing. The dlgHEstimate block below
    // makes the same correction for the height.
    {
        ImFont* measureFont = FontManager::GetSingleton()->GetHeadingFont();
        if (!measureFont) measureFont = ImGui::GetFont();
        const float titleFS = measureFont->FontSize * titleScale;

        // Same strings the draw below produces, so the measurement can't drift
        // from what is printed.
        std::string titleText;
        if (_editingVanillaFormID != 0) {
            titleText = "Note: " + _vanillaNoteName;
        } else {
            const bool  renameMode = (_editingMarkerId != 0);
            const char* key      = renameMode ? "Dialog.RenameMarker" : "Dialog.AddNewMarker";
            const char* fallback = renameMode ? "Edit Marker"         : "Add New Marker";
            titleText = LocalizationManager::GetSingleton()->GetLabel(key);
            if (titleText == key) titleText = fallback;
        }

        const float titleW = measureFont->CalcTextSizeA(titleFS, FLT_MAX, 0.0f,
                                                        titleText.c_str()).x;
        const auto& style  = ImGui::GetStyle();
        const float needed = titleW + style.WindowPadding.x * 2.0f;
        // Cap at 80% of the display: a marker name can be arbitrarily long
        // (mods name things freely), and a dialog wider than the screen is
        // worse than a clipped title. Past the cap the title clips as it did
        // before, the honest failure for a name nothing could fit. The grid
        // minimum still wins if it is larger, so the picker never clips.
        const float maxDlgW = dispSize.x * 0.8f;
        if (needed > dlgW) dlgW = std::min(needed, std::max(dlgW, maxDlgW));
    }
    // Rough estimate, position-clamp only (not the actual window size —
    // AlwaysAutoResize below fits the real content height after Begin()).
    // Just needs to be in the right ballpark so the dialog doesn't spawn
    // mostly off-screen on the first frame near a map edge.
    float dlgHEstimate = 190.0f * bodyScale * fontScale;
    if (_editingVanillaFormID == 0) dlgHEstimate += 150.0f * bodyScale * fontScale; // icon grid
    if (_editingVanillaFormID != 0) dlgHEstimate += ImGui::GetFontSize() * bodyScale * 3.5f; // multiline note
    if (ImFont* hf = FontManager::GetSingleton()->GetHeadingFont()) {
        dlgHEstimate += std::max(0.0f, hf->FontSize - ImGui::GetFontSize()) * titleScale;
    }

    // Open next to the clicked spot (normalized pos captured in OpenNoteDialog),
    // offset down-right a little so the dialog doesn't sit directly on the pin,
    // then clamp so it stays fully on-screen near map edges. Fall back to center
    // if no cursor pos was captured.
    float dlgX, dlgY;
    if (_noteDialogX >= 0.0f && _noteDialogY >= 0.0f) {
        dlgX = _noteDialogX * dispSize.x + 16.0f;
        dlgY = _noteDialogY * dispSize.y + 16.0f;
        dlgX = std::clamp(dlgX, 4.0f, dispSize.x - dlgW - 4.0f);
        dlgY = std::clamp(dlgY, 4.0f, dispSize.y - dlgHEstimate - 4.0f);
    } else {
        dlgX = (dispSize.x - dlgW) * 0.5f;
        dlgY = (dispSize.y - dlgHEstimate) * 0.5f;
    }

    ImGui::SetNextWindowPos(ImVec2(dlgX, dlgY), ImGuiCond_Always);
    // Fixed width, auto height: constrain X to dlgW and let Y float so
    // AlwaysAutoResize only grows/shrinks the vertical axis to fit content.
    ImGui::SetNextWindowSizeConstraints(ImVec2(dlgW, 0.0f), ImVec2(dlgW, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(1.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_AlwaysAutoResize;

    // CHROME, NOT CONTENT. Same rule as the right-click menus: this is a
    // transient dialog you dismiss, not a surface the user is decorating, and
    // its buttons/frame/checkmark are all tuned against a dark ground. Bg and
    // border used to follow Appearance > Global Styles, so a red-themed card
    // produced a red dialog with kDlg* buttons sitting on it. Not user-settable.
    // (The Legend panel keeps following Appearance: it is a persistent panel the
    // user places and keeps open, not a transient one.)
    ImGui::PushStyleColor(ImGuiCol_WindowBg,       MapTheme::kDlgBg());
    ImGui::PushStyleColor(ImGuiCol_Border,         MapTheme::kDlgBorder());
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        MapTheme::kDlgFrameBg());
    ImGui::PushStyleColor(ImGuiCol_Button,         MapTheme::kDlgBtnBg());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  MapTheme::kDlgBtnHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   MapTheme::kDlgBtnActive());
    ImGui::PushStyleColor(ImGuiCol_CheckMark,      MapTheme::kDlgCheckMark());
    ImGui::PushStyleColor(ImGuiCol_Text,           MapTheme::kDlgText());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(8.0f,  8.0f));

    const bool vanillaMode = (_editingVanillaFormID != 0);

    if (ImGui::Begin("##NoteDialog", nullptr, flags)) {
        // In vanilla mode the title is the marker's own name (it owns its title);
        // the note below is our annotation, not a replacement title.
        // Title line on title scale (heading font when the INI loads one),
        // everything below on body scale.
        ImFont* dlgTitleFont = FontManager::GetSingleton()->GetHeadingFont();
        if (dlgTitleFont) ImGui::PushFont(dlgTitleFont);
        ImGui::SetWindowFontScale(titleScale);
        if (vanillaMode) {
            // "Note: %s" kept hardcoded — a %s format string in the translation
            // file is fragile (a bad edit crashes the printf), not worth it here.
            ImGui::TextColored(MapTheme::kDlgTitleText(), "Note: %s", _vanillaNoteName.c_str());
        } else {
            // Renaming an existing pin vs naming a fresh placement are different
            // actions; only the edit path (_editingMarkerId set by
            // BeginEditMarker / the Rename row) says "Rename Marker".
            const bool renameMode = (_editingMarkerId != 0);
            const char* key      = renameMode ? "Dialog.RenameMarker" : "Dialog.AddNewMarker";
            const char* fallback = renameMode ? "Edit Marker"         : "Add New Marker";
            std::string title = LocalizationManager::GetSingleton()->GetLabel(key);
            if (title == key) title = fallback;
            ImGui::TextColored(MapTheme::kDlgTitleText(), "%s", title.c_str());
        }
        if (dlgTitleFont) ImGui::PopFont();
        ImGui::SetWindowFontScale(bodyScale);
        ImGui::Separator();
        ImGui::Spacing();

        const float btnW    = 70.0f * bodyScale;
        // Input + combo sit on their own rows, so they should span the full
        // content width. Reserving button width here (the old behaviour) left a
        // dead gap to the right since OK/Cancel live far below, not beside them.
        const float inputW  = ImGui::GetContentRegionAvail().x;

        // SetKeyboardFocusHere only once when the dialog opens, so we don't
        // steal focus from the Combo box when the user clicks it.
        if (_noteFocusRequested) {
            ImGui::SetKeyboardFocusHere();
            if (ImGui::GetIO().WantTextInput) {
                _noteFocusRequested = false;
            }
        }

        // Vanilla notes are prose (can be long), so they edit in a multiline
        // box where Enter inserts a newline and the whole text is visible;
        // confirm is the OK button. Custom-marker titles stay single-line with
        // Enter-to-confirm.
        bool confirmed = false;
        const bool inputVerbose = Settings::GetSingleton()->inputVerboseLog;
        const int inputCharacters = inputVerbose ? ImGui::GetIO().InputQueueCharacters.Size : 0;
        // Compare content, not just length: replacing a selection can leave
        // the byte count unchanged. Do not copy note text when logging is off.
        const std::string noteBefore = inputVerbose ? std::string(_noteBuffer) : std::string();
        if (vanillaMode) {
            // Soft-wrapped: NoteWrapCallback inserts newlines at ImGui's own
            // break points as you type, and StripSoftWraps takes them back out
            // on commit. See those two for why the buffer is rewritten rather
            // than the display.
            //
            // The wrap width is the box's inner width: the frame padding on both
            // sides, plus a glyph of slack so a character landing exactly on the
            // edge does not sit under the box border.
            NoteWrapCtx wrapCtx;
            wrapCtx.font      = ImGui::GetFont();
            wrapCtx.fontSize  = ImGui::GetFontSize();
            wrapCtx.wrapWidth = inputW - ImGui::GetStyle().FramePadding.x * 2.0f - wrapCtx.fontSize;

            // CallbackAlways for the first frames, CallbackEdit after. An
            // EXISTING note is loaded into the buffer flat, and CallbackEdit
            // fires only on a keystroke, so without this an opened note sat
            // unwrapped until the user typed. The box width is not known until
            // the widget is laid out, so this cannot be done at load time.
            ImGuiInputTextFlags noteFlags = ImGuiInputTextFlags_CallbackEdit;
            if (_noteWrapPrimeFrames > 0) {
                noteFlags = ImGuiInputTextFlags_CallbackAlways;
                --_noteWrapPrimeFrames;
            }
            ImGui::InputTextMultiline("##note", _noteBuffer, sizeof(_noteBuffer),
                                      ImVec2(inputW, ImGui::GetTextLineHeight() * 4.5f),
                                      noteFlags, NoteWrapCallback, &wrapCtx);
        } else {
            ImGui::SetNextItemWidth(inputW);
            confirmed = ImGui::InputText("##note", _noteBuffer, sizeof(_noteBuffer),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
        }
        if (inputVerbose && (inputCharacters > 0 || noteBefore != _noteBuffer || ImGui::IsItemActivated() ||
                             ImGui::IsItemDeactivated() || confirmed || _noteFocusRequested)) {
            logger::info(
                "[InputVerbose] note_field frame={} vanilla={} active={} focused={} activated={} deactivated={} edited={} bufferChanged={} beforeBytes={} afterBytes={} capacity={} focusRequested={} confirmed={} inputCharacters={}",
                ImGui::GetFrameCount(), vanillaMode, ImGui::IsItemActive(), ImGui::IsItemFocused(),
                ImGui::IsItemActivated(), ImGui::IsItemDeactivated(), ImGui::IsItemEdited(),
                noteBefore != _noteBuffer, noteBefore.size(), std::char_traits<char>::length(_noteBuffer),
                sizeof(_noteBuffer), _noteFocusRequested, confirmed, inputCharacters);
        }
        
        ImGui::Spacing();

        // Category lists live at file scope so BeginEditMarker can reverse-map
        // iconType->index. Pick the table matching where this marker lives.
        const auto& catTbl        = CategoriesFor(_noteDialogInterior);
        const auto* categoryNames  = catTbl.names;
        const auto* categoryValues = catTbl.values;
        const int   categoryCount  = catTbl.count;

        // Vanilla markers own their icon and aren't our destinations, so the
        // category grid + destination checkbox are hidden in vanilla-note mode.
        if (!vanillaMode) {
            // Both world and interior pick by icon now: a grid of image
            // buttons (DrawCategoryGrid, shared with the interior picker).
            // Interior skips the tooltip (waypoint glyph names are
            // exterior-flavored and just confuse an interior pick); world
            // wants it since the grid replaces what used to be a text combo.
            constexpr int   kPerRow = 10;
            constexpr float kCellSz = 30.0f;
            MapDraw::DrawCategoryGrid(categoryNames, categoryValues, categoryCount,
                                      _selectedCategory, kPerRow, kCellSz,
                                      /*showTooltip=*/!_noteDialogInterior);

            ImGui::Spacing();

            ImGui::Checkbox(LocalizationManager::Get("Dialog.SetAsDestination").c_str(),
                            &_pendingIsDestination);

            ImGui::Spacing();
        }
        
        // map the visible cursor is Skyrim's MenuCursor, while mouse down/up is
        // polled separately; ImGui sometimes has the right rect and position
        // but still misses activation. Keep the real buttons for rendering and
        // normal ImGui behavior, then mirror a release-inside hit-test here.
        const bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool leftPressed = leftDown && !_notePrevLeftDown;
        const bool leftReleased = !leftDown && _notePrevLeftDown;

        ImVec2 cursorPos(-1.0f, -1.0f);
        if (auto* cursor = RE::MenuCursor::GetSingleton()) {
            if (cursor->screenWidthX > 0.0f && cursor->screenWidthY > 0.0f) {
                cursorPos = ImVec2((cursor->cursorPosX / cursor->screenWidthX) * dispSize.x,
                                   (cursor->cursorPosY / cursor->screenWidthY) * dispSize.y);
            }
        }
        auto cursorInRect = [&](const ImVec2& mn, const ImVec2& mx) {
            return cursorPos.x >= mn.x && cursorPos.x <= mx.x
                && cursorPos.y >= mn.y && cursorPos.y <= mx.y;
        };

        const bool okClicked     = ImGui::Button(LocalizationManager::Get("Dialog.OK").c_str(), ImVec2(btnW, 0));
        const ImVec2 okMin = ImGui::GetItemRectMin();
        const ImVec2 okMax = ImGui::GetItemRectMax();

        ImGui::SameLine();

        const bool cancelClicked = ImGui::Button(LocalizationManager::Get("Dialog.Cancel").c_str(), ImVec2(btnW, 0));
        const ImVec2 cancelMin = ImGui::GetItemRectMin();
        const ImVec2 cancelMax = ImGui::GetItemRectMax();

        // Suppress the manual OK/Cancel fallback while any ImGui popup is open
        // (e.g. the category Combo dropdown). The Combo popup overlays the
        // OK/Cancel buttons, and our raw MenuCursor-in-rect test can't see the
        // popup blocking, so picking an item near the OK row (Town at index 2
        // is the first one that overlaps OK) would fire OK immediately. ImGui's
        // own Button() already returns false correctly in this case; we only
        // need to gate the manual fallback.
        const bool anyPopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        const bool okHoveredDirect = !anyPopupOpen && cursorInRect(okMin, okMax);
        const bool cancelHoveredDirect = !anyPopupOpen && cursorInRect(cancelMin, cancelMax);
        if (leftPressed) {
            // Arm only on press-inside, then fire only on release-inside. This
            // matches normal button semantics and avoids accepting drags that
            // merely pass over OK/Cancel on mouse-up.
            _noteOkArmed = okHoveredDirect;
            _noteCancelArmed = cancelHoveredDirect;
        }

        const bool okReleasedDirect = leftReleased && _noteOkArmed && okHoveredDirect;
        const bool cancelReleasedDirect = leftReleased && _noteCancelArmed && cancelHoveredDirect;
        if (leftReleased) {
            _noteOkArmed = false;
            _noteCancelArmed = false;
        }
        _notePrevLeftDown = leftDown;

        if (okClicked || okReleasedDirect || confirmed) {
            const std::uint32_t iconType =
                (_selectedCategory >= 0 && _selectedCategory < categoryCount)
                    ? categoryValues[_selectedCategory] : 0u;
            if (vanillaMode) {
                // Vanilla-note mode: store the note keyed by the vanilla ref's
                // FormID. Empty buffer clears the note (SetVanillaNote erases).
                // Soft wraps come back out here: the newlines NoteWrapCallback
                // inserted are a typing aid at THIS dialog's width, and the
                // hover panel that displays the note wraps to its own. The
                // empty test below runs on the STRIPPED string -- a buffer
                // holding only breaks and spaces strips to empty, which must
                // read as "clear the note", not a note made of whitespace.
                const std::string noteText = StripSoftWraps(_noteBuffer);
                MarkerManager::GetSingleton()->SetVanillaNote(
                    _editingVanillaFormID, noteText);
                // Show the section the row landed in. Only when a note was
                // actually stored -- an empty buffer ERASES the note, and
                // jumping to Notes to show a row that just disappeared is
                // backwards.
                if (!noteText.empty()) ShowListTab(ListTab::Notes);
                logger::debug("[NoteDialog] vanilla note confirmed for {:08X}: '{}'",
                             _editingVanillaFormID, noteText);
            } else if (_editingMarkerId != 0) {
                // Edit mode: update the existing marker in place, keep its id.
                // An empty buffer now stores an EMPTY note, which UpdateMarker
                // reads as "revert to the auto-name". It no longer bakes the
                // English string in, so the name follows the current language.
                const std::uint32_t id = _editingMarkerId;
                MarkerManager::GetSingleton()->UpdateMarker(id, std::string(_noteBuffer), iconType);
                MarkerManager::GetSingleton()->SetDestination(id, _pendingIsDestination);
            } else {
                // Empty buffer = auto-named marker: the scheme and number are
                // stored instead of the text, and the name is resolved per draw
                // so it retranslates on a language switch.
                const bool named = (_noteBuffer[0] != '\0');
                const std::uint32_t createdID = named ?
                    MarkerManager::GetSingleton()->AddMarker(
                        _pendingWorldSpace, _pendingCell, _pendingPos, iconType,
                        std::string(_noteBuffer)) :
                    MarkerManager::GetSingleton()->AddAutoNamedMarker(
                        _pendingWorldSpace, _pendingCell, _pendingPos, iconType,
                        AutoName::Custom);
                if (createdID != 0 && _pendingIsDestination) {
                    MarkerManager::GetSingleton()->SetDestination(createdID, true);
                }
                ShowListTab(ListTab::Custom);
                logger::debug("[NoteDialog] confirmed note='{}', category={}, dest={}",
                             named ? _noteBuffer : "(auto)", iconType, _pendingIsDestination);
            }
            _editingMarkerId = 0;
            _editingVanillaFormID = 0;
            _noteDialogOpen = false;
        }

        if (_noteDialogOpen && (cancelClicked || cancelReleasedDirect)) {
            logger::debug("[NoteDialog] cancelled");
            _editingMarkerId = 0;
            _editingVanillaFormID = 0;
            _noteDialogOpen = false;
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(8);
}
