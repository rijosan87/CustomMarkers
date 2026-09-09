#pragma once

// Main world map renderer for custom pins, tooltips, list sidebar, and filters.


#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <RE/Skyrim.h>

namespace RE { class MapMenu; }

bool IsMapFading();

class MapMarkerRenderer {
public:

    enum class ListTab : std::int32_t { Overview = 0, Custom, Favorites, Notes, Count };

    static MapMarkerRenderer* GetSingleton() {
        static MapMarkerRenderer singleton;
        return &singleton;
    }

    void Install();

    void Draw();

    std::uint32_t GetRemoveButtonTarget() const { return _removeButtonTarget.load(); }

    std::uint32_t GetDestButtonTarget() const { return _destButtonTarget.load(); }

    std::uint32_t GetEditButtonTarget() const { return _editButtonTarget.load(); }

    void BeginEditMarker(std::uint32_t id);

    void BeginEditVanillaNote(RE::FormID formID, const std::string& markerName);

    void ToggleListPanel() { _showListPanel = !_showListPanel; }

    void OpenTerrainMenu();

    void OpenCustomContextMenu(std::uint32_t id);

    void OpenNoteDialog(const RE::NiPoint3& pos, RE::FormID worldSpaceID, RE::FormID cellID);

    bool IsNoteDialogOpen() const { return _noteDialogOpen; }

    bool IsCustomMenuActive() const { return _openCustomContextMenu.load() || _customMenuVisible; }

    bool IsCustomPinHovered() const { return _pinHovered.load(); }

    std::uint32_t GetHoveredMarkerId() const { return _hoveredMarkerId.load(); }

    bool TryHandlePanelClick();

    bool CursorOverPanel() const;

private:
    MapMarkerRenderer() = default;
    ~MapMarkerRenderer() = default;
    MapMarkerRenderer(const MapMarkerRenderer&) = delete;
    MapMarkerRenderer& operator=(const MapMarkerRenderer&) = delete;

    std::uint32_t              _activeMarkerId = 0;
    std::atomic<bool>          _pinHovered{ false };
    std::atomic<std::uint32_t> _hoveredMarkerId{ 0 };
    std::atomic<std::uint32_t> _removeButtonTarget{ 0 };
    std::atomic<std::uint32_t> _destButtonTarget{ 0 };
    std::atomic<std::uint32_t> _editButtonTarget{ 0 };
    bool                       _showListPanel = true;

    float                      _listScrollY[static_cast<std::size_t>(ListTab::Count)] = {};
    float                      _listScrollMax = 0.0f;

public:

    void ShowListTab(ListTab tab);

private:

    mutable std::mutex _panelMutex;
    bool               _panelVisible    = false;
    float              _panelMinX = 0, _panelMinY = 0;
    float              _panelMaxX = 0, _panelMaxY = 0;
    float              _rowTopY  = 0;
    float              _rowHeight = 0;
    std::vector<std::uint32_t> _rowIds;
    struct Rect2D { float x0, y0, x1, y1; };
    std::vector<Rect2D> _rowRects;

    std::vector<Rect2D> _rowDeleteRects;

    std::vector<RE::FormID> _rowFavIds;
    std::vector<Rect2D>     _rowFavRects;
    std::vector<Rect2D>     _rowFavDeleteRects;

    std::vector<RE::FormID> _rowNoteIds;
    std::vector<Rect2D>     _rowNoteRects;
    std::vector<Rect2D>     _rowNoteDeleteRects;

    std::vector<RE::FormID>   _rowQuestIds;
    std::vector<Rect2D>       _rowQuestRects;
    std::vector<RE::NiPoint3> _rowQuestPositions;

    bool          _destRowValid = false;
    Rect2D        _destRowRect{};
    RE::NiPoint3  _destRowPos{};

    std::atomic<std::uint32_t> _pendingPanTarget{ 0 };
    std::atomic<RE::FormID>    _pendingPanFavTarget{ 0 };
    std::atomic<RE::FormID>    _pendingPanNoteTarget{ 0 };
    std::atomic<RE::FormID>    _pendingPanQuestTarget{ 0 };

    std::atomic<bool>          _pendingPanDestPos{ false };

    std::uint32_t _pulseMarkerId = 0;
    RE::FormID    _pulseFavId = 0;
    RE::FormID    _pulseNoteId = 0;
    RE::FormID    _pulseQuestId = 0;
    std::chrono::steady_clock::time_point _pulseStart{};

    bool _wasDraggingFilter = false;

    float _filterAnim = 1.0f;
    bool  _filterAnimInit = false;

    bool  _draggingPanel = false;
    bool  _panelPrevLeftDown = false;
    float _panelGrabDX = 0, _panelGrabDY = 0;

    void DrawListPanel();

    struct NoteRow { RE::FormID formID; std::string displayName; RE::NiPoint3 position; };
    void DrawFilterPanel();
    void DrawNoteDialog();

    std::atomic<bool> _openTerrainMenu{ false };

    std::atomic<bool> _pendingTerrainPlace{ false };

    float _terrainMenuCursorX = -1.0f;
    float _terrainMenuCursorY = -1.0f;
    void DrawTerrainMenu(bool isLocalMap);
    void ApplyPendingPan(RE::MapMenu* a_menu);

    std::atomic<bool>         _openCustomContextMenu{ false };
    std::uint32_t             _customContextMarkerId = 0;
    bool                      _customMenuVisible = false;
    void DrawCustomContextMenu();

    bool         _noteDialogOpen     = false;
    bool         _noteDialogInterior = false;
    bool         _noteFocusRequested = false;

    int          _noteWrapPrimeFrames = 0;

    bool         _resetImGuiMouseOnNextDraw = false;

    bool         _notePrevLeftDown = false;
    bool         _noteOkArmed      = false;
    bool         _noteCancelArmed  = false;
    RE::NiPoint3 _pendingPos{};
    RE::FormID   _pendingWorldSpace = 0;
    RE::FormID   _pendingCell       = 0;

    char         _noteBuffer[1024]  = {};
    int          _selectedCategory  = 0;
    bool         _pendingIsDestination = false;

    std::uint32_t _editingMarkerId = 0;

    RE::FormID    _editingVanillaFormID = 0;
    std::string   _vanillaNoteName;

    float        _noteDialogX = -1.0f;
    float        _noteDialogY = -1.0f;

    std::unordered_map<std::uint32_t, float> _hoverAnim;

    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> _favBadgeBorn;
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> _noteBadgeBorn;

    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> _clearedBadgeBorn;
    bool _badgesSeeded = false;
};
