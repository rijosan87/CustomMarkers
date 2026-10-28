/*
* For modders: Copy this file into your own project if you wish to use this API
*/
#pragma once
// PartySheetStateAPI.h — public state API for Skyrim Party Sheet.
// No linking against Party Sheet required. If Party Sheet is not installed you
// never receive kMsg_Interface, and every call site below should no-op.
#include <cstdint>
#include "SKSE/SKSE.h"   // SKSE::PluginHandle, SKSE::GetPluginHandle()

namespace PartySheetAPI
{
    inline constexpr const char* kSender             = "SkyrimPartySheet";
    inline constexpr const char* kPartySheetPluginName = "SkyrimPartySheet";

    // Returned by mutating calls so a consumer can tell "done" from "declined".
    enum class APIResult : std::uint8_t {
        OK = 0,                 // state changed as requested
        AlreadySet,             // already in the requested state; nothing to do
        Refused_MenuOpen,       // a blocking game menu is up; Party Sheet keeps control
        Refused_BadHandle,      // a_myPluginHandle was kInvalidPluginHandle
    };

    enum : std::uint32_t {
        kMsg_Interface = 0x50534831,  // 'PSH1' — data is IPartySheetState1*, sent at kPostPostLoad
        kMsg_State     = 0x50534832,  // 'PSH2' — data is const StateMsg*, sent on every change
    };

    enum class Panel : std::uint32_t {
        PartySheet     = 0,  // classic sheet (psUseFullscreenMode off)
        PartySheetV2   = 1,  // fullscreen sheet
        CharacterSheet = 2,
        InspectCard    = 3,  // NOT counted as fullscreen; it coexists with the HUD
        SettingsMenu   = 4,
        Count          = 5,
    };

    // Why hudHidden holds its current value. Treat any unrecognised value as
    // UserToggle: new reasons may be added in later versions.
    enum class Reason : std::uint32_t {
        UserToggle = 0,  // player pressed the HUD toggle hotkey
        MenuOpen   = 1,  // a blocking game menu forced the HUD down
    };

    inline constexpr std::uint32_t PanelBit(Panel p) {
        return 1u << static_cast<std::uint32_t>(p);
    }

    // Payload of kMsg_State. Read-only, valid only for the duration of the callback.
    struct StateMsg {
        std::uint32_t version;     // == 1 for this layout; ignore the message if it differs
        bool          hudHidden;   // intent: the user wants HUD overlays hidden
        Reason        reason;      // why hudHidden holds its current value
        std::uint32_t openPanels;  // fact: bitmask of PanelBit(Panel)
    };

    class IPartySheetState1 {
    public:
        virtual ~IPartySheetState1() = default;

        virtual std::uint32_t GetVersion() const = 0;   // == 1

        // --- Intent ---
        virtual bool IsHUDHidden() const = 0;
        // Ask Party Sheet to hide/show its HUD, e.g. from your own hotkey.
        // Advisory in both directions: Party Sheet may decline, see APIResult.
        // Pass SKSE::GetPluginHandle(). Your handle is resolved to a plugin name
        // and logged, so your mod is identifiable in a user's PartySheet.log when
        // something goes wrong. There is no separate registration call.
        virtual APIResult RequestHUDHidden(SKSE::PluginHandle a_myPluginHandle,
                                           bool a_hidden, Reason a_reason) = 0;

        // --- Facts ---
        virtual bool          IsPanelOpen(Panel a_panel) const = 0;
        virtual std::uint32_t GetOpenPanels() const = 0;
        // True when a panel is up that covers enough screen that an overlay should
        // step aside. Party Sheet owns this judgement so your list never goes stale.
        // Currently: PartySheet, PartySheetV2, CharacterSheet, SettingsMenu.
        virtual bool IsFullscreenPanelOpen() const = 0;
    };
}

// -----------------------------------------------------------------------------
// Consumer-side usage (see also findings/spec-findings-PublicStateAPI.md):
//
// static PartySheetAPI::IPartySheetState1* g_ps = nullptr;
//
// In your own SKSE messaging listener, registered at plugin load:
// if (msg->sender && std::strcmp(msg->sender, PartySheetAPI::kSender) == 0) {
//     if (msg->type == PartySheetAPI::kMsg_Interface) {
//         auto* iface = static_cast<PartySheetAPI::IPartySheetState1*>(msg->data);
//         if (iface && iface->GetVersion() == 1) g_ps = iface;
//     }
// }
//
// Then gate your overlay draw:
// if (g_ps && (g_ps->IsHUDHidden() || g_ps->IsFullscreenPanelOpen()))
//     return;  // Party Sheet says step aside
//
// Optional: drive Party Sheet's HUD toggle from your own hotkey.
// Pass your plugin handle so you are identifiable in the user's PartySheet.log.
// Do NOT call this from inside your kMsg_State handler: that is a feedback loop
// and Party Sheet will log a warning naming your mod.
// if (g_ps) {
//     auto r = g_ps->RequestHUDHidden(SKSE::GetPluginHandle(), true,
//                                     PartySheetAPI::Reason::UserToggle);
//     if (r == PartySheetAPI::APIResult::Refused_MenuOpen) { /* a menu is up; try later */ }
// }
// -----------------------------------------------------------------------------
