#include "EntityMarkerSource.h"
#include "Settings.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace EntityMarkerSource {

RE::TESObjectREFRPtr GetAshPileActor(RE::TESObjectREFR& a_ref) {
    auto* base = a_ref.GetBaseObject();
    if (!base || !base->Is(RE::FormType::Activator)) return nullptr;

    // QuickLoot IE's GetContainerObject uses this same ash-pile-to-owner link.
    // Names/models aren't sufficient: decorative ash must never become loot.
    auto owner = a_ref.extraList.GetAshPileRef().get();
    auto* actor = owner ? owner->As<RE::Actor>() : nullptr;
    if (!actor || !actor->IsDead(true) || actor->IsInKillMove()) return nullptr;
    return owner;
}

namespace {
    struct DiscoveryRayStats {
        std::size_t candidates = 0;
        std::size_t raysCast   = 0;
        std::size_t targetSelfHits = 0;
        double      rayUs      = 0.0;
    };

    std::uint64_t g_discoveryGeneration = 1;

    void BeginDiscoveryBudget(DiscoveryRayBudget* a_budget, bool a_requireLOS) {
        if (!a_budget) return;
        if (!a_requireLOS || a_budget->generation != g_discoveryGeneration) {
            a_budget->pending.clear();
            a_budget->queued.clear();
            a_budget->generation = g_discoveryGeneration;
        }
        a_budget->filteredOut = 0;
        a_budget->candidates = 0;
        a_budget->raysCast   = 0;
        a_budget->discovered = 0;
        a_budget->targetSelfHits = 0;
        a_budget->deferred   = 0;
        a_budget->rayUs      = 0.0;
    }

    bool HasLineOfSight(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_from,
                        const RE::NiPoint3& a_target, RE::FormID a_targetFormID,
                        bool* a_outIgnoredTarget = nullptr) {
        if (a_outIgnoredTarget) *a_outIgnoredTarget = false;
        if (!a_cell) return true;  // no cell to cast in: do not block discovery
        auto* world = a_cell->GetbhkWorld();
        if (!world) return true;

        RE::NiPoint3 dir = a_target - a_from;
        const float len = dir.Length();
        if (len < 1e-3f) return true;
        dir /= len;

        // Pull back from the target so the object's own collision (a chest, a
        // corpse lying on the floor) does not read as its own occluder.
        constexpr float kSurfaceEpsilon = 24.0f;
        const float castLen = (std::max)(len - kSurfaceEpsilon, 1.0f);

        // Cast from roughly eye height, not the player's foot position: a floor
        // level cast clips the ground on any downhill target.
        constexpr float kEyeHeight = 100.0f;
        const RE::NiPoint3 from{ a_from.x, a_from.y, a_from.z + kEyeHeight };

        RE::bhkPickData pick;
        const float scale = RE::bhkWorld::GetWorldScale();
        const RE::NiPoint3 to = from + dir * castLen;
        pick.rayInput.from = from * scale;
        pick.rayInput.to   = to * scale;
        pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);

        if (!world->PickObject(pick) || !pick.rayOutput.HasHit()) return true;

        auto* hitRef = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
        if (hitRef && hitRef->GetFormID() == a_targetFormID) {
            if (a_outIgnoredTarget) *a_outIgnoredTarget = true;
            return true;
        }
        return false;
    }
    std::mutex                       g_discMutex;
    std::unordered_set<RE::FormID>   g_discovered;
    std::unordered_set<RE::FormID>   g_discoveredLOS;

    bool HasCoinPurseModel(RE::TESBoundObject* a_base) {
        auto* model = a_base ? a_base->As<RE::TESModel>() : nullptr;
        const char* path = model ? model->GetModel() : nullptr;
        if (!path || !*path) return false;

        std::string lower(path);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("coinbag") != std::string::npos ||
               lower.find("coinpurse") != std::string::npos ||
               lower.find("coin_purse") != std::string::npos;
    }

    SubCategory ClassifyContainer(RE::TESObjectREFR& a_ref) {
        auto* base = a_ref.GetBaseObject();
        auto* model = base ? base->As<RE::TESModel>() : nullptr;
        const char* mp = model ? model->GetModel() : nullptr;
        if (!mp || !*mp) return SubCategory::None;

        std::string lower(mp);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::replace(lower.begin(), lower.end(), '/', '\\');

        if (lower.find("deaddraugr") != std::string::npos ||
            lower.find("draugrbody") != std::string::npos ||
            lower.find("draugrwrapped") != std::string::npos) {
            return SubCategory::ContCoffin;
        }

        constexpr std::string_view kDwarvenSmallChestModel = "clutter\\dwemer\\dwecontainter02.nif";
        constexpr std::string_view kDwarvenLargeChestModel = "clutter\\dwemer\\dwecontainer01.nif";
        if (lower == kDwarvenSmallChestModel || lower == kDwarvenLargeChestModel) return SubCategory::ContChest;

        constexpr std::string_view kFalmerChestModel = "clutter\\falmer\\falmercontainer.nif";
        constexpr std::string_view kFalmerBossChestModel = "clutter\\falmer\\falmercontainer02.nif";
        if (lower == kFalmerChestModel || lower == kFalmerBossChestModel) return SubCategory::ContChest;

        if (lower.find("barrel") != std::string::npos) return SubCategory::ContBarrel;
        if (lower.find("chest") != std::string::npos) return SubCategory::ContChest;
        if (lower.find("strongbox") != std::string::npos || lower.find("safe") != std::string::npos) return SubCategory::ContStrongbox;
        if (lower.find("satchel") != std::string::npos ||
            lower.find("pouch") != std::string::npos ||
            lower.find("knapsack") != std::string::npos) return SubCategory::ContSatchel;
        if (lower.find("sack") != std::string::npos || lower.find("spidersac") != std::string::npos) return SubCategory::ContSack;
        if (lower.find("urn") != std::string::npos || lower.find("pot") != std::string::npos ||
            lower.find("canopicjar") != std::string::npos) return SubCategory::ContUrn;
        if (lower.find("coffin") != std::string::npos || lower.find("sarcophagus") != std::string::npos) return SubCategory::ContCoffin;
        return SubCategory::None;
    }

    RE::BSContainer::ForEachResult ClassifyAndAppend(RE::TESObjectREFR& ref,
                                                      const RE::NiPoint3& a_playerPos,
                                                      std::vector<EntityMarker>& out,
                                                      float a_revealSq,
                                                      bool a_requireLOS,
                                                      FilterFn a_filter,
                                                      DiscoveryRayBudget* a_rayBudget,
                                                      DiscoveryRayStats& a_rayStats) {
        if (ref.IsDisabled() || ref.IsMarkedForDeletion()) return RE::BSContainer::ForEachResult::kContinue;
        if (!ref.Get3D()) return RE::BSContainer::ForEachResult::kContinue;
        if (ref.GetFormID() == 0x14) return RE::BSContainer::ForEachResult::kContinue;  // player

        auto* base = ref.GetBaseObject();
        if (!base) return RE::BSContainer::ForEachResult::kContinue;

        if (auto* keyed = base->As<RE::BGSKeywordForm>()) {
            constexpr RE::FormID kDummyKeyword = 0x000F3E6C;
            if (keyed->HasKeywordID(kDummyKeyword)) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
        }

        std::optional<Category>    cat;
        SubCategory                sub = SubCategory::None;
        switch (base->GetFormType()) {
            case RE::FormType::Door: {
                cat = Category::Door;
                sub = ref.extraList.HasType<RE::ExtraTeleport>() ? SubCategory::DoorLoad : SubCategory::DoorRegular;
                break;
            }
            case RE::FormType::Container: {
                cat = Category::Container;
                sub = ClassifyContainer(ref);
                break;
            }
            case RE::FormType::Weapon:    cat = Category::LooseLoot; sub = SubCategory::LootWeapon; break;
            case RE::FormType::Ammo:      cat = Category::LooseLoot; sub = SubCategory::LootAmmo;   break;
            case RE::FormType::Armor:     cat = Category::LooseLoot; sub = SubCategory::LootArmor; break;
            case RE::FormType::KeyMaster: cat = Category::LooseLoot; sub = SubCategory::LootKey; break;
            case RE::FormType::AlchemyItem: {
                auto* alch = base->As<RE::AlchemyItem>();
                cat = Category::LooseLoot;
                if (alch && alch->IsFood()) {
                    constexpr RE::FormID kITMPotionUse = 0x000B6435;
                    const bool drink = alch->data.consumptionSound &&
                                       alch->data.consumptionSound->GetFormID() == kITMPotionUse;
                    sub = drink ? SubCategory::LootDrink : SubCategory::LootFood;
                } else {
                    sub = SubCategory::LootPotion;
                }
                break;
            }
            case RE::FormType::Book:        cat = Category::LooseLoot; sub = SubCategory::LootBook; break;
            case RE::FormType::SoulGem:     cat = Category::LooseLoot; sub = SubCategory::LootSoulGem; break;
            case RE::FormType::Scroll:      cat = Category::LooseLoot; sub = SubCategory::LootScroll; break;
            case RE::FormType::Misc: {
                if (base->GetFormID() == 0x0000000F) {
                    cat = Category::LooseLoot;
                    sub = SubCategory::LootGold;
                } else if (HasCoinPurseModel(base)) {
                    cat = Category::LooseLoot;
                    sub = SubCategory::LootCoinPurse;
                }
                break;
            }
            case RE::FormType::Ingredient: cat = Category::Ingredient; sub = SubCategory::IngrLoose; break;
            case RE::FormType::Flora: {
                if ((ref.GetFormFlags() & RE::TESObjectREFR::RecordFlags::kHarvested) != 0) break;
                if (HasCoinPurseModel(base)) {
                    cat = Category::LooseLoot;
                    sub = SubCategory::LootCoinPurse;
                    break;
                }
                cat = Category::Ingredient;
                sub = SubCategory::IngrPlant;
                break;
            }
            case RE::FormType::Activator: {
                if (Settings::GetSingleton()->lootBeamPerfLog && ref.extraList.HasType<RE::ExtraAshPileRef>()) {
                    const auto owner = ref.extraList.GetAshPileRef().get();
                    const auto* actor = owner ? owner->As<RE::Actor>() : nullptr;
                    SKSE::log::info("LootBeam ASHPILE: pile={:08X} owner={:08X} dead={} killmove={}",
                                    ref.GetFormID(), owner ? owner->GetFormID() : 0,
                                    actor && actor->IsDead(true), actor && actor->IsInKillMove());
                }
                // Some mods (e.g. GoldInInventory) author coin purses as ACTI records.
                if (HasCoinPurseModel(base)) {
                    cat = Category::LooseLoot;
                    sub = SubCategory::LootCoinPurse;
                } else if (const auto owner = GetAshPileActor(ref)) {
                    cat = Category::Corpse;
                    auto* race = owner->As<RE::Actor>()->GetRace();
                    sub = (race && race->HasKeywordString("ActorTypeNPC")) ? SubCategory::CorpseHumanoid : SubCategory::CorpseCreature;
                }
                break;
            }
            case RE::FormType::Tree: {
                if ((ref.GetFormFlags() & RE::TESObjectREFR::RecordFlags::kHarvested) != 0) break;
                auto* tree = base->As<RE::TESObjectTREE>();
                if (tree && tree->produceItem) {
                    cat = Category::Ingredient;
                    sub = SubCategory::IngrPlant;
                }
                break;
            }
            case RE::FormType::NPC:
            case RE::FormType::LeveledNPC: {
                auto* actor = ref.As<RE::Actor>();
                if (actor && actor->IsDead(true) && !actor->IsInKillMove()) {
                    cat = Category::Corpse;
                    auto* race = actor->GetRace();
                    sub = (race && race->HasKeywordString("ActorTypeNPC")) ? SubCategory::CorpseHumanoid : SubCategory::CorpseCreature;
                }
                break;
            }
            default: break;
        }
        if (!cat) return RE::BSContainer::ForEachResult::kContinue;

        // Reject uninvited categories or items failing filters before distance, LOS raycast, discovery, or shortlist insertion
        if (a_filter && !a_filter(*cat, sub, ref)) {
            if (a_rayBudget) ++a_rayBudget->filteredOut;
            return RE::BSContainer::ForEachResult::kContinue;
        }

        const RE::NiPoint3 pos = ref.GetPosition();

        auto& discSet = a_requireLOS ? g_discoveredLOS : g_discovered;

        if (pos.GetSquaredDistance(a_playerPos) <= a_revealSq) {
            // LOS gate: only cast when the entity is in range and not already
            // discovered, so an explored room costs nothing.
            const bool already = discSet.count(ref.GetFormID()) != 0;
            bool hasLOS = true;
            if (a_requireLOS && !already) {
                ++a_rayStats.candidates;
                if (a_rayBudget) {
                    ++a_rayBudget->candidates;
                    if (a_rayBudget->queued.insert(ref.GetFormID()).second) {
                        a_rayBudget->pending.push_back({ ref.GetFormID(), *cat, sub });
                    }
                    hasLOS = false;
                } else {
                    const auto rayStart = std::chrono::steady_clock::now();
                    bool ignoredTarget = false;
                    hasLOS = HasLineOfSight(ref.GetParentCell(), a_playerPos, pos,
                                             ref.GetFormID(), &ignoredTarget);
                    a_rayStats.rayUs += std::chrono::duration<double, std::micro>(
                                            std::chrono::steady_clock::now() - rayStart).count();
                    ++a_rayStats.raysCast;
                    if (ignoredTarget) ++a_rayStats.targetSelfHits;
                }
            }
            if (!a_requireLOS || already || hasLOS) {
                discSet.insert(ref.GetFormID());
            }
        }

        const bool discovered = discSet.count(ref.GetFormID()) != 0;

        const float dSq = pos.GetSquaredDistance(a_playerPos);
        if (out.size() < kMaxEntities) {
            out.push_back({ *cat, sub, pos, ref.GetFormID(), discovered });
            return RE::BSContainer::ForEachResult::kContinue;
        }

        std::size_t farthest = 0;
        float farthestSq = out[0].position.GetSquaredDistance(a_playerPos);
        for (std::size_t i = 1; i < out.size(); ++i) {
            const float cand = out[i].position.GetSquaredDistance(a_playerPos);
            if (cand > farthestSq) {
                farthestSq = cand;
                farthest   = i;
            }
        }
        if (dSq < farthestSq) {
            out[farthest] = { *cat, sub, pos, ref.GetFormID(), discovered };
        }
        return RE::BSContainer::ForEachResult::kContinue;
    }

    void ServiceDiscoveryBudget(DiscoveryRayBudget& a_budget,
                                const RE::NiPoint3& a_playerPos,
                                float a_revealSq,
                                FilterFn a_filter,
                                std::vector<EntityMarker>& a_out) {
        const std::size_t initialPending = a_budget.pending.size();
        std::size_t inspected = 0;

        while (inspected < initialPending && a_budget.raysCast < a_budget.maxRaysPerCollect) {
            PendingDiscoveryRay pending = a_budget.pending.front();
            a_budget.pending.pop_front();
            a_budget.queued.erase(pending.formID);
            ++inspected;

            if (g_discoveredLOS.contains(pending.formID)) continue;

            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(pending.formID);
            if (!ref || ref->IsDisabled() || ref->IsMarkedForDeletion() || !ref->Get3D()) continue;
            if (a_filter && !a_filter(pending.category, pending.subCategory, *ref)) continue;

            const RE::NiPoint3 pos = ref->GetPosition();
            if (pos.GetSquaredDistance(a_playerPos) > a_revealSq) continue;

            const auto rayStart = std::chrono::steady_clock::now();
            bool ignoredTarget = false;
            const bool hasLOS = HasLineOfSight(ref->GetParentCell(), a_playerPos, pos,
                                                pending.formID, &ignoredTarget);
            a_budget.rayUs += std::chrono::duration<double, std::micro>(
                                  std::chrono::steady_clock::now() - rayStart).count();
            ++a_budget.raysCast;
            if (ignoredTarget) ++a_budget.targetSelfHits;

            if (hasLOS) {
                g_discoveredLOS.insert(pending.formID);
                ++a_budget.discovered;
            } else {
                a_budget.queued.insert(pending.formID);
                a_budget.pending.push_back(pending);
            }
        }

        a_budget.deferred = a_budget.pending.size();

        // Rays run after classification, so publish discoveries made this tick
        // into the already-built marker list before returning it to the caller.
        for (auto& marker : a_out) {
            if (g_discoveredLOS.contains(marker.formID)) marker.discovered = true;
        }
    }
}

std::vector<EntityMarker> Collect(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_playerPos,
                                  float a_revealRadius, bool a_requireLOS,
                                  FilterFn a_filter, DiscoveryRayBudget* a_rayBudget) {
    std::vector<EntityMarker> out;
    if (!a_cell) return out;
    out.reserve(64);

    const float revealSq = a_revealRadius * a_revealRadius;
    DiscoveryRayStats rayStats;

    std::lock_guard<std::mutex> lock(g_discMutex);
    BeginDiscoveryBudget(a_rayBudget, a_requireLOS);

    a_cell->ForEachReference([&](RE::TESObjectREFR* ref) {
        if (!ref) return RE::BSContainer::ForEachResult::kContinue;
        return ClassifyAndAppend(*ref, a_playerPos, out, revealSq, a_requireLOS,
                                 a_filter, a_rayBudget, rayStats);
    });

    if (a_requireLOS && a_rayBudget) {
        ServiceDiscoveryBudget(*a_rayBudget, a_playerPos, revealSq, a_filter, out);
        rayStats.raysCast = a_rayBudget->raysCast;
        rayStats.targetSelfHits = a_rayBudget->targetSelfHits;
        rayStats.rayUs    = a_rayBudget->rayUs;
    }

    if (auto* settings = Settings::GetSingleton(); a_requireLOS && settings && settings->lootBeamPerfLog) {
        const std::size_t deferred = a_rayBudget ? a_rayBudget->deferred : 0;
        SKSE::log::info(
            "EntityMarkerSource DISCOVERY PERF: scope=cell scheduler={} filtered={} candidates={} rays={} "
            "targetSelfHits={} deferred={} "
            "rayTotal={:.1f}us rayMean={:.1f}us",
            a_rayBudget ? "budgeted" : "immediate", a_rayBudget ? a_rayBudget->filteredOut : 0,
            rayStats.candidates, rayStats.raysCast, rayStats.targetSelfHits, deferred, rayStats.rayUs,
            rayStats.raysCast > 0 ? rayStats.rayUs / static_cast<double>(rayStats.raysCast) : 0.0);
    }

    return out;
}

std::vector<EntityMarker> Collect(RE::TESObjectREFR* a_origin, float a_radius, const RE::NiPoint3& a_playerPos,
                                  float a_revealRadius, bool a_requireLOS,
                                  FilterFn a_filter, DiscoveryRayBudget* a_rayBudget) {
    std::vector<EntityMarker> out;
    if (!a_origin) return out;
    out.reserve(64);

    const float revealSq = a_revealRadius * a_revealRadius;
    DiscoveryRayStats rayStats;

    std::lock_guard<std::mutex> lock(g_discMutex);
    BeginDiscoveryBudget(a_rayBudget, a_requireLOS);

    auto* tes = RE::TES::GetSingleton();
    if (!tes) return out;
    const auto originPos = a_origin->GetPosition();
    const float xMin = originPos.x - a_radius, xMax = originPos.x + a_radius;
    const float yMin = originPos.y - a_radius, yMax = originPos.y + a_radius;
    if (auto* grid = tes->gridCells; grid && grid->length > 0) {
        const std::uint32_t len = grid->length;
        for (std::uint32_t x = 0; x < len; ++x) {
            for (std::uint32_t y = 0; y < len; ++y) {
                auto* cell = grid->GetCell(x, y);
                if (!cell || !cell->IsAttached()) continue;
                const auto* coords = cell->GetCoordinates();
                if (!coords) continue;
                // 4096 = exterior cell edge length in world units.
                if (coords->worldX >= xMax || coords->worldX + 4096.0f <= xMin ||
                    coords->worldY >= yMax || coords->worldY + 4096.0f <= yMin) continue;
                cell->ForEachReferenceInRange(originPos, a_radius,
                    [&](RE::TESObjectREFR* ref) {
                        if (!ref) return RE::BSContainer::ForEachResult::kContinue;
                        return ClassifyAndAppend(*ref, a_playerPos, out, revealSq, a_requireLOS,
                                                 a_filter, a_rayBudget, rayStats);
                    });
            }
        }
    }

    if (a_requireLOS && a_rayBudget) {
        ServiceDiscoveryBudget(*a_rayBudget, a_playerPos, revealSq, a_filter, out);
        rayStats.raysCast = a_rayBudget->raysCast;
        rayStats.targetSelfHits = a_rayBudget->targetSelfHits;
        rayStats.rayUs    = a_rayBudget->rayUs;
    }

    if (auto* settings = Settings::GetSingleton(); a_requireLOS && settings && settings->lootBeamPerfLog) {
        const std::size_t deferred = a_rayBudget ? a_rayBudget->deferred : 0;
        SKSE::log::info(
            "EntityMarkerSource DISCOVERY PERF: scope=exterior scheduler={} filtered={} candidates={} rays={} "
            "targetSelfHits={} deferred={} "
            "rayTotal={:.1f}us rayMean={:.1f}us",
            a_rayBudget ? "budgeted" : "immediate", a_rayBudget ? a_rayBudget->filteredOut : 0,
            rayStats.candidates, rayStats.raysCast, rayStats.targetSelfHits, deferred, rayStats.rayUs,
            rayStats.raysCast > 0 ? rayStats.rayUs / static_cast<double>(rayStats.raysCast) : 0.0);
    }

    return out;
}

void ClearDiscovered() {
    std::lock_guard<std::mutex> lock(g_discMutex);
    SKSE::log::info("EntityMarkerSource: discovery wiped ({} radar + {} LOS entries)",
                    g_discovered.size(), g_discoveredLOS.size());
    g_discovered.clear();
    g_discoveredLOS.clear();
    ++g_discoveryGeneration;
}

const char* IconNameFor(Category, SubCategory a_sub) {
    switch (a_sub) {
        case SubCategory::ContChest:      return "chest";
        case SubCategory::ContBarrel:     return "barrel";
        case SubCategory::ContSack:
        case SubCategory::ContSatchel:    return "sack";
        case SubCategory::ContUrn:        return "urn";
        case SubCategory::ContCoffin:     return "coffin";
        case SubCategory::ContStrongbox:  return "strongbox";
        case SubCategory::DoorRegular:    return "door";
        case SubCategory::DoorLoad:       return "door_load";
        case SubCategory::CorpseHumanoid: return "corpse";
        case SubCategory::CorpseCreature: return "corpse_creature";
        case SubCategory::LootWeapon:     return "weapon";
        case SubCategory::LootAmmo:       return "ammo";
        case SubCategory::LootArmor:      return "armor";
        case SubCategory::LootGold:       return "gold";
        case SubCategory::LootKey:        return "key";
        case SubCategory::LootPotion:     return "potion";
        case SubCategory::LootFood:       return "food";
        case SubCategory::LootDrink:      return "drink";
        case SubCategory::LootBook:       return "book";
        case SubCategory::LootSoulGem:    return "soulgem";
        case SubCategory::LootScroll:     return "scroll";
        case SubCategory::LootCoinPurse:  return "coin_purse";
        case SubCategory::IngrLoose:      return "ingredient";
        case SubCategory::IngrPlant:      return "plant";
        case SubCategory::None:
        default:                          return nullptr;
    }
}

const char* CategoryIconName(Category a_cat) {
    switch (a_cat) {
        case Category::Corpse:     return "corpse";
        case Category::Door:       return "door";
        case Category::Container:  return "container";
        case Category::LooseLoot:  return "loot";
        case Category::Ingredient: return "ingredient";
        default:                   return nullptr;
    }
}

} // namespace EntityMarkerSource
