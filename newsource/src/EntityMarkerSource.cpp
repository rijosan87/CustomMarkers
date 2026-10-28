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

    // Line-of-sight test for the reveal gate. Same cast WorldBeaconRenderer uses
    // for occlusion (bhkWorld::PickObject with kLOS), duplicated rather than
    // shared because that one is file-local there and pulling it into a header
    // is a bigger change than this gate warrants.
    //
    // Only ever called for an entity that is in reveal range and NOT yet
    // discovered, so the cost falls to zero as a room gets explored: an item is
    // tested at most once per collect until it passes, then never again.
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

        // Large chest and corpse collision can extend more than the 24-unit
        // endpoint pullback. Hitting the object being revealed proves the ray
        // reached it; only a different collidable is an occluder.
        auto* hitRef = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
        if (hitRef && hitRef->GetFormID() == a_targetFormID) {
            if (a_outIgnoredTarget) *a_outIgnoredTarget = true;
            return true;
        }
        return false;
    }
    // Session-only discovered set (per-entity fog of war). Guarded because
    // Collect runs on the engine UI thread (PreDisplay) while ClearDiscovered
    // is driven from the SKSE messaging thread on load. Positive-only: once a
    // FormID is in, it stays until an explicit clear.
    std::mutex                       g_discMutex;
    std::unordered_set<RE::FormID>   g_discovered;
    // SEPARATE set for LOS-gated callers (loot beams). One shared set cannot hold
    // two reveal policies: the radar walks with a_requireLOS=false and inserts on
    // distance alone, so a wall-blocked item the beam gate had just rejected was
    // revealed by the radar's walk a fraction of a second later and the beam drew
    // anyway. Confirmed in-game 2026-08-16 -- Lunar Steel Mace 0003B0CF logged
    // "blocked" on all 8 attempts while its beam was visible through a pillar.
    std::unordered_set<RE::FormID>   g_discoveredLOS;

    // Classifies a container's model path into a container subcategory.
    // Runs at snapshot cadence (see Collect's header comment), not per frame,
    // so a small lowercase copy + substring scan is cheap enough. First
    // matching keyword wins; exact-path exceptions precede the generic token
    // scan so one verified asset does not broaden support to similarly named
    // mod meshes.
    SubCategory ClassifyContainer(RE::TESObjectREFR& a_ref) {
        auto* base = a_ref.GetBaseObject();
        auto* model = base ? base->As<RE::TESModel>() : nullptr;
        const char* mp = model ? model->GetModel() : nullptr;
        if (!mp || !*mp) return SubCategory::None;

        std::string lower(mp);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::replace(lower.begin(), lower.end(), '/', '\\');

        // Corpse-type containers: vanilla builds dead bodies (draugr ambush
        // corpses, laid-out draugr, wrapped draugr) as lootable CONT records, not
        // actors, and routes them to the Coffin bucket (closest body-holder).
        // Match by MODEL PATH, not the native EditorID getter: vanilla strips
        // runtime EditorIDs from CONT records and GetFormEditorID() returns empty
        // (proven via in-game log 2026-07-20). po3 Tweaks has a separate exported
        // EditorID cache, but this classifier must also work without that optional
        // dependency. The draugr-body models share the "deaddraugr"
        // / "draugrbody" tokens; checked before the container-shape scan below so a
        // body never falls through to a generic container. See FormID 0008008D
        // (RuinsDeadDraugrWrapped01.nif). Other corpse families (burnt, mammoth,
        // spiderweb) use unrelated model paths -- add tokens here as they surface.
        if (lower.find("deaddraugr") != std::string::npos ||
            lower.find("draugrbody") != std::string::npos ||
            lower.find("draugrwrapped") != std::string::npos) {
            return SubCategory::ContCoffin;
        }

        // Dwarven chests: DweChest01.nif matches "chest" below. TreasDwarvenChestSmall
        // 00020650 uses Bethesda's misspelled DweContainter02.nif, and TreasDwarvenChestLarge
        // 0002069A uses DweContainer01.nif; neither contains "chest" in the filename.
        // Mutagen verified both meshes are used exclusively by Dwarven chest bases and
        // unplaced dev records. Keep normalized paths exact.
        constexpr std::string_view kDwarvenSmallChestModel = "clutter\\dwemer\\dwecontainter02.nif";
        constexpr std::string_view kDwarvenLargeChestModel = "clutter\\dwemer\\dwecontainer01.nif";
        if (lower == kDwarvenSmallChestModel || lower == kDwarvenLargeChestModel) return SubCategory::ContChest;

        if (lower.find("barrel") != std::string::npos) return SubCategory::ContBarrel;
        if (lower.find("chest") != std::string::npos) return SubCategory::ContChest;
        if (lower.find("strongbox") != std::string::npos || lower.find("safe") != std::string::npos) return SubCategory::ContStrongbox;
        if (lower.find("satchel") != std::string::npos ||
            lower.find("pouch") != std::string::npos ||
            lower.find("knapsack") != std::string::npos) return SubCategory::ContSatchel;
        // "spidersac" = frostbite-spider egg sacs (RuinsSpiderSac05.nif, "Egg
        // Sac"), common in spider dens. No "sack" (with k) in the path, so bucket
        // them with sacks explicitly. Full "spidersac" token, not bare "sac",
        // to avoid matching unrelated paths.
        if (lower.find("sack") != std::string::npos || lower.find("spidersac") != std::string::npos) return SubCategory::ContSack;
        // "canopicjar" = Nordic ruin ash urns (RuinsCanopicJar01/02.nif); no "urn"
        // token in the model path so they were falling to generic container until
        // added here (in-game log 2026-07-20). "urn" already catches
        // RuinsLargeUrnNoStand01.nif etc.
        if (lower.find("urn") != std::string::npos || lower.find("pot") != std::string::npos ||
            lower.find("canopicjar") != std::string::npos) return SubCategory::ContUrn;
        if (lower.find("coffin") != std::string::npos || lower.find("sarcophagus") != std::string::npos) return SubCategory::ContCoffin;
        return SubCategory::None;
    }

    // Shared per-ref classify + discover + append body, factored out of the
    // interior cell walk so the exterior radius walk (spec-findings-Exterior-
    // Navigation-Overlay.md decision 8) can reuse it without duplicating the
    // FormType switch. Caller holds g_discMutex. Never returns kStop: the cap is
    // enforced by nearest-wins eviction at the append site, so both walkers
    // always visit every ref in their range.

    RE::BSContainer::ForEachResult ClassifyAndAppend(RE::TESObjectREFR& ref,
                                                      const RE::NiPoint3& a_playerPos,
                                                      std::vector<EntityMarker>& out,
                                                      float a_revealSq,
                                                      bool a_requireLOS,
                                                      FilterFn a_filter,
                                                      DiscoveryRayBudget* a_rayBudget,
                                                      DiscoveryRayStats& a_rayStats) {
        if (ref.IsDisabled() || ref.IsMarkedForDeletion()) return RE::BSContainer::ForEachResult::kContinue;
        // No 3D = not actually in the scene. A despawned corpse stays in the cell's
        // ref list with its inventory intact, so every other test passes and it
        // classifies as a live lootable at its old coordinates (ghost radar icon +
        // ghost loot beam over empty ground). Get3D() is a pointer read on the ref.
        if (!ref.Get3D()) return RE::BSContainer::ForEachResult::kContinue;
        if (ref.GetFormID() == 0x14) return RE::BSContainer::ForEachResult::kContinue;  // player

        auto* base = ref.GetBaseObject();
        if (!base) return RE::BSContainer::ForEachResult::kContinue;

        // Skip Bethesda's Dummy placeholder records (DummyPotion 0006A07E,
        // DummyArrow, DummyBook, DummyFood...). They carry the "Dummy" keyword
        // (000F3E6C), sit invisibly in some cells, and have no world presence,
        // but our cell walk still finds them. DummyPotion in particular is
        // isFood=false so it classified as LootPotion and drew a phantom potion
        // glyph on the radar in a cell with no real potions. Confirmed via the
        // ALCH classify DIAG log: 0006A07E 'DummyPotion' every snapshot pass.
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
                // ExtraTeleport is only attached to load doors (holds the
                // destination/rotation); a door ref without it is a regular
                // swing/hinge door.
                sub = ref.extraList.HasType<RE::ExtraTeleport>() ? SubCategory::DoorLoad : SubCategory::DoorRegular;
                break;
            }
            case RE::FormType::Container: {
                cat = Category::Container;
                sub = ClassifyContainer(ref);
                break;
            }
            // Ammo is split off from Weapon deliberately. On the radar the two
            // read fine as one glyph, but a bandit camp or an archery range puts
            // an arrow every few feet, so anything that draws per-entity in the
            // WORLD (loot beams) has to be able to exclude them.
            case RE::FormType::Weapon:    cat = Category::LooseLoot; sub = SubCategory::LootWeapon; break;
            case RE::FormType::Ammo:      cat = Category::LooseLoot; sub = SubCategory::LootAmmo;   break;
            case RE::FormType::Armor:     cat = Category::LooseLoot; sub = SubCategory::LootArmor; break;
            case RE::FormType::KeyMaster: cat = Category::LooseLoot; sub = SubCategory::LootKey; break;
            case RE::FormType::AlchemyItem: {
                // Food is ALCH too (cheese, Ash Yam, etc.); IsFood() splits it
                // off so edibles don't all render as potion bottles. Drinks
                // (mead/wine/ale) are food-flagged with no keyword of their
                // own; vanilla's only tell is the consumption sound being the
                // potion gulp ITMPotionUse (0x000B6435), Survival/CACO use the
                // same heuristic. Verified: FoodMead carries only
                // VendorItemFood, so keywords can't split drink from food.
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
                // Only gold gets a glyph; every other misc item is deliberately
                // unclassified to avoid cluttering the overlay with clutter
                // (rocks, bowls, generic junk) that has no dedicated icon.
                if (base->GetFormID() == 0x0000000F) { cat = Category::LooseLoot; sub = SubCategory::LootGold; }
                break;
            }
            case RE::FormType::Ingredient: cat = Category::Ingredient; sub = SubCategory::IngrLoose; break;
            case RE::FormType::Flora: {
                // Flora (grasses/mushrooms/etc, TESObjectACTI-based) is a
                // one-shot harvest: once picked the ref sets kHarvested on
                // itself and stays in the cell (empty node), so skip it
                // rather than show a stale glyph over nothing.
                if ((ref.GetFormFlags() & RE::TESObjectREFR::RecordFlags::kHarvested) != 0) break;
                cat = Category::Ingredient;
                sub = SubCategory::IngrPlant;
                break;
            }
            case RE::FormType::Tree: {
                // Only harvestable trees (those with a PFIG produce item, e.g.
                // apple/pine trees) get a glyph; decorative trees have no
                // TESProduceForm payload and should not bait the player over
                // to an empty trunk.
                if ((ref.GetFormFlags() & RE::TESObjectREFR::RecordFlags::kHarvested) != 0) break;
                auto* tree = base->As<RE::TESObjectTREE>();
                if (tree && tree->produceItem) {
                    cat = Category::Ingredient;
                    sub = SubCategory::IngrPlant;
                }
                break;
            }
            // An actor REFERENCE has form type ActorCharacter, but its BASE
            // object is a TESNPC (FormType::NPC) or a leveled actor. Switching
            // on the base means the ActorCharacter case never fires; classify
            // actors off the reference instead. (Confirmed live: corpse=0
            // forever in a cell full of bodies, Fort Frostmoth 2026-07-16.)
            case RE::FormType::NPC:
            case RE::FormType::LeveledNPC: {
                auto* actor = ref.As<RE::Actor>();
                // Live NPCs deliberately skipped in v1 (adds combat-awareness questions).
                if (actor && actor->IsDead(true)) {
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

        // Each policy owns its own set, and a caller only ever reads the set it
        // writes. Sharing one set let the un-gated radar walk reveal an item the
        // LOS-gated beam walk had just rejected.
        //
        // Callers that DO share a set (loot beams and the radar layers, once both
        // are LOS-gated) can still cross-reveal, because they walk with different
        // reveal radii. That is benign in a way the original bug was not: both
        // apply the same LOS test, so the only difference is distance, and a
        // reveal that leaks across is one the other caller's gate would also have
        // allowed on approach. Do not "fix" it by adding a third set without a
        // concrete case -- the sets are keyed by POLICY, not by consumer.
        auto& discSet = a_requireLOS ? g_discoveredLOS : g_discovered;

        // Discovery: insert if the player is currently within reveal radius.
        // Inserted BEFORE we build the marker, so an entity discovered this walk
        // shows the same frame (the membership read below sees the fresh insert).
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

        // Cap keeps the NEAREST kMaxEntities, not the first encountered. The walk
        // used to kStop on hitting the cap, which made the survivors an arbitrary
        // slice in cell-reference order: Bleak Falls Temple holds 3278 refs, so
        // the 256 kept were whatever the cell listed first, at any distance.
        // Downstream that looked like a broken loot-beam range slider -- widening
        // it admitted nothing because the distant refs never reached the filter
        // (confirmed 2026-08-16). Walk everything, evict the farthest instead.
        //
        // Linear max-scan on eviction only. It runs at snapshot cadence, and only
        // once the cap is full, so the cost lands on dense cells where the walk
        // itself already dominates.
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

    // Take the discovery lock for the whole walk. ForEachReference already holds
    // the cell spin lock; this adds one uncontended mutex, no per-ref allocation.
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

    // Hand-rolled grid walk. Do NOT use RE::TES::ForEachReferenceInRange: its
    // sky-cell tail (NG TES.cpp:85, worldSpace->GetSkyCell()) dereferences a
    // garbage worldSpace pointer on AE 1.6.1170 and CTDs (crash 2026-07-17,
    // TESWorldSpace fn 20543). The loop below is NG's same grid walk minus
    // that tail; cell-level ForEachReferenceInRange is proven safe (Live
    // Bridge uses it). Sky cell holds no lootable refs, nothing lost.
    // Loaded refs only; ungenerated/unloaded cells contribute nothing, fine
    // for a proximity radar.
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
    // Log the SIZE before clearing: proves the wipe actually ran and shows how
    // much carried over, which is the first thing to check when a reveal-gate
    // change appears to do nothing on an already-explored save.
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
