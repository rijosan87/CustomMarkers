#include "AutomationTelemetry.h"
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
#include "CompanionTracker.h"
#include "SettingsMenu.h"
#include "InputHandler.h"
#include "MenuUtils.h"
#include "UISound.h"
#include "UIScale.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>

namespace {
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

        while (cut > 0 && (static_cast<unsigned char>(a_text[cut]) & 0xC0) == 0x80) --cut;

        if (const std::size_t sp = a_text.rfind(' ', cut); sp != std::string::npos && sp >= cut / 2) {
            cut = sp;
        }
        if (cut == 0) return kEllipsis;

        return a_text.substr(0, cut) + kEllipsis;
    }

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

        std::string text(a_data->Buf, a_data->Buf + a_data->BufTextLen);

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

        std::string wrapped;
        wrapped.reserve(flat.size() + 8);
        int addedBeforeCursor = 0;
        const char* begin = flat.c_str();
        const char* end   = begin + flat.size();
        const char* line  = begin;
        while (line < end) {
            const char* brk = ctx->font->CalcWordWrapPositionA(scale, line, end, ctx->wrapWidth);
            if (brk >= end) { wrapped.append(line, end); break; }
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

    std::string StripSoftWraps(const char* a_text) {
        std::string out(a_text);
        std::replace(out.begin(), out.end(), '\n', ' ');
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

    constexpr std::uintptr_t kMarkerArrayOffset = 0x30470;
    constexpr std::size_t    kMarkerStride      = 0x38;
    // +0x30478 is the BSTArray capacity, not the count.
    inline std::ptrdiff_t MarkerCountOffset() { return 0x30480; }

    struct MarkerEntry {
        void*         namePtr;  // +0x00  unused here
        RE::RefHandle handle;   // +0x08
        std::uint32_t pad0C;    // +0x0C
        RE::BSFixedString nameStr; // +0x10  unused here
        std::uint32_t type;     // +0x18  0 = base MapMarker, 1 = stacked alias
    };

    constexpr bool IsDungeonMarkerType(std::uint16_t t) {
        switch (t) {
            case 4: case 6: case 7: case 8: case 9: case 10: case 12: case 15:
                return true;   // Cave, Fort, NordicRuin, DwemerRuin, Shipwreck, Grove, DragonLair, Mine
            default: return false;
        }
    }

    // Filled disc + dark checkmark. Disc = the color knob (Settings), check
    // stroke is a fixed dark outline like every other badge.
    inline void DrawClearedCheck(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 outline, float scale = 1.0f) {
        dl->AddCircleFilled(c, r, fill);
        dl->AddCircle(c, r, outline, 0, 1.2f * scale);
        const ImVec2 p0{ c.x - r * 0.45f, c.y + r * 0.05f };
        const ImVec2 p1{ c.x - r * 0.10f, c.y + r * 0.40f };
        const ImVec2 p2{ c.x + r * 0.50f, c.y - r * 0.40f };
        dl->AddLine(p0, p1, outline, std::max(1.5f * scale, r * 0.22f));
        dl->AddLine(p1, p2, outline, std::max(1.5f * scale, r * 0.22f));
    }

    constexpr const char* kWorldCategoryNames[] = {
        "Custom Marker", "City", "Town", "Settlement", "Cave", "Camp", "Fort",
        "Nordic Ruin", "Dwemer Ruin", "Shipwreck", "Grove", "Landmark",
        "Dragon Lair", "Farm", "Wood Mill", "Mine", "Imperial Camp",
        "Stormcloak Camp", "Doomstone", "Wheat Mill", "Smelter", "Temple",
        "Stable", "Clearing", "Altar", "Rock", "Lighthouse", "Dwelling",
        "Docks", "Shrine",
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
    { std::scoped_lock lock{ _controllerContextMutex }; _pendingControllerNoteActivation = false; }

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
    { std::scoped_lock lock{ _controllerContextMutex }; _pendingControllerNoteActivation = false; }

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
    { std::scoped_lock lock{ _controllerContextMutex }; _pendingControllerNoteActivation = false; }

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
    settings->SavePositions();
    logger::debug("List panel visibility preference saved: visible={}", visible);
}

void MapMarkerRenderer::Draw() {
    auto ui = RE::UI::GetSingleton();

    if (IsMapFading()) {
        _activeMarkerId = 0;
        _pinHovered.store(false);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        _openTerrainMenu.store(false);
        _openCustomContextMenu.store(false);
        _pendingTerrainPlace.store(false);
        // Don't touch _showListPanel: the fader also runs during map OPEN, so
        // clearing it here would defeat the default-open state every time.
        if (_noteDialogOpen) {
            _noteDialogOpen = false;
        }
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
            _filterVisible = false;
        }
        _pendingControllerTab.store(-1);
        _pendingControllerFilter.store(-1);
        return;
    }

    if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
        QuestTracker::GetSingleton()->Reset();
        _activeMarkerId = 0;
        _pinHovered.store(false);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        _openTerrainMenu.store(false);
        _openCustomContextMenu.store(false);
        _pendingTerrainPlace.store(false);
        if (_noteDialogOpen) {
            logger::debug("[NoteDialog] map closed while dialog open - force-resetting state");
            _noteDialogOpen = false;
        }
        _showListPanel.store(Settings::GetSingleton()->listPanelVisible);
        for (auto& sy : _listScrollY) sy = 0.0f;
        _listScrollMax = 0.0f;
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
            _filterVisible = false;
            _rowIds.clear();
            _rowDeleteRects.clear();
            _destRowValid = false;
        }
        _pendingPanTarget.store(0);
        _pendingControllerTab.store(-1);
        _pendingControllerFilter.store(-1);
        _pendingPanDestPos.store(false);
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
            _filterVisible = false;
            _rowIds.clear();
            _rowDeleteRects.clear();
            _destRowValid = false;
        }
        _pendingPanTarget.store(0);
        _pendingControllerTab.store(-1);
        _pendingControllerFilter.store(-1);
        _pendingPanDestPos.store(false);
        return;
    }

    auto mapMenu = ui->GetMenu<RE::MapMenu>();
    if (!mapMenu) {
        std::lock_guard<std::mutex> lk(_panelMutex);
        _panelVisible = false;
        _filterVisible = false;
        _pendingControllerTab.store(-1);
        _pendingControllerFilter.store(-1);
        return;
    }

    bool isLocalMap = false;
    {
        const auto base = reinterpret_cast<std::uintptr_t>(mapMenu.get());
        isLocalMap = *reinterpret_cast<const bool*>(base + 0x3045c);
        AutomationTelemetry::MapMode(isLocalMap);
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

    bool isDialogOpen = MenuUtils::IsMapModalOpen();

    if (isDialogOpen) {
        _activeMarkerId = 0;
        _pinHovered.store(false);
        _hoveredMarkerId.store(0);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        _openTerrainMenu.store(false);
        _openCustomContextMenu.store(false);
        _pendingTerrainPlace.store(false);
        // _showListPanel intentionally kept: this branch early-returns so the
        // panel isn't drawn under the modal, and it comes back when it closes.

        _noteDialogOpen = false;
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
            _filterVisible = false;
            _rowIds.clear();
            _rowDeleteRects.clear();
            _destRowValid = false;
        }
        _pendingPanTarget.store(0);
        _pendingControllerTab.store(-1);
        _pendingControllerFilter.store(-1);
        _pendingPanDestPos.store(false);
        if (mouse.x >= 0.0f && mouse.y >= 0.0f) {
            MapDraw::RequestCursor(mouse.x, mouse.y);
        }
        return;
    }

    if (isLocalMap) {
        _activeMarkerId = 0;
        _pinHovered.store(false);
        _hoveredMarkerId.store(0);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
            _filterVisible = false;
            _rowIds.clear();
            _rowDeleteRects.clear();
            _destRowValid = false;
        }
        _pendingPanTarget.store(0);
        _pendingControllerTab.store(-1);
        _pendingControllerFilter.store(-1);
        _pendingPanDestPos.store(false);

        if (_noteDialogOpen) {
            DrawNoteDialog();
        }
        DrawTerrainMenu(true);
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

        if (m.id == kVanillaDestId) {
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

        RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(m.position, useFlatMapPlane);
        float sx, sy, sz;
        bool inFront = RE::NiCamera::WorldPtToScreenPt3(
            camData.worldToCam, camData2.port, mapPos, sx, sy, sz, 1e-5f);
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

    ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
    if (!titleFont) titleFont = ImGui::GetFont();
    const float fontRatio      = titleFont->FontSize / ImGui::GetFontSize();
    const float fontScale      = UIScale::Font();
    const float geometryScale  = UIScale::Master();
    const float titleBaseScale = std::clamp(Settings::GetSingleton()->titleScale, 0.5f, 3.0f) * fontRatio;
    const float titleScale     = titleBaseScale * fontScale;
    const float bodyScale      = std::clamp(Settings::GetSingleton()->bodyScale, 0.5f, 3.0f) * fontScale;

    const float effectivePinScale = Settings::GetSingleton()->markerIconScale *
        (std::min)(UIScale::UserMaster(), 1.5f) * UIScale::PresentationCompensation();
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
                                                                        static_cast<float>(Settings::GetSingleton()->markerHoverOffsetY),
                                                                        geometryScale, titleBaseScale);
                MapDraw::Rect keep = MapDraw::Combine(MapDraw::PinBox(v.px, v.py, geometryScale), pl.panel, 8.0f * geometryScale);
                if (MapDraw::PointInRect(mouse, keep)) active = &v;
                break;
            }
        }
        if (!active) {
            for (const auto& v : visible) {
                if (MapDraw::PointInRect(mouse, MapDraw::PinBox(v.px, v.py, geometryScale))) { active = &v; break; }
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
        if (v.isDestination) MapDraw::DrawDestinationRing(dl, v.px, v.py, timeSec, false, UIScale::Master());
    }
    for (const auto& v : destRings) {
        MapDraw::DrawDestinationRing(dl, v.px, v.py, timeSec, /*centerOnPoint=*/true, UIScale::Master());
    }

    if (Settings::GetSingleton()->enableBadges) {
        const float badgeScale = UIScale::Master();
        const float kBadgeOffsetX = UIScale::Px(12.0f);
        const float kBadgeOffsetY = UIScale::Px(12.0f);
        const float kCullMargin   = UIScale::Px(24.0f);
        constexpr float kBadgePopSec  = 0.2f;
        // Size + color are user-configurable (Settings -> Markers tab),
        // outline stays the fixed dark ring every other badge/pin uses.
        auto* markerSet = Settings::GetSingleton();
        const float kStarRadius = markerSet->favBadgeSize * badgeScale;
        const float kDotRadius  = markerSet->noteBadgeSize * badgeScale;
        const ImU32 favFill  = markerSet->FavBadgeCol();
        const ImU32 noteFill = markerSet->NoteBadgeCol();

        auto onScreen = [&](float x, float y) {
            return x >= -kCullMargin && x <= dispSize.x + kCullMargin &&
                   y >= -kCullMargin && y <= dispSize.y + kCullMargin;
        };

        auto filteredByMode = [](RE::TESObjectREFR* a_ref) {
            if (!a_ref) return false;
            auto* extra = a_ref->extraList.GetByType<RE::ExtraMapMarker>();
            if (!extra || !extra->mapData) return false;
            return MarkerFilterHook::IsMarkerFiltered(a_ref, extra->mapData->type.underlying());
        };

        auto favsAll  = MarkerManager::GetSingleton()->GetFavorites();
        auto notesAll = MarkerManager::GetSingleton()->GetVanillaNotes();

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
                              favFill, MapTheme::kFavBadgeOutline, UIScale::Px(1.0f));
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
            dl->AddCircle(dotCenter, kDotRadius * scale, MapTheme::kNoteBadgeOutline, 0, UIScale::Px(1.2f));
        }

        {
            const float kClearedRadius = markerSet->clearedBadgeSize * badgeScale;
            const ImU32 clearedFill    = markerSet->ClearedBadgeCol();
            // Top-center: no X offset, lifted a touch more than the side badges.
            const float kClearedLiftY = kBadgeOffsetY + UIScale::Px(4.0f);

            auto base   = reinterpret_cast<std::uintptr_t>(mapMenu.get());
            auto arrPtr = *reinterpret_cast<std::uintptr_t*>(base + kMarkerArrayOffset);
            const auto arrSize = arrPtr
                ? *reinterpret_cast<std::uint32_t*>(base + MarkerCountOffset()) : 0u;

            for (std::uint32_t i = 0; i < arrSize; ++i) {
                const auto* e = reinterpret_cast<const MarkerEntry*>(
                    arrPtr + static_cast<std::uintptr_t>(i) * kMarkerStride);
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
                                  kClearedRadius * scale, clearedFill, MapTheme::kClearedBadgeOutline, badgeScale);
            }
        }

        _badgesSeeded = true;
    }

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
                         effectivePinScale,
                         Settings::GetSingleton()->PinTintCol(), 1.0f, UIScale::Master());
    }

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
                const float pulseScale = UIScale::Master();
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
                    const float r0 = (r0Base + e * r0Travel) * pulseScale;
                    const float r1 = (r1Base + e * r1Travel) * pulseScale;
                    const ImU32 col0 = IM_COL32(255, 220, 120, static_cast<int>(alpha * peakA0));
                    const ImU32 col1 = IM_COL32(255, 240, 180, static_cast<int>(alpha * peakA1));
                    dl->AddCircle(ImVec2(px, py), r0, col0, 0, 2.5f * pulseScale);
                    dl->AddCircle(ImVec2(px, py), r1, col1, 0, 1.5f * pulseScale);
                };
                // Same start/travel values as before (outer 18->44, inner 12->28).
                drawWave(0.0f,     18.0f, 26.0f, 12.0f, 16.0f, 220, 180);
                drawWave(kStagger, 18.0f, 26.0f, 12.0f, 16.0f, 220, 180);
            }
        }
    }

    if (active) {
        // Scales + titleFont resolved above, shared with the keep-rect check.
        auto* hoverSet = Settings::GetSingleton();
        MapDraw::InfoPanelLayout pl = MapDraw::ComputeInfoPanel(active->px, active->py, active->note.c_str(),
                                                               dispSize, titleScale, bodyScale,
                                                               static_cast<float>(hoverSet->markerHoverOffsetX),
                                                               static_cast<float>(hoverSet->markerHoverOffsetY),
                                                               geometryScale, titleBaseScale);

        MapDraw::DrawPanelCard(dl, pl.panel.mn, pl.panel.mx, hoverSet->BgCol(), hoverSet->BorderCol(), 1.0f, geometryScale);

        // Header: note name + "Custom Marker" subtitle
        const float fs = ImGui::GetFontSize();
        MapDraw::DrawTextShadowed(dl, titleFont, fs * titleScale,
                                  ImVec2(pl.panel.mn.x + pl.padX, pl.panel.mn.y + pl.padY),
                                  hoverSet->TitleCol(), active->note.c_str(), nullptr,
                                  pl.noteWrapW);
        dl->AddText(ImGui::GetFont(), fs * bodyScale * 0.85f,
                    ImVec2(pl.panel.mn.x + pl.padX, pl.panel.mn.y + pl.padY + pl.nameH + 2.0f * geometryScale),
                    MapDraw::FadeAlpha(hoverSet->BodyCol(), MapDraw::kLabelAlpha),
                    LocalizationManager::Get("Labels.CustomMarker").c_str());
    }
    _removeButtonTarget.store(0);
    _destButtonTarget.store(0);
    _editButtonTarget.store(0);

    // Custom marker list panel (default hotkey C).
    if (_showListPanel.load()) {
        DrawListPanel();
    } else {
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
            _rowIds.clear();
            _rowDeleteRects.clear();
            _pendingControllerTab.store(-1);
        }
        // These earlier returns already exclude fades, modals, SMF, and the
        // local map. The reminder is for the player's saved hidden choice.
        auto* hintSettings = Settings::GetSingleton();
        if (!SettingsMenu::IsVisible() && !_noteDialogOpen && hintSettings->listPanelKey > 0) {
            const std::string bind = InputHandler::FormatHotkeyWithModifier(
                hintSettings->listPanelKey, hintSettings->listPanelModifier);
            std::string label = LocalizationManager::Get("ListPanel.Hint");
            if (label == "ListPanel.Hint") label = "List Panel";

            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize() * UIScale::Font();
            const ImVec2 keySize = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, bind.c_str());
            const ImVec2 labelSize = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label.c_str());
            const float padX = UIScale::Px(7.0f);
            const float padY = UIScale::Px(4.0f);
            const float gap = UIScale::Px(8.0f);
            const float margin = UIScale::Px(20.0f);
            const float keyW = keySize.x + padX * 2.0f;
            const float hintW = keyW + gap + labelSize.x + padX;
            const float hintH = std::max(keySize.y, labelSize.y) + padY * 2.0f;
            const float x = dispSize.x - margin - hintW;
            const float y = margin;
            if (x >= margin && y + hintH < dispSize.y) {
                const float rounding = UIScale::Px(2.0f);
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + hintW, y + hintH),
                                  MapDraw::WithAlpha(MapTheme::kPanelBg, 0.80f), rounding);
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + keyW, y + hintH),
                                  MapTheme::kPanelBg, rounding);
                dl->AddRect(ImVec2(x, y), ImVec2(x + keyW, y + hintH),
                            MapTheme::kPanelBorder, rounding, 0, UIScale::Px(1.0f));
                const float textY = std::floor(y + (hintH - fs) * 0.5f);
                dl->AddText(font, fs, ImVec2(x + padX, textY), MapTheme::kBodyText, bind.c_str());
                dl->AddText(font, fs, ImVec2(x + keyW + gap, textY),
                            MapTheme::kBodyText, label.c_str());
            }
        }
    }

    // Focus-mode filter rows (world map only).
    if (!isLocalMap && Settings::GetSingleton()->showFilterPanel) {
        DrawFilterPanel();
    } else {
        std::lock_guard<std::mutex> lk(_panelMutex);
        _filterVisible = false;
        _pendingControllerFilter.store(-1);
    }

    DrawTerrainMenu(isLocalMap);

    // Right-click menu for our own custom pins (world map path; local map's
    // early-return branch above calls this too).
    DrawCustomContextMenu();

    // Note entry dialog.
    if (_noteDialogOpen) {
        DrawNoteDialog();
    }

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
    if (const int tab = _pendingControllerTab.exchange(-1);
        tab >= 0 && tab < static_cast<int>(ListTab::Count)) {
        settings->listActiveTab = tab;
        settings->SavePositions();
    }

    // Rows are what the PLAYER placed: the synthesized vanilla
    // destination must never appear as a removable/renamable row.
    auto allMarkers = MarkerManager::GetSingleton()->GetUserMarkers();

    auto ui = RE::UI::GetSingleton();
    auto mapMenu = ui ? ui->GetMenu<RE::MapMenu>() : nullptr;
    RE::TESWorldSpace* currentMapWorld = mapMenu ? mapMenu->GetRuntimeData2()->worldSpace : nullptr;

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

    std::vector<ActiveQuestRow> activeQuests;
    if (settings->listShowActiveQuests) {
        QuestTracker::GetSingleton()->Refresh();
        const bool qlog = settings->questRowLog;
        std::vector<std::string> filterLog;
        for (const auto& q : QuestTracker::GetSingleton()->Get()) {
            const bool kept = !q.hasPosition || belongsToMap(q.worldSpaceID);
            if (qlog) {
                const char* kind = q.isMiscellaneous ? "misc" : "regular";
                const std::string& text = q.isMiscellaneous ? q.objectiveText : q.name;
                filterLog.push_back(std::format(
                    "[QuestFilter] {:08X} \"{}\" kind={} instance={} objective={} hasPos={} qworld={:08X} mapworld={:08X} src={} kept={}",
                    q.questID, text, kind, q.instanceID, q.objectiveIndex,
                    q.hasPosition, q.worldSpaceID,
                    currentMapWorld ? currentMapWorld->GetFormID() : 0,
                    q.posSource, kept));
            }
            if (!kept) continue;
            activeQuests.push_back(q);
        }
        if (qlog) {
            static std::vector<std::string> lastFilterLog;
            if (filterLog != lastFilterLog) {
                for (const auto& line : filterLog) {
                    logger::info("{}", line);
                }
                lastFilterLog = std::move(filterLog);
            }
        }
    }

    // List panel is only shown on the world map, filter out interior markers and markers from other maps.
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

        (void)note;
        notes.push_back({ formID, name, ref->GetPosition() });
    }

    const float geometryScale = UIScale::Master();
    const float fontScale = UIScale::Font();
    ImFont* defaultFont   = ImGui::GetFont();
    const float baseFS    = defaultFont ? defaultFont->FontSize : ImGui::GetFontSize();
    // Section headers use the heading font when the INI loads one
    // (HeadingFontSize > FontSize); TitleScale multiplies on top.
    ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
    if (!titleFont) titleFont = defaultFont ? defaultFont : ImGui::GetFont();
    const float logicalTitleFS = titleFont->FontSize * std::clamp(settings->titleScale, 0.5f, 3.0f);
    const float logicalBodyFS  = baseFS * std::clamp(settings->bodyScale, 0.5f, 3.0f);
    const float titleFS   = logicalTitleFS * fontScale;
    const float bodyFS    = logicalBodyFS * fontScale;
    const float pad       = UIScale::Px(10.0f);
    const float rowHeight = std::floor(bodyFS + UIScale::Px(11.0f));
    const float headerH   = std::floor(titleFS + UIScale::Px(12.0f));
    const float gripH     = UIScale::Px(18.0f);
    const float bandLift  = UIScale::Px(3.0f);
    const auto RowTextY = [rowHeight, bandLift](float a_y, float a_fs) {
        return std::floor(a_y - bandLift + (rowHeight - a_fs) * 0.5f);
    };
    // DrawPin uses a 32px footprint for PNGs and an 18px diameter for vector
    // pins. Normalize both paths to the row's requested icon box.
    const auto ListPinScale = [](float a_size, std::uint32_t a_iconType,
                                 ID3D11ShaderResourceView* a_srv) {
        const float baseSize = a_srv && !MapDraw::IsWaypointType(a_iconType)
            ? 32.0f
            : 2.0f * MapDraw::kPinRadius;
        return a_size / baseSize;
    };
    const float panelW    = UIScale::Px(static_cast<float>(settings->listPanelWidth));

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

    const auto StatLabel = [loc](const char* a_key, const char* a_fallback) {
        std::string label = loc->GetLabel(a_key);
        return label == a_key ? std::string(a_fallback) : label;
    };
    struct StatRow { const char* icon; std::string label; std::string value; };
    std::vector<StatRow> statRows;
    if (settings->listShowOverview) {
        statRows.reserve(4);
        statRows.push_back({ "dungeons", StatLabel("Overview.DungeonsLabel", "Dungeons"),
                             std::format("{} / {}", mapStats.dungeonsCleared, mapStats.dungeonsTotal) });
        statRows.push_back({ "locations", StatLabel("Overview.LocationsLabel", "Locations"),
                             std::format("{} / {}", mapStats.locationsDiscovered, mapStats.locationsTotal) });
        statRows.push_back({ "stones", StatLabel("Overview.StandingStonesLabel", "Standing Stones"),
                             std::format("{} / {}", mapStats.stonesFound, mapStats.stonesTotal) });
        statRows.push_back({ "bounty", StatLabel("Overview.TotalBountyLabel", "Total Bounty"),
                             std::to_string(mapStats.totalBounty) });
    }

    // Spouse first; gated with the Overview block.
    std::vector<CompanionTracker::Entry> companions;
    if (settings->listShowOverview && settings->listShowCompanions) {
        companions = CompanionTracker::Get();
        std::ranges::stable_sort(companions, [](const auto& a, const auto& b) { return a.spouse > b.spouse; });
    }
    const std::size_t miscQuestCount = static_cast<std::size_t>(std::ranges::count_if(activeQuests,
        [](const ActiveQuestRow& q) { return q.isMiscellaneous; }));
    const std::size_t regularQuestCount = activeQuests.size() - miscQuestCount;
    const bool hasMisc = miscQuestCount != 0;
    float questSectionH = 0.0f;
    if (hasMisc) {
        questSectionH += (regularQuestCount > 0 ? pad : 0.0f) + headerH + pad;
    }
    for (const auto& q : activeQuests) {
        questSectionH += q.isMiscellaneous
            ? rowHeight
            : (q.objectiveText.empty() ? rowHeight : std::floor(rowHeight * 1.8f));
    }

    const int tabCount = static_cast<int>(ListTab::Count);
    settings->listActiveTab = std::clamp(settings->listActiveTab, 0, tabCount - 1);
    const ListTab activeTab = static_cast<ListTab>(settings->listActiveTab);
    float& scrollY = _listScrollY[settings->listActiveTab];

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
    const float tabStripH = std::floor(bodyFS + UIScale::Px(14.0f));

    float contentH = 0.0f;
    switch (activeTab) {
    case ListTab::Overview:
        if (settings->listShowOverview) {
            contentH += rowHeight * static_cast<float>(statRows.size());
            if (settings->listShowCompanions) {
                contentH += pad + headerH + pad +
                            rowHeight * static_cast<float>((std::max)(companions.size(), std::size_t{ 1 }));
            }
            contentH += pad + headerH + pad + rowHeight;
        }
        if (settings->listShowActiveQuests) {
            // The quest heading carries its own count even when the stats block
            // is hidden. Match the draw pass's leading gap and heading advance.
            if (settings->listShowOverview) contentH += pad;
            contentH += headerH + pad;
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

    const float chromeH = gripH + tabStripH;

    const float maxH = dispSize.y *
        (std::clamp(settings->listPanelMaxHeightPct, 30, 95) / 100.0f);
    const float minH = dispSize.y *
        (std::clamp(settings->listPanelMinHeightPct, 0,
                    std::clamp(settings->listPanelMaxHeightPct, 30, 95)) / 100.0f);

    const float bodyMaxH = std::max(rowHeight * 3.0f, maxH - chromeH);
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

    const float margin = UIScale::Px(24.0f);
    if (settings->listPanelPosX < 0 || settings->listPanelPosY < 0) {
        settings->listPanelPosX = static_cast<std::int32_t>(dispSize.x - panelW - margin);
        settings->listPanelPosY = static_cast<std::int32_t>(margin);
    }

    auto ClampToDisplay = [dispSize, panelW, panelH](ImVec2& a_pos) {
        if (dispSize.x > 0.0f && dispSize.y > 0.0f) {
            if (panelW < dispSize.x) {
                a_pos.x = std::clamp(a_pos.x, 0.0f, dispSize.x - panelW);
            } else {
                a_pos.x = 0.0f;
            }
            if (panelH < dispSize.y) {
                a_pos.y = std::clamp(a_pos.y, 0.0f, dispSize.y - panelH);
            } else {
                a_pos.y = 0.0f;
            }
        }
    };

    ImVec2 mn(static_cast<float>(settings->listPanelPosX),
              static_cast<float>(settings->listPanelPosY));
    ClampToDisplay(mn);
    ImVec2 mx(mn.x + panelW, mn.y + panelH);

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

    const float headerStripBottom = mn.y + gripH;
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
            ClampToDisplay(mn);
            mx.x = mn.x + panelW;
            mx.y = mn.y + panelH;
        } else {
            _draggingPanel = false;
            settings->SavePositions();
        }
    }
    _panelPrevLeftDown = leftDown;

    ID3D11ShaderResourceView* panelBg = nullptr;
    if (settings->listPanelUseCustomBg) {
        panelBg = BackgroundManager::GetBackground(BackgroundManager::Type::ListPanel,
                                                   settings->listPanelBgIndex);
    }

    const float bgBleed = UIScale::Px(14.0f);
    const ImVec2 bgMin(mn.x - bgBleed, mn.y - bgBleed);
    const ImVec2 bgMax(mx.x + bgBleed, mx.y + bgBleed);

    if (panelBg) {
        // Stretched to that rect, which grows with the marker count, so art
        // should be authored tall (portrait) rather than square.
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(panelBg), bgMin, bgMax,
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, MapTheme::kPanelRounding);
    } else {
        // Unified panel background + border (Settings > Appearance).
        MapDraw::DrawPanelCard(dl, bgMin, bgMax, settings->BgCol(), settings->BorderCol(),
                               1.0f, geometryScale);
    }

    {
        const float dotBandH = UIScale::Px(12.0f);
        const float gripMidY = mn.y + std::floor(dotBandH * 0.5f);
        const float gripX0   = mn.x + pad;
        const float gripX1   = mx.x - pad;
        const ImU32 gripCol  = _draggingPanel ? settings->TitleCol() : MapTheme::kSeparator;

        const float dotSz   = (std::max)(1.0f, std::round(UIScale::Px(1.0f))); // square dot
        const float dotStep = UIScale::Px(4.0f);   // dot pitch along the row
        const float rowGap  = UIScale::Px(4.0f);   // vertical gap between the two rows
        for (int r = 0; r < 2; ++r) {
            const float ly = std::floor(gripMidY + (static_cast<float>(r) - 0.5f) * rowGap);
            // Offset every other row by half a step so the two read as a
            // texture rather than a grid.
            const float x0 = gripX0 + static_cast<float>(r) * dotStep * 0.5f;
            for (float x = x0; x <= gripX1 - dotSz; x += dotStep) {
                dl->AddRectFilled(ImVec2(x, ly), ImVec2(x + dotSz, ly + dotSz), gripCol);
            }
        }
    }

    const float rowPadX = UIScale::Px(1.0f);
    const float rowsLeft  = mn.x + rowPadX;
    const float rowsRight = mx.x - rowPadX;
    const bool  cursorInRowsX = mouse.x >= rowsLeft && mouse.x <= rowsRight;

    const float rowTextInset = UIScale::Px(10.0f);
    const float rowContentX = mn.x + rowTextInset;
    const float listIconSlot = std::floor(logicalBodyFS * 1.1f * geometryScale);
    const float listPngIconSz = std::floor(logicalBodyFS * 1.35f * geometryScale);
    const float listIconGap = std::floor(bodyFS * 0.55f);

    const float tabTop    = mn.y + gripH;
    const float tabBottom = tabTop + tabStripH;
    const float tabW      = (mx.x - mn.x) / static_cast<float>(tabs.size());

    dl->AddRectFilled(ImVec2(mn.x + rowPadX, tabTop),
                      ImVec2(mx.x - rowPadX, tabBottom),
                      MapTheme::kTabTrayBg);

    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const float tx0 = mn.x + tabW * static_cast<float>(i);
        const float tx1 = tx0 + tabW;
        AutomationTelemetry::Item("tab", static_cast<std::uint32_t>(tabs[i].tab), tabs[i].label.c_str(), tx0, tabTop, tx1, tabBottom);
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
            dl->AddLine(ImVec2(tx0 + UIScale::Px(4.0f), tabBottom - UIScale::Px(1.0f)),
                        ImVec2(tx1 - UIScale::Px(4.0f), tabBottom - UIScale::Px(1.0f)),
                        settings->TitleCol(), UIScale::Px(2.0f));
        } else if (tabHover) {
            // Deliberately weaker than kTabActiveBg. Both used kRowHoverBg
            // before, so hovering an inactive tab looked like selecting it.
            dl->AddRectFilled(ImVec2(tx0, tabTop), ImVec2(tx1, tabBottom),
                              MapTheme::kTabHoverBg);
        }

        std::string label = tabs[i].label;
        const float availW = tabW - UIScale::Px(8.0f);
        float tabFS = bodyFS * 0.92f;
        ImVec2 lblSz = ImGui::CalcTextSize(label.c_str());
        float  lblW  = lblSz.x * (tabFS / baseFS);
        if (lblW > availW && lblW > 0.0f) {
            tabFS *= availW / lblW;
            lblW = availW;
        }
        const ImU32 tabCol = (isActive || tabHover)
                                 ? settings->TitleCol()
                                 : MapDraw::WithAlpha(settings->TitleCol(), 0.70f);
        MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), tabFS,
                    ImVec2(tx0 + (tabW - lblW) * 0.5f,
                           tabTop + (tabStripH - tabFS) * 0.5f),
                    tabCol, label.c_str());
    }

    for (std::size_t i = 1; i < tabs.size(); ++i) {
        const float dx = mn.x + tabW * static_cast<float>(i);
        dl->AddLine(ImVec2(dx, tabTop + UIScale::Px(3.0f)), ImVec2(dx, tabBottom - UIScale::Px(3.0f)),
                    MapTheme::kTabDivider, UIScale::Px(1.0f));
    }

    const float rowContentRight = mx.x - rowTextInset;
    constexpr std::uint8_t kStatUnderlineAlpha = 65;
    constexpr std::uint8_t kHeadingUnderlineAlpha = 110;
    const auto DrawFadingUnderline = [dl](float a_left, float a_right, float a_y,
                                          std::uint8_t a_alpha) {
        if (a_right <= a_left) return;
        const float fadeW = std::min(UIScale::Px(18.0f), (a_right - a_left) * 0.25f);
        const float bottom = a_y + UIScale::Px(1.0f);
        const ImU32 lineColor = IM_COL32(130, 130, 130, a_alpha);
        const ImU32 lineClear = IM_COL32(130, 130, 130, 0);
        dl->AddRectFilledMultiColor(ImVec2(a_left, a_y), ImVec2(a_left + fadeW, bottom),
                                    lineClear, lineColor, lineColor, lineClear);
        dl->AddRectFilled(ImVec2(a_left + fadeW, a_y),
                          ImVec2(a_right - fadeW, bottom), lineColor);
        dl->AddRectFilledMultiColor(ImVec2(a_right - fadeW, a_y), ImVec2(a_right, bottom),
                                    lineColor, lineClear, lineClear, lineColor);
    };
    const auto DrawFadingVertical = [dl](float a_x, float a_top, float a_bottom,
                                         std::uint8_t a_alpha) {
        if (a_bottom <= a_top) return;
        const float fadeH = std::min(UIScale::Px(18.0f), (a_bottom - a_top) * 0.25f);
        const float right = a_x + UIScale::Px(1.0f);
        const ImU32 lineColor = IM_COL32(130, 130, 130, a_alpha);
        const ImU32 lineClear = IM_COL32(130, 130, 130, 0);
        dl->AddRectFilledMultiColor(ImVec2(a_x, a_top), ImVec2(right, a_top + fadeH),
                                    lineClear, lineClear, lineColor, lineColor);
        dl->AddRectFilled(ImVec2(a_x, a_top + fadeH),
                          ImVec2(right, a_bottom - fadeH), lineColor);
        dl->AddRectFilledMultiColor(ImVec2(a_x, a_bottom - fadeH), ImVec2(right, a_bottom),
                                    lineColor, lineColor, lineClear, lineClear);
    };
    // Keep the number gutter wide enough for 100+ rows without moving the
    // quest text as the user scrolls. Cap the numeral size to its row band.
    const float questIndexFS = std::min(titleFS, rowHeight - UIScale::Px(4.0f));
    const float miscIndexFS = std::min(bodyFS, rowHeight - UIScale::Px(4.0f));
    const std::string widestRegularIndex = std::format("{:02}", regularQuestCount);
    const std::string widestMiscIndex = std::format("{:02}", miscQuestCount);
    const float questIndexW = std::max(
        titleFont->CalcTextSizeA(questIndexFS, FLT_MAX, 0.0f, widestRegularIndex.c_str()).x,
        ImGui::GetFont()->CalcTextSizeA(miscIndexFS, FLT_MAX, 0.0f, widestMiscIndex.c_str()).x);
    const float questIndexRight = rowContentX + questIndexW;
    const float questDividerX = questIndexRight + UIScale::Px(7.0f);
    const float questTextX = questDividerX + UIScale::Px(11.0f);
    const auto DrawQuestHeading = [&](const std::string& a_label, std::size_t a_count,
                                      float a_y) {
        const std::string count = std::to_string(a_count);
        const float countW = titleFont->CalcTextSizeA(titleFS, FLT_MAX, 0.0f, count.c_str()).x;
        const float countX = rowContentRight - countW;
        const float labelRight = countX - UIScale::Px(8.0f);
        if (labelRight > rowContentX) {
            dl->PushClipRect(ImVec2(rowContentX, a_y), ImVec2(labelRight, a_y + headerH), true);
            MapDraw::DrawTextShadowed(dl, titleFont, titleFS,
                                      ImVec2(rowContentX, a_y), settings->TitleCol(), a_label.c_str());
            dl->PopClipRect();
        }
        MapDraw::DrawTextShadowed(dl, titleFont, titleFS,
                                  ImVec2(countX, a_y), settings->TitleCol(), count.c_str());
        DrawFadingUnderline(rowContentX, rowContentRight, a_y + headerH,
                            kHeadingUnderlineAlpha);
    };

    // Delete button tracks the body font so the X and its box grow together.
    const float delBtnW   = std::max(UIScale::Px(20.0f), bodyFS + UIScale::Px(6.0f));
    const float delBtnPad = UIScale::Px(4.0f);
    const float delBtnPadY = UIScale::Px(2.0f);
    const float delBtnX0  = rowContentRight - delBtnW - delBtnPad;
    const float delBtnX1  = rowContentRight - delBtnPad;

    const float rowTextSlack = UIScale::Px(6.0f);
    // Right edge available to a row label. Rows WITH a delete button stop short
    // of it; rows without run to the row edge.
    const float textRightWithBtn = delBtnX0 - rowTextSlack;
    const float textRightNoBtn   = rowContentRight - rowTextSlack;

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

    const ImVec2 bodyMin(mn.x, tabBottom);
    const ImVec2 bodyMax(mx.x, mx.y);
    dl->PushClipRect(bodyMin, bodyMax, true);

    const float rowsTop = bodyMin.y;

    float y = bodyMin.y + pad - scrollY;

    if (activeTab == ListTab::Overview && settings->listShowOverview) {
        // Keep the same text column as Destination, but let the artwork fill
        // more of its row. The 256px PNGs still contain transparent margins.
        for (const auto& row : statRows) {
            const bool rowVisible = (y + rowHeight > rowsTop) && (y < mx.y);
            if (rowVisible) {
                float textX = rowContentX;
                if (auto* icon = TextureManager::GetIconByName(row.icon)) {
                    const float iconX = std::floor(rowContentX - (listPngIconSz - listIconSlot) * 0.5f);
                    const float iconY = RowTextY(y, listPngIconSz);
                    MapDraw::DrawIconFx(dl, icon,
                                        ImVec2(iconX, iconY),
                                        ImVec2(iconX + listPngIconSz, iconY + listPngIconSz),
                                        IM_COL32(210, 210, 210, 255));
                    textX += listIconSlot + listIconGap;
                }
                const float valueWidth = ImGui::CalcTextSize(row.value.c_str()).x * (bodyFS / baseFS);
                const float valueX = rowContentRight - valueWidth;
                const float labelRight = valueX - UIScale::Px(12.0f);
                // Reserve the value column even when a translation needs truncation.
                if (labelRight > textX) {
                    const std::string label = TruncateRowTextToWidth(row.label, labelRight - textX, bodyFS);
                    dl->PushClipRect(ImVec2(textX, bodyMin.y), ImVec2(labelRight, bodyMax.y), true);
                    MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                ImVec2(textX, RowTextY(y, bodyFS)),
                                settings->BodyCol(), label.c_str());
                    dl->PopClipRect();
                }
                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                            ImVec2(valueX, RowTextY(y, bodyFS)),
                            settings->BodyCol(), row.value.c_str());
                DrawFadingUnderline(textX, rowContentRight, y + rowHeight - bandLift,
                                    kStatUnderlineAlpha);
            }
            y += rowHeight;
        }

        // Click pans through the quest-row arrays, same three-array rule as below.
        if (settings->listShowCompanions) {
            y += pad;
            if ((y + headerH > rowsTop) && (y < mx.y)) {
                MapDraw::DrawTextShadowed(dl, titleFont, titleFS,
                            ImVec2(rowContentX, y), settings->TitleCol(), LocalizationManager::Get("Overview.Companions").c_str());
                DrawFadingUnderline(rowContentX, rowContentRight, y + headerH,
                                    kHeadingUnderlineAlpha);
            }
            y += headerH + pad;

            if (companions.empty()) {
                if ((y + rowHeight > rowsTop) && (y < mx.y)) {
                    MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                ImVec2(rowContentX, RowTextY(y, bodyFS)),
                                MapTheme::kMutedText, LocalizationManager::Get("Overview.NoCompanionsAway").c_str());
                }
                y += rowHeight;
            }
            for (const auto& c : companions) {
                const float bandTop    = y - bandLift;
                const float bandBottom = y + rowHeight - bandLift;
                const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
                // Pannable only on the map showing their worldspace.
                const bool  canPan = c.hasMapPos && currentMapWorld &&
                                     c.rootWorldID == currentMapWorld->GetFormID();
                if (rowVisible) {
                    const bool rowHovered = canPan && cursorInRowsX &&
                                            mouse.y >= bandTop && mouse.y <= bandBottom;
                    if (rowHovered) {
                        dl->AddRectFilled(ImVec2(rowsLeft, bandTop), ImVec2(rowsRight, bandBottom),
                                          MapTheme::kRowHoverBg, UIScale::Px(2.0f));
                    }

                    float textX = rowContentX;
                    if (auto* icon = TextureManager::GetIconByName(c.spouse ? "spouse" : "follower")) {
                        const float iconX = std::floor(rowContentX - (listPngIconSz - listIconSlot) * 0.5f);
                        const float iconY = RowTextY(y, listPngIconSz);
                        MapDraw::DrawIconFx(dl, icon, ImVec2(iconX, iconY),
                                            ImVec2(iconX + listPngIconSz, iconY + listPngIconSz),
                                            IM_COL32(210, 210, 210, 255));
                    }
                    textX += listIconSlot + listIconGap;

                    const std::string& place = c.place.empty() ? LocalizationManager::Get("Overview.Travelling") : c.place;
                    const float placeW = ImGui::CalcTextSize(place.c_str()).x * (bodyFS / baseFS);
                    const float placeX = (std::max)(textX, rowContentRight - placeW);
                    const float nameRight = placeX - UIScale::Px(12.0f);
                    const ImU32 textCol = rowHovered ? settings->BodyColHover() : settings->BodyCol();
                    if (nameRight > textX) {
                        const std::string nm = TruncateRowTextToWidth(c.name, nameRight - textX, bodyFS);
                        MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                    ImVec2(textX, RowTextY(y, bodyFS)), textCol, nm.c_str());
                    }
                    MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                ImVec2(placeX, RowTextY(y, bodyFS)),
                                MapDraw::WithAlpha(settings->BodyCol(), 0.60f), place.c_str());
                    DrawFadingUnderline(textX, rowContentRight, bandBottom, kStatUnderlineAlpha);

                    if (canPan) {
                        questIds.push_back(c.actorID);
                        questRects.push_back({ rowsLeft, bandTop, rowsRight, bandBottom });
                        questPositions.push_back(c.mapPos);
                    }
                }
                y += rowHeight;
            }
        }
    }

    bool         destRowValid = false;
    Rect2D       destRowRect{};
    RE::NiPoint3 destRowPos{};
    if (activeTab == ListTab::Overview && settings->listShowOverview) {
        y += pad;
        if ((y + headerH > rowsTop) && (y < mx.y)) {
            MapDraw::DrawTextShadowed(dl, titleFont, titleFS,
                        ImVec2(rowContentX, y),
                        settings->TitleCol(), destHeader.c_str());
            DrawFadingUnderline(rowContentX, rowContentRight, y + headerH,
                                kHeadingUnderlineAlpha);
        }
        y += headerH + pad;

        const float bandTop    = y - bandLift;
        const float bandBottom = y + rowHeight - bandLift;
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
                                  bgFill, UIScale::Px(2.0f));

                // Use the marker's own icon on the left
                ID3D11ShaderResourceView* rowSrv = nullptr;
                if (destRow.isCustom) {
                    rowSrv = TextureManager::GetCategoryIcon(destRow.iconType);
                } else {
                    rowSrv = destRow.iconType == 61u ? TextureManager::GetIconByName("door")
                                                     : TextureManager::GetIconByName("destination");
                }
                const float iconSz = rowSrv && !MapDraw::IsWaypointType(destRow.iconType)
                    ? listPngIconSz : listIconSlot;
                const float iconX = std::floor(rowContentX - (iconSz - listIconSlot) * 0.5f);
                MapDraw::DrawPin(dl, iconX + iconSz * 0.5f,
                                 RowTextY(y, iconSz) + iconSz * 0.5f,
                                 0.0f, destRow.iconType, rowSrv,
                                 ListPinScale(iconSz, destRow.iconType, rowSrv),
                                 settings->PinTintCol(), 1.0f, geometryScale);

                // Match the larger PNG art on the left; keep its right edge fixed.
                const float badgeSz = listPngIconSz;
                const float badgeX1 = rowContentRight - delBtnPad;
                const float badgeX0 = badgeX1 - badgeSz;
                const float badgeY  = RowTextY(y, badgeSz);
                if (auto* destIcon = TextureManager::GetIconByName("destination")) {
                    dl->AddImage(reinterpret_cast<ImTextureID>(destIcon),
                                 ImVec2(badgeX0, badgeY), ImVec2(badgeX1, badgeY + badgeSz),
                                 ImVec2(0, 0), ImVec2(1, 1), MapTheme::kDestBadgeTint);
                }

                const float nameX = rowContentX + listIconSlot + listIconGap;
                const ImU32 textCol = rowHovered ? settings->BodyColHover() : settings->BodyCol();
                const float textRightEdge = badgeX0 - rowTextSlack;
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

    if (activeTab == ListTab::Overview && settings->listShowActiveQuests) {
        {
            if (settings->listShowOverview) y += pad;
            if ((y + headerH > rowsTop) && (y < mx.y)) {
                DrawQuestHeading(questHeader, regularQuestCount, y);
            }
            // Keep the first row's hover band clear of the underline.
            y += headerH + pad;
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
                std::size_t regularRowNumber = 0;
                std::size_t miscRowNumber = 0;
                for (const auto& q : activeQuests) {
                    if (q.isMiscellaneous && !drewMiscHeader) {
                        if (regularQuestCount > 0) y += pad;
                        // This subgroup has no hover band or click rect. Use the
                        // heading height in both the draw and scroll calculations.
                        const bool headerVisible = (y + headerH > rowsTop) && (y < mx.y);
                        if (headerVisible) {
                            DrawQuestHeading(miscHeader, miscQuestCount, y);
                        }
                        y += headerH + pad;
                        drewMiscHeader = true;
                    }

                    std::size_t& rowNumber = q.isMiscellaneous ? miscRowNumber : regularRowNumber;
                    ++rowNumber;
                    const float rowH       = q.isMiscellaneous
                        ? rowHeight
                        : (q.objectiveText.empty() ? rowHeight : std::floor(rowHeight * 1.8f));
                    const float bandTop    = y - bandLift;
                    const float bandBottom = y + rowH - bandLift;
                    const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
                    if (rowVisible) {
                        const bool rowHovered = q.hasPosition && cursorInRowsX &&
                                                mouse.y >= bandTop && mouse.y <= bandBottom;
                        if (rowHovered) {
                            dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                              ImVec2(rowsRight, bandBottom),
                                              MapTheme::kRowHoverBg, UIScale::Px(2.0f));
                        }

                        const ImU32 textCol = rowHovered ? settings->BodyColHover() : settings->BodyCol();
                        const std::string index = std::format("{:02}", rowNumber);
                        ImFont* indexFont = q.isMiscellaneous ? ImGui::GetFont() : titleFont;
                        const float indexFS = q.isMiscellaneous ? miscIndexFS : questIndexFS;
                        const float indexW = indexFont->CalcTextSizeA(indexFS, FLT_MAX,
                                                                     0.0f, index.c_str()).x;
                        const float indexY = std::floor(bandTop + (rowH - indexFS) * 0.5f);
                        MapDraw::DrawTextShadowed(dl, indexFont, indexFS,
                                                  ImVec2(questIndexRight - indexW, indexY),
                                                  MapDraw::WithAlpha(settings->TitleCol(), 0.50f), index.c_str());
                        DrawFadingVertical(questDividerX, bandTop + UIScale::Px(4.0f),
                                           bandBottom - UIScale::Px(4.0f), kStatUnderlineAlpha);
                        const std::string& primary = q.isMiscellaneous ? q.objectiveText : q.name;
                        const std::string primaryTrunc = TruncateRowTextToWidth(
                            primary, textRightNoBtn - questTextX, bodyFS);

                        if (q.isMiscellaneous) {
                            MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                        ImVec2(questTextX, RowTextY(y, bodyFS)), textCol, primaryTrunc.c_str());
                        } else {
                            MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                        ImVec2(questTextX, y + UIScale::Px(3.0f)), textCol, primaryTrunc.c_str());

                            if (!q.objectiveText.empty()) {
                                const float objX = questTextX;
                                const std::string obj = TruncateRowTextToWidth(
                                    q.objectiveText, textRightNoBtn - objX, bodyFS * 0.85f);
                                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS * 0.85f,
                                            ImVec2(objX, y + rowHeight * 0.85f),
                                            MapDraw::WithAlpha(settings->BodyCol(), 0.70f), obj.c_str());
                            }
                        }
                        DrawFadingUnderline(questTextX, rowContentRight, bandBottom,
                                            kStatUnderlineAlpha);

                        if (q.hasPosition) {
                            // All three push together, in the same branch: a parallel array
                            // filled anywhere else desyncs the moment one row is skipped.
                            questIds.push_back(q.questID);
                            questRects.push_back({ rowsLeft, bandTop, rowsRight, bandBottom });
                            questPositions.push_back(q.position);
                            AutomationTelemetry::Item("quest", q.questID, primary.c_str(), rowsLeft, std::max(bandTop, bodyMin.y), rowsRight, std::min(bandBottom, bodyMax.y), &q.position);
                        }
                    }
                    y += rowH;
                }
            }
        }
    }

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

            DrawFadingUnderline(rowContentX, rowContentRight, y + headerH,
                                kHeadingUnderlineAlpha);
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
                const float bandTop    = y - bandLift;
                const float bandBottom = y + rowHeight - bandLift;
                const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
                if (rowVisible) {
                    const bool  rowHovered = cursorInRowsX && mouse.y >= bandTop && mouse.y <= bandBottom;
                    const float delY0      = bandTop + delBtnPadY;
                    const float delY1      = bandBottom - delBtnPadY;
                    const bool  delHovered = rowHovered && mouse.x >= delBtnX0 && mouse.x <= delBtnX1;

                    if (m.isDestination) {
                        const ImU32 bgFill = rowHovered ? MapTheme::kDestRowBgHover : MapTheme::kDestRowBg;
                        dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                          ImVec2(rowsRight, bandBottom),
                                          bgFill, UIScale::Px(2.0f));
                    } else if (rowHovered) {
                        dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                          ImVec2(rowsRight, bandBottom),
                                          MapTheme::kRowHoverBg, UIScale::Px(2.0f));
                    }

                    const ImU32 textCol = rowHovered ? settings->BodyColHover() : settings->BodyCol();
                    const std::string resolvedName = ResolveMarkerName(m);
                    const char* displayName = resolvedName.c_str();

                    auto* rowSrv = TextureManager::GetCategoryIcon(m.iconType);
                    const float iconSz = rowSrv && !MapDraw::IsWaypointType(m.iconType)
                        ? listPngIconSz : listIconSlot;
                    const float iconX = std::floor(rowContentX - (iconSz - listIconSlot) * 0.5f);
                    MapDraw::DrawPin(dl, iconX + iconSz * 0.5f,
                                     RowTextY(y, iconSz) + iconSz * 0.5f,
                                     0.0f, m.iconType, rowSrv,
                                     ListPinScale(iconSz, m.iconType, rowSrv),
                                     settings->PinTintCol(), 1.0f, geometryScale);

                    float textRightBudget = textRightWithBtn;
                    if (m.isDestination) {
                        const float badgeSz  = listPngIconSz;
                        const float badgeGap = UIScale::Px(6.0f);
                        const float badgeX1  = delBtnX0 - badgeGap;
                        const float badgeX0  = badgeX1 - badgeSz;
                        const float badgeY   = RowTextY(y, badgeSz);
                        if (auto* destIcon = TextureManager::GetIconByName("destination")) {
                            dl->AddImage(reinterpret_cast<ImTextureID>(destIcon),
                                         ImVec2(badgeX0, badgeY), ImVec2(badgeX1, badgeY + badgeSz),
                                         ImVec2(0, 0), ImVec2(1, 1), MapTheme::kDestBadgeTint);
                        }
                        textRightBudget = badgeX0 - rowTextSlack;
                    }

                    // Gap scales with the text, not a fixed 6px: at a large body size a
                    // fixed gap reads as the icon crowding the name.
                    const float nameX = rowContentX + listIconSlot + listIconGap;
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
                        dl->AddRectFilled(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), delFill, UIScale::Px(2.0f));
                        dl->AddRect(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), MapTheme::kRemoveBorder, UIScale::Px(2.0f), 0, UIScale::Px(MapTheme::kBtnBorderW));
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
                const float bandTop    = y - bandLift;
                const float bandBottom = y + rowHeight - bandLift;
                const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
                if (rowVisible) {
                    const bool  rowHovered = cursorInRowsX && mouse.y >= bandTop && mouse.y <= bandBottom;
                    const float delY0      = bandTop + delBtnPadY;
                    const float delY1      = bandBottom - delBtnPadY;
                    const bool  delHovered = rowHovered && mouse.x >= delBtnX0 && mouse.x <= delBtnX1;

                    if (rowHovered) {
                        dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                          ImVec2(rowsRight, bandBottom),
                                          MapTheme::kRowHoverBg, UIScale::Px(2.0f));
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
                        dl->AddRectFilled(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), delFill, UIScale::Px(2.0f));
                        dl->AddRect(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), MapTheme::kRemoveBorder, UIScale::Px(2.0f), 0, UIScale::Px(MapTheme::kBtnBorderW));
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
                const float bandTop    = y - bandLift;
                const float bandBottom = y + rowHeight - bandLift;
                const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
                if (rowVisible) {
                    const bool  rowHovered = cursorInRowsX && mouse.y >= bandTop && mouse.y <= bandBottom;
                    const float delY0      = bandTop + delBtnPadY;
                    const float delY1      = bandBottom - delBtnPadY;
                    const bool  delHovered = rowHovered && mouse.x >= delBtnX0 && mouse.x <= delBtnX1;

                    if (rowHovered) {
                        dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                          ImVec2(rowsRight, bandBottom),
                                          MapTheme::kRowHoverBg, UIScale::Px(2.0f));
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
                        dl->AddRectFilled(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), delFill, UIScale::Px(2.0f));
                        dl->AddRect(ImVec2(delBtnX0, delY0), ImVec2(delBtnX1, delY1), MapTheme::kRemoveBorder, UIScale::Px(2.0f), 0, UIScale::Px(MapTheme::kBtnBorderW));
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
        const float thumbH = std::max(UIScale::Px(24.0f), trackH * (bodyH / contentH));
        const float t      = scrollY / _listScrollMax;
        const float thumbY = bodyMin.y + pad + t * (trackH - thumbH);
        const float sbWidth = UIScale::Px(3.0f);
        const float sbMarginRight = UIScale::Px(2.0f);
        dl->AddRectFilled(ImVec2(mx.x - sbMarginRight - sbWidth, thumbY),
                          ImVec2(mx.x - sbMarginRight, thumbY + thumbH),
                          MapTheme::kSeparator, UIScale::Px(2.0f));
    }

    std::lock_guard<std::mutex> lk(_panelMutex);
    _panelVisible = true;
    _panelMinX = mn.x; _panelMinY = mn.y;
    _panelMaxX = mx.x; _panelMaxY = mx.y;
    _controllerTabStrip = { mn.x, tabTop, mx.x, tabBottom };
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

void MapMarkerRenderer::BeginControllerContextFrame() {
    _controllerContextStaged = {};
    std::scoped_lock lock{ _controllerContextMutex };
    _controllerContextFrameSerial = _controllerContextRequestSerial;
}

void MapMarkerRenderer::PublishControllerContextPopup(ContextKind kind, float x0, float y0,
                                                       float x1, float y1,
                                                       const ContextRowHit* rows, std::size_t rowCount) {
    _controllerContextStaged = {};
    _controllerContextStaged.kind = kind;
    _controllerContextStaged.x0 = x0;
    _controllerContextStaged.y0 = y0;
    _controllerContextStaged.x1 = x1;
    _controllerContextStaged.y1 = y1;
    _controllerContextStaged.rowCount = (std::min)(rowCount, std::size_t{ 3 });
    for (std::size_t i = 0; i < _controllerContextStaged.rowCount; ++i) {
        _controllerContextStaged.rows[i] = rows[i];
    }
}

void MapMarkerRenderer::EndControllerContextFrame() {
    auto* ui = RE::UI::GetSingleton();
    const bool suppressed = !ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME) || IsMapFading() ||
        MenuUtils::ShouldBlockMapOverlay() || SettingsMenu::IsVisible() || _noteDialogOpen ||
        MenuUtils::IsMapModalOpen();
    std::scoped_lock lock{ _controllerContextMutex };
    if (suppressed) {
        _controllerContextPublished = {};
        _controllerContextOpening = false;
        _controllerContextActionInFlight = false;
        _pendingControllerContextAction = -1;
        _pendingControllerContextKind = ContextKind::None;
        return;
    }
    if (_controllerContextStaged.kind != ContextKind::None) {
        _controllerContextPublished = _controllerContextStaged;
        _controllerContextOpening = false;
        _controllerContextActionInFlight = false;
    } else if (_controllerContextRequestSerial == _controllerContextFrameSerial) {
        _controllerContextPublished = {};
        _controllerContextOpening = false;
        _controllerContextActionInFlight = false;
        _pendingControllerContextAction = -1;
        _pendingControllerContextKind = ContextKind::None;
    } else {
        // An input request after BeginControllerContextFrame belongs to the next draw.
        _controllerContextPublished = {};
        _controllerContextActionInFlight = false;
    }
}

void MapMarkerRenderer::MarkControllerContextOpening() {
    std::scoped_lock lock{ _controllerContextMutex };
    _controllerContextPublished = {};
    _controllerContextOpening = true;
    _controllerContextActionInFlight = false;
    _pendingControllerContextAction = -1;
    _pendingControllerContextKind = ContextKind::None;
    ++_controllerContextRequestSerial;
}

bool MapMarkerRenderer::IsControllerContextActiveOrOpening() const {
    std::scoped_lock lock{ _controllerContextMutex };
    return _controllerContextOpening || _controllerContextPublished.kind != ContextKind::None;
}

bool MapMarkerRenderer::QueueControllerContextAction() {
    std::scoped_lock lock{ _controllerContextMutex };
    if (_controllerContextOpening) {
        return true;
    }
    const auto& popup = _controllerContextPublished;
    if (popup.kind == ContextKind::None) return false;
    if (_controllerContextActionInFlight || _pendingControllerContextAction != -1) {
        return true;
    }

    auto* cursor = RE::MenuCursor::GetSingleton();
    const auto& io = ImGui::GetIO();
    if (!cursor || cursor->screenWidthX <= 0.0f || cursor->screenWidthY <= 0.0f ||
        io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
        return true;
    }
    const float mx = (cursor->cursorPosX / cursor->screenWidthX) * io.DisplaySize.x;
    const float my = (cursor->cursorPosY / cursor->screenWidthY) * io.DisplaySize.y;
    if (mx < popup.x0 || mx > popup.x1 || my < popup.y0 || my > popup.y1) {
        _pendingControllerContextAction = -2;
        _pendingControllerContextKind = popup.kind;
        return true;
    }
    for (std::size_t i = 0; i < popup.rowCount; ++i) {
        const auto& row = popup.rows[i];
        if (mx < row.x0 || mx > row.x1 || my < row.y0 || my > row.y1) continue;
        if (row.enabled) {
            _pendingControllerContextAction = static_cast<int>(i);
            _pendingControllerContextKind = popup.kind;
        }
        return true;
    }
    return true;
}

bool MapMarkerRenderer::QueueControllerContextClose() {
    std::scoped_lock lock{ _controllerContextMutex };
    if (!_controllerContextOpening && _controllerContextPublished.kind == ContextKind::None) return false;
    if (!_controllerContextActionInFlight) {
        _pendingControllerContextAction = -2;
        _pendingControllerContextKind = _controllerContextOpening ? ContextKind::None : _controllerContextPublished.kind;
    }
    return true;
}

int MapMarkerRenderer::TakeControllerContextAction(ContextKind kind) {
    std::scoped_lock lock{ _controllerContextMutex };
    if (SettingsMenu::IsVisible() || _noteDialogOpen) {
        _pendingControllerContextAction = -1;
        _pendingControllerContextKind = ContextKind::None;
        return -1;
    }
    if (_pendingControllerContextAction == -1 ||
        (_pendingControllerContextKind != kind &&
         !(_pendingControllerContextKind == ContextKind::None && _pendingControllerContextAction == -2))) {
        return -1;
    }
    const int action = _pendingControllerContextAction;
    _pendingControllerContextAction = -1;
    _pendingControllerContextKind = ContextKind::None;
    _controllerContextActionInFlight = true;
    return action;
}

void MapMarkerRenderer::QueueControllerNoteActivation() {
    auto* cursor = RE::MenuCursor::GetSingleton();
    const auto& io = ImGui::GetIO();
    std::scoped_lock lock{ _controllerContextMutex };
    _controllerNoteX = -1.0f;
    _controllerNoteY = -1.0f;
    if (cursor && cursor->screenWidthX > 0.0f && cursor->screenWidthY > 0.0f &&
        io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
        _controllerNoteX = (cursor->cursorPosX / cursor->screenWidthX) * io.DisplaySize.x;
        _controllerNoteY = (cursor->cursorPosY / cursor->screenWidthY) * io.DisplaySize.y;
    }
    _pendingControllerNoteActivation = true;
}

bool MapMarkerRenderer::TakeControllerNoteActivation(float& x, float& y) {
    std::scoped_lock lock{ _controllerContextMutex };
    if (!_pendingControllerNoteActivation) return false;
    x = _controllerNoteX;
    y = _controllerNoteY;
    _pendingControllerNoteActivation = false;
    return true;
}

bool MapMarkerRenderer::TryHandleControllerMapActivation() {
    auto* ui = RE::UI::GetSingleton();
    auto mapMenu = ui ? ui->GetMenu<RE::MapMenu>() : nullptr;
    if (!mapMenu || *reinterpret_cast<const bool*>(reinterpret_cast<std::uintptr_t>(mapMenu.get()) + 0x3045c)) {
        return false;
    }
    auto* cursor = RE::MenuCursor::GetSingleton();
    if (!cursor || cursor->screenWidthX <= 0.0f || cursor->screenWidthY <= 0.0f) return false;

    const auto& io = ImGui::GetIO();
    const float mx = (cursor->cursorPosX / cursor->screenWidthX) * io.DisplaySize.x;
    const float my = (cursor->cursorPosY / cursor->screenWidthY) * io.DisplaySize.y;
    const auto contains = [mx, my](const Rect2D& r) {
        return mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1;
    };

    std::lock_guard<std::mutex> lk(_panelMutex);
    // Legend is an ImGui window and can overlap the hand-drawn List panel.
    if (_filterVisible && contains(_filterRect)) {
        int action = -1;
        if (contains(_filterCollapseRect)) action = -2;
        else if (contains(_filterSettingsRect)) action = -3;
        else {
            for (std::size_t i = 0; i < _filterRowRects.size(); ++i) {
                if (contains(_filterRowRects[i])) {
                    action = static_cast<int>(i);
                    break;
                }
            }
        }
        if (action != -1) _pendingControllerFilter.store(action);
        if (Settings::GetSingleton()->inputVerboseLog) {
            logger::info("[InputVerbose] controller_map_A target=legend action={}", action);
        }
        return true;
    }
    if (_panelVisible && contains(_controllerTabStrip)) {
        const float width = _controllerTabStrip.x1 - _controllerTabStrip.x0;
        if (width <= 0.0f) return false;
        const int tab = std::clamp(static_cast<int>((mx - _controllerTabStrip.x0) *
            static_cast<float>(ListTab::Count) / width), 0, static_cast<int>(ListTab::Count) - 1);
        _pendingControllerTab.store(tab);
        if (Settings::GetSingleton()->inputVerboseLog) {
            logger::info("[InputVerbose] controller_map_A target=list_tab tab={}", tab);
        }
        return true;
    }
    return false;
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

    if (_destRowValid) {
        const auto& r = _destRowRect;
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            _pendingPanDestPos.store(true);
            logger::debug("[PanelClick] hit destination row");
            return true;
        }
    }

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
        std::lock_guard<std::mutex> lk(_panelMutex);
        pos = _pendingPanQuestPos;
        questPos = _pendingPanQuestPos;
        questID = _pendingPanQuestID;
        found = true;
    }

    if (!found) return;
    const auto requestedPos = pos;

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
    REL::Relocation<GlidePan_t> glidePan{ REL::RelocationID(52137, 53022) };
    glidePan(camera, &pos, 0.5f);
    AutomationTelemetry::Pan(questID, requestedPos, pos);
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
    using MarkerFilterHook::FocusMode;

    auto* settings = Settings::GetSingleton();

    const float geometryScale = UIScale::Master();
    const float fontScale     = UIScale::Font();

    const float bodyScale  = std::clamp(settings->bodyScale,  0.5f, 3.0f);
    const float titleScale = std::clamp(settings->titleScale, 0.5f, 3.0f);
    const float effectiveBodyScale  = bodyScale * fontScale;
    const float effectiveTitleScale = titleScale * fontScale;

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
    // Keep Separator's layout advance, but draw the header's fading line below.
    ImGui::PushStyleColor(ImGuiCol_Separator,     ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(UIScale::Px(13.0f), UIScale::Px(8.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(UIScale::Px(8.0f),  UIScale::Px(6.0f)));
    if (ImGui::Begin("Map Filters", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::SetWindowFontScale(effectiveBodyScale);
        const int controllerAction = _pendingControllerFilter.exchange(-1);
        Rect2D collapseRect{};
        Rect2D settingsRect{};
        std::vector<Rect2D> filterRowRects;
        filterRowRects.reserve(static_cast<std::size_t>(FocusMode::kCount));

        const ImVec2 wPos  = ImGui::GetWindowPos();
        const ImVec2 wSize = ImGui::GetWindowSize();

        if (wSize.x > 1.0f && wSize.y > 1.0f) {
            ImDrawList* wdl = ImGui::GetWindowDrawList();
            wdl->PushClipRectFullScreen();
            MapDraw::DrawPanelCard(wdl, wPos, ImVec2(wPos.x + wSize.x, wPos.y + wSize.y),
                                   settings->BgCol(), settings->BorderCol(), 1.0f, geometryScale);
            wdl->PopClipRect();
        }

        {
            const float target    = settings->filterCollapsed ? 0.0f : 1.0f;
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
            if (std::abs(_filterAnim - target) < 0.001f) _filterAnim = target;
        }
        // Keep the state linear, ease only at draw time (quadratic ease-out).
        const float easedOpen = 1.0f - (1.0f - _filterAnim) * (1.0f - _filterAnim);

        ImDrawList* hdrDl = ImGui::GetWindowDrawList();

        ImFont* defaultFont       = ImGui::GetFont();
        const float baseFS        = defaultFont ? defaultFont->FontSize : ImGui::GetFontSize();
        const float logicalBodyFS = baseFS * bodyScale;
        const float fs            = ImGui::GetFontSize();

        const float btnSz = std::floor(logicalBodyFS * 1.35f * geometryScale);

        // Title fades and its width collapses together, so the button slides
        // left into the strip instead of jumping. Measured on the heading font.
        std::string title = LocalizationManager::GetSingleton()->GetLabel("MapFilters");
        if (title == "MapFilters") title = "Map Filters";
        float titleW = 0.0f;
        ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
        if (titleFont) ImGui::PushFont(titleFont);
        ImGui::SetWindowFontScale(effectiveTitleScale);
        const float titleFs = ImGui::GetFontSize();
        titleW = ImGui::CalcTextSize(title.c_str()).x;
        if (titleFont) ImGui::PopFont();
        ImGui::SetWindowFontScale(effectiveBodyScale);

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

        // Legend rows are icon-led. The larger category art and its row receive
        // full geometry scaling while text font response stays dampened.
        const float iconSz        = std::floor(logicalBodyFS * 1.55f * geometryScale);  // category icon, left column
        const float eyeSz         = logicalBodyFS * 1.05f * geometryScale;              // state icon, right column
        const float gap           = UIScale::Px(8.0f);                                  // icon -> label
        const float rowPadY       = UIScale::Px(3.0f);
        const float rowH          = (std::max)(fs, iconSz) + rowPadY * 2.0f;
        const float hoverPadX     = UIScale::Px(4.0f);
        const float hoverRounding = UIScale::Px(3.0f);
        const float tipOffset     = UIScale::Px(8.0f);
        const auto DrawLegendUnderline = [dl](float a_left, float a_right, float a_y,
                                               float a_opacity, std::uint8_t a_alpha) {
            if (a_right <= a_left) return;
            const float fadeW = std::min(UIScale::Px(18.0f), (a_right - a_left) * 0.25f);
            const float bottom = a_y + UIScale::Px(1.0f);
            const ImU32 lineColor = IM_COL32(130, 130, 130,
                                             static_cast<int>(a_alpha * a_opacity));
            const ImU32 lineClear = IM_COL32(130, 130, 130, 0);
            dl->AddRectFilledMultiColor(ImVec2(a_left, a_y), ImVec2(a_left + fadeW, bottom),
                                        lineClear, lineColor, lineColor, lineClear);
            dl->AddRectFilled(ImVec2(a_left + fadeW, a_y),
                              ImVec2(a_right - fadeW, bottom), lineColor);
            dl->AddRectFilledMultiColor(ImVec2(a_right - fadeW, a_y), ImVec2(a_right, bottom),
                                        lineColor, lineClear, lineClear, lineColor);
        };

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
            if (const auto n = MarkerFilterHook::GetModeCount(mode); n > 0) {
                label += " (" + std::to_string(n) + ")";
            }
            labels[i]  = std::move(label);
            maxLabelW  = (std::max)(maxLabelW, ImGui::CalcTextSize(labels[i].c_str()).x);
        }
        {
            std::string setLbl = LocalizationManager::GetSingleton()->GetLabel("ListPanel.Settings");
            if (setLbl == "ListPanel.Settings") setLbl = "Settings";
            maxLabelW = (std::max)(maxLabelW, ImGui::CalcTextSize(setLbl.c_str()).x);
        }
        const float rowWFull = iconSz + gap + maxLabelW + gap * 2.0f + eyeSz;
        const float rowWIcon = (std::max)(iconSz, btnSz);
        const float rowW     = rowWIcon + (rowWFull - rowWIcon) * easedOpen;

        // ---- Header row: chevron button, plus the title when expanded ------
        {
            const ImVec2 hMin = ImGui::GetCursorScreenPos();
            const float  hH   = (std::max)(btnSz, titleFs);

            const float bxExpanded  = hMin.x + rowW - btnSz;
            const float bxCollapsed = hMin.x + (iconSz - btnSz) * 0.5f;
            const float bx = bxCollapsed + (bxExpanded - bxCollapsed) * easedOpen;
            const float by = hMin.y + (hH - btnSz) * 0.5f;
            collapseRect = { bx, by, bx + btnSz, by + btnSz };

            ImGui::PushID("filterCollapse");
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            const bool btnClicked = ImGui::InvisibleButton("##collapse", ImVec2(btnSz, btnSz));
            const bool btnHover   = ImGui::IsItemHovered();
            ImGui::PopID();

            if (btnClicked || controllerAction == -2) {
                settings->filterCollapsed = !settings->filterCollapsed;
                settings->SavePositions();
            }

            const float btnHoverPad = UIScale::Px(3.0f);
            const float btnHoverRounding = UIScale::Px(3.0f);
            if (btnHover) {
                hdrDl->AddRectFilled(ImVec2(bx - btnHoverPad, by - btnHoverPad),
                                     ImVec2(bx + btnSz + btnHoverPad, by + btnSz + btnHoverPad),
                                     MapTheme::kRowHoverBg, btnHoverRounding);
            }

            {
                const float cx   = bx + btnSz * 0.5f;
                const float cy   = by + btnSz * 0.5f;
                const float armX = btnSz * 0.30f;
                const float armY = btnSz * 0.34f;
                // easedOpen 1 = expanded = tip on the left.
                const float tipX = cx + armX * (1.0f - 2.0f * easedOpen);
                const float bkX  = cx - armX * (1.0f - 2.0f * easedOpen);
                const ImU32 chevCol = btnHover ? settings->BodyColHover() : settings->TitleCol();
                const ImVec2 top(bkX, cy - armY);
                const ImVec2 tip(tipX, cy);
                const ImVec2 bottom(bkX, cy + armY);
                hdrDl->AddTriangleFilled(top, tip, bottom, chevCol);
                hdrDl->AddTriangle(top, tip, bottom, IM_COL32(0, 0, 0, 255), UIScale::Px(1.5f));
            }

            // Title: fades out as it collapses. Skipped entirely below ~2%
            // alpha so a fully collapsed strip does no text work at all.
            if (easedOpen > 0.02f) {
                if (titleFont) ImGui::PushFont(titleFont);
                ImGui::SetWindowFontScale(effectiveTitleScale);
                const ImU32 tc = MapDraw::WithAlpha(settings->TitleCol(), easedOpen);
                MapDraw::DrawTextShadowed(hdrDl, ImGui::GetFont(), titleFs,
                                          ImVec2(hMin.x, hMin.y + (hH - titleFs) * 0.5f),
                                          tc, title.c_str());
                if (titleFont) ImGui::PopFont();
                ImGui::SetWindowFontScale(effectiveBodyScale);
            }

            // Restore layout flow: the button used SetCursorScreenPos, so the
            // cursor is wherever that left it. Put it back under the header.
            ImGui::SetCursorScreenPos(ImVec2(hMin.x, hMin.y + hH));
            ImGui::Dummy(ImVec2(rowW, UIScale::Px(2.0f)));
        }
        ImGui::Separator();
        if (easedOpen > 0.02f) {
            const ImVec2 sepMin = ImGui::GetItemRectMin();
            const ImVec2 sepMax = ImGui::GetItemRectMax();
            DrawLegendUnderline(sepMin.x, sepMin.x + rowW,
                                (sepMin.y + sepMax.y) * 0.5f, easedOpen, 110);
        }
        ImGui::Spacing();

        ID3D11ShaderResourceView* eyeSrv = TextureManager::GetIconByName("eye");

        std::string  tipText;
        ImVec2       tipAnchor{};

        for (std::size_t i = 0; i < labels.size(); ++i) {
            const auto  mode     = static_cast<FocusMode>(i);
            const bool  on       = MarkerFilterHook::IsModeEnabled(mode);
            const char* modeName = MarkerFilterHook::GetModeName(mode);

            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            filterRowRects.push_back({ rowMin.x, rowMin.y, rowMin.x + rowW, rowMin.y + rowH });

            ImGui::PushID(modeName);
            const bool clicked = ImGui::InvisibleButton("##row", ImVec2(rowW, rowH));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clicked || controllerAction == static_cast<int>(i)) {
                MarkerFilterHook::SetModeEnabled(mode, !on);
            }

            if (hovered) {
                dl->AddRectFilled(ImVec2(rowMin.x - hoverPadX, rowMin.y),
                                  ImVec2(rowMin.x + rowW + hoverPadX, rowMin.y + rowH),
                                  MapTheme::kRowHoverBg, hoverRounding);
                if (easedOpen <= 0.02f) {
                    tipText   = labels[i];
                    tipAnchor = ImVec2(rowMin.x + rowW + tipOffset, rowMin.y);
                }
            }

            const float    dim      = on ? 1.0f : 0.35f;
            const ImU32    iconTint = IM_COL32(255, 255, 255, static_cast<int>(255 * dim));
            const ImU32    textCol  = on ? (hovered ? settings->BodyColHover() : settings->BodyCol())
                                         : MapTheme::kMutedText;

            // Category icon. Blank entries reserve the column so every label
            // starts on the same x.
            if (kModeIcons[i][0] != '\0') {
                if (auto* srv = TextureManager::GetIconByName(kModeIcons[i])) {
                    const float iy = rowMin.y + (rowH - iconSz) * 0.5f;
                    MapDraw::DrawIconFx(dl, srv,
                                        ImVec2(rowMin.x, iy),
                                        ImVec2(rowMin.x + iconSz, iy + iconSz),
                                        iconTint);
                }
            }

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
                DrawLegendUnderline(rowMin.x + iconSz + gap, rowMin.x + rowW,
                                    rowMin.y + rowH - rowPadY, easedOpen, 65);
            }
        }

        {
            ImGui::Spacing();
            const ImVec2 setMin = ImGui::GetCursorScreenPos();
            settingsRect = { setMin.x, setMin.y, setMin.x + rowW, setMin.y + rowH };

            ImGui::PushID("filterSettings");
            const bool setClicked = ImGui::InvisibleButton("##settings", ImVec2(rowW, rowH));
            const bool setHovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (setClicked || controllerAction == -3) SettingsMenu::Toggle();

            if (setHovered) {
                dl->AddRectFilled(ImVec2(setMin.x - hoverPadX, setMin.y),
                                  ImVec2(setMin.x + rowW + hoverPadX, setMin.y + rowH),
                                  MapTheme::kRowHoverBg, hoverRounding);
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
                tipAnchor = ImVec2(setMin.x + rowW + tipOffset, setMin.y);
            }
        }

        if (!tipText.empty()) {
            ImDrawList* tipDl = ImGui::GetForegroundDrawList();
            const std::string& tip  = tipText;
            const ImVec2       tsz  = ImGui::CalcTextSize(tip.c_str());
            const float        tpad = UIScale::Px(6.0f);
            const float        tipBorderW = UIScale::Px(1.0f);
            const float        tipH = tsz.y + tpad * 2.0f;
            const ImVec2 tmn(tipAnchor.x, tipAnchor.y + (rowH - tipH) * 0.5f);
            const ImVec2 tmx = ImVec2(tmn.x + tsz.x + tpad * 2.0f,
                                      tmn.y + tipH);
            tipDl->AddRectFilled(tmn, tmx, ImGui::GetColorU32(settings->BgColVec4()));
            tipDl->AddRect(tmn, tmx, ImGui::GetColorU32(settings->BorderColVec4()), 0.0f, 0, tipBorderW);
            tipDl->AddText(ImGui::GetFont(), fs, ImVec2(tmn.x + tpad, tmn.y + tpad),
                           settings->BodyCol(), tip.c_str());
        }

        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        settings->filterPosX = static_cast<std::int32_t>(pos.x);
        settings->filterPosY = static_cast<std::int32_t>(pos.y);

        const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (_wasDraggingFilter && !down) {
            settings->SavePositions();
        }
        _wasDraggingFilter = down && ImGui::IsWindowFocused();

        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _filterVisible = size.x > 1.0f && size.y > 1.0f;
            _filterRect = { pos.x, pos.y, pos.x + size.x, pos.y + size.y };
            _filterCollapseRect = collapseRect;
            _filterSettingsRect = settingsRect;
            _filterRowRects = std::move(filterRowRects);
        }

        ImGui::SetWindowFontScale(1.0f);
    } else {
        std::lock_guard<std::mutex> lk(_panelMutex);
        _filterVisible = false;
        _pendingControllerFilter.store(-1);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
}

void MapMarkerRenderer::OpenTerrainMenu() {
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        _terrainMenuCursorX = cursor->cursorPosX;
        _terrainMenuCursorY = cursor->cursorPosY;
    }
    // Defer the actual OpenPopup to the render thread; see DrawTerrainMenu.
    _openTerrainMenu.store(true);
}

void MapMarkerRenderer::DrawTerrainMenu(bool isLocalMap) {
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

    ImGui::PushStyleColor(ImGuiCol_PopupBg,       MapTheme::kPopupBg());
    ImGui::PushStyleColor(ImGuiCol_Border,        MapTheme::kPopupBorder());
    ImGui::PushStyleColor(ImGuiCol_Text,          MapTheme::kPopupText());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MapTheme::kPopupHoverBg());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  MapTheme::kPopupHoverBg());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(UIScale::Px(10.0f), UIScale::Px(8.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(UIScale::Px(8.0f),  UIScale::Px(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, UIScale::Px(1.5f));
    if (ImGui::BeginPopup("TerrainContextMenu")) {
        const float effectiveBodyScale = std::clamp(settings->bodyScale, 0.5f, 3.0f) * UIScale::Font();
        ImGui::SetWindowFontScale(effectiveBodyScale);
        const int controllerAction = TakeControllerContextAction(ContextKind::Terrain);
        if (controllerAction == -2) ImGui::CloseCurrentPopup();
        ContextRowHit rows[2]{};
        const bool addClicked = ImGui::MenuItem(LocalizationManager::Get("Menu.AddMarkerHere").c_str());
        const ImVec2 addMin = ImGui::GetItemRectMin();
        const ImVec2 addMax = ImGui::GetItemRectMax();
        rows[0] = { addMin.x, addMin.y, addMax.x, addMax.y, true };
        AutomationTelemetry::Item("terrainAdd", 0, "Add Marker Here", addMin.x, addMin.y, addMax.x, addMax.y);
        if (addClicked || controllerAction == 0) {
            _pendingTerrainPlace.store(true);
            if (controllerAction == 0) ImGui::CloseCurrentPopup();
        }
        // Always disabled: bare terrain has no location to travel to. Same
        // wording as the other two menus -- one destination verb everywhere.
        ImGui::BeginDisabled();
        ImGui::MenuItem(LocalizationManager::Get("Menu.SetDestination").c_str());
        const ImVec2 destMin = ImGui::GetItemRectMin();
        const ImVec2 destMax = ImGui::GetItemRectMax();
        rows[1] = { destMin.x, destMin.y, destMax.x, destMax.y, false };
        ImGui::EndDisabled();
        if (controllerAction != -2 && controllerAction != 0 && !addClicked) {
            const ImVec2 pos = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            PublishControllerContextPopup(ContextKind::Terrain, pos.x, pos.y,
                                          pos.x + size.x, pos.y + size.y, rows, 2);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);

    if (_pendingTerrainPlace.exchange(false)) {
        if (auto* ui = RE::UI::GetSingleton()) {
            if (auto mm = ui->GetMenu<RE::MapMenu>()) {
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
    _customContextMarkerId = id;
    _openCustomContextMenu.store(true);
}

void MapMarkerRenderer::DrawCustomContextMenu() {
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

    ImGui::PushStyleColor(ImGuiCol_PopupBg,       MapTheme::kPopupBg());
    ImGui::PushStyleColor(ImGuiCol_Border,        MapTheme::kPopupBorder());
    ImGui::PushStyleColor(ImGuiCol_Text,          MapTheme::kPopupText());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MapTheme::kPopupHoverBg());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  MapTheme::kPopupHoverBg());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(UIScale::Px(10.0f), UIScale::Px(8.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(UIScale::Px(8.0f),  UIScale::Px(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, UIScale::Px(1.5f));
    const bool popupOpen = ImGui::BeginPopup("CustomContextMenu");
    // Feeds IsCustomMenuActive() so the hover panels stay hidden while the
    // menu is up, and reappear the frame after it closes.
    _customMenuVisible = popupOpen;
    if (popupOpen) {
        const float effectiveBodyScale = std::clamp(Settings::GetSingleton()->bodyScale, 0.5f, 3.0f) * UIScale::Font();
        ImGui::SetWindowFontScale(effectiveBodyScale);
        const std::uint32_t id = _customContextMarkerId;
        const int controllerAction = TakeControllerContextAction(ContextKind::Custom);
        if (controllerAction == -2) ImGui::CloseCurrentPopup();
        ContextRowHit rows[3]{};
        bool actionRan = false;

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
        const bool destClicked = ImGui::MenuItem(LocalizationManager::Get(isDest ? "Menu.RemoveDestination"
                                                                                : "Menu.SetDestination").c_str());
        const ImVec2 destMin = ImGui::GetItemRectMin();
        const ImVec2 destMax = ImGui::GetItemRectMax();
        rows[0] = { destMin.x, destMin.y, destMax.x, destMax.y, hasMarker };
        if (hasMarker && (destClicked || controllerAction == 0)) {
            logger::debug("[CustomContextMenu] Toggle Destination clicked for id {}", id);
            MarkerManager::GetSingleton()->SetDestination(id, !isDest);
            actionRan = true;
            if (controllerAction == 0) ImGui::CloseCurrentPopup();
        }

        // Localized "Rename" row label (ListPanel.Rename).
        const bool renameClicked = ImGui::MenuItem(LocalizationManager::Get("ListPanel.Rename").c_str());
        const ImVec2 renameMin = ImGui::GetItemRectMin();
        const ImVec2 renameMax = ImGui::GetItemRectMax();
        rows[1] = { renameMin.x, renameMin.y, renameMax.x, renameMax.y, hasMarker };
        if (hasMarker && (renameClicked || controllerAction == 1)) {
            logger::debug("[CustomContextMenu] Rename clicked for id {}", id);
            BeginEditMarker(id);
            actionRan = true;
            if (controllerAction == 1) ImGui::CloseCurrentPopup();
        }

        const bool removeClicked = ImGui::MenuItem(LocalizationManager::Get("Menu.RemoveMarker").c_str());
        const ImVec2 removeMin = ImGui::GetItemRectMin();
        const ImVec2 removeMax = ImGui::GetItemRectMax();
        rows[2] = { removeMin.x, removeMin.y, removeMax.x, removeMax.y, hasMarker };
        if (hasMarker && (removeClicked || controllerAction == 2)) {
            logger::debug("[CustomContextMenu] Remove clicked for id {}", id);
            MarkerManager::GetSingleton()->RemoveMarker(id);
            actionRan = true;
            if (controllerAction == 2) ImGui::CloseCurrentPopup();
        }
        if (!hasMarker) ImGui::EndDisabled();

        if (controllerAction != -2 && !actionRan) {
            const ImVec2 pos = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            PublishControllerContextPopup(ContextKind::Custom, pos.x, pos.y,
                                          pos.x + size.x, pos.y + size.y, rows, 3);
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}

void MapMarkerRenderer::DrawNoteDialog() {
    float controllerX = -1.0f, controllerY = -1.0f;
    const bool controllerActivate = TakeControllerNoteActivation(controllerX, controllerY);
    const ImVec2 controllerPos(controllerX, controllerY);
    const auto controllerInRect = [&](const ImVec2& mn, const ImVec2& mx) {
        return controllerActivate && controllerX >= mn.x && controllerX <= mx.x &&
            controllerY >= mn.y && controllerY <= mx.y;
    };
    if (_resetImGuiMouseOnNextDraw) {
        auto& io = ImGui::GetIO();
        io.MouseDown[0] = false;
        io.AddMouseButtonEvent(0, false);
        _resetImGuiMouseOnNextDraw = false;
    }

    const ImVec2 dispSize = ImGui::GetIO().DisplaySize;
    auto* settings = Settings::GetSingleton();

    const float geometryScale = UIScale::Master();
    const float fontScale     = UIScale::Font();

    const float bodyScale  = std::clamp(settings->bodyScale,  0.5f, 3.0f);
    const float titleScale = std::clamp(settings->titleScale, 0.5f, 3.0f);
    const float effectiveBodyScale  = bodyScale * fontScale;
    const float effectiveTitleScale = titleScale * fontScale;

    const bool vanillaMode = (_editingVanillaFormID != 0);
    std::string dialogTitle;
    if (vanillaMode) {
        constexpr auto key = "Dialog.Note";
        std::string noteLabel = LocalizationManager::Get(key);
        if (noteLabel == key) noteLabel = "Note";
        dialogTitle = noteLabel + ": " + _vanillaNoteName;
    } else {
        const bool  renameMode = (_editingMarkerId != 0);
        const char* key      = renameMode ? "Dialog.RenameMarker" : "Dialog.AddNewMarker";
        const char* fallback = renameMode ? "Edit Marker"         : "Add New Marker";
        dialogTitle = LocalizationManager::Get(key);
        if (dialogTitle == key) dialogTitle = fallback;
    }

    ImFont* defaultFont   = ImGui::GetFont();
    const float baseFS    = defaultFont ? defaultFont->FontSize : ImGui::GetFontSize();
    const float fontRatio = baseFS / 13.0f;

    // Fixed width drives the input and category-grid layout. Preserve the
    // original 13px-font relationship, then apply interface geometry scaling.
    float dlgW = 340.0f * bodyScale * fontRatio * geometryScale;

    // Both world and interior modes now use the icon grid picker.
    // Synchronize minimum dialog width with the 10-column DrawCategoryGrid layout.
    if (_editingVanillaFormID == 0) {
        const float cellW = 38.0f * geometryScale + (8.0f * geometryScale);
        dlgW = (std::max)(dlgW, cellW * 10.0f + (16.0f * geometryScale));
    }

    // The constrained width must include the actual heading font and effective
    // title scale or long marker names clip even when the body layout fits.
    {
        ImFont* measureFont = FontManager::GetSingleton()->GetHeadingFont();
        if (!measureFont) measureFont = ImGui::GetFont();
        const float titleFS = measureFont->FontSize * effectiveTitleScale;

        const float titleW = measureFont->CalcTextSizeA(titleFS, FLT_MAX, 0.0f,
                                                        dialogTitle.c_str()).x;
        const float windowPaddingX = UIScale::Px(12.0f);
        const float needed = titleW + windowPaddingX * 2.0f;
        // Cap at 80% of the display as an unscaled display percentage.
        const float maxDlgW = dispSize.x * 0.8f;
        if (needed > dlgW) dlgW = (std::min)(needed, (std::max)(dlgW, maxDlgW));
    }

    // Scaled height estimate for initial screen-edge clamping.
    float dlgHEstimate = 190.0f * bodyScale * fontRatio * geometryScale;
    if (_editingVanillaFormID == 0) {
        dlgHEstimate += 150.0f * bodyScale * fontRatio * geometryScale; // icon grid
    }
    if (_editingVanillaFormID != 0) {
        dlgHEstimate += baseFS * effectiveBodyScale * 3.5f; // multiline note
    }
    if (ImFont* hf = FontManager::GetSingleton()->GetHeadingFont()) {
        dlgHEstimate += (std::max)(0.0f, hf->FontSize - baseFS) * effectiveTitleScale;
    }

    float dlgX, dlgY;
    const float openOffset = UIScale::Px(16.0f);
    const float edgeMargin = UIScale::Px(4.0f);
    if (_noteDialogX >= 0.0f && _noteDialogY >= 0.0f) {
        dlgX = _noteDialogX * dispSize.x + openOffset;
        dlgY = _noteDialogY * dispSize.y + openOffset;
        const float maxDlgX = (std::max)(edgeMargin, dispSize.x - dlgW - edgeMargin);
        const float maxDlgY = (std::max)(edgeMargin, dispSize.y - dlgHEstimate - edgeMargin);
        dlgX = std::clamp(dlgX, edgeMargin, maxDlgX);
        dlgY = std::clamp(dlgY, edgeMargin, maxDlgY);
    } else {
        dlgX = (dispSize.x - dlgW) * 0.5f;
        dlgY = (dispSize.y - dlgHEstimate) * 0.5f;
    }

    ImGui::SetNextWindowPos(ImVec2(dlgX, dlgY), ImGuiCond_Always);
    // Width is fixed; AlwaysAutoResize owns the content-dependent height.
    ImGui::SetNextWindowSizeConstraints(ImVec2(dlgW, 0.0f), ImVec2(dlgW, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(1.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::PushStyleColor(ImGuiCol_WindowBg,       MapTheme::kDlgBg());
    ImGui::PushStyleColor(ImGuiCol_Border,         MapTheme::kDlgBorder());
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        MapTheme::kDlgFrameBg());
    ImGui::PushStyleColor(ImGuiCol_Button,         MapTheme::kDlgBtnBg());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  MapTheme::kDlgBtnHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   MapTheme::kDlgBtnActive());
    ImGui::PushStyleColor(ImGuiCol_CheckMark,      MapTheme::kDlgCheckMark());
    ImGui::PushStyleColor(ImGuiCol_Text,           MapTheme::kDlgText());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, UIScale::Px(1.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(UIScale::Px(12.0f), UIScale::Px(10.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(UIScale::Px(8.0f),  UIScale::Px(8.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,     ImVec2(UIScale::Px(4.0f),  UIScale::Px(3.0f)));

    if (ImGui::Begin("##NoteDialog", nullptr, flags)) {
        ImGui::SetWindowFontScale(effectiveBodyScale);

        ImFont* dlgTitleFont = FontManager::GetSingleton()->GetHeadingFont();
        if (dlgTitleFont) ImGui::PushFont(dlgTitleFont);
        ImGui::SetWindowFontScale(effectiveTitleScale);
        ImGui::TextColored(MapTheme::kDlgTitleText(), "%s", dialogTitle.c_str());
        if (dlgTitleFont) ImGui::PopFont();
        ImGui::SetWindowFontScale(effectiveBodyScale);
        ImGui::Separator();
        ImGui::Spacing();

        const float btnW    = 70.0f * bodyScale * geometryScale;
        const float inputW  = ImGui::GetContentRegionAvail().x;

        // SetKeyboardFocusHere only once when the dialog opens, so we don't
        // steal focus from the Combo box when the user clicks it.
        if (_noteFocusRequested) {
            ImGui::SetKeyboardFocusHere();
            if (ImGui::GetIO().WantTextInput) {
                _noteFocusRequested = false;
            }
        }

        bool confirmed = false;
        const bool inputVerbose = Settings::GetSingleton()->inputVerboseLog;
        const int inputCharacters = inputVerbose ? ImGui::GetIO().InputQueueCharacters.Size : 0;
        // Compare content, not just length: replacing a selection can leave
        // the byte count unchanged. Do not copy note text when logging is off.
        const std::string noteBefore = inputVerbose ? std::string(_noteBuffer) : std::string();
        if (vanillaMode) {
            NoteWrapCtx wrapCtx;
            wrapCtx.font      = ImGui::GetFont();
            wrapCtx.fontSize  = ImGui::GetFontSize();
            wrapCtx.wrapWidth = inputW - ImGui::GetStyle().FramePadding.x * 2.0f - wrapCtx.fontSize;

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
            constexpr int   kPerRow = 10;
            const float     kCellSz = 30.0f * geometryScale;
            MapDraw::DrawCategoryGrid(categoryNames, categoryValues, categoryCount,
                                      _selectedCategory, kPerRow, kCellSz,
                                      /*showTooltip=*/!_noteDialogInterior,
                                      geometryScale,
                                      controllerActivate ? &controllerPos : nullptr);

            ImGui::Spacing();

            ImGui::Checkbox(LocalizationManager::Get("Dialog.SetAsDestination").c_str(),
                            &_pendingIsDestination);
            if (controllerInRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) {
                _pendingIsDestination = !_pendingIsDestination;
            }

            ImGui::Spacing();
        }

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
        AutomationTelemetry::Item("dialogOk", 0, "OK", okMin.x, okMin.y, okMax.x, okMax.y);

        ImGui::SameLine();

        const bool cancelClicked = ImGui::Button(LocalizationManager::Get("Dialog.Cancel").c_str(), ImVec2(btnW, 0));
        const ImVec2 cancelMin = ImGui::GetItemRectMin();
        const ImVec2 cancelMax = ImGui::GetItemRectMax();
        AutomationTelemetry::Item("dialogCancel", 0, "Cancel", cancelMin.x, cancelMin.y, cancelMax.x, cancelMax.y);

        const bool anyPopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        const bool okController = !anyPopupOpen && controllerInRect(okMin, okMax);
        const bool cancelController = !anyPopupOpen && controllerInRect(cancelMin, cancelMax);
        const bool okHoveredDirect = !anyPopupOpen && cursorInRect(okMin, okMax);
        const bool cancelHoveredDirect = !anyPopupOpen && cursorInRect(cancelMin, cancelMax);
        if (leftPressed) {
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

        if (okClicked || okReleasedDirect || okController || confirmed) {
            const std::uint32_t iconType =
                (_selectedCategory >= 0 && _selectedCategory < categoryCount)
                    ? categoryValues[_selectedCategory] : 0u;
            if (vanillaMode) {
                const std::string noteText = StripSoftWraps(_noteBuffer);
                MarkerManager::GetSingleton()->SetVanillaNote(
                    _editingVanillaFormID, noteText);
                if (!noteText.empty()) ShowListTab(ListTab::Notes);
                logger::debug("[NoteDialog] vanilla note confirmed for {:08X}: '{}'",
                             _editingVanillaFormID, noteText);
            } else if (_editingMarkerId != 0) {
                const std::uint32_t id = _editingMarkerId;
                MarkerManager::GetSingleton()->UpdateMarker(id, std::string(_noteBuffer), iconType);
                MarkerManager::GetSingleton()->SetDestination(id, _pendingIsDestination);
            } else {
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

        if (_noteDialogOpen && (cancelClicked || cancelReleasedDirect || cancelController)) {
            logger::debug("[NoteDialog] cancelled");
            _editingMarkerId = 0;
            _editingVanillaFormID = 0;
            _noteDialogOpen = false;
        }

        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();

    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(8);
}
