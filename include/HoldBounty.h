#pragma once

// Resolves hold ownership and crime bounties for map locations.

#include <RE/Skyrim.h>
#include <array>
#include <cstdint>

namespace HoldBounty {
    struct HoldCrime { RE::FormID hold; RE::FormID faction; };
    inline constexpr std::array<HoldCrime, 9> kHoldCrimeFactions{ {
        { 0x00016770, 0x00029DB0 },
        { 0x0001676A, 0x000267E3 },
        { 0x00016772, 0x000267EA },
        { 0x0001676C, 0x0002816B },
        { 0x00016769, 0x0002816C },
        { 0x0001676E, 0x0002816D },
        { 0x0001676D, 0x0002816E },
        { 0x0001676B, 0x0002816F },
        { 0x0001676F, 0x00028170 },
    } };

    inline constexpr const char* kDragonbornPlugin      = "Dragonborn.esm";
    inline constexpr RE::FormID  kSolstheimHoldLocalID  = 0x016E2A;
    inline constexpr RE::FormID  kRavenRockFactLocalID  = 0x018279;

    inline RE::BGSLocation* SolstheimHold() {
        static RE::BGSLocation* cached =
            RE::TESDataHandler::GetSingleton()
                ? RE::TESDataHandler::GetSingleton()->LookupForm<RE::BGSLocation>(
                      kSolstheimHoldLocalID, kDragonbornPlugin)
                : nullptr;
        return cached;
    }
    inline RE::TESFaction* RavenRockCrimeFaction() {
        static RE::TESFaction* cached =
            RE::TESDataHandler::GetSingleton()
                ? RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESFaction>(
                      kRavenRockFactLocalID, kDragonbornPlugin)
                : nullptr;
        return cached;
    }

    inline std::uint32_t BountyForFaction(RE::TESFaction* a_faction) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!a_faction || !player) return 0;
        return player->GetViolentCrimeGoldValue(a_faction) +
               player->GetNonViolentCrimeGoldValue(a_faction);
    }

    inline std::uint32_t ResolveBounty(RE::BGSLocation* a_hold) {
        if (!a_hold) return 0;
        const RE::FormID holdID = a_hold->GetFormID();
        for (const auto& e : kHoldCrimeFactions) {
            if (e.hold == holdID) {
                return BountyForFaction(RE::TESForm::LookupByID<RE::TESFaction>(e.faction));
            }
        }
        if (auto* sh = SolstheimHold(); sh && sh->GetFormID() == holdID) {
            return BountyForFaction(RavenRockCrimeFaction());
        }
        return 0;
    }
}
