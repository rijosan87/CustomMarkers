// Main world map renderer for custom pins, tooltips, list sidebar, and filters.

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
#include "UISound.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>

// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):

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
        static constexpr char kEllipsis[] = "...";
        const float ellipsisW = widthOf(kEllipsis, kEllipsis + 3);
        const float bodyW     = a_availW - ellipsisW;
        if (bodyW <= 0.0f) return kEllipsis;
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
        auto* ctx = static_cast<NoteWrapCtx*>(a_data->UserData);
        if (!ctx || !ctx->font || ctx->wrapWidth <= 0.0f || ctx->fontSize <= 0.0f) return 0;
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
            const char* next = brk;
            if (next < end && *next == ' ') ++next;
            if (next < end) {
                wrapped.push_back('\n');
                if (brkOff < cursor) ++addedBeforeCursor;
            }
            line = next;
        }
        if (wrapped != text) {
            if (static_cast<int>(wrapped.size()) < a_data->BufSize) {
                a_data->DeleteChars(0, a_data->BufTextLen);
                a_data->InsertChars(0, wrapped.c_str(), wrapped.c_str() + wrapped.size());
                a_data->CursorPos = std::clamp(cursor + addedBeforeCursor,
                                               0, static_cast<int>(wrapped.size()));
                a_data->BufDirty = true;
            }
        }
        return 0;
    }

    std::string StripSoftWraps(const char* a_text) {
        std::string out(a_text);
        std::replace(out.begin(), out.end(), '\n', ' ');
        const auto tail = std::unique(out.begin(), out.end(),
                                      [](char l, char r) { return l == ' ' && r == ' '; });
        out.erase(tail, out.end());
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
    inline std::ptrdiff_t MarkerCountOffset() { return REL::Relocate<std::ptrdiff_t>(0x30480, 0x30478); }

    struct MarkerEntry {
        void*         namePtr;
        RE::RefHandle handle;
        std::uint32_t pad0C;
        RE::BSFixedString nameStr;
        std::uint32_t type;
    };

    constexpr bool IsDungeonMarkerType(std::uint16_t t) {
        switch (t) {
            case 4: case 6: case 7: case 8: case 9: case 10: case 12: case 15:
                return true;
            default: return false;
        }
    }

    inline void DrawClearedCheck(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 outline) {
        dl->AddCircleFilled(c, r, fill);
        dl->AddCircle(c, r, outline, 0, 1.2f);
        const ImVec2 p0{ c.x - r * 0.45f, c.y + r * 0.05f };
        const ImVec2 p1{ c.x - r * 0.10f, c.y + r * 0.40f };
        const ImVec2 p2{ c.x + r * 0.50f, c.y - r * 0.40f };
        dl->AddLine(p0, p1, outline, std::max(1.5f, r * 0.22f));
        dl->AddLine(p1, p2, outline, std::max(1.5f, r * 0.22f));
    }

    constexpr const char* kWorldCategoryNames[] = {
        "Custom Marker", "City", "Town", "Settlement", "Cave", "Camp", "Fort",
        "Nordic Ruin", "Dwemer Ruin", "Shipwreck", "Grove", "Landmark",
        "Dragon Lair", "Farm", "Wood Mill", "Mine", "Imperial Camp",
        "Stormcloak Camp", "Doomstone", "Wheat Mill", "Smelter", "Temple",
        "Stable", "Clearing", "Altar", "Rock", "Lighthouse", "Dwelling",
        "Docks", "Shrine",
        "Imperial Tower", "Pass", "Orc Stronghold", "Giant Camp", "Nordic Tower",
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
        "Animal Den"
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
    _noteDialogInterior = (worldSpaceID == 0);
    _noteBuffer[0]  = '\0';
    _selectedCategory = 0;
    _pendingIsDestination = false;
    _editingMarkerId = 0;
    _editingVanillaFormID = 0;
    _noteDialogOpen = true;
    _noteFocusRequested = true;
    _noteWrapPrimeFrames = 2;
    _notePrevLeftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    _noteOkArmed = false;
    _noteCancelArmed = false;
    _resetImGuiMouseOnNextDraw = true;

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

    std::snprintf(_noteBuffer, sizeof(_noteBuffer), "%s", target->note.c_str());
    _noteDialogInterior   = (target->worldSpaceID == 0);
    _selectedCategory     = CategoryIndexForIcon(target->iconType, _noteDialogInterior);
    _pendingIsDestination = target->isDestination;
    _editingMarkerId      = id;
    _editingVanillaFormID = 0;
    _noteDialogOpen       = true;
    _noteFocusRequested   = true;
    _noteWrapPrimeFrames  = 2;
    _notePrevLeftDown     = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    _noteOkArmed          = false;
    _noteCancelArmed      = false;
    _resetImGuiMouseOnNextDraw = true;

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

    std::string existing = MarkerManager::GetSingleton()->GetVanillaNote(formID);
    std::snprintf(_noteBuffer, sizeof(_noteBuffer), "%s", existing.c_str());

    _editingMarkerId      = 0;
    _editingVanillaFormID = formID;
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

void MapMarkerRenderer::Draw() {
    auto ui = RE::UI::GetSingleton();

    if (IsMapFading()) {
        _activeMarkerId = 0;
        _pinHovered.store(false);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
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
        if (_noteDialogOpen) {
            logger::debug("[NoteDialog] map closed while dialog open - force-resetting state");
            _noteDialogOpen = false;
        }
        _showListPanel = true;
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
        _favBadgeBorn.clear();
        _noteBadgeBorn.clear();
        _clearedBadgeBorn.clear();
        _badgesSeeded = false;
        return;
    }

    auto mapMenu = ui->GetMenu<RE::MapMenu>();
    if (!mapMenu) return;

    bool isLocalMap = false;
    {
        const auto base = reinterpret_cast<std::uintptr_t>(mapMenu.get());
        isLocalMap = *reinterpret_cast<const bool*>(base + 0x3045c);
    }

    ImDrawList* dl       = ImGui::GetBackgroundDrawList();
    ImVec2      dispSize = ImGui::GetIO().DisplaySize;

    ImVec2 mouse{ -1.0f, -1.0f };
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        float w = cursor->screenWidthX;
        float h = cursor->screenWidthY;
        if (w > 0.0f && h > 0.0f) {
            mouse = ImVec2((cursor->cursorPosX / w) * dispSize.x,
                           (cursor->cursorPosY / h) * dispSize.y);
        }
    }

    bool isDialogOpen = ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME) ||
                        ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                        ui->IsMenuOpen(RE::JournalMenu::MENU_NAME);

    if (isDialogOpen) {
        _activeMarkerId = 0;
        _pinHovered.store(false);
        _hoveredMarkerId.store(0);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
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
        _hoveredMarkerId.store(0);
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        {
            std::lock_guard<std::mutex> lk(_panelMutex);
            _panelVisible = false;
            _rowIds.clear();
            _rowDeleteRects.clear();
            _destRowValid = false;
        }
        _pendingPanTarget.store(0);
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

    ApplyPendingPan(mapMenu.get());

    struct Projected {
        std::uint32_t id;
        float         px, py;
        std::string   note;
        std::uint32_t iconType;
        bool          isDestination;
    };
    std::vector<Projected> visible;
    std::vector<Projected> destRings;
    auto markers = MarkerManager::GetSingleton()->GetMarkers();
    visible.reserve(markers.size());

    for (const auto& m : markers) {
        if (m.worldSpaceID == 0) continue;
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
    const float fontRatio  = titleFont->FontSize / ImGui::GetFontSize();
    const float titleScale = std::clamp(Settings::GetSingleton()->titleScale, 0.5f, 3.0f) * fontRatio;
    const float bodyScale  = std::clamp(Settings::GetSingleton()->bodyScale,  0.5f, 3.0f);

    const Projected* active = nullptr;

    if (!_noteDialogOpen && !IsCustomMenuActive()) {
        if (_activeMarkerId != 0) {
            for (const auto& v : visible) {
                if (v.id != _activeMarkerId) continue;
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

    const float timeSec = std::chrono::duration<float>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const auto& v : visible) {
        if (v.isDestination) MapDraw::DrawDestinationRing(dl, v.px, v.py, timeSec);
    }
    for (const auto& v : destRings) {
        MapDraw::DrawDestinationRing(dl, v.px, v.py, timeSec, true);
    }

    {
        constexpr float kBadgeOffsetX = 12.0f;
        constexpr float kBadgeOffsetY = 12.0f;
        constexpr float kCullMargin   = 24.0f;
        constexpr float kBadgePopSec  = 0.2f;
        auto* markerSet = Settings::GetSingleton();
        const float kStarRadius = markerSet->favBadgeSize;
        const float kDotRadius  = markerSet->noteBadgeSize;
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
            return 1.0f - std::pow(1.0f - w, 3.0f);
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
        {
            const float kClearedRadius = markerSet->clearedBadgeSize;
            const ImU32 clearedFill    = markerSet->ClearedBadgeCol();
            const float kClearedLiftY = kBadgeOffsetY + 4.0f;
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
                auto* extra = refr->extraList.GetByType<RE::ExtraMapMarker>();
                if (!extra || !extra->mapData) continue;
                const auto markerType = extra->mapData->type.underlying();
                if (!IsDungeonMarkerType(markerType)) continue;
                if (MarkerFilterHook::IsMarkerFiltered(refr.get(), markerType)) continue;
                auto* loc = refr->GetEditorLocation();
                if (!loc || !loc->IsCleared()) continue;
                auto* ws = refr->GetWorldspace();
                if (!ws) continue;
                bool worldMatch = false;
                for (auto* w = ws; w; w = w->parentWorld) {
                    if (w == currentMapWorld) { worldMatch = true; break; }
                }
                if (!worldMatch) continue;
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

    {
        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
        for (const auto& v : visible) {
            float& anim = _hoverAnim[v.id];
            const float target = (v.id == _activeMarkerId) ? 1.0f : 0.0f;
            const float rate = (target > anim) ? (dt / 0.26f) : (dt / 0.30f);
            anim = std::clamp(anim + (target - anim > 0.0f ? rate : -rate), 0.0f, 1.0f);
        }
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
                std::lock_guard<std::mutex> lk(_panelMutex);
                for (std::size_t i = 0; i < _rowQuestIds.size() && i < _rowQuestPositions.size(); ++i) {
                    if (_rowQuestIds[i] != _pulseQuestId) continue;
                    RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(
                        _rowQuestPositions[i], useFlatMapPlane);
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
            if (foundPos) {
                constexpr float kWaveLife = 1.4f;
                constexpr float kStagger  = 0.4f;
                auto drawWave = [&](float startT, float r0Base, float r0Travel,
                                    float r1Base, float r1Travel,
                                    int peakA0, int peakA1) {
                    const float localT = t - startT;
                    if (localT < 0.0f || localT > kWaveLife) return;
                    const float w = localT / kWaveLife;
                    const float e = 1.0f - std::pow(1.0f - w, 3.0f);
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
                drawWave(0.0f,     18.0f, 26.0f, 12.0f, 16.0f, 220, 180);
                drawWave(kStagger, 18.0f, 26.0f, 12.0f, 16.0f, 220, 180);
            }
        }
    }

    if (active) {
        auto* hoverSet = Settings::GetSingleton();
        MapDraw::InfoPanelLayout pl = MapDraw::ComputeInfoPanel(active->px, active->py, active->note.c_str(),
                                                                 dispSize, titleScale, bodyScale,
                                                                 static_cast<float>(hoverSet->markerHoverOffsetX),
                                                                 static_cast<float>(hoverSet->markerHoverOffsetY));
        MapDraw::DrawPanelCard(dl, pl.panel.mn, pl.panel.mx, hoverSet->BgCol(), hoverSet->BorderCol());
        const float fs   = ImGui::GetFontSize();
        const float hPadX = MapDraw::kHeaderPadX;
        const float hPadY = MapDraw::kHeaderPadY;
        MapDraw::DrawTextShadowed(dl, titleFont, fs * titleScale,
                                  ImVec2(pl.panel.mn.x + hPadX, pl.panel.mn.y + hPadY),
                                  hoverSet->TitleCol(), active->note.c_str(), nullptr,
                                  pl.noteWrapW);
        dl->AddText(ImGui::GetFont(), fs * bodyScale * 0.85f,
                    ImVec2(pl.panel.mn.x + hPadX, pl.panel.mn.y + hPadY + pl.nameH + 2.0f),
                    MapDraw::FadeAlpha(hoverSet->BodyCol(), MapDraw::kLabelAlpha),
                    LocalizationManager::Get("Labels.CustomMarker").c_str());
    }

    _removeButtonTarget.store(0);
    _destButtonTarget.store(0);
    _editButtonTarget.store(0);

    if (_showListPanel) {
        DrawListPanel();
    } else {
        std::lock_guard<std::mutex> lk(_panelMutex);
        _panelVisible = false;
        _rowIds.clear();
        _rowDeleteRects.clear();
    }

    if (!isLocalMap && Settings::GetSingleton()->showFilterPanel) {
        DrawFilterPanel();
    }

    DrawTerrainMenu(isLocalMap);

    DrawCustomContextMenu();

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

    auto* settings = Settings::GetSingleton();

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
        if (!currentMapWorld) return true;
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
        bool          isCustom = false;
        bool          canPan   = false;
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
        for (const auto& q : QuestTracker::GetSingleton()->Get()) {
            const bool kept = !q.hasPosition || belongsToMap(q.worldSpaceID);
            if (qlog) {
                logger::info("[QuestFilter] {:08X} \"{}\" hasPos={} qworld={:08X} mapworld={:08X} src={} kept={}",
                             q.questID, q.name, q.hasPosition, q.worldSpaceID,
                             currentMapWorld ? currentMapWorld->GetFormID() : 0,
                             q.posSource, kept);
            }
            if (!kept) continue;
            activeQuests.push_back(q);
        }
    }

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

    const float baseFS    = ImGui::GetFontSize();

    ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
    if (!titleFont) titleFont = ImGui::GetFont();
    const float titleFS   = titleFont->FontSize * std::clamp(settings->titleScale, 0.5f, 3.0f);
    const float bodyFS    = baseFS * std::clamp(settings->bodyScale,  0.5f, 3.0f);
    const float pad       = 10.0f;

    const float rowHeight = std::floor(bodyFS + 11.0f);
    const float headerH   = std::floor(titleFS + 12.0f);

    constexpr float kGripH = 18.0f;

    constexpr float kBandLift = 3.0f;
    const auto RowTextY = [rowHeight](float a_y, float a_fs) {
        return std::floor(a_y - kBandLift + (rowHeight - a_fs) * 0.5f);
    };
    const float panelW    = static_cast<float>(settings->listPanelWidth);

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

    std::vector<std::string> statRows;
    if (settings->listShowOverview) {
    statRows.reserve(4);
    statRows.push_back(LocalizationManager::Format("Overview.Dungeons", mapStats.dungeonsCleared, mapStats.dungeonsTotal));
    statRows.push_back(LocalizationManager::Format("Overview.Locations", mapStats.locationsDiscovered, mapStats.locationsTotal));
    statRows.push_back(LocalizationManager::Format("Overview.StandingStones", mapStats.stonesFound, mapStats.stonesTotal));
    statRows.push_back(LocalizationManager::Format("Overview.TotalBounty", mapStats.totalBounty));
    }

    float questSectionH = 0.0f;
    for (const auto& q : activeQuests) {
        questSectionH += q.objectiveText.empty() ? rowHeight : std::floor(rowHeight * 1.8f);
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

    tabs.push_back({ ListTab::Overview,  statsHeader,  activeQuests.size() });
    tabs.push_back({ ListTab::Custom,    customHeader, markers.size() });
    tabs.push_back({ ListTab::Favorites, favHeader,    favs.size() });
    tabs.push_back({ ListTab::Notes,     notesHeader,  notes.size() });

    const float tabStripH = std::floor(bodyFS + 14.0f);

    float contentH = 0.0f;
    switch (activeTab) {
    case ListTab::Overview:
        if (settings->listShowOverview) {
            contentH += rowHeight * static_cast<float>(statRows.size());
            contentH += pad + headerH + pad + rowHeight;
        }
        if (settings->listShowActiveQuests) {
            if (settings->listShowOverview) contentH += pad + headerH + pad;
            contentH += activeQuests.empty() ? rowHeight : questSectionH;
        }
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

    const float chromeH = kGripH + tabStripH;

    const float maxH = dispSize.y *
        (std::clamp(settings->listPanelMaxHeightPct, 30, 95) / 100.0f);

    const float minH = dispSize.y *
        (std::clamp(settings->listPanelMinHeightPct, 0,
                    std::clamp(settings->listPanelMaxHeightPct, 30, 95)) / 100.0f);

    const float bodyMaxH = std::max(rowHeight * 3.0f, maxH - chromeH);

    const float bodyMinH = std::clamp(minH - chromeH, 0.0f, bodyMaxH);
    const float bodyH    = std::clamp(contentH, bodyMinH, bodyMaxH);
    const float panelH   = chromeH + bodyH;

    ImVec2 mouse{ -1.0f, -1.0f };
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        const float cw = cursor->screenWidthX;
        const float ch = cursor->screenWidthY;
        if (cw > 0.0f && ch > 0.0f) {
            mouse = ImVec2((cursor->cursorPosX / cw) * dispSize.x,
                           (cursor->cursorPosY / ch) * dispSize.y);
        }
    }

    const float margin = 24.0f;
    if (settings->listPanelPosX < 0 || settings->listPanelPosY < 0) {
        settings->listPanelPosX = static_cast<std::int32_t>(dispSize.x - panelW - margin);
        settings->listPanelPosY = static_cast<std::int32_t>(margin);
    }

    ImVec2 mn(static_cast<float>(settings->listPanelPosX),
              static_cast<float>(settings->listPanelPosY));
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

    ID3D11ShaderResourceView* panelBg = nullptr;
    if (settings->listPanelUseCustomBg) {
        panelBg = BackgroundManager::GetBackground(BackgroundManager::Type::ListPanel,
                                                   settings->listPanelBgIndex);
    }

    constexpr float kBgBleed = 14.0f;
    const ImVec2 bgMin(mn.x - kBgBleed, mn.y - kBgBleed);
    const ImVec2 bgMax(mx.x + kBgBleed, mx.y + kBgBleed);

    if (panelBg) {
        dl->AddImageRounded(reinterpret_cast<ImTextureID>(panelBg), bgMin, bgMax,
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32_WHITE, MapTheme::kPanelRounding);
    } else {
        MapDraw::DrawPanelCard(dl, bgMin, bgMax, settings->BgCol(), settings->BorderCol());
    }

    {
        constexpr float kDotBandH = 12.0f;
        const float gripMidY = mn.y + std::floor(kDotBandH * 0.5f);
        const float gripX0   = mn.x + pad;
        const float gripX1   = mx.x - pad;
        const ImU32 gripCol  = _draggingPanel ? settings->TitleCol() : MapTheme::kSeparator;
        constexpr float kDotSz   = 1.0f;
        constexpr float kDotStep = 4.0f;
        constexpr float kRowGap  = 4.0f;
        for (int r = 0; r < 2; ++r) {
            const float ly = std::floor(gripMidY + (static_cast<float>(r) - 0.5f) * kRowGap);
            const float x0 = gripX0 + static_cast<float>(r) * kDotStep * 0.5f;
            for (float x = x0; x <= gripX1 - kDotSz; x += kDotStep) {
                dl->AddRectFilled(ImVec2(x, ly), ImVec2(x + kDotSz, ly + kDotSz), gripCol);
            }
        }
    }

    constexpr float kRowPadX = 1.0f;
    const float rowsLeft  = mn.x + kRowPadX;
    const float rowsRight = mx.x - kRowPadX;
    const bool  cursorInRowsX = mouse.x >= rowsLeft && mouse.x <= rowsRight;

    constexpr float kRowTextInset = 10.0f;
    const float rowContentX = mn.x + kRowTextInset;

    const float tabTop    = mn.y + kGripH;
    const float tabBottom = tabTop + tabStripH;
    const float tabW      = (mx.x - mn.x) / static_cast<float>(tabs.size());

    dl->AddRectFilled(ImVec2(mn.x + 1.0f, tabTop),
                      ImVec2(mx.x - 1.0f, tabBottom),
                      MapTheme::kTabTrayBg);

    for (std::size_t i = 0; i < tabs.size(); ++i) {
        const float tx0 = mn.x + tabW * static_cast<float>(i);
        const float tx1 = tx0 + tabW;
        const bool  isActive  = (tabs[i].tab == activeTab);
        const bool  tabHover  = mouse.x >= tx0 && mouse.x <= tx1 &&
                                mouse.y >= tabTop && mouse.y <= tabBottom;
        if (tabHover && leftPressed && !_draggingPanel) {
            settings->listActiveTab = static_cast<std::int32_t>(tabs[i].tab);
            settings->SavePositions();
        }
        if (isActive) {
            dl->AddRectFilled(ImVec2(tx0, tabTop), ImVec2(tx1, tabBottom),
                              MapTheme::kTabActiveBg);
            dl->AddLine(ImVec2(tx0 + 4.0f, tabBottom - 1.0f),
                        ImVec2(tx1 - 4.0f, tabBottom - 1.0f),
                        settings->TitleCol(), 2.0f);
        } else if (tabHover) {
            dl->AddRectFilled(ImVec2(tx0, tabTop), ImVec2(tx1, tabBottom),
                              MapTheme::kTabHoverBg);
        }
        std::string label = tabs[i].label;
        const float availW = tabW - 8.0f;
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
        dl->AddLine(ImVec2(dx, tabTop + 3.0f), ImVec2(dx, tabBottom - 3.0f),
                    MapTheme::kTabDivider, 1.0f);
    }

    const float rowContentRight = mx.x - kRowTextInset;

    const float delBtnW   = std::max(20.0f, bodyFS + 6.0f);
    const float delBtnPad = 4.0f;
    const float delBtnX0  = rowContentRight - delBtnW - delBtnPad;
    const float delBtnX1  = rowContentRight - delBtnPad;

    constexpr float kRowTextSlack = 6.0f;

    const float textRightWithBtn = delBtnX0 - kRowTextSlack;
    const float textRightNoBtn   = rowContentRight - kRowTextSlack;

    const float idxGutterW = ImGui::CalcTextSize("00").x * (bodyFS / baseFS);

    const float idxGapW    = std::floor(bodyFS * 0.5f);
    const float idxTextX   = rowContentX + idxGutterW + idxGapW;

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
        for (const auto& row : statRows) {
            const bool rowVisible = (y + rowHeight > rowsTop) && (y < mx.y);
            if (rowVisible) {
                MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                            ImVec2(rowContentX, RowTextY(y, bodyFS)),
                            settings->BodyCol(), row.c_str());
            }
            y += rowHeight;
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
                const bool rowHovered = destRow.canPan && cursorInRowsX &&
                                        mouse.y >= bandTop && mouse.y <= bandBottom;
                const ImU32 bgFill = rowHovered ? MapTheme::kDestRowBgHover : MapTheme::kDestRowBg;
                dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                  ImVec2(rowsRight, bandBottom),
                                  bgFill, 2.0f);
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

    if (activeTab == ListTab::Overview && settings->listShowActiveQuests) {
        {
            if (settings->listShowOverview) {
                y += pad;
                if ((y + headerH > rowsTop) && (y < mx.y)) {
                    MapDraw::DrawTextShadowed(dl, titleFont, titleFS,
                                ImVec2(rowContentX, y),
                                settings->TitleCol(), questHeader.c_str());
                    const float sepLineY = y + headerH;
                    dl->AddLine(ImVec2(rowContentX, sepLineY),
                                ImVec2(rowContentRight, sepLineY),
                                MapTheme::kSeparator, 1.0f);
                }
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
                for (const auto& q : activeQuests) {
                    const float rowH       = q.objectiveText.empty() ? rowHeight : std::floor(rowHeight * 1.8f);
                    const float bandTop    = y - 3.0f;
                    const float bandBottom = y + rowH - 3.0f;
                    const bool  rowVisible = (bandBottom > rowsTop) && (bandTop < mx.y);
                    if (rowVisible) {
                        const bool rowHovered = q.hasPosition && cursorInRowsX &&
                                                mouse.y >= bandTop && mouse.y <= bandBottom;
                        if (rowHovered) {
                            dl->AddRectFilled(ImVec2(rowsLeft, bandTop),
                                              ImVec2(rowsRight, bandBottom),
                                              MapTheme::kRowHoverBg, 2.0f);
                        }
                        const ImU32 textCol = rowHovered ? settings->BodyColHover() : settings->BodyCol();
                        const std::string qName = TruncateRowTextToWidth(
                            q.name, textRightNoBtn - rowContentX, bodyFS);
                        MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                    ImVec2(rowContentX, y), textCol, qName.c_str());
                        if (!q.objectiveText.empty()) {
                            const float objX = rowContentX + 10.0f;
                            const std::string obj = TruncateRowTextToWidth(
                                q.objectiveText, textRightNoBtn - objX, bodyFS * 0.85f);
                            MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS * 0.85f,
                                        ImVec2(objX, y + rowHeight * 0.9f),
                                        MapDraw::WithAlpha(settings->BodyCol(), 0.70f), obj.c_str());
                        }
                        if (q.hasPosition) {
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
                    const std::string resolvedName = ResolveMarkerName(m);
                    const char* displayName = resolvedName.c_str();
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
                    const float nameX = rowContentX + iconSz + std::floor(bodyFS * 0.55f);
                    const std::string rowName =
                        TruncateRowTextToWidth(displayName, textRightBudget - nameX, bodyFS);
                    MapDraw::DrawTextShadowed(dl, ImGui::GetFont(), bodyFS,
                                ImVec2(nameX, RowTextY(y, bodyFS)), textCol, rowName.c_str());
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

void MapMarkerRenderer::ShowListTab(ListTab tab) {
    const auto idx = static_cast<std::int32_t>(tab);
    if (idx < 0 || idx >= static_cast<std::int32_t>(ListTab::Count)) return;

    auto* settings = Settings::GetSingleton();
    settings->listActiveTab = idx;
    _listScrollY[idx] = 0.0f;

    settings->SavePositions();

    _showListPanel = true;
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

    for (int i = 0; i < static_cast<int>(_rowDeleteRects.size()); ++i) {
        const auto& r = _rowDeleteRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            const std::uint32_t id = _rowIds[i];
            logger::debug("[PanelClick] delete btn custom row {} -> marker id {}", i, id);
            MarkerManager::GetSingleton()->RemoveMarker(id);
            return true;
        }
    }

    for (int i = 0; i < static_cast<int>(_rowFavDeleteRects.size()); ++i) {
        const auto& r = _rowFavDeleteRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            const RE::FormID id = _rowFavIds[i];
            logger::debug("[PanelClick] delete btn fav row {} -> form id {}", i, id);
            MarkerManager::GetSingleton()->ToggleFavorite(id, 0, 0, {}, "");
            return true;
        }
    }

    for (int i = 0; i < static_cast<int>(_rowNoteDeleteRects.size()); ++i) {
        const auto& r = _rowNoteDeleteRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            const RE::FormID id = _rowNoteIds[i];
            logger::debug("[PanelClick] delete btn note row {} -> form id {}", i, id);
            MarkerManager::GetSingleton()->SetVanillaNote(id, "");
            return true;
        }
    }

    for (int i = 0; i < static_cast<int>(_rowRects.size()); ++i) {
        const auto& r = _rowRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            _pendingPanTarget.store(_rowIds[i]);
            logger::debug("[PanelClick] hit custom row {} -> marker id {}", i, _rowIds[i]);
            return true;
        }
    }

    for (int i = 0; i < static_cast<int>(_rowFavRects.size()); ++i) {
        const auto& r = _rowFavRects[i];
        if (mx >= r.x0 && mx <= r.x1 && my >= r.y0 && my <= r.y1) {
            _pendingPanFavTarget.store(_rowFavIds[i]);
            logger::debug("[PanelClick] hit fav row {} -> form id {}", i, _rowFavIds[i]);
            return true;
        }
    }

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
            _pendingPanQuestTarget.store(_rowQuestIds[i]);
            logger::debug("[PanelClick] hit quest row {} -> quest id {:08X}", i, _rowQuestIds[i]);
            return true;
        }
    }

    return true;
}

void MapMarkerRenderer::ApplyPendingPan(RE::MapMenu* a_menu) {
    std::uint32_t target = _pendingPanTarget.exchange(0);
    RE::FormID favTarget = _pendingPanFavTarget.exchange(0);
    RE::FormID noteTarget = _pendingPanNoteTarget.exchange(0);
    RE::FormID questTarget = _pendingPanQuestTarget.exchange(0);
    const bool destTarget = _pendingPanDestPos.exchange(false);

    if ((target == 0 && favTarget == 0 && noteTarget == 0 && questTarget == 0 &&
         !destTarget) || !a_menu) return;

    RE::NiPoint3 pos{};
    bool found = false;

    if (destTarget) {
        std::lock_guard<std::mutex> lk(_panelMutex);
        if (_destRowValid) {
            pos   = _destRowPos;
            found = true;
        }
    }

    if (!found && target != 0) {
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
        if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(noteTarget)) {
            pos = ref->GetPosition();
            found = true;
        }
    } else if (questTarget != 0) {
        std::lock_guard<std::mutex> lk(_panelMutex);
        for (std::size_t i = 0; i < _rowQuestIds.size() && i < _rowQuestPositions.size(); ++i) {
            if (_rowQuestIds[i] == questTarget) {
                pos = _rowQuestPositions[i];
                found = true;
                break;
            }
        }
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
        if (questTarget != 0) _pendingPanQuestTarget.store(questTarget);
        if (destTarget) _pendingPanDestPos.store(true);
        return;
    }

    using GlidePan_t = void(*)(RE::TESCamera*, const RE::NiPoint3*, float);

    REL::Relocation<GlidePan_t> glidePan{ REL::RelocationID(52137, 53022) };
    glidePan(camera, &pos, 0.5f);
    logger::debug("[Pan] target custom {} fav {} glidePan called", target, favTarget);

    _pulseMarkerId = target;
    _pulseFavId    = (target == 0) ? favTarget : 0;
    _pulseNoteId   = (target == 0 && favTarget == 0) ? noteTarget : 0;
    _pulseQuestId  = (target == 0 && favTarget == 0 && noteTarget == 0) ? questTarget : 0;
    _pulseStart = std::chrono::steady_clock::now();
}

void MapMarkerRenderer::DrawFilterPanel() {
    using MarkerFilterHook::FocusMode;

    auto* settings = Settings::GetSingleton();

    if (settings->filterPosX >= 0 && settings->filterPosY >= 0) {
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(settings->filterPosX),
                                       static_cast<float>(settings->filterPosY)),
                                ImGuiCond_FirstUseEver);
    }

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImVec4(settings->bodyColorR / 255.0f, settings->bodyColorG / 255.0f,
                                 settings->bodyColorB / 255.0f, 1.0f));

    ImGui::PushStyleColor(ImGuiCol_Separator,     MapTheme::kSeparator);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(8.0f,  6.0f));

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
        {
            const float target    = settings->filterCollapsed ? 0.0f : 1.0f;
            if (!_filterAnimInit) {
                _filterAnim     = target;
                _filterAnimInit = true;
            }
            const float dt        = std::min(ImGui::GetIO().DeltaTime, 0.1f);
            const float smoothing = 0.0005f;
            _filterAnim = target + (_filterAnim - target) * std::pow(smoothing, dt);
            _filterAnim = std::clamp(_filterAnim, 0.0f, 1.0f);
            if (std::abs(_filterAnim - target) < 0.001f) _filterAnim = target;
        }
        const float easedOpen = 1.0f - (1.0f - _filterAnim) * (1.0f - _filterAnim);
        ImDrawList* hdrDl = ImGui::GetWindowDrawList();
        const float btnSz = std::floor(ImGui::GetFontSize() * 1.1f);
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
        static constexpr const char* kModeIcons[] = {
            "city",
            "stable",
            "cave",
            "camp",
            "smelter",
            "landmark",
            "dwelling",
            "quest",
            "shop",
            "loot",
            "custom",
        };
        static_assert(std::size(kModeIcons) == static_cast<std::size_t>(FocusMode::kCount),
                      "kModeIcons must stay in sync with FocusMode");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float fs      = ImGui::GetFontSize();
        const float iconSz  = std::floor(fs * 1.35f);
        const float eyeSz   = fs * 1.05f;
        const float gap     = 8.0f;
        const float rowPadY = 3.0f;
        const float rowH    = std::max(fs, iconSz) + rowPadY * 2.0f;
        float maxLabelW = 0.0f;
        std::vector<std::string> labels(static_cast<std::size_t>(FocusMode::kCount));
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const auto  mode     = static_cast<FocusMode>(i);
            const char* modeName = MarkerFilterHook::GetModeName(mode);
            std::string label = LocalizationManager::GetSingleton()->GetLabel(
                std::string{ "FilterMode." } + modeName);
            if (label.starts_with("FilterMode.")) label = modeName;
            if (const auto n = MarkerFilterHook::GetModeCount(mode); n > 0) {
                label += " (" + std::to_string(n) + ")";
            }
            labels[i]  = std::move(label);
            maxLabelW  = std::max(maxLabelW, ImGui::CalcTextSize(labels[i].c_str()).x);
        }
        {
            std::string setLbl = LocalizationManager::GetSingleton()->GetLabel("ListPanel.Settings");
            if (setLbl == "ListPanel.Settings") setLbl = "Settings";
            maxLabelW = std::max(maxLabelW, ImGui::CalcTextSize(setLbl.c_str()).x);
        }
        const float rowWFull = iconSz + gap + maxLabelW + gap * 2.0f + eyeSz;
        const float rowWIcon = std::max(iconSz, btnSz);
        const float rowW     = rowWIcon + (rowWFull - rowWIcon) * easedOpen;
        {
            const ImVec2 hMin = ImGui::GetCursorScreenPos();
            const float  hH   = std::max(btnSz, titleFs);
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
                settings->SavePositions();
            }
            if (btnHover) {
                hdrDl->AddRectFilled(ImVec2(bx - 3.0f, by - 3.0f),
                                     ImVec2(bx + btnSz + 3.0f, by + btnSz + 3.0f),
                                     MapTheme::kRowHoverBg, 3.0f);
            }
            {
                const float cx   = bx + btnSz * 0.5f;
                const float cy   = by + btnSz * 0.5f;
                const float armX = btnSz * 0.22f;
                const float armY = btnSz * 0.28f;
                const float tipX = cx + armX * (1.0f - 2.0f * easedOpen);
                const float bkX  = cx - armX * (1.0f - 2.0f * easedOpen);
                const ImU32 chevCol = btnHover ? settings->BodyColHover() : settings->TitleCol();
                hdrDl->AddTriangleFilled(ImVec2(bkX, cy - armY),
                                         ImVec2(tipX, cy),
                                         ImVec2(bkX, cy + armY), chevCol);
            }
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
            ImGui::SetCursorScreenPos(ImVec2(hMin.x, hMin.y + hH));
            ImGui::Dummy(ImVec2(rowW, 2.0f));
        }
        ImGui::Separator();
        ImGui::Spacing();
        ID3D11ShaderResourceView* eyeSrv = TextureManager::GetIconByName("eye");
        std::string  tipText;
        ImVec2       tipAnchor{};
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const auto  mode     = static_cast<FocusMode>(i);
            const bool  on       = MarkerFilterHook::IsModeEnabled(mode);
            const char* modeName = MarkerFilterHook::GetModeName(mode);
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
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
                if (easedOpen <= 0.02f) {
                    tipText   = labels[i];
                    tipAnchor = ImVec2(rowMin.x + rowW + 8.0f, rowMin.y);
                }
            }
            const float    dim      = on ? 1.0f : 0.35f;
            const ImU32    iconTint = IM_COL32(255, 255, 255, static_cast<int>(255 * dim));
            const ImU32    textCol  = on ? (hovered ? settings->BodyColHover() : settings->BodyCol())
                                         : MapTheme::kMutedText;
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
            }
        }
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
                tipText   = setLabel;
                tipAnchor = ImVec2(setMin.x + rowW + 8.0f, setMin.y);
            }
        }
        if (!tipText.empty()) {
            ImDrawList* tipDl = ImGui::GetForegroundDrawList();
            const std::string& tip  = tipText;
            const ImVec2       tsz  = ImGui::CalcTextSize(tip.c_str());
            constexpr float    tpad = 6.0f;
            const ImVec2 tmn = tipAnchor;
            const ImVec2 tmx = ImVec2(tmn.x + tsz.x + tpad * 2.0f,
                                      tmn.y + tsz.y + tpad * 2.0f);
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
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        _terrainMenuCursorX = cursor->cursorPosX;
        _terrainMenuCursorY = cursor->cursorPosY;
    }

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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(8.0f,  6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    if (ImGui::BeginPopup("TerrainContextMenu")) {
        ImGui::SetWindowFontScale(std::clamp(settings->bodyScale, 0.5f, 3.0f));
        if (ImGui::MenuItem(LocalizationManager::Get("Menu.AddMarkerHere").c_str())) {
            _pendingTerrainPlace.store(true);
        }
        ImGui::BeginDisabled();
        ImGui::MenuItem(LocalizationManager::Get("Menu.SetDestination").c_str());
        ImGui::EndDisabled();
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(8.0f,  6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    const bool popupOpen = ImGui::BeginPopup("CustomContextMenu");

    _customMenuVisible = popupOpen;
    if (popupOpen) {
        ImGui::SetWindowFontScale(std::clamp(Settings::GetSingleton()->bodyScale, 0.5f, 3.0f));
        const std::uint32_t id = _customContextMarkerId;
        std::string note;
        bool isDest = false;
        bool hasMarker = false;
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
    if (_resetImGuiMouseOnNextDraw) {
        auto& io = ImGui::GetIO();
        io.MouseDown[0] = false;
        io.AddMouseButtonEvent(0, false);
        _resetImGuiMouseOnNextDraw = false;
    }

    ImVec2 dispSize = ImGui::GetIO().DisplaySize;

    auto* settings = Settings::GetSingleton();
    const float bodyScale  = std::clamp(settings->bodyScale,  0.5f, 3.0f);
    const float titleScale = std::clamp(settings->titleScale, 0.5f, 3.0f);
    const float fontScale  = ImGui::GetFontSize() / 13.0f;
    float dlgW = 340.0f * bodyScale * fontScale;

    if (_editingVanillaFormID == 0) {
        const auto& style = ImGui::GetStyle();
        const float cellW = 38.0f + style.ItemSpacing.x;
        dlgW = std::max(dlgW, cellW * 10.0f + style.WindowPadding.x * 2.0f);
    }

    {
        ImFont* measureFont = FontManager::GetSingleton()->GetHeadingFont();
        if (!measureFont) measureFont = ImGui::GetFont();
        const float titleFS = measureFont->FontSize * titleScale;
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
        const float maxDlgW = dispSize.x * 0.8f;
        if (needed > dlgW) dlgW = std::min(needed, std::max(dlgW, maxDlgW));
    }

    float dlgHEstimate = 190.0f * bodyScale * fontScale;
    if (_editingVanillaFormID == 0) dlgHEstimate += 150.0f * bodyScale * fontScale;
    if (_editingVanillaFormID != 0) dlgHEstimate += ImGui::GetFontSize() * bodyScale * 3.5f;
    if (ImFont* hf = FontManager::GetSingleton()->GetHeadingFont()) {
        dlgHEstimate += std::max(0.0f, hf->FontSize - ImGui::GetFontSize()) * titleScale;
    }

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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(8.0f,  8.0f));

    const bool vanillaMode = (_editingVanillaFormID != 0);

    if (ImGui::Begin("##NoteDialog", nullptr, flags)) {
        ImFont* dlgTitleFont = FontManager::GetSingleton()->GetHeadingFont();
        if (dlgTitleFont) ImGui::PushFont(dlgTitleFont);
        ImGui::SetWindowFontScale(titleScale);
        if (vanillaMode) {
            ImGui::TextColored(MapTheme::kDlgTitleText(), "Note: %s", _vanillaNoteName.c_str());
        } else {
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
        const float inputW  = ImGui::GetContentRegionAvail().x;
        if (_noteFocusRequested) {
            ImGui::SetKeyboardFocusHere();
            if (ImGui::GetIO().WantTextInput) {
                _noteFocusRequested = false;
            }
        }
        bool confirmed = false;
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
        ImGui::Spacing();
        const auto& catTbl        = CategoriesFor(_noteDialogInterior);
        const auto* categoryNames  = catTbl.names;
        const auto* categoryValues = catTbl.values;
        const int   categoryCount  = catTbl.count;
        if (!vanillaMode) {
            constexpr int   kPerRow = 10;
            constexpr float kCellSz = 30.0f;
            MapDraw::DrawCategoryGrid(categoryNames, categoryValues, categoryCount,
                                      _selectedCategory, kPerRow, kCellSz,
                                      !_noteDialogInterior);
            ImGui::Spacing();
            ImGui::Checkbox(LocalizationManager::Get("Dialog.SetAsDestination").c_str(),
                            &_pendingIsDestination);
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
        ImGui::SameLine();
        const bool cancelClicked = ImGui::Button(LocalizationManager::Get("Dialog.Cancel").c_str(), ImVec2(btnW, 0));
        const ImVec2 cancelMin = ImGui::GetItemRectMin();
        const ImVec2 cancelMax = ImGui::GetItemRectMax();
        const bool anyPopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
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
        if (okClicked || okReleasedDirect || confirmed) {
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
