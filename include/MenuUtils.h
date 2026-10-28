#pragma once
// Helper utilities for querying active Skyrim UI menus and cursor state.

#include <RE/Skyrim.h>
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace MenuUtils {

    namespace detail {

        // Scan [base, base+size) for pattern. '\xFF' in mask = compare, '\x00' = wildcard.
        inline std::uint8_t* PatternScan(std::uint8_t* base, std::size_t size,
                                         const std::uint8_t* pat, const char* mask,
                                         std::size_t patLen, std::size_t startAt = 0) {
            if (size < patLen) return nullptr;
            for (std::size_t i = startAt; i <= size - patLen; ++i) {
                bool hit = true;
                for (std::size_t j = 0; j < patLen; ++j) {
                    if (mask[j] == 'x' && base[i + j] != pat[j]) { hit = false; break; }
                }
                if (hit) return base + i;
            }
            return nullptr;
        }

        // Module .text span from the PE headers.
        inline bool GetModuleSpan(HMODULE mod, std::uint8_t*& outBase, std::size_t& outSize) {
            if (!mod) return false;
            auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
            auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
                reinterpret_cast<std::uint8_t*>(mod) + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
            outBase = reinterpret_cast<std::uint8_t*>(mod);
            outSize = nt->OptionalHeader.SizeOfImage;
            return true;
        }
    }  // namespace detail

    inline bool IsModexOpen() {
        using Fn = bool (*)();
        static bool initialized = false;
        static Fn thunk = nullptr;

        if (!initialized) {
            initialized = true;

            HMODULE mod = GetModuleHandleA("Modex");
            std::uint8_t* base = nullptr;
            std::size_t size = 0;
            if (detail::GetModuleSpan(mod, base, size)) {
                // 1) Unique IsMenuOpen body: CMP [RCX+0xA0],0 ; SETNZ AL ; RET
                const std::uint8_t imoPat[] = {
                    0x48, 0x83, 0xB9, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x95, 0xC0, 0xC3
                };
                std::uint8_t* imo = detail::PatternScan(
                    base, size, imoPat, "xxxxxxxxxxxx", sizeof(imoPat));

                if (imo) {
                    const std::uint8_t thkPat[] = {
                        0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00, 0x00, 0x00, 0x00,
                        0x48, 0x8B, 0xC8, 0x48, 0x83, 0xC4, 0x28, 0xE9
                    };
                    const char thkMask[] = "xxxxx????xxxxxxxx";
                    std::size_t off = 0;
                    while (off <= size - sizeof(thkPat)) {
                        std::uint8_t* cand = detail::PatternScan(
                            base, size, thkPat, thkMask, sizeof(thkPat), off);
                        if (!cand) break;
                        // Decode the trailing E9 rel32 (last 5 bytes: E9 dd dd dd dd).
                        std::uint8_t* jmpOp = cand + sizeof(thkPat) - 1;  // points at 0xE9
                        std::int32_t rel = 0;
                        std::memcpy(&rel, jmpOp + 1, sizeof(rel));
                        std::uint8_t* target = jmpOp + 5 + rel;
                        if (target == imo) {
                            thunk = reinterpret_cast<Fn>(cand);
                            break;
                        }
                        off = static_cast<std::size_t>(cand - base) + 1;
                    }
                }
            }

            if (thunk) {
                logger::info("[Modex] detection wired: thunk @ {}",
                    reinterpret_cast<void*>(thunk));
            } else if (mod) {
                logger::warn("[Modex] loaded but IsMenuOpen thunk not found "
                             "(layout changed?); overlay won't hide for Modex");
            }
        }

        return thunk ? thunk() : false;
    }

    inline bool IsDialogueHistoryOpen() {
        static bool initialized = false;
        static const volatile std::uint8_t* openFlag = nullptr;
        if (!initialized) {
            initialized = true;
            HMODULE mod = GetModuleHandleA("po3_DialogueHistory.dll");
            std::uint8_t* base = nullptr;
            std::size_t size = 0;
            if (detail::GetModuleSpan(mod, base, size)) {
                auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
                auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                auto sections = IMAGE_FIRST_SECTION(nt);
                for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
                    const auto& section = sections[i];
                    if (std::memcmp(section.Name, ".text", 5) != 0) continue;
                    const std::size_t textOffset = section.VirtualAddress;
                    const std::size_t textSize = section.Misc.VirtualSize;
                    if (textOffset > size || textSize > size - textOffset) break;
                    auto textBase = base + textOffset;
                    // CMP byte [RIP+disp32],0; JE; empty queue; choose original queue.
                    const std::uint8_t pattern[] = {
                        0x80, 0x3D, 0, 0, 0, 0, 0x00, 0x74, 0x10,
                        0x48, 0xC7, 0x44, 0x24, 0x20, 0, 0, 0, 0,
                        0x48, 0x8D, 0x54, 0x24, 0x20, 0xEB, 0x03, 0x48, 0x8B, 0xD3
                    };
                    const char mask[] = "xx????xxxxxxxxxxxxxxxxxxxxxx";
                    static_assert(sizeof(mask) - 1 == sizeof(pattern));
                    auto match = detail::PatternScan(textBase, textSize, pattern, mask, sizeof(pattern));
                    // A changed DLL must fail open instead of selecting an ambiguous byte.
                    if (!match || detail::PatternScan(textBase, textSize, pattern, mask,
                            sizeof(pattern), static_cast<std::size_t>(match - textBase) + 1)) break;
                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, match + 2, sizeof(displacement));
                    const auto target = reinterpret_cast<std::uintptr_t>(match + 7) + displacement;
                    const auto moduleBase = reinterpret_cast<std::uintptr_t>(base);
                    if (target < moduleBase || target - moduleBase >= size) break;
                    // The decoded byte must belong to writable module data.
                    for (unsigned j = 0; j < nt->FileHeader.NumberOfSections; ++j) {
                        const auto& data = sections[j];
                        if (!(data.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
                        const std::size_t offset = data.VirtualAddress;
                        const std::size_t dataSize = data.Misc.VirtualSize;
                        if (offset > size || dataSize > size - offset) continue;
                        if (target - moduleBase >= offset && target - moduleBase - offset < dataSize) {
                            openFlag = reinterpret_cast<const volatile std::uint8_t*>(target);
                            break;
                        }
                    }
                    break;
                }
            }
            if (openFlag) {
                logger::info("[DialogueHistory] detection wired: open flag @ {}",
                    reinterpret_cast<std::uintptr_t>(openFlag));
            } else if (mod) {
                logger::warn("[DialogueHistory] open-state pattern missing or ambiguous; detection disabled");
            }
        }

        const bool open = openFlag && *openFlag == 1;
        static bool logged = false;
        static bool lastOpen = false;
        if (openFlag && (!logged || open != lastOpen)) {
            logged = true;
            lastOpen = open;
            logger::debug("[DialogueHistory] open={}", open);
        }
        return open;
    }

    // ============ SKSE Menu Framework 3 Check ============
    // Dynamically queries the exported API; safe if the DLL is absent
    inline bool IsSKSEMenuFrameworkOpen() {
        static bool initialized = false;
        static bool (*IsMenuFrameworkOpenFunc)() = nullptr;
        if (!initialized) {
            if (auto handle = GetModuleHandleA("SKSEMenuFramework")) {
                IsMenuFrameworkOpenFunc =
                    reinterpret_cast<bool(*)()>(GetProcAddress(handle, "IsAnyBlockingWindowOpened"));
            }
            initialized = true;
        }
        return IsMenuFrameworkOpenFunc ? IsMenuFrameworkOpenFunc() : false;
    }

    inline bool ShouldBlockMapOverlay() {
        return IsDialogueHistoryOpen() || IsSKSEMenuFrameworkOpen();
    }

    inline bool IsMapModalOpen() {
        auto* ui = RE::UI::GetSingleton();
        return ui && (ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME) ||
                      ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                      ui->IsMenuOpen(RE::JournalMenu::MENU_NAME) ||
                      ui->IsMenuOpen("MapExtension"));
    }

    // ============ Core Blocking Check ============
    // All blocking menus - used by FollowerHUD, InspectCard
    inline bool IsBlockingMenuOpen() {
        auto ui = RE::UI::GetSingleton();
        if (!ui) return false;

        return IsDialogueHistoryOpen() ||
               ui->IsMenuOpen("Console") ||
               ui->IsMenuOpen("MapMenu") ||
               ui->IsMenuOpen("ContainerMenu") ||
               ui->IsMenuOpen("BarterMenu") ||
               ui->IsMenuOpen("Journal Menu") ||
               ui->IsMenuOpen("Main Menu") ||
               ui->IsMenuOpen("Loading Menu") ||
               ui->IsMenuOpen("Fader Menu") ||
               ui->IsMenuOpen("Lockpicking Menu") ||
               ui->IsMenuOpen("Sleep/Wait Menu") ||
               ui->IsMenuOpen("Crafting Menu") ||
               ui->IsMenuOpen("Book Menu") ||
               ui->IsMenuOpen("TweenMenu") ||  // Restored for FollowerHUD/InspectCard
               ui->IsMenuOpen("GiftMenu") ||
               ui->IsMenuOpen("FavoritesMenu") ||
               ui->IsMenuOpen("StatsMenu") ||
               ui->IsMenuOpen("MessageBoxMenu") ||
               ui->IsMenuOpen("Dialogue Menu") ||         // Auto-hide in dialogue
               // RaceMenu - both vanilla and mod
               ui->IsMenuOpen("RaceSex Menu") ||
               ui->IsMenuOpen("RaceMenu") ||
               // Mod menus
               ui->IsMenuOpen("BestiaryMenu") ||      // Dragonborn Bestiary
               ui->IsMenuOpen("CustomMenu") ||        // Generic custom menus
               ui->IsMenuOpen("LevelUp Menu") ||      // Vanilla Level Up
               ui->IsMenuOpen("ShowStats") ||          // Skyrim Character Sheet mod
               ui->IsMenuOpen("QuestMenu") ||          // Quest journal overhaul custom menu
               ui->IsMenuOpen("CharacterSheet") ||      // CharacterSheet SE by Jpsteel
               ui->IsMenuOpen("AchievementMenu") ||     // Achievement Injector - full list overlay
               // Non-native ImGui frameworks (no IMenu registration)
               IsSKSEMenuFrameworkOpen() ||
               IsModexOpen();                           // Modex (DXGI Present hook, no IMenu)
    }

    inline bool IsBlockingMenuOpenExceptTween() {
        auto ui = RE::UI::GetSingleton();
        if (!ui) return false;

        return IsDialogueHistoryOpen() ||
               ui->IsMenuOpen("Console") ||
               ui->IsMenuOpen("MapMenu") ||
               ui->IsMenuOpen("ContainerMenu") ||
               ui->IsMenuOpen("BarterMenu") ||
               ui->IsMenuOpen("Journal Menu") ||
               ui->IsMenuOpen("Main Menu") ||
               ui->IsMenuOpen("Loading Menu") ||
               ui->IsMenuOpen("Fader Menu") ||
               ui->IsMenuOpen("Lockpicking Menu") ||
               ui->IsMenuOpen("Sleep/Wait Menu") ||
               ui->IsMenuOpen("Crafting Menu") ||
               ui->IsMenuOpen("Book Menu") ||
               ui->IsMenuOpen("GiftMenu") ||
               ui->IsMenuOpen("FavoritesMenu") ||
               ui->IsMenuOpen("StatsMenu") ||
               ui->IsMenuOpen("MessageBoxMenu") ||
               ui->IsMenuOpen("Dialogue Menu") ||
               ui->IsMenuOpen("RaceSex Menu") ||
               ui->IsMenuOpen("RaceMenu") ||
               ui->IsMenuOpen("BestiaryMenu") ||
               ui->IsMenuOpen("CustomMenu") ||
               ui->IsMenuOpen("LevelUp Menu") ||
               ui->IsMenuOpen("ShowStats") ||
               ui->IsMenuOpen("QuestMenu") ||          // Quest journal overhaul custom menu
               ui->IsMenuOpen("CharacterSheet") ||      // CharacterSheet SE by Jpsteel
               ui->IsMenuOpen("AchievementMenu") ||     // Achievement Injector - full list overlay
               // Non-native ImGui frameworks (no IMenu registration)
               IsSKSEMenuFrameworkOpen() ||
               IsModexOpen();                           // Modex (DXGI Present hook, no IMenu)
    }

    inline bool IsInPhotoMode() {
        auto global = RE::TESForm::LookupByEditorID<RE::TESGlobal>("PhotoMode_IsActive");
        if (global) {
            return global->value > 0.5f;
        }
        return false;  // Photo Mode mod not installed
    }

    inline bool AreFightingControlsDisabled() {
        auto* controls = RE::ControlMap::GetSingleton();
        if (!controls) return false;

        const bool fightingEnabled = controls->IsFightingControlsEnabled();

        // Transition-only diagnostics.
        static bool initialized = false;
        static bool lastFightingEnabled = true;
        if (!initialized || fightingEnabled != lastFightingEnabled) {
            initialized = true;
            lastFightingEnabled = fightingEnabled;
            auto* ui = RE::UI::GetSingleton();
            logger::debug(
                "[HUDGate] fighting={} movement={} looking={} menusShowing={}",
                fightingEnabled,
                controls->IsMovementControlsEnabled(),
                controls->IsLookingControlsEnabled(),
                ui && ui->IsShowingMenus());
        }

        return !fightingEnabled;
    }

    // ============ Component-Specific Checks ============

    // Blocks on all menus including TweenMenu + Inventory/Magic, plus photo mode and disabled combat controls
    inline bool ShouldBlockOverlay() {
        auto ui = RE::UI::GetSingleton();
        if (!ui) return false;

        // Block on Inventory/Magic (these components don't overlay game menus)
        if (ui->IsMenuOpen("InventoryMenu") || ui->IsMenuOpen("MagicMenu")) {
            return true;
        }

        return IsBlockingMenuOpen() || IsInPhotoMode() || AreFightingControlsDisabled();
    }

    inline bool ShouldBlockCharacterSheet() {
        return IsBlockingMenuOpenExceptTween() || IsInPhotoMode();
    }

}  // namespace MenuUtils
