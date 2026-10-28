#include "PCH.h"
#include "LootBeamRenderer.h"

#include "EntityMarkerSource.h"
#include "MenuUtils.h"
#include "BeastForm.h"
#include "PartySheetState.h"
#include "Settings.h"
#include "TextureManager.h"
#include "MapDrawHelpers.h"
#include "MapTheme.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ============================================================================
// LootBeamRenderer: in-world loot beams
// ----------------------------------------------------------------------------
// STATUS:
//   WORKING and settings-driven. Draws short beams for 11 configurable subtypes:
//   humanoid/creature corpses; chests, strongboxes, satchels; weapons, armor,
//   gold, potions, soul gems, and scrolls. Other clutter is rejected before the
//   256-entity shortlist and discovery work. Min Gold Value applies only to
//   loose loot. Range is a live XY gate; Reveal is session-persistent discovery;
//   Floor Band is the independent Z gate.
//   Toggle-only visibility hotkey (2026-09-15) stores session state in the
//   renderer and returns before collection, rays, or GPU work while hidden.
//   Confirmed in-game 2026-09-15: the configured key toggles beams off and on.
//
//   This file owns a 1s collection cadence because beams must work when Glance
//   and Inspect are hidden. It shares an eight-ray frame budget between queued
//   discovery LOS and round-robin live occlusion. Preserve the discovery FIFO,
//   failed-candidate rotation, and g_occlusion state across cache rebuilds.
//
//   Hide After Looting listens for a positive transfer from a holder to the
//   player and suppresses that chest/corpse after its first taken item. Opening
//   the loot menu alone does not count. The tracked FormIDs are session-only and
//   cleared on load/new game; no cosave record is written. Verified 2026-09-14:
//   the setting worked on chests and corpses, taking one item hid the beam while
//   opening/leaving all items did not. Corpse ref 000F14F3 logged one MARK
//   [LOOTED], immediate PRUNE [LOOTED], then REJECT [LOOTED].
//
//   Empty containers and corpses are rejected during collection and pruned once
//   per frame before occlusion. HasActualLoot excludes authored LVLI template
//   rows because CommonLib's GetInventoryCount() counts those after resolved loot
//   is removed. Verified 2026-09-06 on Wyrmstooth satchel 0x08004285: one
//   PRUNE [EMPTY/INVALID], then REJECT [EMPTY], with no later ray or draw entry.
//   Non-playable base forms are also excluded: checks virtual GetPlayable(),
//   formFlags kNonPlayable (armor/skins like Chaurus spit skin 000C5A9D), and
//   weaponData.flags kCantDrop / kNonPlayable (creature weapons like Frost
//   Giant Club 02012D14). Post-fix log shows IGNORE [NON_PLAYABLE] followed by
//   REJECT [EMPTY] when corpses hold only unplayable creature equipment.
//
//   Dwarven chests using DweContainter02.nif (small/jewelry) and DweContainer01.nif
//   (large) are classified as ContChest. Boss chests (DweChest01.nif) match the
//   generic "chest" substring scan. Verified in Mzinchaleft 2026-09-14: boss chest
//   ref 000172B4 already admitted/drew; adjacent small chest ref 000BE606 was absent
//   before the fix, then repeatedly logged ADMIT [Chest] + DRAW SUCCESS at 2.6-3.7m
//   after it. LootBeamPerfLog reports REJECT [UNCLASSIFIED_CONTAINER] for gaps.
//
//   Occlusion probes 40u above the reference, starts 40u forward of the camera,
//   and rejects near-camera self hits below 60u. A hit on the target itself is
//   clear LOS, not a wall; kDeadBip hits are ignored because corpses must not
//   occlude loot beams. These rules were confirmed on chests and corpse clusters.
//
//   Ownership uses IsCrimeToActivate(), not GetOwner(), so faction access,
//   purchased homes, and quest access follow engine rules. Corpses tested as
//   non-crime. Combat, beast form, vanilla menus, and Party Sheet panels hard-cut
//   the overlay before collection.
//
//   Entity positions already carry real world Z, so this renderer must not add a
//   WorldBeaconRenderer-style ground ray. Buried-corpse beams can be legitimate
//   load-order artifacts when an overhaul adds geometry over an existing save.
//   Beam mesh and diamond framing are shared with WorldBeaconRenderer through
//   MapDraw::DrawFeatheredBeam and MapDraw::DrawDiamondBadgeFrame.
//
//   PERF: permanent gated telemetry is [Logging] LootBeamPerfLog. At the tested
//   17-28 cached beams, 300-frame means were 74-391us, worst 4.16ms, and ray means
//   about 22us. The empty-satchel test at 1-2 beams measured 81-85us mean and
//   0.50-2.65ms worst; total rays/frame fell from 4 to 2. The 256-eligible case
//   remains untested. Do not turn that boundary into a confirmed defect.
//
//   Tuning remains subjective: 1s collection, beam length/brightness, range,
//   reveal, Floor Band, and subtype defaults. Five-tier rarity coloring is not
//   implemented because Skyrim has no rarity field.
// ============================================================================

namespace {
    std::mutex                     g_lootedHoldersMutex;
    std::unordered_set<RE::FormID> g_lootedHolders;

    bool IsHolderCategory(EntityMarkerSource::Category a_cat) {
        using Cat = EntityMarkerSource::Category;
        return a_cat == Cat::Container || a_cat == Cat::Corpse;
    }

    bool WasLootedThisSession(RE::FormID a_formID) {
        std::scoped_lock lock(g_lootedHoldersMutex);
        return g_lootedHolders.contains(a_formID);
    }

    bool ShouldHideAfterLooting(EntityMarkerSource::Category a_cat, RE::FormID a_formID) {
        auto* settings = Settings::GetSingleton();
        return settings && settings->lootBeamHideAfterLooting &&
               IsHolderCategory(a_cat) && WasLootedThisSession(a_formID);
    }

    const char* SubCategoryToString(EntityMarkerSource::SubCategory a_sub) {
        using Sub = EntityMarkerSource::SubCategory;
        switch (a_sub) {
            case Sub::CorpseHumanoid: return "CorpseHumanoid";
            case Sub::CorpseCreature: return "CorpseCreature";
            case Sub::ContChest:      return "Chest";
            case Sub::ContStrongbox:  return "Strongbox";
            case Sub::ContSatchel:    return "Satchel";
            case Sub::LootWeapon:     return "Weapon";
            case Sub::LootArmor:      return "Armor";
            case Sub::LootGold:       return "Gold";
            case Sub::LootSoulGem:    return "SoulGem";
            case Sub::LootScroll:     return "Scroll";
            case Sub::LootPotion:     return "Potion";
            default:                  return "Other";
        }
    }

    // True if a container or corpse contains at least one real item with net count > 0.
    // CommonLib's GetInventoryCount() uses an accept-all filter and retains authored
    // LeveledItem (LVLI) template entries with count=1 even after their resolved
    // contents have been completely taken. Creature skins, unplayable creature
    // weapons (e.g. Frost Giant Club), and hidden equipment can also remain as
    // concrete positive-count entries after the loot menu is empty, so non-playable
    // forms do not count as player-accessible loot.
    bool HasActualLoot(RE::TESObjectREFR& a_ref, bool a_logIgnored = false) {
        const RE::FormID holderFormID = a_ref.GetFormID();
        const auto counts = a_ref.GetInventoryCounts(
            [a_logIgnored, holderFormID](RE::TESBoundObject& a_item) {
                if (a_item.Is(RE::FormType::LeveledItem)) return false;

                bool isNonPlayable = !a_item.GetPlayable() ||
                                     ((a_item.GetFormFlags() & RE::TESForm::RecordFlags::kNonPlayable) != 0);
                if (!isNonPlayable) {
                    if (const auto* weap = a_item.As<RE::TESObjectWEAP>()) {
                        if (weap->weaponData.flags.any(RE::TESObjectWEAP::Data::Flag::kCantDrop,
                                                       RE::TESObjectWEAP::Data::Flag::kNonPlayable)) {
                            isNonPlayable = true;
                        }
                    }
                }

                if (isNonPlayable) {
                    if (a_logIgnored) {
                        SKSE::log::info("LootBeam IGNORE [NON_PLAYABLE]: holder={:08X} item={:08X} type={}",
                                        holderFormID, a_item.GetFormID(),
                                        static_cast<std::uint32_t>(a_item.GetFormType()));
                    }
                    return false;
                }
                return true;
            },
            false);

        for (const auto& [item, count] : counts) {
            if (item && count > 0) return true;
        }
        return false;
    }

    // Settings-driven loot beam filter predicate: checks the 11 subtype toggles,
    // the Min Gold Value threshold for loose items, and non-empty status for holders.
    bool IsBeamSubtypeEnabled(EntityMarkerSource::Category a_cat,
                              EntityMarkerSource::SubCategory a_sub,
                              RE::TESObjectREFR& a_ref) {
        auto* s = Settings::GetSingleton();
        if (!s) return false;
        using Sub = EntityMarkerSource::SubCategory;
        bool typeEnabled = false;
        switch (a_sub) {
            case Sub::CorpseHumanoid: typeEnabled = s->lootBeamShowCorpseHumanoid; break;
            case Sub::CorpseCreature: typeEnabled = s->lootBeamShowCorpseCreature; break;
            case Sub::ContChest:      typeEnabled = s->lootBeamShowContChest;      break;
            case Sub::ContStrongbox:  typeEnabled = s->lootBeamShowContStrongbox;  break;
            case Sub::ContSatchel:    typeEnabled = s->lootBeamShowContSatchel;    break;
            case Sub::LootWeapon:     typeEnabled = s->lootBeamShowLootWeapon;     break;
            case Sub::LootArmor:      typeEnabled = s->lootBeamShowLootArmor;      break;
            case Sub::LootGold:       typeEnabled = s->lootBeamShowLootGold;       break;
            case Sub::LootPotion:     typeEnabled = s->lootBeamShowLootPotion;     break;
            case Sub::LootSoulGem:    typeEnabled = s->lootBeamShowLootSoulGem;    break;
            case Sub::LootScroll:     typeEnabled = s->lootBeamShowLootScroll;     break;
            default:
                if (s->lootBeamPerfLog &&
                    a_cat == EntityMarkerSource::Category::Container &&
                    a_sub == Sub::None) {
                    auto* base = a_ref.GetBaseObject();
                    auto* model = base ? base->As<RE::TESModel>() : nullptr;
                    const char* path = model ? model->GetModel() : nullptr;
                    SKSE::log::info("LootBeam REJECT [UNCLASSIFIED_CONTAINER]: formID={:08X} model={}",
                                    a_ref.GetFormID(), path && *path ? path : "<none>");
                }
                return false;
        }
        if (!typeEnabled) return false;

        if (ShouldHideAfterLooting(a_cat, a_ref.GetFormID())) {
            if (s->lootBeamPerfLog) {
                SKSE::log::info("LootBeam REJECT [LOOTED]: formID={:08X} sub={}",
                                a_ref.GetFormID(), SubCategoryToString(a_sub));
            }
            return false;
        }

        // Min gold threshold (loose loot items only; corpses/containers are multi-item holders and exempt)
        if (s->lootBeamMinGoldValue > 0 && a_cat == EntityMarkerSource::Category::LooseLoot) {
            auto* base = a_ref.GetBaseObject();
            if (!base) return false;
            std::int32_t val = base->GetGoldValue();
            if (base->GetFormID() == 0x0000000F) {
                // Loose gold stack size
                val *= (std::max)(1, static_cast<int>(a_ref.extraList.GetCount()));
            }
            if (val < s->lootBeamMinGoldValue) return false;
        }

        using Cat = EntityMarkerSource::Category;
        if ((a_cat == Cat::Container || a_cat == Cat::Corpse) &&
            !HasActualLoot(a_ref, s->lootBeamPerfLog)) {
            if (s->lootBeamPerfLog) {
                SKSE::log::info("LootBeam REJECT [EMPTY]: formID={:08X} sub={}",
                                a_ref.GetFormID(), SubCategoryToString(a_sub));
            }
            return false;
        }

        return true;
    }

    // Owned-loot cut. IsCrimeToActivate() is the engine's own theft answer, so it
    // folds in faction membership (Thieves Guild fence chests), purchased player
    // housing, and quest-granted access; a raw GetOwner() check gets all three
    // wrong. IsOffLimits() is a pure alias for it, so call the primary name.
    //
    // Runs in the 1Hz collect block only.
    //
    // Also runs when lootBeamPerfLog is on and the setting is OFF, so the crime
    // result can be surveyed per subtype without hiding anything. That survey is
    // what cleared corpses for the ON-by-default switch (see the STATUS block);
    // keep the dual-mode behaviour, it is the only way to re-check the category
    // split without turning the feature off for the player.
    bool IsOwnedAndHidden(RE::FormID a_formID, EntityMarkerSource::SubCategory a_sub) {
        auto* settings = Settings::GetSingleton();
        if (!settings) return false;
        if (!settings->lootBeamHideOwned && !settings->lootBeamPerfLog) return false;

        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_formID);
        if (!ref) return false;  // gone this frame: let the existing existence test drop it

        const bool isCrime = ref->IsCrimeToActivate();
        if (settings->lootBeamPerfLog) {
            SKSE::log::info("LootBeam crime check: formID={:08X} sub={} isCrime={}",
                            a_formID, static_cast<int>(a_sub), isCrime ? "true" : "false");
        }
        return settings->lootBeamHideOwned && isCrime;
    }

    // Far fewer steps than the destination pillar's 24: that count exists to stop
    // a 2000-unit gradient banding, and a ~110-unit beam does not need it. Stays
    // a constant even though length is now a setting: the slider's ceiling (600)
    // is still far short of where banding shows.
    constexpr int   kBeamSteps       = 10;
    // Loot does not move, so cadence only buys reacting to the player entering a
    // new area and to items being taken. The actor layer runs at 2s for things
    // that DO move; half that is plenty here and still nowhere near the per-frame
    // walk that cost ~30fps (EntityMarkerSource.h).
    constexpr float kCollectSeconds  = 1.0f;
    constexpr float kPad             = 45.0f;

    struct CachedBeam {
        RE::NiPoint3                    position;
        RE::FormID                      formID;
        EntityMarkerSource::Category    category;
        EntityMarkerSource::SubCategory subCategory;
    };

    std::vector<CachedBeam>               g_beams;
    std::chrono::steady_clock::time_point g_lastCollectAt{};
    RE::FormID                            g_lastCellID  = 0;
    RE::FormID                            g_lastWorldID = 0;

    // Discovery and live occlusion share one frame-level Havok budget. The
    // discovery queue is serviced on the 1 Hz collect frame; occlusion gets the
    // remainder and all eight slots on ordinary frames.
    constexpr std::size_t kMaxLootBeamRaysPerFrame = 8;
    EntityMarkerSource::DiscoveryRayBudget g_discoveryRayBudget{ kMaxLootBeamRaysPerFrame };

    constexpr float kOcclusionInterval = 0.25f;  // 4 Hz cadence (re-cast every 250ms)
    constexpr float kOcclusionFadeSecs = 0.18f;  // ease so nothing snaps

    struct LootOcclusionState {
        float  blocked  = 0.0f; // 0 = fully visible, 1 = fully blocked (eased per frame)
        float  target   = 0.0f; // raw result from last 4Hz cast
        double lastCast = 0.0;
        bool   seeded   = false;
    };
    std::unordered_map<RE::FormID, LootOcclusionState> g_occlusion;

    bool IsPointOccluded(RE::TESObjectCELL* a_cell,
                         const RE::NiPoint3& a_camPos,
                         const RE::NiPoint3& a_target,
                         RE::FormID a_targetFormID,
                         float* a_outHitDist = nullptr,
                         float* a_outHitFrac = nullptr,
                         RE::FormID* a_outHitFormID = nullptr,
                         std::uint32_t* a_outHitLayer = nullptr,
                         bool* a_outIgnoredTarget = nullptr,
                         bool* a_outIgnoredDeadBip = nullptr) {
        if (a_outIgnoredTarget) *a_outIgnoredTarget = false;
        if (a_outIgnoredDeadBip) *a_outIgnoredDeadBip = false;
        if (!a_cell) return false;
        auto* world = a_cell->GetbhkWorld();
        if (!world) return false;

        RE::NiPoint3 dir = a_target - a_camPos;
        const float len = dir.Length();
        if (len < 1e-3f) return false;
        dir /= len;

        // Step forward past player body and equipped gear (LookPinRenderer.cpp:171-179).
        // In first person and third person, kLOS hits the player's own body/gear at ~2 units.
        constexpr float kStartOffset = 40.0f;
        const float startDist = (len > kStartOffset * 2.0f) ? kStartOffset : 0.0f;

        constexpr float kSurfaceEpsilon = 24.0f;
        const float castLen = (std::max)(len - startDist - kSurfaceEpsilon, 1.0f);

        RE::bhkPickData pick;
        const float scale = RE::bhkWorld::GetWorldScale();
        const RE::NiPoint3 from = a_camPos + dir * startDist;
        const RE::NiPoint3 to   = from + dir * castLen;
        pick.rayInput.from = from * scale;
        pick.rayInput.to   = to * scale;
        pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);

        if (!world->PickObject(pick) || !pick.rayOutput.HasHit()) {
            return false;
        }

        // Secondary self-hit reject: gear/weapons can extend up to ~60 units from camera
        constexpr float kSelfHitReject = 60.0f;
        const float hitDistance = startDist + (castLen * pick.rayOutput.hitFraction);
        if (hitDistance < kSelfHitReject) {
            return false;
        }

        const auto hitLayer = pick.rayOutput.rootCollidable->GetCollisionLayer();
        auto* hitRef = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);

        if (a_outHitFrac) *a_outHitFrac = pick.rayOutput.hitFraction;
        if (a_outHitDist) *a_outHitDist = hitDistance;
        if (a_outHitLayer) *a_outHitLayer = static_cast<std::uint32_t>(hitLayer);
        if (a_outHitFormID) *a_outHitFormID = hitRef ? hitRef->GetFormID() : 0;

        // Reaching the requested reference is clear LOS, not a wall hit. Large
        // chest collision routinely extends beyond the endpoint epsilon in
        // interiors; confirmed with target-self fractions of 0.95 to 1.00.
        if (hitRef && hitRef->GetFormID() == a_targetFormID) {
            if (a_outIgnoredTarget) *a_outIgnoredTarget = true;
            return false;
        }

        // A corpse may be the target itself or another body lying across the ray.
        // Neither is a wall, and creature collision can extend above the visual mesh.
        if (hitLayer == RE::COL_LAYER::kDeadBip) {
            if (a_outIgnoredDeadBip) *a_outIgnoredDeadBip = true;
            return false;
        }
        return true;
    }


    // Colour per CATEGORY, three values, not per subtype and not per rarity.
    // Corpses read warm-red, containers gold, loose loot blue-white.
    void ColoursFor(EntityMarkerSource::Category a_cat,
                    ImU32& a_core, ImU32& a_glow, ImU32& a_halo) {
        using Cat = EntityMarkerSource::Category;
        switch (a_cat) {
            case Cat::Corpse:
                a_core = IM_COL32(255, 240, 235, 255);
                a_glow = IM_COL32(255,  85,  55, 255);
                a_halo = IM_COL32(210,  35,  20, 255);
                break;
            case Cat::Container:
                a_core = IM_COL32(255, 250, 220, 255);
                a_glow = IM_COL32(255, 195,  40, 255);
                a_halo = IM_COL32(230, 140,  15, 255);
                break;
            default:  // LooseLoot (Door/Ingredient are never eligible)
                a_core = IM_COL32(240, 250, 255, 255);
                a_glow = IM_COL32( 70, 200, 255, 255);
                a_halo = IM_COL32( 20, 130, 235, 255);
                break;
        }
    }

    // Re-walk when the timer expires OR the player changed cell/worldspace.
    // Without the space check, walking through a door keeps showing the previous
    // room's loot for a full cadence tick.
    bool ShouldCollect(RE::FormID a_cellID, RE::FormID a_worldID) {
        if (g_lastCellID != a_cellID || g_lastWorldID != a_worldID) return true;
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(now - g_lastCollectAt).count() >= kCollectSeconds;
    }

    // Filter by discovery, ownership, floor band, and radius. Pure Radius mode
    // admits every eligible survivor; there is no separate beam-count cap.
    //
    // XY-only, matching the destination beacon's distance math: including Z would
    // let a corpse one floor below outrank an item in this room.
    void RebuildCache(const std::vector<EntityMarkerSource::EntityMarker>& a_ents,
                      const RE::NiPoint3& a_playerPos) {
        g_beams.clear();

        auto* s = Settings::GetSingleton();
        const float rangeSq = s ? s->lootBeamRangeUnits * s->lootBeamRangeUnits : 0.0f;
        const float zBand   = s ? s->lootBeamZBandUnits : 0.0f;
        const bool  perfLog = s && s->lootBeamPerfLog;

        g_beams.reserve(a_ents.size());
        std::unordered_set<RE::FormID> activeIDs;
        activeIDs.reserve(a_ents.size());

        for (const auto& e : a_ents) {
            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(e.formID);
            if (!ref) continue;

            const float dx = e.position.x - a_playerPos.x;
            const float dy = e.position.y - a_playerPos.y;
            const float dSq = dx * dx + dy * dy;
            const float dH = std::sqrt(dSq);
            const float dZ = std::fabs(e.position.z - a_playerPos.z);
            const float distM = dH / Settings::kUnitsPerMeter;
            const float zM    = dZ / Settings::kUnitsPerMeter;

            if (!e.discovered) {
                if (perfLog) {
                    SKSE::log::info("LootBeam REJECT [NOT_DISCOVERED]: formID={:08X} sub={} dist={:.1f}m zDelta={:.1f}m",
                                    e.formID, SubCategoryToString(e.subCategory), distM, zM);
                }
                continue;
            }
            if (IsOwnedAndHidden(e.formID, e.subCategory)) {
                if (perfLog) {
                    SKSE::log::info("LootBeam REJECT [CRIME_OWNED]: formID={:08X} sub={} dist={:.1f}m",
                                    e.formID, SubCategoryToString(e.subCategory), distM);
                }
                continue;
            }
            if (s && dZ > zBand) {
                if (perfLog) {
                    SKSE::log::info("LootBeam REJECT [Z_BAND]: formID={:08X} sub={} dist={:.1f}m zDelta={:.1f}m (band={:.1f}m)",
                                    e.formID, SubCategoryToString(e.subCategory), distM, zM, zBand / Settings::kUnitsPerMeter);
                }
                continue;
            }
            if (s && dSq > rangeSq) {
                if (perfLog) {
                    SKSE::log::info("LootBeam REJECT [OUT_OF_RANGE]: formID={:08X} sub={} dist={:.1f}m (range={:.1f}m)",
                                    e.formID, SubCategoryToString(e.subCategory), distM, s->lootBeamRangeUnits / Settings::kUnitsPerMeter);
                }
                continue;
            }

            if (perfLog) {
                SKSE::log::info("LootBeam ADMIT: formID={:08X} sub={} dist={:.1f}m zDelta={:.1f}m",
                                e.formID, SubCategoryToString(e.subCategory), distM, zM);
            }

            g_beams.push_back(CachedBeam{ e.position, e.formID, e.category, e.subCategory });
            activeIDs.insert(e.formID);
        }

        // Prune stale occlusion entries without wiping active beam state
        std::erase_if(g_occlusion, [&](const auto& pair) {
            return !activeIDs.contains(pair.first);
        });
    }

    // Per-frame staleness test. The collect cadence alone is not enough: a looted
    // corpse would keep its beam for up to a second, which is directly in the
    // player's face and reads as broken. At cap 8 this is at most 8 lookups per
    // frame, orders of magnitude below the cell walk it stands in for.
    bool IsStillLootable(const CachedBeam& a_beam) {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_beam.formID);
        if (!ref) return false;
        if (ref->IsDisabled() || ref->IsMarkedForDeletion() || ref->IsDeleted()) return false;

        // Despawn/dissolve does not disable or delete the ref, it just drops its
        // 3D. Without this the beam kept drawing over empty ground because the
        // corpse still held inventory. Also covers the ref surviving into an
        // unloaded cell between collects.
        if (!ref->Get3D()) return false;

        // Containers and corpses persist after being emptied, so existence alone
        // does not prove there is anything left to take. HasActualLoot checks for
        // real items with net count > 0 while excluding authored LeveledItem (LVLI)
        // templates that CommonLib's GetInventoryCount() would retain as phantom loot.
        //
        // Loose loot needs no such test: picking an item up deletes or disables
        // the reference, which the checks above already catch.
        using Cat = EntityMarkerSource::Category;
        // A "beam outlives a resurrected corpse" re-test was tried here and
        // REMOVED 2026-08-16. Diagnostic logging proved it never fired: every
        // corpse beam reported isDead=true, drift=0, has3D=true. The apparent
        // ghost beams were real corpses BURIED under a dungeon-overhaul mod's
        // added geometry (see the STATUS note below). Do not re-add it without
        // evidence of a beam whose ref actually reads alive.
        if ((a_beam.category == Cat::Container || a_beam.category == Cat::Corpse) &&
            !HasActualLoot(*ref)) {
            return false;
        }
        return true;
    }

}

void LootBeamRenderer::InstallEventSink() {
    if (_eventSinkInstalled) return;

    auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!holder) {
        SKSE::log::error("LootBeam: failed to get ScriptEventSourceHolder");
        return;
    }

    holder->AddEventSink<RE::TESContainerChangedEvent>(this);
    _eventSinkInstalled = true;
    SKSE::log::info("LootBeam TESContainerChangedEvent sink installed");
}

void LootBeamRenderer::ClearLootedHolders() {
    std::scoped_lock lock(g_lootedHoldersMutex);
    g_lootedHolders.clear();
}

RE::BSEventNotifyControl LootBeamRenderer::ProcessEvent(
    const RE::TESContainerChangedEvent* a_event,
    RE::BSTEventSource<RE::TESContainerChangedEvent>*) {
    if (!a_event || a_event->itemCount <= 0 || a_event->oldContainer == 0) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || a_event->newContainer != player->GetFormID() ||
        a_event->oldContainer == player->GetFormID()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* holder = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->oldContainer);
    if (!holder) return RE::BSEventNotifyControl::kContinue;

    auto* base = holder->GetBaseObject();
    const bool isContainer = base && base->Is(RE::FormType::Container);
    const auto* actor = holder->As<RE::Actor>();
    if (!isContainer && (!actor || !actor->IsDead())) {
        return RE::BSEventNotifyControl::kContinue;
    }

    bool inserted = false;
    {
        std::scoped_lock lock(g_lootedHoldersMutex);
        inserted = g_lootedHolders.insert(a_event->oldContainer).second;
    }

    auto* settings = Settings::GetSingleton();
    if (inserted && settings && settings->lootBeamPerfLog) {
        SKSE::log::info("LootBeam MARK [LOOTED]: holder={:08X} item={:08X} count={}",
                        a_event->oldContainer, a_event->baseObj, a_event->itemCount);
    }

    return RE::BSEventNotifyControl::kContinue;
}

void LootBeamRenderer::Draw() {
    // The toggle is a runtime visibility override. Keep it ahead of every
    // gameplay lookup so hidden beams do no collection, raycast, or draw work.
    if (!IsVisible()) return;

    // Menu gate, same shape as WorldBeaconRenderer: beams are a gameplay-view
    // overlay and must not paint over ANY vanilla menu (Tween/Tab, Dialogue,
    // Fader, containers, the map) or a loading screen.
    // Party Sheet's panels are ImGui, not registered IMenus, so ShouldBlockOverlay
    // cannot see them. Its state API can; no-ops when Party Sheet isn't installed.
    auto* ui = RE::UI::GetSingleton();
    if (!ui || MenuUtils::ShouldBlockOverlay() || PartySheetState::ShouldHideOverlay()) {
        return;
    }

    // Beast form: a werewolf cannot open a container at all, so a loot beam
    // would point at something the player physically can't act on. Hard cut,
    // matching the combat gate — beams carry no fade state.
    if (Settings::GetSingleton()->hideInBeastForm && BeastForm::IsBeastForm()) {
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    auto* parentCell = player->GetParentCell();
    if (!parentCell) return;

    auto* cam = RE::Main::WorldRootCamera();
    if (!cam) return;

    auto* settings = Settings::GetSingleton();
    if (!settings || !settings->lootBeamVisible || !settings->LootBeamAnySubtype()) return;

    // Beams have no fade state, so this is a hard cut rather than the eased
    // out-and-back the destination beacon does. Sits ahead of ShouldCollect so
    // combat also skips the 1Hz cell walk, not just the draw.
    if (settings->lootBeamHideInCombat && player->IsInCombat()) return;

    const RE::NiPoint3 playerPos  = player->GetPosition();
    const bool         isExterior = parentCell->IsExteriorCell();
    auto*              world      = player->GetWorldspace();
    const RE::FormID   cellID     = parentCell->GetFormID();
    const RE::FormID   worldID    = (isExterior && world) ? world->GetFormID() : 0;
    std::size_t discoveryRaysThisFrame = 0;

    static bool s_logDrawPass = false;
    if (ShouldCollect(cellID, worldID)) {
        // Interior walks the cell; exterior radius-walks the loaded grid (there
        // is no single cell to walk outdoors). Radius reuses the Inspect radar
        // range rather than introducing a knob this pass does not want.
        // Reveal radius is OUR setting, not the shared 1000-unit default: an
        // entity must pass reveal before the range filter ever sees it, so the
        // shared constant silently capped the range slider at ~14m.
        //
        // Exterior WALK radius is the larger of our range and our reveal, not
        // navInspectRadarRangeUnits (which it used to borrow): a different
        // feature's setting must not decide how far this one can look.
        // PERF INSTRUMENTATION, permanent, behind [Logging] LootBeamPerfLog
        // (default off). Kept rather than stripped: it answered two real
        // questions in one session (which gate actually limits beams, and how
        // draw cost scales with beam count), and rebuilding it each time is the
        // expensive part.
        //
        // Times the two cost paths SEPARATELY because they scale differently:
        // the walk is 1 Hz and scales with cell ref count, the per-frame beam
        // check runs on beams within range. Conflating them is how "the mod
        // is slow" gets blamed on the wrong half -- a 25-bandit test already
        // showed an fps drop that was spawned AI, not us.
        const bool  perfLog   = settings->lootBeamPerfLog;
        const auto  tWalk0    = std::chrono::steady_clock::now();

        std::vector<EntityMarkerSource::EntityMarker> ents;
        const float range    = settings->lootBeamRangeUnits;
        const bool  needLOS  = settings->lootBeamRequireLOS;
        if (isExterior) {
            ents = EntityMarkerSource::Collect(player, range, playerPos, range, needLOS,
                                                IsBeamSubtypeEnabled, &g_discoveryRayBudget);
        } else {
            ents = EntityMarkerSource::Collect(parentCell, playerPos, range, needLOS,
                                                IsBeamSubtypeEnabled, &g_discoveryRayBudget);
        }
        discoveryRaysThisFrame = g_discoveryRayBudget.raysCast;

        const auto tWalk1 = std::chrono::steady_clock::now();
        RebuildCache(ents, playerPos);
        const auto tRebuild1 = std::chrono::steady_clock::now();

        if (perfLog) {
            const float walkUs    = std::chrono::duration<float, std::micro>(tWalk1 - tWalk0).count();
            const float rebuildUs = std::chrono::duration<float, std::micro>(tRebuild1 - tWalk1).count();
            // classified = what survived the walk and competes for the shortlist.
            // atShortlistCap=YES means the walk filled kMaxEntities and the
            // nearest-wins eviction ran its linear max-scan for every further
            // classified ref -- the one superlinear term in the system.
            SKSE::log::info(
                "LootBeam PERF: walk={:.0f}us rebuild={:.0f}us classified={} beams={} "
                "(shortlistCap={} atShortlistCap={}) filtered={} discoveryCandidates={} "
                "discoveryRays={} discoveryPassed={} discoveryTargetSelfHits={} "
                "discoveryDeferred={} discoveryRayTotal={:.1f}us",
                walkUs, rebuildUs, ents.size(), g_beams.size(),
                EntityMarkerSource::kMaxEntities,
                ents.size() >= EntityMarkerSource::kMaxEntities ? "YES" : "no",
                g_discoveryRayBudget.filteredOut, g_discoveryRayBudget.candidates,
                g_discoveryRayBudget.raysCast, g_discoveryRayBudget.discovered,
                g_discoveryRayBudget.targetSelfHits, g_discoveryRayBudget.deferred,
                g_discoveryRayBudget.rayUs);
        }

        g_lastCollectAt = std::chrono::steady_clock::now();
        g_lastCellID    = cellID;
        g_lastWorldID   = worldID;
        s_logDrawPass   = perfLog;
    }

    const bool perfLog = settings && settings->lootBeamPerfLog;
    std::erase_if(g_beams, [&](const CachedBeam& a_beam) {
        if (ShouldHideAfterLooting(a_beam.category, a_beam.formID)) {
            g_occlusion.erase(a_beam.formID);
            if (perfLog) {
                SKSE::log::info("LootBeam PRUNE [LOOTED]: formID={:08X}", a_beam.formID);
            }
            return true;
        }
        if (IsStillLootable(a_beam)) return false;
        g_occlusion.erase(a_beam.formID);
        if (perfLog) {
            SKSE::log::info("LootBeam PRUNE [EMPTY/INVALID]: formID={:08X}", a_beam.formID);
        }
        return true;
    });

    if (g_beams.empty()) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;

    const RE::NiPoint3 camPos = cam->world.translate;
    const float timeSec = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double nowSec = static_cast<double>(timeSec);
    const float dt = ImGui::GetIO().DeltaTime;

    static double s_accumRayUs = 0.0;
    static int    s_rayCount   = 0;
    static std::size_t s_maxTotalRaysInFrame = 0;

    const bool perfFrameLog = settings->lootBeamPerfLog;
    auto tDraw0 = std::chrono::steady_clock::now();

    std::size_t raysCastThisFrame = 0;
    static std::size_t s_occlusionCursor = 0;
    const std::size_t occlusionRayBudget =
        kMaxLootBeamRaysPerFrame - (std::min)(discoveryRaysThisFrame, kMaxLootBeamRaysPerFrame);

    const std::size_t numBeams = g_beams.size();
    if (numBeams > 0 && settings->lootBeamOcclusion) {
        if (s_occlusionCursor >= numBeams) s_occlusionCursor = 0;
        for (std::size_t i = 0; i < numBeams && raysCastThisFrame < occlusionRayBudget; ++i) {
            const std::size_t idx = (s_occlusionCursor + i) % numBeams;
            const auto& b = g_beams[idx];
            auto& occ = g_occlusion[b.formID];

            if (!occ.seeded || (nowSec - occ.lastCast) >= kOcclusionInterval) {
                occ.lastCast = nowSec;
                const auto tRay0 = std::chrono::steady_clock::now();
                constexpr float probeZOffset = 40.0f;
                const RE::NiPoint3 probePoint{ b.position.x, b.position.y, b.position.z + probeZOffset };
                float hitDist = 0.0f;
                float hitFrac = 0.0f;
                RE::FormID hitFormID = 0;
                std::uint32_t hitLayer = 0;
                bool ignoredTarget = false;
                bool ignoredDeadBip = false;
                const bool occluded = IsPointOccluded(parentCell, camPos, probePoint, b.formID,
                                                       &hitDist, &hitFrac, &hitFormID, &hitLayer,
                                                       &ignoredTarget, &ignoredDeadBip);
                const float target = occluded ? 1.0f : 0.0f;
                occ.target = target;
                if (!occ.seeded) occ.blocked = target;
                occ.seeded = true;
                raysCastThisFrame++;

                if (s_logDrawPass && ignoredTarget) {
                    const float dx = b.position.x - playerPos.x;
                    const float dy = b.position.y - playerPos.y;
                    const float distM = std::sqrt(dx * dx + dy * dy) / Settings::kUnitsPerMeter;
                    SKSE::log::info(
                        "LootBeam RAY IGNORE [TARGET_SELF]: formID={:08X} hitRef={:08X} "
                        "dist={:.1f}m probeZ=+{:.1f} hitDist={:.1f}m frac={:.2f}",
                        b.formID, hitFormID, distM, probeZOffset,
                        hitDist / Settings::kUnitsPerMeter, hitFrac);
                } else if (s_logDrawPass && ignoredDeadBip) {
                    const float dx = b.position.x - playerPos.x;
                    const float dy = b.position.y - playerPos.y;
                    const float distM = std::sqrt(dx * dx + dy * dy) / Settings::kUnitsPerMeter;
                    SKSE::log::info(
                        "LootBeam RAY IGNORE [DEAD_BIP]: formID={:08X} hitRef={:08X} "
                        "sameTarget={} dist={:.1f}m probeZ=+{:.1f} hitDist={:.1f}m frac={:.2f}",
                        b.formID, hitFormID, hitFormID == b.formID ? "YES" : "no",
                        distM, probeZOffset, hitDist / Settings::kUnitsPerMeter, hitFrac);
                } else if (s_logDrawPass && occluded) {
                    const float dx = b.position.x - playerPos.x;
                    const float dy = b.position.y - playerPos.y;
                    const float distM = std::sqrt(dx * dx + dy * dy) / Settings::kUnitsPerMeter;
                    SKSE::log::info(
                        "LootBeam RAY HIT: formID={:08X} hitRef={:08X} sameTarget={} layer={} "
                        "dist={:.1f}m probeZ=+{:.1f} hitDist={:.1f}m frac={:.2f}",
                        b.formID, hitFormID, hitFormID == b.formID ? "YES" : "no", hitLayer,
                        distM, probeZOffset, hitDist / Settings::kUnitsPerMeter, hitFrac);
                }

                if (perfFrameLog) {
                    const float rayUs = std::chrono::duration<float, std::micro>(
                                            std::chrono::steady_clock::now() - tRay0).count();
                    s_accumRayUs += rayUs;
                    s_rayCount++;
                }
            }
        }
        s_occlusionCursor = (s_occlusionCursor + raysCastThisFrame) % numBeams;
    }
    s_maxTotalRaysInFrame = (std::max)(s_maxTotalRaysInFrame,
                                       discoveryRaysThisFrame + raysCastThisFrame);

    for (const auto& b : g_beams) {
        const float dx = b.position.x - playerPos.x;
        const float dy = b.position.y - playerPos.y;
        const float distM = std::sqrt(dx * dx + dy * dy) / Settings::kUnitsPerMeter;

        const RE::NiPoint3 basePos = b.position;
        const RE::NiPoint3 topPos{ basePos.x, basePos.y, basePos.z + settings->lootBeamLengthUnits };

        auto& occ = g_occlusion[b.formID];
        if (!settings->lootBeamOcclusion) {
            occ.target  = 0.0f;
            occ.blocked = 0.0f;
            occ.seeded  = true;
        } else {
            const float rate = std::clamp(dt / kOcclusionFadeSecs, 0.0f, 1.0f);
            const float step = std::clamp(occ.target - occ.blocked, -rate, rate);
            occ.blocked = std::clamp(occ.blocked + step, 0.0f, 1.0f);

            if (occ.blocked >= 0.99f) {
                if (s_logDrawPass) {
                    SKSE::log::info("LootBeam DRAW CULL [OCCLUDED]: formID={:08X} dist={:.1f}m blocked={:.2f} target={:.2f}",
                                    b.formID, distM, occ.blocked, occ.target);
                }
                continue;
            }
        }

        // Both endpoints must be in front of the camera for the screen-space
        // segment to mean anything: behind it the projection mirrors and the
        // "segment" is not the beam the player sees.
        ImVec2 baseScr, topScr;
        bool baseInFront = false, topInFront = false;
        const bool baseProj = MapDraw::ProjectWorldPoint(cam, basePos, baseScr, baseInFront);
        const bool topProj  = MapDraw::ProjectWorldPoint(cam, topPos, topScr, topInFront);

        if (!baseProj || !topProj || !baseInFront || !topInFront) {
            if (s_logDrawPass) {
                SKSE::log::info("LootBeam DRAW CULL [BEHIND_CAMERA]: formID={:08X} dist={:.1f}m baseInFront={} topInFront={} baseProj={} topProj={}",
                                b.formID, distM, baseInFront, topInFront, baseProj, topProj);
            }
            continue;
        }

        // Visibility is a SEGMENT test over the whole beam, never a point test on
        // either end.
        float segT0 = 0.0f, segT1 = 1.0f;
        if (!MapDraw::SegmentInRect(baseScr, topScr, kPad, kPad,
                                    disp.x - kPad, disp.y - kPad, segT0, segT1)) {
            if (s_logDrawPass) {
                SKSE::log::info("LootBeam DRAW CULL [OFFSCREEN]: formID={:08X} dist={:.1f}m baseScr=({:.0f},{:.0f}) topScr=({:.0f},{:.0f})",
                                b.formID, distM, baseScr.x, baseScr.y, topScr.x, topScr.y);
            }
            continue;
        }

        ImU32 core = 0, glow = 0, halo = 0;
        ColoursFor(b.category, core, glow, halo);

        // Badge location at the top of the beam
        const float tTop = (topScr.y <= baseScr.y) ? segT1 : segT0;
        const ImVec2 iconAt(baseScr.x + (topScr.x - baseScr.x) * tTop,
                            baseScr.y + (topScr.y - baseScr.y) * tTop);

        const float iconHalf = static_cast<float>(settings->lootBeamIconSize) * 0.5f;
        const float badgeR = iconHalf * 1.55f;

        // The beam connects the ground base directly to the bottom vertex of the diamond badge
        const ImVec2 beamStart = baseScr;
        const ImVec2 beamEnd = (iconAt.y < baseScr.y)
            ? ImVec2(iconAt.x, iconAt.y + badgeR)
            : iconAt;

        const float occMul = 1.0f - occ.blocked;
        const float alphaScale = std::clamp(settings->lootBeamBrightness, 0.10f, 1.0f) * occMul;

        if (s_logDrawPass) {
            SKSE::log::info("LootBeam DRAW SUCCESS: formID={:08X} dist={:.1f}m baseScr=({:.0f},{:.0f}) topScr=({:.0f},{:.0f}) alphaScale={:.2f}",
                            b.formID, distM, baseScr.x, baseScr.y, topScr.x, topScr.y, alphaScale);
        }

        // 1. Ground contact stroked rings (drawn when base is on-screen)
        // Replaces solid filled discs with thin stroked concentric ellipses (like DrawDestinationRing)
        if (segT0 <= 0.01f) {
            constexpr int kRingPts = 16;
            ImVec2 ringPts[kRingPts];

            auto drawStrokedRing = [&](float rx, float ry, ImU32 col, float thickness) {
                for (int i = 0; i < kRingPts; ++i) {
                    const float a = (static_cast<float>(i) / static_cast<float>(kRingPts)) * 6.2831853f;
                    ringPts[i] = ImVec2(baseScr.x + std::cos(a) * rx, baseScr.y + std::sin(a) * ry);
                }
                dl->AddPolyline(ringPts, kRingPts, MapDraw::WithAlpha(col, alphaScale), ImDrawFlags_Closed, thickness);
            };

            // 3 delicate concentric light rings on the ground plane
            drawStrokedRing( 8.0f, 3.0f, MapDraw::WithAlpha(core, 0.85f), 1.5f);
            drawStrokedRing(14.0f, 5.0f, MapDraw::WithAlpha(glow, 0.60f), 1.3f);
            drawStrokedRing(22.0f, 8.0f, MapDraw::WithAlpha(halo, 0.35f), 1.0f);
        }

        // 2. Feathered-alpha luminous beam: tapers from 18px base to 5px at the badge tip,
        // with outer rail alpha=0 so there are ZERO hard edges.
        MapDraw::DrawFeatheredBeam(
            dl, beamStart, beamEnd, /*steps=*/2,
            /*wBase=*/18.0f, /*wTop=*/5.0f,
            core, glow, halo, alphaScale,
            [](float) { return 1.0f; });

        // 3. Diamond badge frame
        MapDraw::DrawDiamondBadgeFrame(
            dl, iconAt, badgeR, core, glow, halo, alphaScale);

        // 4. Subcategory PNG inside badge
        ID3D11ShaderResourceView* icon = nullptr;
        if (const char* sub = EntityMarkerSource::IconNameFor(b.category, b.subCategory)) {
            icon = TextureManager::GetIconByName(sub);
        }
        if (!icon) {
            icon = TextureManager::GetIconByName(EntityMarkerSource::CategoryIconName(b.category));
        }
        if (icon) {
            MapDraw::DrawIconFx(dl, icon,
                                ImVec2(iconAt.x - iconHalf, iconAt.y - iconHalf),
                                ImVec2(iconAt.x + iconHalf, iconAt.y + iconHalf),
                                MapDraw::WithAlpha(IM_COL32_WHITE, alphaScale));
        }
    }
    s_logDrawPass = false;

    // Accumulate and report every 300 frames: mean AND worst, because a mean
    // hides a periodic spike.
    if (perfFrameLog) {
        static double s_accumUs = 0.0;
        static float  s_worstUs = 0.0f;
        static int    s_frames  = 0;

        const float us = std::chrono::duration<float, std::micro>(
                             std::chrono::steady_clock::now() - tDraw0).count();
        s_accumUs += us;
        if (us > s_worstUs) s_worstUs = us;
        if (++s_frames >= 300) {
            const float avgRayUs = (s_rayCount > 0) ? static_cast<float>(s_accumRayUs / s_rayCount) : 0.0f;
            SKSE::log::info(
                "LootBeams::Draw: mean {:.1f}us, worst {:.1f}us over {} frames "
                "({} beams, {} occlusion rays, ray mean {:.1f}us, max total rays/frame={})",
                s_accumUs / s_frames, s_worstUs, s_frames, g_beams.size(), s_rayCount,
                avgRayUs, s_maxTotalRaysInFrame);
            s_accumUs    = 0.0;
            s_worstUs    = 0.0f;
            s_frames     = 0;
            s_accumRayUs = 0.0;
            s_rayCount   = 0;
            s_maxTotalRaysInFrame = 0;
        }
    }
}
