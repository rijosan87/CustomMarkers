#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <RE/Skyrim.h>

namespace RE { class MapMenu; }

// True while the screen is mid-fade (FaderMenu open). The engine raises the
// fader during both the map open and close transitions, covering the screen in
// black. Renderers gate on this so our ImGui layer never paints on top of the
// black fade, while staying visible during normal interactive map use (fader
// down). Confirmed in-game; see plugin.cpp for the close-transition timeline.
bool IsMapFading();

// Draws our custom markers on the world map by projecting each saved world
// position to screen coordinates with the engine's own NiCamera matrix.
// Hover panels are drawn manually with ImGui draw lists; the remove action is
// routed through the placement hook (see GetRemoveButtonTarget).
class MapMarkerRenderer {
public:
    // List-panel tabs. One section is visible at a time, which is what keeps
    // the scroll region bounded no matter how many favorites or notes exist.
    // APPEND-ONLY before Count: the active tab persists to the INI as an int
    // (Settings::listActiveTab), so inserting a tab mid-list shifts every
    // saved index. Overview carries BOTH the stat rows and Active Quests.
    enum class ListTab : std::int32_t { Overview = 0, Custom, Favorites, Notes, Count };

    static MapMarkerRenderer* GetSingleton() {
        static MapMarkerRenderer singleton;
        return &singleton;
    }

    void Install();

    // Draw markers using ImGui (called every frame while map is open)
    void Draw();

    // Marker id whose remove button the menu cursor is currently over, or 0.
    // PlaceMarkerHook reads this to turn a map click into a delete instead of
    // a new marker. Written on the render thread, read on the game thread.
    std::uint32_t GetRemoveButtonTarget() const { return _removeButtonTarget.load(); }

    // Marker id whose "Set as Destination" / "Clear Destination" button the
    // cursor is currently over, or 0. MarkerHoverHook reads this on click to
    // toggle the destination flag via MarkerManager::SetDestination.
    std::uint32_t GetDestButtonTarget() const { return _destButtonTarget.load(); }

    // Marker id whose "Edit Note" button the cursor is currently over, or 0.
    // MarkerHoverHook reads this on click and calls BeginEditMarker.
    std::uint32_t GetEditButtonTarget() const { return _editButtonTarget.load(); }

    // Reopen the note dialog seeded with an existing marker's note/category/
    // destination so the user can edit it. On confirm the dialog updates that
    // marker in place instead of adding a new one. Called from MarkerHoverHook
    // when the hover-panel "Edit Note" button is clicked.
    void BeginEditMarker(std::uint32_t id);

    // Open the note dialog bound to a vanilla map marker (not one of our custom
    // pins). On confirm the note is stored via MarkerManager::SetVanillaNote,
    // keyed by the vanilla ref's FormID. The category combo and destination
    // checkbox are hidden in this mode (a vanilla marker has neither). Called
    // from HoverImageRenderer's right-click "Add/Edit Note" menu item.
    void BeginEditVanillaNote(RE::FormID formID, const std::string& markerName);

    // Toggle the custom marker list panel.
    void ToggleListPanel();

    // Right-click on empty terrain opens this context menu ("Add Marker Here",
    // "Set Destination Here"). Called from InputHook on the right-button press
    // when nothing (no vanilla marker, no custom pin) is under the cursor.
    // Just requests the popup; it's actually opened on the render thread in
    // DrawTerrainMenu() so OpenPopup runs inside the ImGui frame.
    void OpenTerrainMenu();

    // Right-click on one of our own custom pins (world OR local map — both
    // renderers call this, the popup itself is drawn once here since
    // MarkerManager/BeginEditMarker are owned by this class). Same deferred-
    // open pattern as OpenTerrainMenu: cache the id now, OpenPopup next frame
    // inside DrawCustomContextMenu() so it runs inside the ImGui frame. The
    // note (header label) is looked up live from MarkerManager at draw time
    // instead of cached, so a Rename made through the menu itself is reflected
    // immediately if the popup stays open.
    void OpenCustomContextMenu(std::uint32_t id);

    // Called by PlaceMarkerHook instead of AddMarker directly. Opens the note
    // entry dialog; the marker is added when the user confirms.
    void OpenNoteDialog(const RE::NiPoint3& pos, RE::FormID worldSpaceID, RE::FormID cellID);

    // True while the note entry dialog is open and waiting for input.
    bool IsNoteDialogOpen() const { return _noteDialogOpen; }

    // True from the right-click that requests the custom-pin context menu until
    // the popup closes. Hover panels (world + local) hide instantly on it,
    // matching how the vanilla hover card vanishes the moment its menu opens.
    bool IsCustomMenuActive() const { return _openCustomContextMenu.load() || _customMenuVisible; }

    // True when the cursor is hovering over one of our own custom pins.
    // Used by MarkerHoverHook to swallow the click and prevent re-placement.
    // _pinHovered is atomic so it's safe to read from the MapMenu update thread.
    bool IsCustomPinHovered() const { return _pinHovered.load(); }

    // id of the currently-hovered custom pin, or 0. Written alongside
    // _pinHovered in Draw(); read by InputHook's right-click handler to open
    // DrawCustomContextMenu for the correct marker.
    std::uint32_t GetHoveredMarkerId() const { return _hoveredMarkerId.load(); }

    // Called by PlaceMarkerHook before placing. Returns true if the click was
    // inside the list panel (panned to a row, or just absorbed on the chrome).
    // When true, the placement hook must NOT add a new marker.
    bool TryHandlePanelClick();

    // True when the engine menu cursor is currently inside the visible list
    // panel rectangle. Sampled on the game thread by MarkerHoverHook to force
    // MapMenu+0x30558 to -1 so vanilla markers under the panel don't steal
    // the click from MapMenu::OnMouseDown.
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

    // List-panel scroll. Render thread only (DrawListPanel), so no mutex.
    // One offset PER TAB: a shared offset would carry a deep scroll in Custom
    // over to a two-row Favorites tab and land it clamped at a random spot.
    // Not persisted: reopening the map should start at the top.
    float                      _listScrollY[static_cast<std::size_t>(ListTab::Count)] = {};
    float                      _listScrollMax = 0.0f;

public:
    // Called from the add paths (place marker, favorite on, note save) so the
    // panel jumps to the tab the new row landed in -- otherwise the row is
    // added to a section the player cannot see. Resets that tab's scroll so
    // the section starts at the top. Game thread; the render thread only reads
    // these, and a one-frame-stale tab index is harmless.
    void ShowListTab(ListTab tab);

private:

    // List-panel hit testing. Written on the render thread inside Draw() and
    // read on the game thread by TryHandlePanelClick() / PlaceMarkerHook.
    // Stored as floats packed into atomics via simple type punning would be
    // overkill - the renderer thread updates them every frame and the game
    // thread only samples them on a click, so a torn read at worst aborts one
    // click. Plain members guarded by a mutex are clearest here.
    mutable std::mutex _panelMutex;
    bool               _panelVisible    = false; // true while a panel rect is valid
    float              _panelMinX = 0, _panelMinY = 0;
    float              _panelMaxX = 0, _panelMaxY = 0;
    float              _rowTopY  = 0;     // y of first row (legacy for custom markers)
    float              _rowHeight = 0;    // height per row
    std::vector<std::uint32_t> _rowIds;   // marker id per visible row, in order
    struct Rect2D { float x0, y0, x1, y1; };
    std::vector<Rect2D> _rowRects;        // row hitbox for panning

    // Per-row delete button rects, parallel to _rowIds.
    std::vector<Rect2D> _rowDeleteRects;
    
    std::vector<RE::FormID> _rowFavIds;       // favorite formID per visible row
    std::vector<Rect2D>     _rowFavRects;     // favorite row hitbox for panning
    std::vector<Rect2D>     _rowFavDeleteRects; // favorite delete button rects

    std::vector<RE::FormID> _rowNoteIds;        // vanilla note formID per visible row
    std::vector<Rect2D>     _rowNoteRects;      // note row hitbox for panning
    std::vector<Rect2D>     _rowNoteDeleteRects; // note delete button rects

    // Active-quest rows. Only quests with a resolved position (sources 1..4) get
    // an entry, row-aligned with _rowQuestRects and _rowQuestPositions. The position
    // is cached here rather than re-derived at pan time: a quest FormID alone cannot
    // distinguish multiple miscellaneous objectives sharing one quest, and the compass
    // buffer may have changed by the time the pan runs. No delete rects, quest rows have no X button.
    std::vector<RE::FormID>   _rowQuestIds;
    std::vector<Rect2D>       _rowQuestRects;
    std::vector<RE::NiPoint3> _rowQuestPositions;

    // Overview tab's Destination row. Carries the POSITION directly rather than
    // an id: a vanilla destination set on a local-map door has no marker id to
    // key a lookup on, and the record is deliberately unreachable from the UI.
    // _destRowValid is false when no destination is set, or when it is an
    // interior door with no world position to pan to.
    bool          _destRowValid = false;
    Rect2D        _destRowRect{};
    RE::NiPoint3  _destRowPos{};

    // Set by TryHandlePanelClick; consumed next frame by ApplyPendingPan so the
    // camera write happens on the render thread alongside the existing camera
    // read. 0 = nothing pending.
    std::atomic<std::uint32_t> _pendingPanTarget{ 0 };
    std::atomic<RE::FormID>    _pendingPanFavTarget{ 0 };
    std::atomic<RE::FormID>    _pendingPanNoteTarget{ 0 };
    RE::NiPoint3               _pendingPanQuestPos{};        // guarded by _panelMutex
    RE::FormID                 _pendingPanQuestID = 0;       // guarded by _panelMutex, diagnostics only
    std::atomic<bool>          _pendingPanQuestPosValid{ false };
    // Destination row pan. A flag, not an id: the position is already published
    // in _destRowPos, and a vanilla door destination has no id to resolve.
    std::atomic<bool>          _pendingPanDestPos{ false };

    // Highlighted marker: drawn as a fading pulse ring after a list-panel row
    // click. Cleared when the fade finishes. id+startTime are set together on
    // the render thread, so no atomicity beyond the id store is needed.
    std::uint32_t _pulseMarkerId = 0;
    RE::FormID    _pulseFavId = 0;
    RE::FormID    _pulseNoteId = 0;   // vanilla-note row pan; projected like favorites
    RE::NiPoint3  _pulseQuestPos{};   // render-thread state; exact world position of the panned quest
    RE::FormID    _pulseQuestId = 0;  // diagnostics and active-pulse gate (> 0 means active)
    std::chrono::steady_clock::time_point _pulseStart{};

    // Map Filters window: edge-detect left-button release to auto-persist its
    // position to INI (it's a real ImGui window, so ImGui owns the drag).
    bool _wasDraggingFilter = false;
    // Legend collapse animation, 1 = expanded, 0 = icon strip. Must live here,
    // not as a local in DrawFilterPanel: that function re-runs every present,
    // so a local would reset to its initializer and never animate. Seeded from
    // the persisted setting on first draw (see _filterAnimInit).
    float _filterAnim = 1.0f;
    bool  _filterAnimInit = false;

    // List-panel drag. The panel is hand-drawn on the background draw list, so
    // there's no ImGui window to grab. We track the drag ourselves against the
    // engine MenuCursor: press inside the header strip starts a drag, motion
    // moves listPanelPosX/Y, release writes them to INI. Grab offset keeps the
    // cursor at the same spot on the header through the drag.
    bool  _draggingPanel = false;
    bool  _panelPrevLeftDown = false;
    float _panelGrabDX = 0, _panelGrabDY = 0;

    void DrawListPanel();
    // One row of the "Notes" section: vanilla note formID + resolved live
    // marker name/position, so the section can pan/delete without caching
    // stale state across cell transitions.
    struct NoteRow { RE::FormID formID; std::string displayName; RE::NiPoint3 position; };
    void DrawFilterPanel();
    void DrawNoteDialog();

    // Empty-terrain right-click context menu. Set on the game thread by
    // OpenTerrainMenu(), consumed on the render thread by DrawTerrainMenu()
    // (OpenPopup must run inside the ImGui frame). For now only the menu shell:
    // "Add Marker Here" is not wired yet, "Set Destination Here" is greyed.
    std::atomic<bool> _openTerrainMenu{ false };
    // Set when "Add Marker Here" is clicked; consumed same frame to call the
    // vanilla MapMenu::PlaceMarker at the cursor. See DrawTerrainMenu.
    std::atomic<bool> _pendingTerrainPlace{ false };
    // Screen pos where the terrain menu was opened (engine cursor at the
    // right-click). PlaceMarker reads the *live* cursor, which by click-time
    // has moved onto the menu item, so we may need to restore the cursor to
    // this point before calling. Captured in OpenTerrainMenu.
    float _terrainMenuCursorX = -1.0f;
    float _terrainMenuCursorY = -1.0f;
    void DrawTerrainMenu(bool isLocalMap);
    void ApplyPendingPan(RE::MapMenu* a_menu);

    // Custom-pin right-click context menu ("CustomContextMenu" popup): Set/
    // Clear Destination, Rename Marker, Remove Marker. Same three actions the
    // old hover-panel buttons drove, now behind a right-click menu (mirrors
    // HoverImageRenderer's vanilla-marker "VanillaContextMenu"). Shared by
    // world and local map pins — LocalMapRenderer calls OpenCustomContextMenu
    // too, since MarkerManager/BeginEditMarker are owned here either way.
    std::atomic<bool>         _openCustomContextMenu{ false };
    std::uint32_t             _customContextMarkerId = 0; // id at menu-open time
    bool                      _customMenuVisible = false; // popup drawn last frame
    void DrawCustomContextMenu();

    // Note dialog state. Written on the game thread (OpenNoteDialog),
    // read and drawn on the render thread (DrawNoteDialog).
    bool         _noteDialogOpen     = false;
    bool         _noteDialogInterior = false;  // combo shows interior category list
    bool         _noteFocusRequested = false;
    // Frames of ImGuiInputTextFlags_CallbackAlways to run when the note dialog
    // opens, so an EXISTING note gets soft-wrapped before the first keystroke.
    // CallbackEdit alone fires only on a change, and the box width the wrap
    // needs is not known until the widget is laid out. Counts down to 0, after
    // which the callback goes back to edit-only.
    int          _noteWrapPrimeFrames = 0;
    // One-shot: clear ImGui's MouseDown[0] on the first frame the dialog draws.
    // OpenNoteDialog is triggered by a left-click that is still physically held
    // when the dialog first appears, so ImGui inherits a stale "down" state and
    // the first click on the Combo/Checkbox doesn't register. We flip this flag
    // to force-release once, then resume normal polling.
    bool         _resetImGuiMouseOnNextDraw = false;
    // Fallback click tracking for OK/Cancel. ImGui's Button() can miss mouse
    // activation on the world map because cursor position and button state are
    // injected from different paths; these fields implement normal press-inside,
    // release-inside behavior against Skyrim's MenuCursor.
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
    // Edit mode: id of the marker being edited, or 0 for a fresh placement.
    // Set by BeginEditMarker; on dialog confirm, non-zero routes to UpdateMarker
    // instead of AddMarker. Cleared whenever the dialog opens or closes.
    std::uint32_t _editingMarkerId = 0;
    // Vanilla-note mode: FormID of the vanilla marker being annotated, or 0.
    // Set by BeginEditVanillaNote. When non-zero the dialog hides the category
    // combo + destination checkbox and confirm routes to SetVanillaNote.
    // Mutually exclusive with _editingMarkerId. Cleared on open/close.
    RE::FormID    _editingVanillaFormID = 0;
    std::string   _vanillaNoteName;          // header label (the location name)
    // Screen pos (px) where placement happened, captured in OpenNoteDialog from
    // the engine cursor. DrawNoteDialog opens the dialog here instead of screen
    // center so it appears next to the clicked spot. -1 = fall back to center.
    float        _noteDialogX = -1.0f;
    float        _noteDialogY = -1.0f;

    // Hover fade for custom pin tint: 0..1 eased toward hovered/unhovered per
    // marker id, so the yellow highlight glides instead of snapping. Rising
    // faster (0.15s) than falling (0.25s) reads as a responsive-but-soft glow.
    std::unordered_map<std::uint32_t, float> _hoverAnim;

    // Badge scale-in: birth timestamp per FormID, first frame it's seen while
    // the map is open. Age drives a 0.2s cubic ease-out scale so a NEW
    // favorite/note badge pops in instead of appearing instantly. Seeded to a
    // far-past time on map (re)open so already-favorited/noted markers never
    // animate; only additions made while the map stays open animate.
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> _favBadgeBorn;
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> _noteBadgeBorn;
    // Cleared-dungeon badge birth times. Enumerated fresh from the vanilla
    // marker array each frame (no stored list, unlike fav/note), keyed by
    // the ref's FormID. Same seed-on-open gate as _favBadgeBorn/_noteBadgeBorn.
    std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> _clearedBadgeBorn;
    bool _badgesSeeded = false; // false right after map-closed reset; forces a seed pass
};
