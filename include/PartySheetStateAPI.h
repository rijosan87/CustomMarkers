#pragma once

// Interface for querying Party Sheet visibility and layout.


#include <cstdint>
#include "SKSE/SKSE.h"

namespace PartySheetAPI
{
    inline constexpr const char* kSender             = "SkyrimPartySheet";
    inline constexpr const char* kPartySheetPluginName = "SkyrimPartySheet";

    enum class APIResult : std::uint8_t {
        OK = 0,
        AlreadySet,
        Refused_MenuOpen,
        Refused_BadHandle,
    };

    enum : std::uint32_t {
        kMsg_Interface = 0x50534831,
        kMsg_State     = 0x50534832,
    };

    enum class Panel : std::uint32_t {
        PartySheet     = 0,
        PartySheetV2   = 1,
        CharacterSheet = 2,
        InspectCard    = 3,
        SettingsMenu   = 4,
        Count          = 5,
    };

    enum class Reason : std::uint32_t {
        UserToggle = 0,
        MenuOpen   = 1,
    };

    inline constexpr std::uint32_t PanelBit(Panel p) {
        return 1u << static_cast<std::uint32_t>(p);
    }

    struct StateMsg {
        std::uint32_t version;
        bool          hudHidden;
        Reason        reason;
        std::uint32_t openPanels;
    };

    class IPartySheetState1 {
    public:
        virtual ~IPartySheetState1() = default;
        virtual std::uint32_t GetVersion() const = 0;
        virtual bool IsHUDHidden() const = 0;
        virtual APIResult RequestHUDHidden(SKSE::PluginHandle a_myPluginHandle,
                                           bool a_hidden, Reason a_reason) = 0;
        virtual bool          IsPanelOpen(Panel a_panel) const = 0;
        virtual std::uint32_t GetOpenPanels() const = 0;
        virtual bool IsFullscreenPanelOpen() const = 0;
    };
}
