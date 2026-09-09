// Renders 3D light beams over valuable loot, corpses, and containers.

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

// STATUS:

namespace {
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

    bool HasActualLoot(RE::TESObjectREFR& a_ref) {
        const auto counts = a_ref.GetInventoryCounts(
            [](RE::TESBoundObject& a_item) {
                return !a_item.Is(RE::FormType::LeveledItem);
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
            default:                  return false;
        }
        if (!typeEnabled) return false;
        if (s->lootBeamMinGoldValue > 0 && a_cat == EntityMarkerSource::Category::LooseLoot) {
            auto* base = a_ref.GetBaseObject();
            if (!base) return false;
            std::int32_t val = base->GetGoldValue();
            if (base->GetFormID() == 0x0000000F) {
                val *= (std::max)(1, static_cast<int>(a_ref.extraList.GetCount()));
            }
            if (val < s->lootBeamMinGoldValue) return false;
        }
        using Cat = EntityMarkerSource::Category;
        if ((a_cat == Cat::Container || a_cat == Cat::Corpse) && !HasActualLoot(a_ref)) {
            if (s->lootBeamPerfLog) {
                SKSE::log::info("LootBeam REJECT [EMPTY]: formID={:08X} sub={}",
                                a_ref.GetFormID(), SubCategoryToString(a_sub));
            }
            return false;
        }
        return true;
    }

    // what cleared corpses for the ON-by-default switch (see the STATUS block);

    bool IsOwnedAndHidden(RE::FormID a_formID, EntityMarkerSource::SubCategory a_sub) {
        auto* settings = Settings::GetSingleton();
        if (!settings) return false;
        if (!settings->lootBeamHideOwned && !settings->lootBeamPerfLog) return false;
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_formID);
        if (!ref) return false;
        const bool isCrime = ref->IsCrimeToActivate();
        if (settings->lootBeamPerfLog) {
            SKSE::log::info("LootBeam crime check: formID={:08X} sub={} isCrime={}",
                            a_formID, static_cast<int>(a_sub), isCrime ? "true" : "false");
        }
        return settings->lootBeamHideOwned && isCrime;
    }

    constexpr int   kBeamSteps       = 10;

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

    constexpr std::size_t kMaxLootBeamRaysPerFrame = 8;
    EntityMarkerSource::DiscoveryRayBudget g_discoveryRayBudget{ kMaxLootBeamRaysPerFrame };

    constexpr float kOcclusionInterval = 0.25f;
    constexpr float kOcclusionFadeSecs = 0.18f;

    struct LootOcclusionState {
        float  blocked  = 0.0f;
        float  target   = 0.0f;
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
            default:
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
            g_beams.push_back(CachedBeam{ e.position, e.formID, e.category, e.subCategory });
            activeIDs.insert(e.formID);
        }
        std::erase_if(g_occlusion, [&](const auto& pair) {
            return !activeIDs.contains(pair.first);
        });
    }

    bool IsStillLootable(const CachedBeam& a_beam) {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_beam.formID);
        if (!ref) return false;
        if (ref->IsDisabled() || ref->IsMarkedForDeletion() || ref->IsDeleted()) return false;
        if (!ref->Get3D()) return false;
        using Cat = EntityMarkerSource::Category;
        // added geometry (see the STATUS note below). Do not re-add it without
        if ((a_beam.category == Cat::Container || a_beam.category == Cat::Corpse) &&
            !HasActualLoot(*ref)) {
            return false;
        }
        return true;
    }
}

void LootBeamRenderer::Draw() {
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

    auto* cam = RE::Main::WorldRootCamera();
    if (!cam) return;

    auto* settings = Settings::GetSingleton();
    if (!settings || !settings->lootBeamVisible || !settings->LootBeamAnySubtype()) return;

    if (settings->lootBeamHideInCombat && player->IsInCombat()) return;

    const RE::NiPoint3 playerPos  = player->GetPosition();
    const bool         isExterior = parentCell->IsExteriorCell();
    auto*              world      = player->GetWorldspace();
    const RE::FormID   cellID     = parentCell->GetFormID();
    const RE::FormID   worldID    = (isExterior && world) ? world->GetFormID() : 0;
    std::size_t discoveryRaysThisFrame = 0;

    static bool s_logDrawPass = false;
    if (ShouldCollect(cellID, worldID)) {
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
        const float tTop = (topScr.y <= baseScr.y) ? segT1 : segT0;
        const ImVec2 iconAt(baseScr.x + (topScr.x - baseScr.x) * tTop,
                            baseScr.y + (topScr.y - baseScr.y) * tTop);
        const float iconHalf = static_cast<float>(settings->lootBeamIconSize) * 0.5f;
        const float badgeR = iconHalf * 1.55f;
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
            drawStrokedRing( 8.0f, 3.0f, MapDraw::WithAlpha(core, 0.85f), 1.5f);
            drawStrokedRing(14.0f, 5.0f, MapDraw::WithAlpha(glow, 0.60f), 1.3f);
            drawStrokedRing(22.0f, 8.0f, MapDraw::WithAlpha(halo, 0.35f), 1.0f);
        }
        MapDraw::DrawFeatheredBeam(
            dl, beamStart, beamEnd, 2,
            18.0f, 5.0f,
            core, glow, halo, alphaScale,
            [](float) { return 1.0f; });
        MapDraw::DrawDiamondBadgeFrame(
            dl, iconAt, badgeR, core, glow, halo, alphaScale);
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
