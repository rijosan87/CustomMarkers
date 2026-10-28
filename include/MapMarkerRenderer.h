#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
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

    // Draw markers using ImGui (called every frame while map is open)
    void Draw();

    std::uint32_t GetRemoveButtonTarget() const { return _removeButtonTarget.load(); }

    std::uint32_t GetDestButtonTarget() const { return _destButtonTarget.load(); }

    // Marker id whose "Edit Note" button the cursor is currently over, or 0.
    // MarkerHoverHook reads this on click and calls BeginEditMarker.
    std::uint32_t GetEditButtonTarget() const { return _editButtonTarget.load(); }

    void BeginEditMarker(std::uint32_t id);

    void BeginEditVanillaNote(RE::FormID formID, const std::string& markerName);

    // Toggle the custom marker list panel.
    void ToggleListPanel();

    void OpenTerrainMenu();

    void OpenCustomContextMenu(std::uint32_t id);

    void OpenNoteDialog(const RE::NiPoint3& pos, RE::FormID worldSpaceID, RE::FormID cellID);

    // True while the note entry dialog is open and waiting for input.
    bool IsNoteDialogOpen() const { return _noteDialogOpen; }

    bool IsCustomMenuActive() const { return _openCustomContextMenu.load() || _customMenuVisible; }

    bool IsCustomPinHovered() const { return _pinHovered.load(); }

    std::uint32_t GetHoveredMarkerId() const { return _hoveredMarkerId.load(); }

    bool TryHandlePanelClick();

    bool TryHandleControllerMapActivation();

    enum class ContextKind : std::uint8_t { None, Terrain, Custom, Vanilla, LocalDoor };
    struct ContextRowHit { float x0, y0, x1, y1; bool enabled; };

    // Input queues context actions; the popup draw consumes them in its own ImGui frame.
    void BeginControllerContextFrame();
    void PublishControllerContextPopup(ContextKind kind, float x0, float y0,
                                       float x1, float y1,
                                       const ContextRowHit* rows, std::size_t rowCount);
    void EndControllerContextFrame();
    void MarkControllerContextOpening();
    bool IsControllerContextActiveOrOpening() const;
    bool QueueControllerContextAction();
    bool QueueControllerContextClose();
    int TakeControllerContextAction(ContextKind kind);
    void QueueControllerNoteActivation();

    bool CursorOverPanel() const;

private:
    MapMarkerRenderer() = default;
    ~MapMarkerRenderer() = default;
    MapMarkerRenderer(const MapMarkerRenderer&) = delete;
    MapMarkerRenderer& operator=(const MapMarkerRenderer&) = delete;

    std::uint32_t              _activeMarkerId = 0;      // marker whose panel is shown
    std::atomic<bool>          _pinHovered{ false };     // written by Draw(), read by hook thread
    std::atomic<std::uint32_t> _hoveredMarkerId{ 0 };    // id twin of _pinHovered, for right-click routing
    std::atomic<std::uint32_t> _removeButtonTarget{ 0 }; // X button under the cursor
    std::atomic<std::uint32_t> _destButtonTarget{ 0 };   // Set/Clear-destination btn under cursor
    std::atomic<std::uint32_t> _editButtonTarget{ 0 };   // Edit-Note button under cursor
    std::atomic<bool>          _showListPanel{ true };  // seeded from Settings::listPanelVisible between map sessions

    float                      _listScrollY[static_cast<std::size_t>(ListTab::Count)] = {};
    float                      _listScrollMax = 0.0f;

public:
    void ShowListTab(ListTab tab);

private:

    struct ControllerContextPopup {
        ContextKind kind = ContextKind::None;
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        ContextRowHit rows[3]{};
        std::size_t rowCount = 0;
    };
    mutable std::mutex _controllerContextMutex;
    ControllerContextPopup _controllerContextPublished{}; // input thread reads only this completed frame
    ControllerContextPopup _controllerContextStaged{};    // render thread only until EndControllerContextFrame
    bool _controllerContextOpening = false;
    bool _controllerContextActionInFlight = false;
    int _pendingControllerContextAction = -1; // -1 none, -2 close, 0..2 row
    ContextKind _pendingControllerContextKind = ContextKind::None;
    std::uint64_t _controllerContextRequestSerial = 0;
    std::uint64_t _controllerContextFrameSerial = 0; // render thread only
    bool _pendingControllerNoteActivation = false;
    float _controllerNoteX = -1.0f, _controllerNoteY = -1.0f;

    mutable std::mutex _panelMutex;
    struct Rect2D { float x0, y0, x1, y1; };
    bool               _panelVisible    = false; // true while a panel rect is valid
    float              _panelMinX = 0, _panelMinY = 0;
    float              _panelMaxX = 0, _panelMaxY = 0;
    Rect2D             _controllerTabStrip{};
    bool               _filterVisible = false;
    Rect2D             _filterRect{};
    Rect2D             _filterCollapseRect{};
    Rect2D             _filterSettingsRect{};
    std::vector<Rect2D> _filterRowRects;
    std::atomic<std::int32_t> _pendingControllerTab{ -1 };
    // -1 = none, -2 = collapse, -3 = settings, 0..N-1 = filter category.
    std::atomic<std::int32_t> _pendingControllerFilter{ -1 };
    float              _rowTopY  = 0;     // y of first row (legacy for custom markers)
    float              _rowHeight = 0;    // height per row
    std::vector<std::uint32_t> _rowIds;   // marker id per visible row, in order
    std::vector<Rect2D> _rowRects;        // row hitbox for panning

    // Per-row delete button rects, parallel to _rowIds.
    std::vector<Rect2D> _rowDeleteRects;

    std::vector<RE::FormID> _rowFavIds;       // favorite formID per visible row
    std::vector<Rect2D>     _rowFavRects;     // favorite row hitbox for panning
    std::vector<Rect2D>     _rowFavDeleteRects; // favorite delete button rects

    std::vector<RE::FormID> _rowNoteIds;        // vanilla note formID per visible row
    std::vector<Rect2D>     _rowNoteRects;      // note row hitbox for panning
    std::vector<Rect2D>     _rowNoteDeleteRects; // note delete button rects

    std::vector<RE::FormID>   _rowQuestIds;
    std::vector<Rect2D>       _rowQuestRects;
    std::vector<RE::NiPoint3> _rowQuestPositions;

    bool          _destRowValid = false;
    Rect2D        _destRowRect{};
    RE::NiPoint3  _destRowPos{};

    std::atomic<std::uint32_t> _pendingPanTarget{ 0 };
    std::atomic<RE::FormID>    _pendingPanFavTarget{ 0 };
    std::atomic<RE::FormID>    _pendingPanNoteTarget{ 0 };
    RE::NiPoint3               _pendingPanQuestPos{};        // guarded by _panelMutex
    RE::FormID                 _pendingPanQuestID = 0;       // guarded by _panelMutex, diagnostics only
    std::atomic<bool>          _pendingPanQuestPosValid{ false };
    // Destination row pan. A flag, not an id: the position is already published
    // in _destRowPos, and a vanilla door destination has no id to resolve.
    std::atomic<bool>          _pendingPanDestPos{ false };

    std::uint32_t _pulseMarkerId = 0;
    RE::FormID    _pulseFavId = 0;
    RE::FormID    _pulseNoteId = 0;   // vanilla-note row pan; projected like favorites
    RE::NiPoint3  _pulseQuestPos{};   // render-thread state; exact world position of the panned quest
    RE::FormID    _pulseQuestId = 0;  // diagnostics and active-pulse gate (> 0 means active)
    std::chrono::steady_clock::time_point _pulseStart{};

    // Map Filters window: edge-detect left-button release to auto-persist its
    // position to INI (it's a real ImGui window, so ImGui owns the drag).
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
    bool TakeControllerNoteActivation(float& x, float& y);

    std::atomic<bool> _openTerrainMenu{ false };
    // Set when "Add Marker Here" is clicked; consumed same frame to call the
    // vanilla MapMenu::PlaceMarker at the cursor. See DrawTerrainMenu.
    std::atomic<bool> _pendingTerrainPlace{ false };
    float _terrainMenuCursorX = -1.0f;
    float _terrainMenuCursorY = -1.0f;
    void DrawTerrainMenu(bool isLocalMap);
    void ApplyPendingPan(RE::MapMenu* a_menu);

    std::atomic<bool>         _openCustomContextMenu{ false };
    std::uint32_t             _customContextMarkerId = 0; // id at menu-open time
    bool                      _customMenuVisible = false; // popup drawn last frame
    void DrawCustomContextMenu();

    // Note dialog state. Written on the game thread (OpenNoteDialog),
    // read and drawn on the render thread (DrawNoteDialog).
    bool         _noteDialogOpen     = false;
    bool         _noteDialogInterior = false;  // combo shows interior category list
    bool         _noteFocusRequested = false;
    int          _noteWrapPrimeFrames = 0;
    bool         _resetImGuiMouseOnNextDraw = false;
    bool         _notePrevLeftDown = false;
    bool         _noteOkArmed      = false;
    bool         _noteCancelArmed  = false;
    RE::NiPoint3 _pendingPos{};
    RE::FormID   _pendingWorldSpace = 0;
    RE::FormID   _pendingCell       = 0;
    // 1024 so long vanilla-marker notes fit the multiline editor; custom
    // titles rarely pass 60 chars but share the buffer.
    char         _noteBuffer[1024]  = {};
    int          _selectedCategory  = 0;
    bool         _pendingIsDestination = false; // note dialog checkbox state
    std::uint32_t _editingMarkerId = 0;
    RE::FormID    _editingVanillaFormID = 0;
    std::string   _vanillaNoteName;          // header label (the location name)
    float        _noteDialogX = -1.0f;
    float        _noteDialogY = -1.0f;

    std::unordered_map<std::uint32_t, float> _hoverAnim;

    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> _favBadgeBorn;
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> _noteBadgeBorn;
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> _clearedBadgeBorn;
    bool _badgesSeeded = false; // false right after map-closed reset; forces a seed pass
};
