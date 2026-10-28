#include "MapHoverHook.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <Windows.h>
#include <memory>

// ============================================================================
// MapHoverHook — Ctrl-suppress vanilla local-map marker hover-zoom
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working: while Ctrl is held on the local (interior) map, the vanilla
//            hover-zoom on local-map markers (doors etc.) and its description
//            card are suppressed, so a custom pin can be dropped near a door
//            without the engine snapping focus to the door first.
//   Why this is separate from MarkerHoverHook: the world map drives hover via
//            FxDelegate::Invoke (the AS-event wrapper FUN_140fbe900), which
//            MarkerHoverHook catches. The local map does NOT use that wrapper.
//            Its per-frame hover poll FUN_14097d690 (RVA 0x97d690, ID 52980)
//            calls SetSelectedMarker through the low-level
//            RE::GFxValue::ObjectInterface::Invoke directly, and updates the
//            C++ description card through FUN_140982750 directly. So the
//            world-map hook never sees these. See
//            findings/findings-LocalMap-Hover-Suppress.md.
//   Two hooks run:
//     1. Entry detour on RE::GFxValue::ObjectInterface::Invoke (ID 82256,
//        RVA 0xFAC020). This is the engine-wide low-level Scaleform invoke, so
//        it is filtered by pointer-equality of a_name against the resolved
//        .rdata literal "SetSelectedMarker" (ID 216427, VA 0x141906F78), the
//        same exact-match technique MarkerHoverHook uses for the world map.
//        When the call is SetSelectedMarker and Ctrl is held, args[0] is
//        rewritten to -1.0, so AS plays the hide tween instead of the zoom-up.
//     2. Entry detour on FUN_140982750 (ID 53079, RVA 0x982750), the C++
//        description-card updater. When Ctrl is held, the index arg is forced
//        to -1 so the card is hidden rather than populated.
//   The hovered-marker index that LocalMapClickHook reads from
//   LocalMapMenu+0x303F8 is separately treated as -1 under Ctrl inside that
//   hook, so placement is allowed even if the engine remembered a hovered
//   marker this frame.
//
//   WORLD-MAP CARD SUPPRESSION (2026-07-14, confirmed in-game). Despite this
//   file's name, FUN_140982750 (SE 52187 / AE 53079) is the card populator for
//   BOTH maps, not just the local one. HookedUpdateDescriptionCard now also
//   skips it whenever the WORLD map is showing (IsWorldMapShowing(): the local
//   mode byte at MapMenu+0x60+0x303FC != 1), so vanilla never writes the hover
//   card's title. This is the first half of the stat-card takeover
//   (findings/spec-findings-Hover-Card-Takeover.md).
//   The gate is mandatory: ungated, this blanks the LOCAL map's door-name card
//   too, which we still want.
//   SKIPPING IS ONLY HALF THE JOB. With Title.text never written, the AS
//   TextField shows its DESIGN-TIME placeholder, the literal string
//   "Marker Name" baked into map.swf. HoverImageRenderer::SampleCardState
//   supplies the other half: it sets the holder clip's _visible=false every
//   frame. Remove either half and the placeholder card returns.
//   AE 1.6.1170 ONLY. IDs 82256 / 53079 / 216427. The "SetSelectedMarker"
//   literal VA 0x141906F78 is shared with the world-map hook.
// ============================================================================
namespace {
    // Dual-build (SE 1.5.97 / AE 1.6.1170). RelocationID is (seID, aeID).
    // SE IDs from findings/findings-SE-Hook-IDs.md.
    // kObjectInterfaceInvokeID: ObjectInterface::Invoke, the engine-wide
    // Scaleform AS2 invoke (distinct from FxDelegate::Invoke 82640/80520).
    // SE twin found by fingerprint match (4 args, [RAX+0x98], vtable CALL
    // [RAX+0xE0]) from the SE local-map hover poll's "SetSelectedMarker" path.
    constexpr REL::RelocationID kObjectInterfaceInvokeID{ 80233, 82256 };
    constexpr REL::RelocationID kUpdateDescriptionCardID{ 52187, 53079 };
    constexpr REL::RelocationID kSetSelectedMarkerStringID{ 270681, 216427 };

    std::uint64_t                              g_invokeTrampoline = 0;
    std::unique_ptr<PLH::x64Detour>            g_invokeDetour;

    std::uint64_t                              g_cardTrampoline = 0;
    std::unique_ptr<PLH::x64Detour>            g_cardDetour;

    // Resolved at install time. The Invoke detour pointer-compares the methodName
    // arg against this so it ignores every Scaleform invoke except SetSelectedMarker.
    const char* g_setSelectedMarkerLiteral = nullptr;

    using ObjectInterfaceInvokeFn =
        bool(*)(RE::GFxValue::ObjectInterface* a_this, void* a_data,
                RE::GFxValue* a_result, const char* a_name,
                RE::GFxValue* a_args, RE::UPInt a_numArgs, bool a_isDObj);

    using UpdateDescriptionCardFn =
        void(*)(void* a_obj, void* a_menu, std::int32_t a_index, void* a_unk);

    bool CtrlHeld() {
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    }

    // Local-map mode byte. MapMenu+0x60 = LocalMapMenu; +0x303FC == 1 means the
    // local map is the one on screen. Same predicate LocalMapRenderer::Draw and
    // GetActiveLocalMapBase already use (src/LocalMapRenderer.cpp:77,80,168), so
    // it is proven, not new. Anything other than 1 = world map. Same on SE/AE.
    constexpr std::uintptr_t kLocalMapBaseOffset = 0x60;
    constexpr std::uintptr_t kModeByteOffset     = 0x303FC;

    // True when the WORLD map is the visible one. The card populator
    // (SE 52187 / AE 53079) is shared world+local, so the suppression MUST be
    // gated by map or the local map's door-name card dies with it.
    // Mirrored in HoverImageRenderer.cpp, which hides the holder clip.
    bool IsWorldMapShowing() {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) return false;
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) return false;
        const auto lmBase =
            reinterpret_cast<std::uintptr_t>(mapMenu.get()) + kLocalMapBaseOffset;
        return *reinterpret_cast<const std::uint8_t*>(lmBase + kModeByteOffset) != 1;
    }

    bool HookedObjectInterfaceInvoke(RE::GFxValue::ObjectInterface* a_this,
                                     void* a_data, RE::GFxValue* a_result,
                                     const char* a_name, RE::GFxValue* a_args,
                                     RE::UPInt a_numArgs, bool a_isDObj) {
        if (a_name == g_setSelectedMarkerLiteral && a_args && a_numArgs > 0 &&
            CtrlHeld()) {
            a_args[0].SetNumber(-1.0);
        }
        return reinterpret_cast<ObjectInterfaceInvokeFn>(g_invokeTrampoline)(
            a_this, a_data, a_result, a_name, a_args, a_numArgs, a_isDObj);
    }

    void HookedUpdateDescriptionCard(void* a_obj, void* a_menu,
                                     std::int32_t a_index, void* a_unk) {
        // Do NOT rewrite a_index to -1 here. The caller (FUN_14097d690) only
        // reaches this call after its own `index >= 0` guard, and this function
        // dereferences the index without re-validating it (indexes a marker
        // slot). Passing -1 reads array[-1] and crashes ([r15+0x08] AV, seen
        // in crash-2026-06-03-14-48-01). To suppress the card, skip the
        // original call entirely when Ctrl is held: the card is simply never
        // populated this frame.
        if (CtrlHeld()) {
            return;
        }

        // Stat-card takeover (2026-07-14, confirmed in-game): on the WORLD map we
        // draw our own card, so vanilla's never gets populated. Skipping the call
        // stops Title.text from being written.
        //
        // NOT SUFFICIENT ON ITS OWN. With nothing written, the AS TextField falls
        // back to its DESIGN-TIME placeholder, the literal "Marker Name" authored
        // into map.swf. So HoverImageRenderer::SampleCardState also sets the
        // holder clip's _visible=false each frame. Both halves are required; drop
        // either and the placeholder card comes back.
        //
        // Gated to the world map because this populator is SHARED with the local
        // map, whose door-name card we still want. Ungated, it blanks that too.
        if (IsWorldMapShowing()) {
            static std::int32_t s_lastIdx = -2;
            if (a_index != s_lastIdx) {
                s_lastIdx = a_index;
                logger::debug("[CardSuppress] skipped populator, world map | idx={}",
                              a_index);
            }
            return;
        }

        reinterpret_cast<UpdateDescriptionCardFn>(g_cardTrampoline)(
            a_obj, a_menu, a_index, a_unk);
    }
}

void MapHoverHook::Install() {
    // Resolve the runtime address of the ".rdata" literal "SetSelectedMarker"
    // so the Invoke detour can pointer-compare against it (exact-match filter).
    REL::Relocation<const char*> stringLit{ kSetSelectedMarkerStringID };
    g_setSelectedMarkerLiteral = stringLit.get();

    REL::Relocation<std::uintptr_t> invokeTarget{ kObjectInterfaceInvokeID };
    const std::uint64_t invokeAddr = invokeTarget.address();
    const std::uint64_t invokeHook =
        reinterpret_cast<std::uint64_t>(&HookedObjectInterfaceInvoke);

    // Defensive: if the id ever fails to resolve (address 0), skip only this
    // detour rather than hooking address 0; the card detour below still installs.
    if (invokeAddr == 0) {
        logger::warn("MapHoverHook: ObjectInterface::Invoke target unresolved; skipping Invoke detour");
    } else {
        g_invokeDetour = std::make_unique<PLH::x64Detour>(
            invokeAddr, invokeHook, &g_invokeTrampoline);

        if (!g_invokeDetour->hook()) {
            logger::error("MapHoverHook: ObjectInterface::Invoke detour failed");
            g_invokeDetour.reset();
        } else {
            logger::info("MapHoverHook: Invoke hook installed | target={:X} | trampoline={:X} | literalVA={}",
                         invokeAddr, g_invokeTrampoline,
                         static_cast<const void*>(g_setSelectedMarkerLiteral));
        }
    }

    REL::Relocation<std::uintptr_t> cardTarget{ kUpdateDescriptionCardID };
    const std::uint64_t cardAddr = cardTarget.address();
    const std::uint64_t cardHook =
        reinterpret_cast<std::uint64_t>(&HookedUpdateDescriptionCard);

    g_cardDetour = std::make_unique<PLH::x64Detour>(
        cardAddr, cardHook, &g_cardTrampoline);

    if (!g_cardDetour->hook()) {
        logger::error("MapHoverHook: description-card detour failed");
        g_cardDetour.reset();
        return;
    }

    logger::info("MapHoverHook: card hook installed | target={:X} | trampoline={:X}",
                 cardAddr, g_cardTrampoline);
}
