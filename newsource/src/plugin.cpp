// ============================================================================
// plugin.cpp — SKSE entry point, input routing, menu cursor management
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working:
//     - SKSE plugin load, settings/localization init, serialization callbacks
//       (cosave 'CMMK'), all hook installs (PlaceMarkerHook, MarkerClickHook,
//       LocalMapClickHook, CompassHook), LocalMapRenderer + ImGuiRenderer init.
//       NG 7.x trampoline storage is reserved once here for the input and
//       Present call stubs; later installers only consume the shared pool.
//     - SKSE Menu Framework registration at kDataLoaded: registers Custom
//       Markers / HUD Settings if SKSEMenuFramework.dll is loaded.
//     - LootBeamRenderer's container-change sink is registered at kDataLoaded;
//       its session-only looted-holder state resets on load/new game/revert.
//     - Input: every hotkey is a key + its OWN modifier, stored as DIK in
//       Settings (no more hard-coded scancodes). Default C toggles the world-map
//       list panel while the map is open; default T toggles interior path
//       tracking only while MapMenu is open, so it does not collide with vanilla
//       Sleep Wait during gameplay. (The old Y "place at player" hotkey was
//       removed: it fired on every Y press, and right-click placement covers it.)
//     - ModifierMatches is STRICT: modifier 0 means no modifier may be held, so
//       X and Ctrl+X are distinct. The old IsModifierKeyPressed short-circuited
//       true on 0 without reading the keyboard, so a plain-X bind also fired on
//       Ctrl+X. Strictness is what lets Ctrl+N (Look & Pin) and plain N (Inspect)
//       share a base key. Non-static: InputHook calls it too, so the swallow and
//       the dispatch always agree.
//     - Keys unify keyboard/mouse/gamepad through KeycodeFromScancode (mouse
//       +256, gamepad via SKSE's mask table, NOT +266). Key < 1 = unbound and can
//       never match. Gamepad binds do not consume, so they double-fire vanilla.
//     - Press-to-capture globals (g_hkCaptureCb/g_hkCaptureArmedAt) live here but
//       fire from InputHook. The cancel sweep at the top of ProcessEvent drops a
//       stale capture three ways: settings closed, another menu opened, or 5s
//       with no bindable key. It sits BEFORE the kButton early-return so
//       mouse-move events keep the timeout alive.
//     - Settings hotkey routing now lives in InputHook (Hook_ProcessInputQueue)
//       to support both MapMenu and gameplay hosts, consume presses before the
//       engine dispatcher, and maintain a press-through-release latch.
//     - Menu cursor: vanilla Scaleform cursor is hidden while MapMenu is open
//       and restored on close. Dual-event handler (MapMenu + CursorMenu) is
//       intentional — CursorMenu isn't on the stack when MapMenu fires opening.
//       CursorMenu::AdvanceMovie also reasserts the hide after the movie advances
//       while our map cursor is eligible to draw. CursorVerboseLog records the
//       event hide, persistence corrections, and full map-session result.
//       InputVerboseLog snapshots the separate Windows pointer/focus/clip state
//       at MapMenu open/close; Scaleform visibility cannot diagnose cursor bleed
//       onto a second monitor.
//     - Party Sheet state API consumer (PartySheetState::*). Second messaging
//       listener with a null sender picks up Party Sheet's kMsg_Interface;
//       MinimapRenderer::Draw bails while its HUD is hidden or a fullscreen
//       panel is open. Soft dep: no Party Sheet = no-op.
//   Broken/limited: InspectCard does not hide the radar (Party Sheet doesn't
//     count it as fullscreen). Left as-is pending in-game feel.
//   Open work owned by this file: the AdvanceMovie persistence correction needs
//     local and affected-user in-game verification; see
//     findings/spec-findings-Double-Cursor.md.
// ============================================================================

#include <Shlobj.h>
#include <Windows.h>
#include <cstring>
#include <spdlog/sinks/basic_file_sink.h>

#include "LocalizationManager.h"
#include "MapMarkerRenderer.h"
#include "ImGui/Renderer.h"
#include "MarkerManager.h"
#include "PlaceMarkerHook.h"
#include "MarkerClickHook.h"
#include "LocalMapClickHook.h"
#include "MapHoverHook.h"
#include "MarkerHoverHook.h"
#include "MarkerFilterHook.h"
#include "MapLookRemapHook.h"
#include "HoverImageRenderer.h"
#include "CompassHook.h"
#include "HUDMarkerHook.h"
#include "LocalMapRenderer.h"
#include "LootBeamRenderer.h"
#include "InputHook.h"
// #include "LocalMapHudSpike.h"
#include "Settings.h"
#include "PathTracker.h"
#include "SettingsMenu.h"
#include "EntityMarkerSource.h"
#include "RenderTargetUpscaler.h"
#include "PartySheetState.h"
#include "PartySheetStateAPI.h"

// ---------------- MODIFIER KEY CHECK --------------------
// Strict match. 0 means NO modifier may be held, so X and Ctrl+X stay distinct.
// Replaces IsModifierKeyPressed, which short-circuited true on 0 without reading
// the keyboard at all: a plain-X bind fired on Ctrl+X too. Strictness is also
// what lets Ctrl+N (Look & Pin) and plain N (Inspect) share a base key, since
// exactly one case here can be true for any single press.
// Non-static: InputHook's Look & Pin swallow block calls this too (declared in
// InputHandler.h) so the swallow and the dispatch agree on what "Ctrl is held"
// means. If they disagree the key fires without being swallowed, or vice versa.
bool ModifierMatches(std::int32_t modifierType) {
  const bool alt   = (GetAsyncKeyState(VK_MENU)     & 0x8000) != 0;
  const bool ctrl  = (GetAsyncKeyState(VK_CONTROL)  & 0x8000) != 0;
  const bool shift = (GetAsyncKeyState(VK_SHIFT)    & 0x8000) != 0;
  const bool lalt  = (GetAsyncKeyState(VK_LMENU)    & 0x8000) != 0;
  const bool ralt  = (GetAsyncKeyState(VK_RMENU)    & 0x8000) != 0;
  const bool lctrl = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
  const bool rctrl = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
  const bool lshft = (GetAsyncKeyState(VK_LSHIFT)   & 0x8000) != 0;
  const bool rshft = (GetAsyncKeyState(VK_RSHIFT)   & 0x8000) != 0;

  switch (modifierType) {
  case 1:  return alt   && !ctrl && !shift;
  case 2:  return ctrl  && !alt  && !shift;
  case 3:  return shift && !alt  && !ctrl;
  // Side-specific. The opposite side of the SAME key must be up, or an
  // LCtrl-bound hotkey also fires while RCtrl is held (the generic VK_CONTROL
  // check cannot tell the sides apart, so it has to be spelled out per side).
  case 4:  return lalt  && !ralt  && !ctrl && !shift;
  case 5:  return ralt  && !lalt  && !ctrl && !shift;
  case 6:  return lctrl && !rctrl && !alt  && !shift;
  case 7:  return rctrl && !lctrl && !alt  && !shift;
  case 8:  return lshft && !rshft && !alt  && !ctrl;
  case 9:  return rshft && !lshft && !alt  && !ctrl;
  default: return !alt && !ctrl && !shift;  // 0 (or unresolved) = none held
  }
}

// Device scancode -> unified keycode. Keyboard 1-255, mouse +256, gamepad via
// SKSE's table. NOT scancode + 266 for gamepad: a pad ButtonEvent's IDCode is
// the XInput BITMASK (A = 0x1000), not an index, so arithmetic produces garbage
// no INI value can match. Returns kMaxMacros (282) on an unknown mask, which no
// bind matches, so a bad mask stays inert.
std::uint32_t KeycodeFromScancode(std::uint32_t scancode, RE::INPUT_DEVICE device) {
  switch (device) {
  case RE::INPUT_DEVICE::kMouse:   return scancode + 256;
  case RE::INPUT_DEVICE::kGamepad: return SKSE::InputMap::GamepadMaskToKeycode(scancode);
  default:                         return scancode;
  }
}

// Devices a hotkey may be bound to. Gamepad binds do NOT consume the event (this
// sink returns kContinue; only the ProcessInputQueue hook can stop vanilla), so
// a bound pad button fires BOTH our action and its vanilla one. LStick (272) /
// RStick (273) are the least-bad targets. Proper consumption needs chords in the
// input hook, which is a separate feature.
static bool IsHotkeyDevice(RE::INPUT_DEVICE device) {
  return device == RE::INPUT_DEVICE::kKeyboard ||
         device == RE::INPUT_DEVICE::kMouse ||
         device == RE::INPUT_DEVICE::kGamepad;
}

// True when a stored bind matches this press. Key < 1 is unbound, so it can
// never match: that is the whole unbind mechanism.
bool HotkeyMatches(std::uint32_t code, std::int32_t key, std::int32_t modifier) {
  return key >= 1 && code == static_cast<std::uint32_t>(key) && ModifierMatches(modifier);
}

// ---------------- PRESS-TO-CAPTURE REBINDING --------------------
// Armed by SettingsMenu's PanelHotkeys; InputHook's ProcessInputQueue detour
// fires it with the next non-modifier key and swallows that key. The callback
// MUST run there, not in this file's sink: the hook unlinks the consumed event
// from the queue, so by the time the sink runs the event is gone, and only the
// hook can stop vanilla acting on the key (M opens the map, F opens the finder).
std::function<void(std::uint32_t)> g_hkCaptureCb = nullptr;
// Arm time of the current capture. Only meaningful while g_hkCaptureCb is set;
// used below to drop a capture no key ever reaches.
std::chrono::steady_clock::time_point g_hkCaptureArmedAt{};

// Forward decl: defined alongside the menu open/close handler below.
extern std::atomic<bool> g_mapMenuOpen;

// ---------------- INPUT HANDLER -------------------
class InputHandler : public RE::BSTEventSink<RE::InputEvent *> {
public:
  RE::BSEventNotifyControl
  ProcessEvent(RE::InputEvent *const *ev,
               RE::BSTEventSource<RE::InputEvent *> *) override {
    if (!ev || !*ev) {
      return RE::BSEventNotifyControl::kContinue;
    }

    auto e = *ev;

    // PRESS-TO-CAPTURE CANCEL SWEEP. Runs on EVERY input event (any type, any
    // state), before the button-only early-outs below, so mouse-move events keep
    // the stream alive and the idle timeout actually fires. Behind the kButton
    // return it would only run while a key is already down, which is exactly
    // when it is not needed. Three ways a capture dies without the user pressing
    // a bindable key:
    if (g_hkCaptureCb) {
      // a) Settings closed mid-bind. The bind UI is gone, so a stale callback
      //    must never eat a later keypress.
      if (!SettingsMenu::IsVisible()) {
        logger::debug("Hotkey capture cancelled: settings closed mid-bind");
        g_hkCaptureCb = nullptr;
      } else if (auto *ui = RE::UI::GetSingleton();
                 ui && (ui->IsMenuOpen(RE::JournalMenu::MENU_NAME) ||
                        ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                        ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME))) {
        // b) Another menu opened on top. Its input path is independent of ours,
        //    so a stray key can fire its action while our prompt sits armed.
        logger::debug("Hotkey capture cancelled: another menu opened mid-bind");
        g_hkCaptureCb = nullptr;
      } else if (std::chrono::steady_clock::now() - g_hkCaptureArmedAt >
                 std::chrono::seconds(5)) {
        // c) Backstop: no bindable key reached us (another mod ate it entirely).
        logger::debug("Hotkey capture timed out after 5s");
        g_hkCaptureCb = nullptr;
      }
    }

    if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
      return RE::BSEventNotifyControl::kContinue;
    }

    auto *btn = e->AsButtonEvent();
    if (!btn || !btn->IsDown()) {
      return RE::BSEventNotifyControl::kContinue;
    }

    auto settings = Settings::GetSingleton();

    if (!IsHotkeyDevice(btn->GetDevice())) {
      return RE::BSEventNotifyControl::kContinue;
    }
    const std::uint32_t code = KeycodeFromScancode(btn->GetIDCode(), btn->GetDevice());

    // Marker list panel, world map only (default C; vanilla map has no C
    // binding: L, J, E, F, M1, wheel).
    if (HotkeyMatches(code, settings->listPanelKey, settings->listPanelModifier)) {
      if (g_mapMenuOpen) {
        logger::debug("List panel hotkey: toggling custom marker list panel");
        MapMarkerRenderer::GetSingleton()->ToggleListPanel();
      }
      return RE::BSEventNotifyControl::kContinue;
    }

    // Path tracking, MapMenu only (default T). In gameplay T is vanilla Sleep
    // Wait, so running this globally would clear the path whenever the player
    // opens Sleep Wait.
    if (g_mapMenuOpen.load() &&
        settings->enablePathTracking &&
        HotkeyMatches(code, settings->pathTrackKey, settings->pathTrackModifier)) {
      logger::debug("Path tracking hotkey: toggling path tracking");
      PathTracker::GetSingleton()->ToggleTracking();
      return RE::BSEventNotifyControl::kContinue;
    }

    return RE::BSEventNotifyControl::kContinue;
  }
};

static InputHandler g_inputHandler;

// ---------------- MENU OPEN/CLOSE HANDLER ----------------
// Hides the vanilla Scaleform cursor while the map is open so our ImGui
// cursor (drawn on top of all markers) is the only one visible.
//
// WHY DUAL-EVENT: The MenuOpenCloseEvent for MapMenu::opening fires *before*
// CursorMenu is pushed onto the UI stack, so GetMenu<CursorMenu>() returns
// null at that moment (confirmed in log: "CursorMenu/uiMovie unavailable").
// Solution: track g_mapMenuOpen, then hide CursorMenu the moment it opens
// (which happens right after MapMenu). On MapMenu close, restore visibility.
std::atomic<bool> g_mapMenuOpen{ false };

// True while the screen is mid-fade (black). The engine raises FaderMenu during
// both the map open and close transitions. Confirmed in-game: on close, the
// fader comes up ~0.8s BEFORE the camera enters its Exit state and stays up
// ~0.5s AFTER MapMenu closes, which is the whole black-screen window where our
// ImGui overlay used to paint on top of the black. Gating on the fader hides
// our layer for that entire window, while leaving it visible during normal
// interactive map use (fader down). The camera Exit-state signal
// (findings-MapCamera-Exit-State.md) only covers a tiny slice of this window,
// so it isn't enough on its own.
bool IsMapFading() {
    auto* ui = RE::UI::GetSingleton();
    return ui && ui->IsMenuOpen(RE::FaderMenu::MENU_NAME);
}

static void SetCursorVisible(bool visible) {
    auto cursorMenu = RE::UI::GetSingleton()->GetMenu<RE::CursorMenu>();
    if (cursorMenu && cursorMenu->uiMovie) {
        cursorMenu->uiMovie->SetVisible(visible);
        logger::debug("Vanilla cursor visibility set to: {}", visible);
        if (Settings::GetSingleton()->cursorVerboseLog) {
            logger::info(
                "[CursorVerbose] SetVisible requested={} actual={} cursorMenu={} uiMovie={}",
                visible, cursorMenu->uiMovie->GetVisible(),
                static_cast<const void*>(cursorMenu.get()),
                static_cast<const void*>(cursorMenu->uiMovie.get()));
        }
    } else {
        logger::debug("SetCursorVisible({}) — CursorMenu/uiMovie unavailable", visible);
        if (Settings::GetSingleton()->cursorVerboseLog) {
            logger::info(
                "[CursorVerbose] SetVisible requested={} actual=unavailable cursorMenu={} uiMovie=null",
                visible, static_cast<const void*>(cursorMenu.get()));
        }
    }
}

namespace {
    using CursorAdvanceMovie_t = void (*)(RE::IMenu*, float, std::uint32_t);
    CursorAdvanceMovie_t g_cursorAdvanceMovie = nullptr;
    std::atomic<std::uint64_t> g_cursorEligibleFrames{ 0 };
    std::atomic<std::uint64_t> g_cursorVisibleBeforeHide{ 0 };
    std::atomic<std::uint64_t> g_cursorVisibleAfterHide{ 0 };
    std::atomic<bool> g_cursorCorrectionLogged{ false };

    bool CustomMapCursorEligible() {
        if (!g_mapMenuOpen.load() || IsMapFading()) {
            return false;
        }

        // The world and local renderers share these guards before requesting
        // the custom cursor. Valid MenuCursor dimensions are also required to
        // turn its coordinates into an ImGui position.
        auto* cursor = RE::MenuCursor::GetSingleton();
        return cursor && cursor->screenWidthX > 0.0f && cursor->screenWidthY > 0.0f;
    }

    void ResetCursorPersistenceStats() {
        g_cursorEligibleFrames = 0;
        g_cursorVisibleBeforeHide = 0;
        g_cursorVisibleAfterHide = 0;
        g_cursorCorrectionLogged = false;
    }

    void LogCursorPersistenceStats() {
        if (!Settings::GetSingleton()->cursorVerboseLog) {
            return;
        }

        logger::info(
            "[CursorVerbose] persistence_session_end eligibleFrames={} visibleBeforeHide={} visibleAfterHide={}",
            g_cursorEligibleFrames.load(), g_cursorVisibleBeforeHide.load(),
            g_cursorVisibleAfterHide.load());
    }

    void CursorAdvanceMovieHook(RE::IMenu* a_menu, float a_interval,
                                std::uint32_t a_currentTime) {
        g_cursorAdvanceMovie(a_menu, a_interval, a_currentTime);

        if (!a_menu || !a_menu->uiMovie || !CustomMapCursorEligible()) {
            return;
        }

        ++g_cursorEligibleFrames;
        const bool visibleBefore = a_menu->uiMovie->GetVisible();
        if (visibleBefore) {
            ++g_cursorVisibleBeforeHide;
        }

        // Keep this after CursorMenu's own AdvanceMovie. An affected setup was
        // observed restoring the movie one frame after the open-event hide; the
        // movie must be hidden again before the Scaleform composite pass.
        a_menu->uiMovie->SetVisible(false);
        const bool visibleAfter = a_menu->uiMovie->GetVisible();
        if (visibleAfter) {
            ++g_cursorVisibleAfterHide;
        }

        if (Settings::GetSingleton()->cursorVerboseLog && visibleBefore &&
            !g_cursorCorrectionLogged.exchange(true)) {
            logger::info(
                "[CursorVerbose] persistence_correction before=true after={} cursorMenu={} uiMovie={}",
                visibleAfter, static_cast<const void*>(a_menu),
                static_cast<const void*>(a_menu->uiMovie.get()));
        }
    }

    void InstallCursorPersistenceHook() {
        if (g_cursorAdvanceMovie) {
            return;
        }

        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_CursorMenu[0] };
        g_cursorAdvanceMovie = reinterpret_cast<CursorAdvanceMovie_t>(
            vtbl.write_vfunc(0x5, &CursorAdvanceMovieHook));
        logger::info("CursorMenu::AdvanceMovie persistence hook installed");
    }
}

class MenuOpenCloseHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (!a_event)
            return RE::BSEventNotifyControl::kContinue;

        // Track MapMenu state
        if (a_event->menuName == RE::MapMenu::MENU_NAME) {
            if (a_event->opening) {
                g_mapMenuOpen = true;
                ResetCursorPersistenceStats();
                logger::debug("MapMenu opened — waiting for CursorMenu to hide it");
                ImGuiRenderer::LogInputEnvironment("map_open");
                // CursorMenu isn't on the stack yet; the CursorMenu open event
                // below will hide it once it arrives.
            } else {
                g_mapMenuOpen = false;
                LogCursorPersistenceStats();
                logger::debug("MapMenu closed — restoring vanilla cursor");
                ImGuiRenderer::LogInputEnvironment("map_close");
                SetCursorVisible(true);
                SettingsMenu::Hide();
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        // Hide CursorMenu the moment it (re-)opens while the map is up
        if (a_event->menuName == RE::CursorMenu::MENU_NAME && a_event->opening) {
            if (g_mapMenuOpen) {
                logger::debug("CursorMenu opened while MapMenu is active — hiding it");
                SetCursorVisible(false);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

        // Force-close our settings menu when the Journal opens so it doesn't 
        // reappear unexpectedly when tracking a quest on the map.
        if (a_event->menuName == RE::JournalMenu::MENU_NAME && a_event->opening) {
            SettingsMenu::Hide();
            return RE::BSEventNotifyControl::kContinue;
        }

        return RE::BSEventNotifyControl::kContinue;
    }
};

static MenuOpenCloseHandler g_menuHandler;

// ---------------- SKSE Message Listener ----------------
void SKSEMessageListener(SKSE::MessagingInterface::Message *a_msg) {
  switch (a_msg->type) {
  case SKSE::MessagingInterface::kInputLoaded:
    logger::info("kInputLoaded message received");

    if (auto input = RE::BSInputDeviceManager::GetSingleton()) {
      input->AddEventSink(&g_inputHandler);
      logger::info("InputHandler registered");
    } else {
      logger::error("Failed to get BSInputDeviceManager!");
    }
    break;

  case SKSE::MessagingInterface::kDataLoaded:
    logger::info("kDataLoaded message received");
    InstallCursorPersistenceHook();
    if (auto* ui = RE::UI::GetSingleton())
        ui->AddEventSink(&g_menuHandler);
    // TESDataHandler is populated by now, so LookupModByName works.
    HoverImageRenderer::Init();
    LootBeamRenderer::GetSingleton()->InstallEventSink();
    // Render targets exist by now (kPostLoad is too early per finding). Upscale
    // kLOCAL_MAP + kLOCAL_MAP_SWAP to 1920x1080 so the Inspect/Glance backdrop
    // is sharp. findings-LocalMap-RenderTarget-Resolution.md.
    RenderTargetUpscaler::Apply();
    SettingsMenu::RegisterWithMenuFramework();
    break;

  case SKSE::MessagingInterface::kPostLoadGame:
    logger::info("kPostLoadGame message received");

    if (auto input = RE::BSInputDeviceManager::GetSingleton()) {
      input->AddEventSink(&g_inputHandler);
      logger::info("InputHandler re-registered [kPostLoadGame]");
    }
    // Wipe entity proximity-discovery: prevents false knowledge carrying over
    // when loading an older save. Session-memory only; cosave persistence later.
    EntityMarkerSource::ClearDiscovered();
    LootBeamRenderer::GetSingleton()->ClearLootedHolders();
    break;

  case SKSE::MessagingInterface::kNewGame:
    logger::info("kNewGame message received");
    EntityMarkerSource::ClearDiscovered();
    LootBeamRenderer::GetSingleton()->ClearLootedHolders();
    break;

  default:
    break;
  }
}

// ---------------- PARTY SHEET STATE API ----------------
// Optional soft dependency. Party Sheet broadcasts its interface at
// kPostPostLoad under its own sender name, so this needs a second listener
// registered with a null sender - the "SKSE" listener above only sees SKSE's
// own messages. Stays null when Party Sheet isn't installed.
namespace {
    PartySheetAPI::IPartySheetState1* g_partySheet = nullptr;

    void ForeignPluginMessageListener(SKSE::MessagingInterface::Message* a_msg) {
        // SKSE delivers a foreign plugin's broadcast to only ONE null-sender
        // listener per receiving plugin handle — the first registered. A second
        // RegisterListener(nullptr, ...) still returns true but never fires.
        // Confirmed 2026-07-24: MarkerFilterHook's own listener logged zero
        // messages while this one saw every plugin's traffic, including the
        // 'MapMarkerFramework' type=0 dataLen=40 dispatch it was waiting for.
        // So EVERY foreign-plugin handshake has to be routed through here.
        if (!a_msg) {
            return;
        }

        // CoMAP (MapMarkerFramework): hands back per-marker custom icon NAMES,
        // which is what lets the map filter bucket modded markers by category
        // instead of dumping them all in kModded. Query first — it ignores
        // messages that aren't its own, so order between the two blocks is
        // irrelevant. No-op when CoMAP isn't installed.
        MarkerFilterHook::OnForeignMessage(a_msg);

        if (!a_msg->sender ||
            std::strcmp(a_msg->sender, PartySheetAPI::kSender) != 0) {
            return;
        }
        if (a_msg->type != PartySheetAPI::kMsg_Interface) {
            return;
        }
        auto* iface = static_cast<PartySheetAPI::IPartySheetState1*>(a_msg->data);
        // Version gate: a future layout change means the vtable below this point
        // is not what we compiled against. Ignore rather than call into it.
        if (iface && iface->GetVersion() == 1) {
            g_partySheet = iface;
            logger::info("Party Sheet state API acquired (version 1).");
        } else {
            logger::warn("Party Sheet state API rejected: unsupported version.");
        }
    }
}

void PartySheetState::RegisterListener() {
    // Null sender = receive messages from every plugin, not just SKSE.
    // This is the plugin's ONE foreign-message listener (see the dedupe note in
    // ForeignPluginMessageListener); adding a second one elsewhere silently
    // does nothing. New cross-plugin handshakes go in that function.
    if (!SKSE::GetMessagingInterface()->RegisterListener(
            nullptr, ForeignPluginMessageListener)) {
        logger::warn("Failed to register foreign-plugin message listener.");
    }
}

bool PartySheetState::ShouldHideOverlay() {
    if (!g_partySheet) {
        return false;  // Party Sheet not installed; nothing to yield to.
    }
    return g_partySheet->IsHUDHidden() || g_partySheet->IsFullscreenPanelOpen();
}

bool PartySheetState::IsFullscreenPanelOpen() {
    return g_partySheet && g_partySheet->IsFullscreenPanelOpen();
}

// ---------------- SERIALIZATION CALLBACKS ----------------
static void SaveCallback(SKSE::SerializationInterface *serde) {
  MarkerManager::GetSingleton()->OnGameSave(serde);
  PathTracker::GetSingleton()->OnGameSave(serde);
}

static void LoadCallback(SKSE::SerializationInterface *serde) {
  MarkerManager::GetSingleton()->OnGameLoad(serde);
}

static void RevertCallback(SKSE::SerializationInterface *serde) {
  MarkerManager::GetSingleton()->OnRevert(serde);
  PathTracker::GetSingleton()->OnRevert();
  LootBeamRenderer::GetSingleton()->ClearLootedHolders();
}

// ---------------- SKSE PLUGIN LOAD ----------------
extern "C" DLLEXPORT bool SKSEPlugin_Load(const SKSE::LoadInterface *skse) {
  // NG 7.x initializes the shared trampoline once. Reserve both 14-byte call
  // stubs up front so the input and Present hooks do not compete for a
  // one-hook pool. Logging stays false because this plugin installs its own
  // CustomMarkers.log sink below.
  SKSE::Init(skse, {
      .log = false,
      .trampoline = true,
      .trampolineSize = 14 * 2
  });

  // Setup logging
  wchar_t *documentsPath = nullptr;
  SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documentsPath);
  auto path = std::filesystem::path(documentsPath);
  CoTaskMemFree(documentsPath);

  path /= "My Games/Skyrim Special Edition/SKSE/CustomMarkers.log";

  std::filesystem::create_directories(path.parent_path());

  auto sink =
      std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
  auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));
  // Start at info so Settings::Load() output is visible. Promoted below if
  // the INI requests a finer threshold.
  log->set_level(spdlog::level::info);
  log->flush_on(spdlog::level::info);
  spdlog::set_default_logger(std::move(log));

  logger::info("CustomMarkers Plugin Loading...");

  // Load settings
  Settings::GetSingleton()->Load();

  // Apply the configured log threshold. Valid spdlog names:
  // trace, debug, info, warn, err, critical, off. Unknown name falls back
  // to info and logs a warning so the author notices the typo.
  {
    const auto& levelStr = Settings::GetSingleton()->logLevel;
    auto level = spdlog::level::from_str(levelStr);
    if (level == spdlog::level::off && levelStr != "off") {
      logger::warn("Unknown log level '{}', defaulting to 'info'", levelStr);
      level = spdlog::level::info;
    }
    spdlog::default_logger()->set_level(level);
    spdlog::default_logger()->flush_on(level);
    logger::info("Log level threshold set to '{}'",
                 spdlog::level::to_string_view(level));
  }
  // HoverImageRenderer::Init() runs on kDataLoaded; it needs TESDataHandler
  // to resolve "Plugin.esm|0xLOCALID" keys to live runtime FormIDs.

  // Load localization
  auto loc = LocalizationManager::GetSingleton();
  loc->LoadLanguage(Settings::GetSingleton()->language);

  // Register SKSE message listener
  if (!SKSE::GetMessagingInterface()->RegisterListener("SKSE",
                                                       SKSEMessageListener)) {
    logger::error("Failed to register SKSE message listener!");
    return false;
  }
  logger::info("SKSE message listener registered.");

  // The plugin's one foreign-plugin message listener. Soft dependencies (Party
  // Sheet, CoMAP) are all handled inside it; a second null-sender listener
  // would register successfully and never fire.
  PartySheetState::RegisterListener();

  // Re-enabled to read RUNTIME_DATA.mapMarker (the destination flag ref) after
  // vanilla PlaceMarker runs - the click coords might land on that ref.
  PlaceMarkerHook::Install();
  MarkerClickHook::Install();
  LocalMapClickHook::Install();
  MapHoverHook::Install();
  MarkerHoverHook::Install();
  MarkerFilterHook::Install();
  MapLookRemapHook::Install();
  CompassHook::Install();
  HUDMarkerHook::Install();
  InputHook::Install();
  LocalMapRenderer::GetSingleton()->Install();
  // LocalMapHudSpike::Install();

  // Install ImGui hook
  ImGuiRenderer::Install();

  // Register serialization
  auto *serde = SKSE::GetSerializationInterface();
  if (serde) {
    serde->SetUniqueID('CMMK');
    serde->SetSaveCallback(SaveCallback);
    serde->SetLoadCallback(LoadCallback);
    serde->SetRevertCallback(RevertCallback);
    logger::info("Serialization callbacks registered.");
  } else {
    logger::error("Failed to get Serialization Interface!");
  }

  logger::info("CustomMarkers Plugin Loaded");
  return true;
}
