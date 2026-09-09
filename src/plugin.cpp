// SKSE plugin entry point and messaging listener.

// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):

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
#include "InputHook.h"

#include "Settings.h"
#include "PathTracker.h"
#include "SettingsMenu.h"
#include "EntityMarkerSource.h"
#include "RenderTargetUpscaler.h"
#include "PartySheetState.h"
#include "PartySheetStateAPI.h"

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
  case 4:  return lalt  && !ralt  && !ctrl && !shift;
  case 5:  return ralt  && !lalt  && !ctrl && !shift;
  case 6:  return lctrl && !rctrl && !alt  && !shift;
  case 7:  return rctrl && !lctrl && !alt  && !shift;
  case 8:  return lshft && !rshft && !alt  && !ctrl;
  case 9:  return rshft && !lshft && !alt  && !ctrl;
  default: return !alt && !ctrl && !shift;
  }
}

std::uint32_t KeycodeFromScancode(std::uint32_t scancode, RE::INPUT_DEVICE device) {
  switch (device) {
  case RE::INPUT_DEVICE::kMouse:   return scancode + 256;
  case RE::INPUT_DEVICE::kGamepad: return SKSE::InputMap::GamepadMaskToKeycode(scancode);
  default:                         return scancode;
  }
}

static bool IsHotkeyDevice(RE::INPUT_DEVICE device) {
  return device == RE::INPUT_DEVICE::kKeyboard ||
         device == RE::INPUT_DEVICE::kMouse ||
         device == RE::INPUT_DEVICE::kGamepad;
}

bool HotkeyMatches(std::uint32_t code, std::int32_t key, std::int32_t modifier) {
  return key >= 1 && code == static_cast<std::uint32_t>(key) && ModifierMatches(modifier);
}

std::function<void(std::uint32_t)> g_hkCaptureCb = nullptr;

std::chrono::steady_clock::time_point g_hkCaptureArmedAt{};

extern std::atomic<bool> g_mapMenuOpen;

class InputHandler : public RE::BSTEventSink<RE::InputEvent *> {
public:
  RE::BSEventNotifyControl
  ProcessEvent(RE::InputEvent *const *ev,
               RE::BSTEventSource<RE::InputEvent *> *) override {
    if (!ev || !*ev) {
      return RE::BSEventNotifyControl::kContinue;
    }
    auto e = *ev;
    if (g_hkCaptureCb) {
      if (!SettingsMenu::IsVisible()) {
        logger::debug("Hotkey capture cancelled: settings closed mid-bind");
        g_hkCaptureCb = nullptr;
      } else if (auto *ui = RE::UI::GetSingleton();
                 ui && (ui->IsMenuOpen(RE::JournalMenu::MENU_NAME) ||
                        ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                        ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME))) {
        logger::debug("Hotkey capture cancelled: another menu opened mid-bind");
        g_hkCaptureCb = nullptr;
      } else if (std::chrono::steady_clock::now() - g_hkCaptureArmedAt >
                 std::chrono::seconds(5)) {
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
    if (HotkeyMatches(code, settings->listPanelKey, settings->listPanelModifier)) {
      if (g_mapMenuOpen) {
        logger::debug("List panel hotkey: toggling custom marker list panel");
        MapMarkerRenderer::GetSingleton()->ToggleListPanel();
      }
      return RE::BSEventNotifyControl::kContinue;
    }
    if (g_mapMenuOpen.load() &&
        HotkeyMatches(code, settings->pathTrackKey, settings->pathTrackModifier)) {
      logger::debug("Path tracking hotkey: toggling path tracking");
      PathTracker::GetSingleton()->ToggleTracking();
      return RE::BSEventNotifyControl::kContinue;
    }
    return RE::BSEventNotifyControl::kContinue;
  }
};

static InputHandler g_inputHandler;

std::atomic<bool> g_mapMenuOpen{ false };

bool IsMapFading() {
    auto* ui = RE::UI::GetSingleton();
    return ui && ui->IsMenuOpen(RE::FaderMenu::MENU_NAME);
}

static void SetCursorVisible(bool visible) {
    auto cursorMenu = RE::UI::GetSingleton()->GetMenu<RE::CursorMenu>();
    if (cursorMenu && cursorMenu->uiMovie) {
        cursorMenu->uiMovie->SetVisible(visible);
        logger::debug("Vanilla cursor visibility set to: {}", visible);
    } else {
        logger::debug("SetCursorVisible({}) - CursorMenu/uiMovie unavailable", visible);
    }
}

class MenuOpenCloseHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (!a_event)
            return RE::BSEventNotifyControl::kContinue;
        if (a_event->menuName == RE::MapMenu::MENU_NAME) {
            if (a_event->opening) {
                g_mapMenuOpen = true;
                logger::debug("MapMenu opened - waiting for CursorMenu to hide it");
            } else {
                g_mapMenuOpen = false;
                logger::debug("MapMenu closed - restoring vanilla cursor");
                SetCursorVisible(true);
                SettingsMenu::Hide();
            }
            return RE::BSEventNotifyControl::kContinue;
        }
        if (a_event->menuName == RE::CursorMenu::MENU_NAME && a_event->opening) {
            if (g_mapMenuOpen) {
                logger::debug("CursorMenu opened while MapMenu is active - hiding it");
                SetCursorVisible(false);
            }
            return RE::BSEventNotifyControl::kContinue;
        }
        if (a_event->menuName == RE::JournalMenu::MENU_NAME && a_event->opening) {
            SettingsMenu::Hide();
            return RE::BSEventNotifyControl::kContinue;
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

static MenuOpenCloseHandler g_menuHandler;

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
    if (auto* ui = RE::UI::GetSingleton())
        ui->AddEventSink(&g_menuHandler);
    HoverImageRenderer::Init();
    RenderTargetUpscaler::Apply();
    break;
  case SKSE::MessagingInterface::kPostLoadGame:
    logger::info("kPostLoadGame message received");
    if (auto input = RE::BSInputDeviceManager::GetSingleton()) {
      input->AddEventSink(&g_inputHandler);
      logger::info("InputHandler re-registered [kPostLoadGame]");
    }
    EntityMarkerSource::ClearDiscovered();
    break;
  case SKSE::MessagingInterface::kNewGame:
    logger::info("kNewGame message received");
    EntityMarkerSource::ClearDiscovered();
    break;
  default:
    break;
  }
}

namespace {
    PartySheetAPI::IPartySheetState1* g_partySheet = nullptr;

    void ForeignPluginMessageListener(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) {
            return;
        }
        MarkerFilterHook::OnForeignMessage(a_msg);
        if (!a_msg->sender ||
            std::strcmp(a_msg->sender, PartySheetAPI::kSender) != 0) {
            return;
        }
        if (a_msg->type != PartySheetAPI::kMsg_Interface) {
            return;
        }
        auto* iface = static_cast<PartySheetAPI::IPartySheetState1*>(a_msg->data);
        if (iface && iface->GetVersion() == 1) {
            g_partySheet = iface;
            logger::info("Party Sheet state API acquired (version 1).");
        } else {
            logger::warn("Party Sheet state API rejected: unsupported version.");
        }
    }
}

void PartySheetState::RegisterListener() {
    if (!SKSE::GetMessagingInterface()->RegisterListener(
            nullptr, ForeignPluginMessageListener)) {
        logger::warn("Failed to register foreign-plugin message listener.");
    }
}

bool PartySheetState::ShouldHideOverlay() {
    if (!g_partySheet) {
        return false;
    }
    return g_partySheet->IsHUDHidden() || g_partySheet->IsFullscreenPanelOpen();
}

bool PartySheetState::IsFullscreenPanelOpen() {
    return g_partySheet && g_partySheet->IsFullscreenPanelOpen();
}

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
}

extern "C" DLLEXPORT bool SKSEPlugin_Load(const SKSE::LoadInterface *skse) {
  SKSE::Init(skse);

  wchar_t *documentsPath = nullptr;
  SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documentsPath);
  auto path = std::filesystem::path(documentsPath);
  CoTaskMemFree(documentsPath);

  path /= "My Games/Skyrim Special Edition/SKSE/CustomMarkers.log";

  std::filesystem::create_directories(path.parent_path());

  auto sink =
      std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
  auto log = std::make_shared<spdlog::logger>("global log", std::move(sink));

  log->set_level(spdlog::level::info);
  log->flush_on(spdlog::level::info);
  spdlog::set_default_logger(std::move(log));

  logger::info("CustomMarkers Plugin Loading...");

  Settings::GetSingleton()->Load();

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

  auto loc = LocalizationManager::GetSingleton();
  loc->LoadLanguage(Settings::GetSingleton()->language);

  if (!SKSE::GetMessagingInterface()->RegisterListener("SKSE",
                                                       SKSEMessageListener)) {
    logger::error("Failed to register SKSE message listener!");
    return false;
  }
  logger::info("SKSE message listener registered.");

  PartySheetState::RegisterListener();

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

  ImGuiRenderer::Install();

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
