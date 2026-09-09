#pragma once

// Helper utilities for querying active Skyrim UI menus and cursor state.


#include <RE/Skyrim.h>
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace MenuUtils {
    namespace detail {
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
    }

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
                        std::uint8_t* jmpOp = cand + sizeof(thkPat) - 1;
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

    inline bool IsBlockingMenuOpen() {
        auto ui = RE::UI::GetSingleton();
        if (!ui) return false;
        return ui->IsMenuOpen("Console") ||
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
               ui->IsMenuOpen("TweenMenu") ||
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
               ui->IsMenuOpen("QuestMenu") ||
               ui->IsMenuOpen("CharacterSheet") ||
               ui->IsMenuOpen("AchievementMenu") ||
               IsSKSEMenuFrameworkOpen() ||
               IsModexOpen();
    }

    inline bool IsBlockingMenuOpenExceptTween() {
        auto ui = RE::UI::GetSingleton();
        if (!ui) return false;
        return ui->IsMenuOpen("Console") ||
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
               ui->IsMenuOpen("QuestMenu") ||
               ui->IsMenuOpen("CharacterSheet") ||
               ui->IsMenuOpen("AchievementMenu") ||
               IsSKSEMenuFrameworkOpen() ||
               IsModexOpen();
    }

    inline bool IsInPhotoMode() {
        auto global = RE::TESForm::LookupByEditorID<RE::TESGlobal>("PhotoMode_IsActive");
        if (global) {
            return global->value > 0.5f;
        }
        return false;
    }

    inline bool ShouldBlockOverlay() {
        auto ui = RE::UI::GetSingleton();
        if (!ui) return false;
        if (ui->IsMenuOpen("InventoryMenu") || ui->IsMenuOpen("MagicMenu")) {
            return true;
        }
        return IsBlockingMenuOpen() || IsInPhotoMode();
    }

    inline bool ShouldBlockCharacterSheet() {
        return IsBlockingMenuOpenExceptTween() || IsInPhotoMode();
    }
}
