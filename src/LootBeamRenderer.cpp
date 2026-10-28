#include "PCH.h"
#include "LootBeamRenderer.h"
#include "LootBeamBlacklist.h"
#include "LootBeamDesigns.h"
#include "TCCItemStatus.h"

#include "EntityMarkerSource.h"
#include "MenuUtils.h"
#include "BeastForm.h"
#include "PartySheetState.h"
#include "Settings.h"
#include "TextureManager.h"
#include "MapDrawHelpers.h"
#include "MapTheme.h"
#include "UIScale.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
    std::mutex                     g_lootedHoldersMutex;
    std::unordered_set<RE::FormID> g_lootedHolders;
    std::unordered_set<RE::FormID> g_inspectedHolders;

    using Clock = std::chrono::steady_clock;
    constexpr float kFreshKillDelaySeconds = 5.0f;
    std::mutex g_freshKillMutex;
    std::unordered_map<RE::FormID, Clock::time_point> g_freshKills;

    std::mutex                     g_droppedWeaponsMutex;
    std::unordered_set<RE::FormID> g_droppedWeapons;

    bool IsTrackedDroppedWeapon(RE::FormID a_formID) {
        std::scoped_lock lock(g_droppedWeaponsMutex);
        return g_droppedWeapons.contains(a_formID);
    }

    RE::TESObjectREFR* FindDroppedRef(RE::FormID a_baseID, RE::PlayerCharacter* a_player) {
        constexpr float kSearchRadius = 512.0f;
        const auto origin = a_player->GetPosition();
        RE::TESObjectREFR* best = nullptr;
        float bestSq = kSearchRadius * kSearchRadius;

        auto visit = [&](RE::TESObjectREFR* a_ref) {
            if (!a_ref || !a_ref->IsDynamicForm() || a_ref->IsDisabled() ||
                a_ref->IsMarkedForDeletion()) return RE::BSContainer::ForEachResult::kContinue;
            auto* base = a_ref->GetBaseObject();
            if (!base || base->GetFormID() != a_baseID) return RE::BSContainer::ForEachResult::kContinue;
            if (IsTrackedDroppedWeapon(a_ref->GetFormID())) return RE::BSContainer::ForEachResult::kContinue;
            const float dSq = a_ref->GetPosition().GetSquaredDistance(origin);
            if (dSq < bestSq) {
                bestSq = dSq;
                best = a_ref;
            }
            return RE::BSContainer::ForEachResult::kContinue;
        };

        auto* cell = a_player->GetParentCell();
        if (!cell) return nullptr;
        if (cell->IsInteriorCell()) {
            cell->ForEachReferenceInRange(origin, kSearchRadius, visit);
            return best;
        }
        auto* tes = RE::TES::GetSingleton();
        auto* grid = tes ? tes->gridCells : nullptr;
        if (!grid) return nullptr;
        for (std::uint32_t x = 0; x < grid->length; ++x) {
            for (std::uint32_t y = 0; y < grid->length; ++y) {
                auto* gc = grid->GetCell(x, y);
                if (!gc || !gc->IsAttached()) continue;
                const auto* coords = gc->GetCoordinates();
                if (!coords) continue;
                // 4096 = exterior cell edge length in world units.
                if (coords->worldX >= origin.x + kSearchRadius || coords->worldX + 4096.0f <= origin.x - kSearchRadius ||
                    coords->worldY >= origin.y + kSearchRadius || coords->worldY + 4096.0f <= origin.y - kSearchRadius) continue;
                gc->ForEachReferenceInRange(origin, kSearchRadius, visit);
            }
        }
        return best;
    }

    constexpr float kDropResolveSeconds = 2.0f;
    struct PendingDrop {
        RE::FormID        baseID;
        Clock::time_point deadline;
    };
    std::vector<PendingDrop> g_pendingDrops;          // guarded by g_droppedWeaponsMutex
    std::atomic_bool         g_resolveTaskQueued{ false };

    void QueueDropResolve() {
        {
            std::scoped_lock lock(g_droppedWeaponsMutex);
            if (g_pendingDrops.empty()) return;
        }
        if (g_resolveTaskQueued.exchange(true)) return;
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            g_resolveTaskQueued = false;
            return;
        }
        tasks->AddTask([]() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* settings = Settings::GetSingleton();
            const bool log = settings && settings->lootBeamPerfLog;
            std::vector<PendingDrop> pending;
            {
                std::scoped_lock lock(g_droppedWeaponsMutex);
                pending.swap(g_pendingDrops);
            }
            const auto now = Clock::now();
            std::vector<PendingDrop> keep;
            for (const auto& p : pending) {
                auto* ref = player ? FindDroppedRef(p.baseID, player) : nullptr;
                if (ref) {
                    {
                        std::scoped_lock lock(g_droppedWeaponsMutex);
                        g_droppedWeapons.insert(ref->GetFormID());
                    }
                    if (log) {
                        SKSE::log::info("LootBeam DROP RESOLVED: item={:08X} ref={:08X}",
                                        p.baseID, ref->GetFormID());
                    }
                } else if (now < p.deadline) {
                    keep.push_back(p);
                } else if (log) {
                    SKSE::log::info("LootBeam DROP UNRESOLVED: item={:08X}", p.baseID);
                }
            }
            {
                std::scoped_lock lock(g_droppedWeaponsMutex);
                g_pendingDrops.insert(g_pendingDrops.end(), keep.begin(), keep.end());
            }
            g_resolveTaskQueued = false;
        });
    }

    bool IsHolderCategory(EntityMarkerSource::Category a_cat) {
        using Cat = EntityMarkerSource::Category;
        return a_cat == Cat::Container || a_cat == Cat::Corpse;
    }

    RE::FormID GetLootHolderID(RE::FormID a_formID) {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_formID);
        const auto owner = ref ? EntityMarkerSource::GetAshPileActor(*ref) : nullptr;
        return owner ? owner->GetFormID() : a_formID;
    }

    bool WasLootedThisSession(RE::FormID a_formID) {
        const auto holderID = GetLootHolderID(a_formID);
        std::scoped_lock lock(g_lootedHoldersMutex);
        return g_lootedHolders.contains(a_formID) || g_lootedHolders.contains(holderID);
    }

    bool ShouldHideAfterLooting(EntityMarkerSource::Category a_cat, RE::FormID a_formID) {
        auto* settings = Settings::GetSingleton();
        return settings && settings->lootBeamHideAfterLooting &&
               IsHolderCategory(a_cat) && WasLootedThisSession(a_formID);
    }

    bool ShouldHideAfterInspecting(EntityMarkerSource::Category a_cat, RE::FormID a_formID) {
        auto* settings = Settings::GetSingleton();
        if (!settings || !settings->lootBeamHideAfterInspecting || !IsHolderCategory(a_cat)) return false;
        const auto holderID = GetLootHolderID(a_formID);
        std::scoped_lock lock(g_lootedHoldersMutex);
        return g_inspectedHolders.contains(a_formID) || g_inspectedHolders.contains(holderID);
    }

    bool IsFreshKillDelayed(RE::FormID a_formID, float& a_remainingSeconds, bool& a_becameReady) {
        a_remainingSeconds = 0.0f;
        a_becameReady = false;

        std::scoped_lock lock(g_freshKillMutex);
        const auto it = g_freshKills.find(a_formID);
        if (it == g_freshKills.end()) return false;

        const float elapsed = std::chrono::duration<float>(Clock::now() - it->second).count();
        if (elapsed >= kFreshKillDelaySeconds) {
            g_freshKills.erase(it);
            a_becameReady = true;
            return false;
        }

        a_remainingSeconds = kFreshKillDelaySeconds - elapsed;
        return true;
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
            case Sub::LootCoinPurse:  return "CoinPurse";
            case Sub::LootSoulGem:    return "SoulGem";
            case Sub::LootScroll:     return "Scroll";
            case Sub::LootBook:       return "Book";
            case Sub::LootPotion:     return "Potion";
            default:                  return "Other";
        }
    }

    bool HasActualLoot(RE::TESObjectREFR& a_ref, bool a_logIgnored = false) {
        const auto owner = EntityMarkerSource::GetAshPileActor(a_ref);
        // Ash piles retain their visible 3D while the actor retains the inventory.
        // Never initialize inventory on an activator with an unresolved owner.
        auto* base = a_ref.GetBaseObject();
        if (base && base->Is(RE::FormType::Activator) &&
            a_ref.extraList.HasType<RE::ExtraAshPileRef>() && !owner) return false;
        auto* holder = owner ? owner.get() : &a_ref;
        const RE::FormID holderFormID = holder->GetFormID();
        if (owner && a_logIgnored) {
            SKSE::log::info("LootBeam ASHPILE INVENTORY: pile={:08X} holder={:08X}",
                            a_ref.GetFormID(), holderFormID);
        }
        const auto counts = holder->GetInventoryCounts(
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

    bool IsBeamSubtypeEnabled(EntityMarkerSource::Category a_cat,
                              EntityMarkerSource::SubCategory a_sub,
                              RE::TESObjectREFR& a_ref) {
        auto* s = Settings::GetSingleton();
        if (!s) return false;

        bool baseMatch = false;
        if (LootBeamBlacklist::IsReferenceBlacklisted(a_ref, &baseMatch)) {
            if (s->lootBeamPerfLog) {
                SKSE::log::info("LootBeam REJECT [{}]: formID={:08X}",
                    baseMatch ? "BLACKLIST_BASE" : "BLACKLIST_REF", a_ref.GetFormID());
            }
            return false;
        }
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
            case Sub::LootCoinPurse:  typeEnabled = s->lootBeamShowLootCoinPurse;  break;
            case Sub::LootPotion:     typeEnabled = s->lootBeamShowLootPotion;     break;
            case Sub::LootSoulGem:    typeEnabled = s->lootBeamShowLootSoulGem;    break;
            case Sub::LootScroll:     typeEnabled = s->lootBeamShowLootScroll;     break;
            case Sub::LootBook:       typeEnabled = s->lootBeamShowLootBook;       break;
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

        if (a_cat == EntityMarkerSource::Category::Corpse) {
            float remainingSeconds = 0.0f;
            bool becameReady = false;
            if (IsFreshKillDelayed(GetLootHolderID(a_ref.GetFormID()), remainingSeconds, becameReady)) {
                if (s->lootBeamPerfLog) {
                    SKSE::log::info("LootBeam REJECT [FRESH_KILL]: formID={:08X} remaining={:.2f}s",
                                    a_ref.GetFormID(), remainingSeconds);
                }
                return false;
            }
            if (becameReady && s->lootBeamPerfLog) {
                SKSE::log::info("LootBeam READY [FRESH_KILL]: formID={:08X} hold={:.1f}s",
                                a_ref.GetFormID(), kFreshKillDelaySeconds);
            }
        }

        if (ShouldHideAfterLooting(a_cat, a_ref.GetFormID())) {
            if (s->lootBeamPerfLog) {
                SKSE::log::info("LootBeam REJECT [LOOTED]: formID={:08X} sub={}",
                                a_ref.GetFormID(), SubCategoryToString(a_sub));
            }
            return false;
        }
        if (ShouldHideAfterInspecting(a_cat, a_ref.GetFormID())) {
            if (s->lootBeamPerfLog) {
                SKSE::log::info("LootBeam REJECT [INSPECTED]: formID={:08X} sub={}",
                                a_ref.GetFormID(), SubCategoryToString(a_sub));
            }
            return false;
        }

        if (s->lootBeamMinGoldValue > 0 && a_cat == EntityMarkerSource::Category::LooseLoot && a_sub != Sub::LootCoinPurse) {
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

    constexpr float kCollectSeconds  = 1.0f;
    constexpr float kPad             = 45.0f;

    struct CachedBeam {
        RE::NiPoint3                    position;
        RE::FormID                      formID;
        EntityMarkerSource::Category    category;
        EntityMarkerSource::SubCategory subCategory;
        TCCItemStatus::Status           museumStatus;
        bool                            dropped = false;  // player's lost weapon
    };

    std::vector<CachedBeam>               g_beams;
    std::chrono::steady_clock::time_point g_lastCollectAt{};
    RE::FormID                            g_lastCellID  = 0;
    RE::FormID                            g_lastWorldID = 0;

    std::mutex g_modelAnchorMutex;
    std::uint64_t g_modelAnchorGeneration = 0;
    bool g_modelAnchorsReady = false;
    std::unordered_map<RE::FormID, RE::NiPoint3> g_modelAnchorSnapshot;
    std::unordered_map<RE::FormID, RE::NiPoint3> g_renderModelAnchors;

    void ClearModelAnchors() {
        std::scoped_lock lock(g_modelAnchorMutex);
        ++g_modelAnchorGeneration;
        g_modelAnchorSnapshot.clear();
        g_modelAnchorsReady = true;
    }

    void ConsumeModelAnchors() {
        std::scoped_lock lock(g_modelAnchorMutex);
        if (!g_modelAnchorsReady) return;
        g_renderModelAnchors.swap(g_modelAnchorSnapshot);
        g_modelAnchorSnapshot.clear();
        g_modelAnchorsReady = false;
    }

    void RequestModelAnchors(RE::FormID a_cellID, bool a_log) {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) return;

        std::vector<RE::FormID> ids;
        for (const auto& beam : g_beams) {
            if (beam.category == EntityMarkerSource::Category::LooseLoot) {
                ids.push_back(beam.formID);
            }
        }
        if (ids.empty()) {
            ClearModelAnchors();
            return;
        }
        std::uint64_t generation;
        {
            std::scoped_lock lock(g_modelAnchorMutex);
            generation = ++g_modelAnchorGeneration;
        }
        // Physics can settle a dropped mesh below its reference position.
        // Read scene transforms on the game thread; Present consumes only positions.
        tasks->AddTask([ids = std::move(ids), generation, a_cellID, a_log]() {
            {
                std::scoped_lock lock(g_modelAnchorMutex);
                if (generation != g_modelAnchorGeneration) return;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->GetParentCell() : nullptr;
            if (!cell || cell->GetFormID() != a_cellID) return;

            std::unordered_map<RE::FormID, RE::NiPoint3> anchors;
            anchors.reserve(ids.size());
            for (const auto id : ids) {
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
                if (!ref || ref->IsDisabled() || ref->IsMarkedForDeletion()) continue;
                const auto referencePos = ref->GetPosition();
                auto* model = ref->Get3D();
                const auto modelPos = model ? model->world.translate : referencePos;
                const bool usableModel = model && std::isfinite(modelPos.x) &&
                    std::isfinite(modelPos.y) && std::isfinite(modelPos.z);
                if (usableModel) anchors.emplace(id, modelPos);
                if (a_log) {
                    SKSE::log::info("LootBeam ANCHOR: ref={:08X} source={} reference=({:.2f},{:.2f},{:.2f}) model=({:.2f},{:.2f},{:.2f}) deltaZ={:.2f}",
                        id, usableModel ? "model" : "reference", referencePos.x, referencePos.y, referencePos.z,
                        modelPos.x, modelPos.y, modelPos.z, modelPos.z - referencePos.z);
                }
            }
            std::scoped_lock lock(g_modelAnchorMutex);
            if (generation != g_modelAnchorGeneration) return;
            g_modelAnchorSnapshot = std::move(anchors);
            g_modelAnchorsReady = true;
        });
    }

    constexpr std::size_t kMaxLootBeamRaysPerFrame = 8;
    EntityMarkerSource::DiscoveryRayBudget g_discoveryRayBudget{ kMaxLootBeamRaysPerFrame };

    constexpr float kOcclusionInterval = 0.25f;
    constexpr float kOcclusionFadeSecs = 0.18f;  // ease so nothing snaps
    constexpr float kActivationFadeOutSecs = 0.18f;
    constexpr float kActivationFadeInSecs  = 0.28f;
    constexpr float kAdmissionRevealSecs   = 0.36f;

    struct LootOcclusionState {
        float  blocked  = 0.0f; // 0 = fully visible, 1 = fully blocked (eased per frame)
        float  target   = 0.0f; // raw result from last 4Hz cast
        double lastCast = 0.0;
        bool   seeded   = false;
        float  activationVisible = 1.0f;
        float  admissionProgress = 0.0f;
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

        if (hitRef && hitRef->GetFormID() == a_targetFormID) {
            if (a_outIgnoredTarget) *a_outIgnoredTarget = true;
            return false;
        }

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

    bool ShouldCollect(RE::FormID a_cellID, RE::FormID a_worldID) {
        if (g_lastCellID != a_cellID || g_lastWorldID != a_worldID) return true;
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(now - g_lastCollectAt).count() >= kCollectSeconds;
    }

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

            TCCItemStatus::Status museumStatus = TCCItemStatus::Status::None;
            if (e.category == EntityMarkerSource::Category::LooseLoot) {
                if (auto* base = ref->GetBaseObject()) {
                    museumStatus = TCCItemStatus::Get(base->GetFormID());
                    if (perfLog) {
                        SKSE::log::info("LootBeam TCC: ref={:08X} item={:08X} status={}",
                                        e.formID, base->GetFormID(), TCCItemStatus::Name(museumStatus));
                    }
                }
            }
            g_beams.push_back(CachedBeam{ e.position, e.formID, e.category,
                                          e.subCategory, museumStatus });
            activeIDs.insert(e.formID);
        }

        std::erase_if(g_occlusion, [&](const auto& pair) {
            return !activeIDs.contains(pair.first) && !IsTrackedDroppedWeapon(pair.first);
        });
        std::erase_if(g_renderModelAnchors, [&](const auto& pair) {
            return !activeIDs.contains(pair.first) && !IsTrackedDroppedWeapon(pair.first);
        });
    }

    void MergeDroppedWeapons(RE::TESObjectCELL* a_cell, bool a_isExterior, bool a_log) {
        auto* s = Settings::GetSingleton();
        if (!s || !s->lootBeamTrackDroppedWeapons) return;

        std::vector<RE::FormID> ids;
        {
            std::scoped_lock lock(g_droppedWeaponsMutex);
            if (g_droppedWeapons.empty()) return;
            ids.assign(g_droppedWeapons.begin(), g_droppedWeapons.end());
        }

        std::vector<RE::FormID> gone;
        for (const auto id : ids) {
            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
            if (!ref || ref->IsDeleted() || ref->IsMarkedForDeletion() || ref->IsDisabled()) {
                gone.push_back(id);
                continue;
            }
            // No 3D = unloaded or far away. Keep tracking; the player may come back.
            if (!ref->Get3D()) continue;
            if (!a_isExterior && ref->GetParentCell() != a_cell) continue;

            const auto existing = std::ranges::find(g_beams, id, &CachedBeam::formID);
            if (existing != g_beams.end()) {
                existing->dropped = true;
                continue;
            }
            g_beams.push_back(CachedBeam{ ref->GetPosition(), id,
                                          EntityMarkerSource::Category::LooseLoot,
                                          EntityMarkerSource::SubCategory::LootWeapon,
                                          TCCItemStatus::Status::None, true });
            if (a_log) {
                SKSE::log::info("LootBeam ADMIT [DROPPED]: ref={:08X}", id);
            }
        }

        if (!gone.empty()) {
            std::scoped_lock lock(g_droppedWeaponsMutex);
            for (const auto id : gone) {
                g_droppedWeapons.erase(id);
                if (a_log) SKSE::log::info("LootBeam UNTRACK [DROPPED_GONE]: ref={:08X}", id);
            }
        }
    }

    bool IsStillLootable(const CachedBeam& a_beam) {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_beam.formID);
        if (!ref) return false;
        if (ref->IsDisabled() || ref->IsMarkedForDeletion() || ref->IsDeleted()) return false;

        if (!ref->Get3D()) return false;

        // Harvested flora (e.g. coin purses) sets kHarvested immediately on interaction.
        if ((ref->GetFormFlags() & RE::TESObjectREFR::RecordFlags::kHarvested) != 0) return false;

        using Cat = EntityMarkerSource::Category;
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
    auto* crosshairSource = SKSE::GetCrosshairRefEventSource();
    if (!crosshairSource) {
        SKSE::log::warn("LootBeam: crosshair event source unavailable; activation fade disabled");
    }

    holder->AddEventSink<RE::TESContainerChangedEvent>(this);
    holder->AddEventSink<RE::TESDeathEvent>(this);
    if (crosshairSource) crosshairSource->AddEventSink(this);
    _eventSinkInstalled = true;
    SKSE::log::info("LootBeam container-change + death event sinks installed; crosshair={}",
                    crosshairSource ? "yes" : "no");
}

void LootBeamRenderer::ClearLootedHolders() {
    {
        std::scoped_lock lock(g_lootedHoldersMutex);
        g_lootedHolders.clear();
    }
    // Same load/new-game lifetime. Dropped refs are dynamic forms and their
    // FormIDs mean nothing in another save.
    std::scoped_lock lock(g_droppedWeaponsMutex);
    g_droppedWeapons.clear();
    g_pendingDrops.clear();
}

void LootBeamRenderer::ClearInspectedHolders() {
    std::scoped_lock lock(g_lootedHoldersMutex);
    g_inspectedHolders.clear();
}

void LootBeamRenderer::MarkInspectedHolder() {
    const auto handle = RE::ContainerMenu::GetTargetRefHandle();
    RE::NiPointer<RE::TESObjectREFR> holder;
    const bool resolved = RE::LookupReferenceByHandle(handle, holder) && holder;
    auto* settings = Settings::GetSingleton();
    const bool perfLog = settings && settings->lootBeamPerfLog;
    if (!resolved) {
        if (perfLog) SKSE::log::info("LootBeam INSPECT: ContainerMenu opened without resolved target");
        return;
    }

    if (const auto owner = EntityMarkerSource::GetAshPileActor(*holder)) {
        holder = owner;
    }
    auto* base = holder->GetBaseObject();
    const bool isContainer = base && base->Is(RE::FormType::Container);
    const auto* actor = holder->As<RE::Actor>();
    if (!isContainer && (!actor || !actor->IsDead())) return;

    bool inserted = false;
    {
        std::scoped_lock lock(g_lootedHoldersMutex);
        inserted = g_inspectedHolders.insert(holder->GetFormID()).second;
    }
    if (inserted && perfLog) {
        SKSE::log::info("LootBeam MARK [INSPECTED]: holder={:08X}", holder->GetFormID());
    }
}

void LootBeamRenderer::ClearFreshKillDelays() {
    std::scoped_lock lock(g_freshKillMutex);
    g_freshKills.clear();
}

void LootBeamRenderer::ClearActivationFade() {
    _crosshairTarget.store(0);
    _activationFadeReset.fetch_add(1);
    ClearModelAnchors();
}

RE::BSEventNotifyControl LootBeamRenderer::ProcessEvent(
    const SKSE::CrosshairRefEvent* a_event,
    RE::BSTEventSource<SKSE::CrosshairRefEvent>*) {
    const RE::FormID targetID = a_event && a_event->crosshairRef
        ? a_event->crosshairRef->GetFormID() : 0;
    const RE::FormID previousID = _crosshairTarget.exchange(targetID);
    if (previousID != targetID) {
        if (auto* settings = Settings::GetSingleton(); settings && settings->lootBeamPerfLog) {
            SKSE::log::info("LootBeam TARGET: previous={:08X} current={:08X}",
                            previousID, targetID);
        }
    }
    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl LootBeamRenderer::ProcessEvent(
    const RE::TESContainerChangedEvent* a_event,
    RE::BSTEventSource<RE::TESContainerChangedEvent>*) {
    if (!a_event || a_event->itemCount <= 0) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return RE::BSEventNotifyControl::kContinue;
    const RE::FormID playerID = player->GetFormID();

    if (a_event->oldContainer == playerID && a_event->newContainer == 0) {
        auto* item = RE::TESForm::LookupByID(a_event->baseObj);
        if (item && item->Is(RE::FormType::Weapon)) {
            const auto dropped = a_event->reference.get();
            auto* settings = Settings::GetSingleton();
            if (settings && settings->lootBeamPerfLog) {
                SKSE::log::info("LootBeam DROP EVENT: item={:08X} count={} ref={:08X} uniqueID={}",
                                a_event->baseObj, a_event->itemCount,
                                dropped ? dropped->GetFormID() : 0, a_event->uniqueID);
            }
            if (dropped) {
                std::scoped_lock lock(g_droppedWeaponsMutex);
                g_droppedWeapons.insert(dropped->GetFormID());
            } else if (settings && settings->lootBeamTrackDroppedWeapons) {
                std::scoped_lock lock(g_droppedWeaponsMutex);
                g_pendingDrops.push_back({ a_event->baseObj,
                                           Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                               std::chrono::duration<float>(kDropResolveSeconds)) });
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
    if (a_event->oldContainer == 0) {
        if (a_event->newContainer == playerID) {
            if (const auto picked = a_event->reference.get()) {
                std::scoped_lock lock(g_droppedWeaponsMutex);
                g_droppedWeapons.erase(picked->GetFormID());
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
    if (!player || a_event->newContainer != player->GetFormID() ||
        a_event->oldContainer == player->GetFormID()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* holder = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->oldContainer);
    if (!holder) return RE::BSEventNotifyControl::kContinue;

    const auto owner = EntityMarkerSource::GetAshPileActor(*holder);
    if (owner) holder = owner.get();
    auto* base = holder->GetBaseObject();
    const bool isContainer = base && base->Is(RE::FormType::Container);
    const auto* actor = holder->As<RE::Actor>();
    if (!isContainer && (!actor || !actor->IsDead())) {
        return RE::BSEventNotifyControl::kContinue;
    }

    bool inserted = false;
    {
        std::scoped_lock lock(g_lootedHoldersMutex);
        inserted = g_lootedHolders.insert(holder->GetFormID()).second;
    }

    auto* settings = Settings::GetSingleton();
    if (inserted && settings && settings->lootBeamPerfLog) {
        SKSE::log::info("LootBeam MARK [LOOTED]: holder={:08X} item={:08X} count={}",
                        holder->GetFormID(), a_event->baseObj, a_event->itemCount);
    }

    return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl LootBeamRenderer::ProcessEvent(
    const RE::TESDeathEvent* a_event,
    RE::BSTEventSource<RE::TESDeathEvent>*) {
    if (!a_event || !a_event->actorDying) {
        return RE::BSEventNotifyControl::kContinue;
    }

    auto* actor = a_event->actorDying->As<RE::Actor>();
    if (!actor || actor->IsPlayerRef()) {
        return RE::BSEventNotifyControl::kContinue;
    }

    const RE::FormID formID = actor->GetFormID();
    const auto now = Clock::now();
    {
        std::scoped_lock lock(g_freshKillMutex);
        std::erase_if(g_freshKills, [&](const auto& pair) {
            return std::chrono::duration<float>(now - pair.second).count() >= kFreshKillDelaySeconds;
        });
        g_freshKills[formID] = now;
    }

    if (auto* settings = Settings::GetSingleton(); settings && settings->lootBeamPerfLog) {
        SKSE::log::info("LootBeam MARK [FRESH_KILL]: formID={:08X} phase={} hold={:.1f}s",
                        formID, a_event->dead ? "final" : "start", kFreshKillDelaySeconds);
    }

    return RE::BSEventNotifyControl::kContinue;
}

void LootBeamRenderer::Draw() {
    // Ahead of every gate: a console player.drop happens with the console open,
    // and the 2s resolve deadline must not run out behind the menu gate.
    QueueDropResolve();

    // The toggle is a runtime visibility override. Keep it ahead of every
    // gameplay lookup so hidden beams do no collection, raycast, or draw work.
    if (!IsVisible()) return;

    auto* ui = RE::UI::GetSingleton();
    if (!ui || MenuUtils::ShouldBlockOverlay() || PartySheetState::ShouldHideOverlay()) {
        return;
    }

    if (Settings::GetSingleton()->hideInBeastForm && BeastForm::IsBeastForm()) {
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    auto* parentCell = player->GetParentCell();
    if (!parentCell) return;

    static RE::FormID s_lastLoggedBlacklistCell = 0;
    if (LootBeamBlacklist::IsEnvironmentBlacklisted(parentCell)) {
        const RE::FormID currentCellID = parentCell->GetFormID();
        if (Settings::GetSingleton()->lootBeamPerfLog && currentCellID != s_lastLoggedBlacklistCell) {
            s_lastLoggedBlacklistCell = currentCellID;
            SKSE::log::info("LootBeam REJECT [BLACKLIST_ENV]: cell={:08X}", currentCellID);
        }
        return;
    }
    s_lastLoggedBlacklistCell = 0;

    auto* cam = RE::Main::WorldRootCamera();
    if (!cam) return;

    auto* settings = Settings::GetSingleton();
    if (!settings || !settings->lootBeamVisible) return;
    const bool trackDropped = settings->lootBeamTrackDroppedWeapons;
    if (!settings->LootBeamAnySubtype() && !trackDropped) return;

    const bool combatCut = settings->lootBeamHideInCombat && player->IsInCombat();
    if (combatCut && !trackDropped) return;
    const bool collectNormal = !combatCut && settings->LootBeamAnySubtype();

    const RE::NiPoint3 playerPos  = player->GetPosition();
    const bool         isExterior = parentCell->IsExteriorCell();
    auto*              world      = player->GetWorldspace();
    const RE::FormID   cellID     = parentCell->GetFormID();
    const RE::FormID   worldID    = (isExterior && world) ? world->GetFormID() : 0;
    std::size_t discoveryRaysThisFrame = 0;

    static bool s_logDrawPass = false;
    ConsumeModelAnchors();
    if (ShouldCollect(cellID, worldID)) {
        if (g_lastCellID != cellID || g_lastWorldID != worldID) {
            ClearModelAnchors();
            ConsumeModelAnchors();
        }
        const bool  perfLog   = settings->lootBeamPerfLog;
        const auto  tWalk0    = std::chrono::steady_clock::now();

        std::vector<EntityMarkerSource::EntityMarker> ents;
        if (collectNormal) {
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
        }

        const auto tWalk1 = std::chrono::steady_clock::now();
        RebuildCache(ents, playerPos);
        MergeDroppedWeapons(parentCell, isExterior, perfLog);
        RequestModelAnchors(cellID, perfLog);
        const auto tRebuild1 = std::chrono::steady_clock::now();

        if (perfLog && collectNormal) {
            const float walkUs    = std::chrono::duration<float, std::micro>(tWalk1 - tWalk0).count();
            const float rebuildUs = std::chrono::duration<float, std::micro>(tRebuild1 - tWalk1).count();
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
    // Combat can start mid-cadence; drop normal beams this frame, not next collect.
    if (!collectNormal) {
        std::erase_if(g_beams, [](const CachedBeam& a_beam) { return !a_beam.dropped; });
    }
    MergeDroppedWeapons(parentCell, isExterior, perfLog);
    std::erase_if(g_beams, [&](const CachedBeam& a_beam) {
        if (a_beam.category == EntityMarkerSource::Category::Corpse) {
            float remainingSeconds = 0.0f;
            bool becameReady = false;
            if (IsFreshKillDelayed(a_beam.formID, remainingSeconds, becameReady)) {
                g_occlusion.erase(a_beam.formID);
                if (perfLog) {
                    SKSE::log::info("LootBeam PRUNE [FRESH_KILL]: formID={:08X} remaining={:.2f}s",
                                    a_beam.formID, remainingSeconds);
                }
                return true;
            }
            if (becameReady && perfLog) {
                SKSE::log::info("LootBeam READY [FRESH_KILL]: formID={:08X} hold={:.1f}s",
                                a_beam.formID, kFreshKillDelaySeconds);
            }
        }
        if (ShouldHideAfterLooting(a_beam.category, a_beam.formID)) {
            g_occlusion.erase(a_beam.formID);
            if (perfLog) {
                SKSE::log::info("LootBeam PRUNE [LOOTED]: formID={:08X}", a_beam.formID);
            }
            return true;
        }
        if (ShouldHideAfterInspecting(a_beam.category, a_beam.formID)) {
            g_occlusion.erase(a_beam.formID);
            if (perfLog) {
                SKSE::log::info("LootBeam PRUNE [INSPECTED]: formID={:08X}", a_beam.formID);
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

    // Save/new-game callbacks run on the game thread. Apply their reset here,
    // where the Present thread owns the per-beam fade state.
    static std::uint32_t s_seenActivationFadeReset = 0;
    const std::uint32_t fadeReset = _activationFadeReset.load();
    if (fadeReset != s_seenActivationFadeReset) {
        for (auto& [formID, state] : g_occlusion) {
            state.activationVisible = 1.0f;
            state.admissionProgress = 0.0f;
        }
        s_seenActivationFadeReset = fadeReset;
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;

    const RE::NiPoint3 camPos = cam->world.translate;
    const float timeSec = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double nowSec = static_cast<double>(timeSec);
    const float dt = ImGui::GetIO().DeltaTime;
    const float fadeDt = std::clamp(dt, 0.0f, 0.1f);
    const RE::FormID activationTarget = _crosshairTarget.load();

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

    const auto design = static_cast<LootBeamDesigns::Design>(
        std::clamp(settings->lootBeamDesign, 0, 1));
    for (const auto& b : g_beams) {
        const float dx = b.position.x - playerPos.x;
        const float dy = b.position.y - playerPos.y;
        const float distM = std::sqrt(dx * dx + dy * dy) / Settings::kUnitsPerMeter;

        auto& occ = g_occlusion[b.formID];
        occ.admissionProgress = std::clamp(
            occ.admissionProgress + fadeDt / kAdmissionRevealSecs, 0.0f, 1.0f);
        const float admissionReveal = occ.admissionProgress * occ.admissionProgress *
                                      (3.0f - 2.0f * occ.admissionProgress);
        RE::NiPoint3 basePos = b.position;
        bool modelAnchorUsed = false;
        if (b.category == EntityMarkerSource::Category::LooseLoot) {
            if (const auto it = g_renderModelAnchors.find(b.formID); it != g_renderModelAnchors.end()) {
                basePos = it->second;
                modelAnchorUsed = true;
            }
        }
        const RE::NiPoint3 topPos{
            basePos.x, basePos.y,
            basePos.z + settings->lootBeamLengthUnits *
                LootBeamDesigns::LengthScale(design) * admissionReveal };

        const float activationGoal = b.formID == activationTarget ? 0.0f : 1.0f;
        const float fadeSecs = activationGoal == 0.0f
            ? kActivationFadeOutSecs : kActivationFadeInSecs;
        const float fadeRate = std::clamp(fadeDt / fadeSecs, 0.0f, 1.0f);
        const float fadeStep = std::clamp(activationGoal - occ.activationVisible,
                                          -fadeRate, fadeRate);
        occ.activationVisible = std::clamp(occ.activationVisible + fadeStep, 0.0f, 1.0f);
        const float activationAlpha = occ.activationVisible * occ.activationVisible *
                                      (3.0f - 2.0f * occ.activationVisible);
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
        if (activationAlpha <= 0.001f) {
            if (s_logDrawPass) {
                SKSE::log::info("LootBeam DRAW CULL [ACTIVATION_TARGET]: formID={:08X}", b.formID);
            }
            continue;
        }

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
        const float edgePad = UIScale::Px(kPad);
        if (!MapDraw::SegmentInRect(baseScr, topScr, edgePad, edgePad,
                                    disp.x - edgePad, disp.y - edgePad, segT0, segT1)) {
            if (s_logDrawPass) {
                SKSE::log::info("LootBeam DRAW CULL [OFFSCREEN]: formID={:08X} dist={:.1f}m baseScr=({:.0f},{:.0f}) topScr=({:.0f},{:.0f})",
                                b.formID, distM, baseScr.x, baseScr.y, topScr.x, topScr.y);
            }
            continue;
        }

        ImU32 core = 0, glow = 0, halo = 0;
        ColoursFor(b.category, core, glow, halo);

        const float occMul = 1.0f - occ.blocked;
        const float alphaScale = std::clamp(settings->lootBeamBrightness, 0.10f, 1.0f) *
                                 occMul * activationAlpha * admissionReveal;

        if (s_logDrawPass) {
            SKSE::log::info("LootBeam DRAW SUCCESS: formID={:08X} dist={:.1f}m baseScr=({:.0f},{:.0f}) topScr=({:.0f},{:.0f}) alphaScale={:.2f} activationAlpha={:.2f} reveal={:.2f} anchorSource={} anchorZ={:.2f}",
                            b.formID, distM, baseScr.x, baseScr.y, topScr.x, topScr.y,
                            alphaScale, activationAlpha, admissionReveal,
                            modelAnchorUsed ? "model" : "reference", basePos.z);
        }

        // Shared admission, projection, clipping, and icon fallback feed both
        // designs. Each file owns only its visible geometry.
        ID3D11ShaderResourceView* icon = nullptr;
        ImU32 statusTint = 0;
        const char* statusIconName = nullptr;
        switch (b.museumStatus) {
            case TCCItemStatus::Status::New:
                statusIconName = "lotd-new";
                statusTint = IM_COL32(90, 185, 255, 255);
                break;
            case TCCItemStatus::Status::Found:
                statusIconName = "lotd-found";
                statusTint = IM_COL32(255, 184, 125, 255);
                break;
            case TCCItemStatus::Status::Displayed:
                statusIconName = "lotd-displayed";
                statusTint = IM_COL32(99, 210, 115, 255);
                break;
            default: break;
        }
        if (statusTint) {
            core = glow = halo = statusTint;
            icon = TextureManager::GetIconByName(statusIconName);
        }
        // Lost weapon reads violet so it stands apart from ordinary loose loot.
        if (b.dropped) {
            core = IM_COL32(250, 235, 255, 255);
            glow = IM_COL32(200, 110, 255, 255);
            halo = IM_COL32(140,  50, 230, 255);
        }
        // The state PNGs already contain their colors. Only opacity may change.
        const bool preserveIconColor = icon != nullptr;
        if (s_logDrawPass && statusIconName) {
            SKSE::log::info("LootBeam TCC ART: ref={:08X} status={} icon={} loaded={} preserveColor={} beamTint={:08X}",
                            b.formID, TCCItemStatus::Name(b.museumStatus), statusIconName,
                            icon != nullptr, preserveIconColor, statusTint);
        }
        if (!icon) {
            if (const char* sub = EntityMarkerSource::IconNameFor(b.category, b.subCategory)) {
                icon = TextureManager::GetIconByName(sub);
            }
        }
        if (!icon) {
            icon = TextureManager::GetIconByName(EntityMarkerSource::CategoryIconName(b.category));
        }
        const LootBeamDesigns::DrawContext designContext{
            dl, baseScr, topScr, segT0, segT1,
            UIScale::Px(static_cast<float>(settings->lootBeamIconSize) * 0.5f),
            alphaScale, core, glow, halo, b.formID, icon, preserveIconColor,
            design == LootBeamDesigns::Design::Modern ? settings->lootBeamModernBottomWidth : settings->lootBeamWispBottomWidth,
            design == LootBeamDesigns::Design::Modern ? settings->lootBeamModernTopWidth : settings->lootBeamWispTopWidth,
            design == LootBeamDesigns::Design::Modern ? settings->lootBeamModernGroundGlow : settings->lootBeamWispGroundGlow };
        switch (design) {
            case LootBeamDesigns::Design::Modern:
                LootBeamDesigns::DrawModern(designContext);
                break;
            case LootBeamDesigns::Design::Wisp:
                LootBeamDesigns::DrawWisp(designContext);
                break;
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
