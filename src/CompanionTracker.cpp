#include "CompanionTracker.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cfloat>
#include <mutex>

namespace CompanionTracker {
    namespace {
        constexpr RE::FormID kDismissedFollowerFaction = 0x0005C84C;
        constexpr RE::FormID kPlayerMarriedFaction     = 0x000C6472;
        constexpr int        kMaxDoorHops              = 4;

        std::mutex         g_mutex;
        std::vector<Entry> g_list;
        float              g_builtAt = -1.0f;

        // Same offset transform as HoverImageRenderer::ConvertToMapWorldspace.
        RE::TESWorldSpace* ToRootFrame(RE::TESWorldSpace* a_ws, RE::NiPoint3& a_pos) {
            auto* ws = a_ws;
            for (int hop = 0; ws && ws->parentWorld && hop < 8; ++hop) {
                a_pos.x += ws->worldMapOffsetData.mapOffsetX;
                a_pos.y += ws->worldMapOffsetData.mapOffsetY;
                ws = ws->parentWorld;
            }
            return ws;
        }

        // Breadth-first over interior cells, so the fewest-hop exit wins. Ties go
        // to the exit nearest the location marker (Dragonsreach's Great Porch door
        // would otherwise win over the front doors).
        RE::TESObjectREFR* FindExteriorDoor(RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_anchor) {
            std::vector<RE::TESObjectCELL*> frontier{ a_cell };
            std::vector<RE::TESObjectCELL*> seen{ a_cell };
            for (int hop = 0; hop < kMaxDoorHops && !frontier.empty(); ++hop) {
                std::vector<RE::TESObjectCELL*>   next;
                std::vector<RE::TESObjectREFR*>   exits;
                for (auto* cell : frontier) {
                    cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
                        auto* tele = a_ref ? a_ref->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
                        if (!tele || !tele->teleportData) return RE::BSContainer::ForEachResult::kContinue;
                        auto linked = tele->teleportData->linkedDoor.get();
                        if (!linked) return RE::BSContainer::ForEachResult::kContinue;
                        // Worldspace test, not parent cell: exterior doors in unloaded areas have no parent cell.
                        if (linked->GetWorldspace()) {
                            exits.push_back(linked.get());
                            return RE::BSContainer::ForEachResult::kContinue;
                        }
                        auto* destCell = linked->GetParentCell();
                        if (destCell && std::ranges::find(seen, destCell) == seen.end()) {
                            seen.push_back(destCell);
                            next.push_back(destCell);
                        }
                        return RE::BSContainer::ForEachResult::kContinue;
                    });
                }
                if (!exits.empty()) {
                    RE::TESObjectREFR* best = exits.front();
                    // Root frame: the anchor and the doors can sit in different child worldspaces.
                    RE::NiPoint3       ap   = a_anchor ? a_anchor->GetPosition() : RE::NiPoint3{};
                    auto*              apRoot = a_anchor ? ToRootFrame(a_anchor->GetWorldspace(), ap) : nullptr;
                    if (apRoot && exits.size() > 1) {
                        float bestD = FLT_MAX;
                        for (auto* door : exits) {
                            RE::NiPoint3 dp = door->GetPosition();
                            if (ToRootFrame(door->GetWorldspace(), dp) != apRoot) continue;
                            const float d = (dp.x - ap.x) * (dp.x - ap.x) + (dp.y - ap.y) * (dp.y - ap.y);
                            if (d < bestD) { bestD = d; best = door; }
                        }
                    }
                    return best;
                }
                frontier = std::move(next);
            }
            return nullptr;
        }

        // Interior markers are skipped: their MNAM can sit inside the location itself.
        RE::TESObjectREFR* LocationMarker(RE::BGSLocation* a_loc) {
            int guard = 0;
            for (auto* cur = a_loc; cur && guard < 16; cur = cur->parentLoc, ++guard) {
                auto marker = cur->worldLocMarker.get();
                if (!marker || !marker->GetWorldspace()) continue;
                auto* cell = marker->GetParentCell();
                if (cell && cell->IsInteriorCell()) continue;
                return marker.get();
            }
            return nullptr;
        }

        void ResolveMapPos(RE::Actor* a_actor, Entry& a_out) {
            RE::TESObjectREFR* anchor = a_actor;
            auto*              cell   = a_actor->GetParentCell();
            if (cell && cell->IsInteriorCell()) {
                anchor = FindExteriorDoor(cell, LocationMarker(cell->GetLocation()));
                if (!anchor) return;
            }
            auto* ws = anchor->GetWorldspace();
            if (!ws) return;
            RE::NiPoint3 pos  = anchor->GetPosition();
            auto*        root = ToRootFrame(ws, pos);
            if (!root) return;
            a_out.hasMapPos   = true;
            a_out.rootWorldID = root->GetFormID();
            a_out.mapPos      = pos;
        }

        void Rebuild() {
            g_list.clear();

            // Looked up before the all-forms lock.
            auto* dismissedFac = RE::TESForm::LookupByID<RE::TESFaction>(kDismissedFollowerFaction);
            auto* marriedFac   = RE::TESForm::LookupByID<RE::TESFaction>(kPlayerMarriedFaction);
            auto* player       = RE::PlayerCharacter::GetSingleton();

            struct Candidate { RE::Actor* actor; bool spouse; };
            std::vector<Candidate> candidates;
            {
                const auto& [map, lock] = RE::TESForm::GetAllForms();
                const RE::BSReadLockGuard guard{ lock };
                if (map) {
                    for (const auto& [id, form] : *map) {
                        if (!form || form->GetFormType() != RE::FormType::ActorCharacter) continue;
                        auto* actor = form->As<RE::Actor>();
                        if (!actor || actor == player || actor->IsDead()) continue;

                        const bool teammate  = actor->IsPlayerTeammate();
                        const bool waiting   = actor->AsActorValueOwner()->GetActorValue(
                                                   RE::ActorValue::kWaitingForPlayer) != 0.0f;
                        const bool spouse    = marriedFac && actor->GetFactionRank(marriedFac, false) >= 0;
                        const bool dismissed = !teammate && dismissedFac &&
                                               actor->GetFactionRank(dismissedFac, false) >= 0;

                        // Following = standing next to the player, spouse included.
                        if (teammate && !waiting) continue;
                        if (!spouse && !teammate && !dismissed) continue;
                        candidates.push_back({ actor, spouse });
                    }
                }
            }

            // Door walks take their own locks, so they run outside the all-forms lock.
            for (const auto& c : candidates) {
                const char* nm = c.actor->GetDisplayFullName();
                if (!nm || !*nm) continue;
                Entry e;
                e.actorID = c.actor->GetFormID();
                e.name    = nm;
                e.spouse  = c.spouse;
                if (auto* cell = c.actor->GetParentCell()) {
                    e.loc = cell->GetLocation();
                    if (e.loc && e.loc->GetFullName()) e.place = e.loc->GetFullName();
                }
                ResolveMapPos(c.actor, e);
                g_list.push_back(std::move(e));
            }
        }
    }

    std::vector<Entry> Get() {
        auto*       cal = RE::Calendar::GetSingleton();
        const float now = cal ? cal->GetCurrentGameTime() : 0.0f;
        std::lock_guard lk(g_mutex);
        if (now != g_builtAt) {
            g_builtAt = now;
            Rebuild();
        }
        return g_list;
    }
}
