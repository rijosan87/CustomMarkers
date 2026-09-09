#pragma once

// Tracks interactive world entities (doors, containers, corpses, flora) for radar.

#include <RE/Skyrim.h>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

namespace EntityMarkerSource {
    enum class Category : std::uint8_t { Corpse, Door, Container, LooseLoot, Ingredient };

    enum class SubCategory : std::uint8_t {
        None,
        ContChest, ContBarrel, ContSack, ContUrn, ContCoffin, ContStrongbox,
        DoorRegular, DoorLoad,
        CorpseHumanoid, CorpseCreature,
        LootWeapon, LootArmor, LootGold, LootKey, LootPotion, LootFood, LootDrink, LootBook, LootSoulGem, LootScroll,
        IngrLoose, IngrPlant,
        LootAmmo,
        ContSatchel,
    };

    struct EntityMarker {
        Category     category;
        SubCategory  subCategory;
        RE::NiPoint3 position;
        RE::FormID   formID;
        bool         discovered;
    };

    constexpr float kEntityRevealRadius = 1000.0f;

    constexpr std::size_t kMaxEntities = 256;

    using FilterFn = bool (*)(Category, SubCategory, RE::TESObjectREFR&);

    struct PendingDiscoveryRay {
        RE::FormID  formID;
        Category    category;
        SubCategory subCategory;
    };

    struct DiscoveryRayBudget {
        explicit DiscoveryRayBudget(std::size_t a_maxRaysPerCollect = 8) :
            maxRaysPerCollect(a_maxRaysPerCollect) {}
        std::size_t maxRaysPerCollect = 8;
        std::deque<PendingDiscoveryRay> pending;
        std::unordered_set<RE::FormID> queued;
        std::size_t filteredOut = 0;
        std::size_t candidates = 0;
        std::size_t raysCast   = 0;
        std::size_t discovered = 0;
        std::size_t targetSelfHits = 0;
        std::size_t deferred   = 0;
        double      rayUs      = 0.0;
        std::uint64_t generation = 0;
    };

    std::vector<EntityMarker> Collect(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_playerPos,
                                      float a_revealRadius = kEntityRevealRadius,
                                      bool a_requireLOS = false,
                                      FilterFn a_filter = nullptr,
                                      DiscoveryRayBudget* a_rayBudget = nullptr);

    std::vector<EntityMarker> Collect(RE::TESObjectREFR* a_origin, float a_radius, const RE::NiPoint3& a_playerPos,
                                      float a_revealRadius = kEntityRevealRadius,
                                      bool a_requireLOS = false,
                                      FilterFn a_filter = nullptr,
                                      DiscoveryRayBudget* a_rayBudget = nullptr);

    const char* IconNameFor(Category a_cat, SubCategory a_sub);

    const char* CategoryIconName(Category a_cat);

    void ClearDiscovered();
}
