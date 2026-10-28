#pragma once
// MenuUtils.h - Unified menu detection utilities
// Single source of truth for menu blocking across all UI components

#include <RE/Skyrim.h>
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace MenuUtils {

    // ============ Modex (ModExplorerMenu) Check ============
    // Modex is a CommonLibSSE ImGui plugin driven off a DXGI Present hook. It registers
    // no Scaleform IMenu, so ui->IsMenuOpen() can't see it. Modex.dll exports only the 3
    // standard SKSE symbols (no RequestPluginAPI), and calling its Papyrus IsMenuOpen()
    // from our Present path crashed the VM. So we detect via a render-safe AOB scan.
    //
    // Modex tracks open state in Modex::UIManager::m_gui (unique_ptr at +0xA0): non-null
    // exactly when the window shows. UIManager::IsMenuOpen() reads that, but it's
    // __thiscall. A parameterless thunk (Papyrus binding) does GetSingleton()+IsMenuOpen()
    // and returns bool with no args. We resolve that thunk and call it.
    //
    // We can't AOB the thunk's prologue directly (4 collisions) nor GetSingleton (every
    // CommonLib Meyers singleton is byte-identical). The unique, stable anchor is the
    // IsMenuOpen body. So: find IsMenuOpen, then find the thunk whose tail JMP targets it.
    // RE: F:\SOFTWARE\findings\findings-modex-menuopen.md (build 2026-06-01).
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
            initialized = true;  // never re-scan, even on failure

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
                    // 2) Thunk prologue: SUB RSP,0x28 ; CALL rel32 ; MOV RCX,RAX ;
                    //    ADD RSP,0x28 ; JMP rel32 . Several candidates; the right one's
                    //    final JMP targets the IsMenuOpen body we just found.
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

    // MapMenu is itself a blocker for gameplay overlays, so map renderers
    // cannot use ShouldBlockOverlay(). Keep this narrow: SMF owns the screen
    // and input while its blocking window is open, but the underlying MapMenu
    // stays alive and our map surfaces return unchanged when SMF closes.
    inline bool ShouldBlockMapOverlay() {
        return IsSKSEMenuFrameworkOpen();
    }

    // ============ Core Blocking Check ============
    // All blocking menus - used by FollowerHUD, InspectCard
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
    
    // ============ Sheets Blocking Check (Skyrim Souls RE compatible) ============
    // Same as above but excludes TweenMenu for bTweenMenu=true compatibility
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
               // TweenMenu excluded for Skyrim Souls RE (bTweenMenu=true)
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
    
    // ============ Photo Mode Check ============
    // Uses the PhotoMode_IsActive global from Photo Mode SSE mod
    // This is reliable unlike IsInFreeCameraMode() which can get stuck
    inline bool IsInPhotoMode() {
        auto global = RE::TESForm::LookupByEditorID<RE::TESGlobal>("PhotoMode_IsActive");
        if (global) {
            return global->value > 0.5f;
        }
        return false;  // Photo Mode mod not installed
    }
    
    // ============ Component-Specific Checks ============
    
    // FollowerHUD, Renderer - blocks on all menus including TweenMenu + Inventory/Magic
    inline bool ShouldBlockOverlay() {
        auto ui = RE::UI::GetSingleton();
        if (!ui) return false;
        
        // Block on Inventory/Magic (these components don't overlay game menus)
        if (ui->IsMenuOpen("InventoryMenu") || ui->IsMenuOpen("MagicMenu")) {
            return true;
        }
        
        return IsBlockingMenuOpen() || IsInPhotoMode();
    }
    
    // CharacterSheet/PartySheet - allows TweenMenu (Skyrim Souls RE) and Inventory/Magic
    inline bool ShouldBlockCharacterSheet() {
        return IsBlockingMenuOpenExceptTween() || IsInPhotoMode();
    }

}  // namespace MenuUtils
