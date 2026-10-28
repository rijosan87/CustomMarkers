// include/HoldBounty.h
#pragma once
#include <RE/Skyrim.h>
#include <array>
#include <cstdint>

// Hold LCTN -> crime FACT. The game data has NO edge between these two (a
// hold location carries no faction field; crime factions attach to NPCs via
// CRIF), so the mapping lives here. The 9 Skyrim holds are Skyrim.esm base
// forms, so the local IDs are load-order stable and LookupByID resolves them
// directly. Solstheim (Raven Rock) DOES have a bounty system (Redoran/Raven
// Rock guards); its forms live in Dragonborn.esm so the runtime FormID prefix
// is load-order dependent and must be resolved via the data handler, not
// hardcoded (see kDragonbornPlugin block below).
// Do NOT use BGSLocation::unreportedCrimeFaction: that is the faction that
// IGNORES crimes at a location, not the one holding your bounty.
// Promoted out of HoverImageRenderer.cpp (2026-07-15, spec-findings-Map-Stats)
// so MapStats can sum the total without a second copy of the table drifting
// from the per-marker one. Do NOT duplicate this table anywhere else.
namespace HoldBounty {
    struct HoldCrime { RE::FormID hold; RE::FormID faction; };
    inline constexpr std::array<HoldCrime, 9> kHoldCrimeFactions{ {
        { 0x00016770, 0x00029DB0 },  // Haafingar   (Solitude)
        { 0x0001676A, 0x000267E3 },  // Eastmarch   (Windhelm)
        { 0x00016772, 0x000267EA },  // Whiterun
        { 0x0001676C, 0x0002816B },  // The Rift    (Riften)
        { 0x00016769, 0x0002816C },  // The Reach   (Markarth)
        { 0x0001676E, 0x0002816D },  // Hjaalmarch  (Morthal)
        { 0x0001676D, 0x0002816E },  // The Pale    (Dawnstar)
        { 0x0001676B, 0x0002816F },  // Winterhold
        { 0x0001676F, 0x00028170 },  // Falkreath
    } };

    // Solstheim crime (Dragonborn.esm; DLC forms have a load-order-dependent
    // prefix so they can't live in the constexpr table). The HOLD LCTN that
    // carries LocTypeHold is DLC2SolstheimLocation, NOT Raven Rock: Raven Rock
    // is LocTypeCity and parents up to Solstheim, so ResolveHoldLocation's
    // parentLoc walk lands on Solstheim. Key by Solstheim so the hover card's
    // hold walk matches. Local IDs:
    //   DLC2SolstheimLocation  0x016E2A  (the hold LCTN, has LocTypeHold)
    //   DLC2RavenRockFaction   0x018279  (the crime FACT the player owes)
    // The crime faction is "Raven Rock" (0x018279), NOT "Raven Rock Guard
    // Faction" (0x0195AD): confirmed live via Captain Veleth's crimeFaction
    // field (2026-07-15). The guard faction is membership; the crime gold
    // registers to the plain Raven Rock faction. Wrong ID = bounty reads 0.
    // Resolved once through the data handler; both null if Dragonborn absent.
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

    // Outstanding bounty (violent + non-violent) the player owes the hold
    // that contains a_hold. Bounty is PLAYER state keyed by faction: the
    // player owns the "who I owe" table, the faction is only the key. 0 =
    // clean record, or the hold has no crime faction (modded worldspaces).
    inline std::uint32_t ResolveBounty(RE::BGSLocation* a_hold) {
        if (!a_hold) return 0;

        const RE::FormID holdID = a_hold->GetFormID();

        // Skyrim.esm holds: constexpr table.
        for (const auto& e : kHoldCrimeFactions) {
            if (e.hold == holdID) {
                return BountyForFaction(RE::TESForm::LookupByID<RE::TESFaction>(e.faction));
            }
        }

        // Solstheim (Dragonborn.esm), runtime-resolved. Hold walk lands here
        // for any Solstheim marker; the crime FACT is the Raven Rock guard.
        if (auto* sh = SolstheimHold(); sh && sh->GetFormID() == holdID) {
            return BountyForFaction(RavenRockCrimeFaction());
        }

        return 0;
    }
}
