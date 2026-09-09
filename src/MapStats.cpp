#include "MapStats.h"
#include "HoldBounty.h"
#include "Settings.h"

#include <map>

// STATUS: shipped 2026-07-15 per findings/spec-findings-Map-Stats.md. Two

namespace {
    constexpr RE::FormID kLocTypeClearable = 0x000F5E80;

    constexpr std::uint16_t kStandingStoneTypes[] = { 18, 55 };

    bool IsStandingStoneType(std::uint16_t a_type) {
        for (const auto t : kStandingStoneTypes) {
            if (t == a_type) return true;
        }
        return false;
    }
    constexpr std::uintptr_t kMarkerArrayOffset = 0x30470;
    constexpr std::size_t    kMarkerStride      = 0x38;

    inline std::ptrdiff_t MarkerCountOffset() {
        return REL::Relocate<std::ptrdiff_t>(0x30480, 0x30478);
    }

    struct MarkerEntry {
        void*             namePtr;
        RE::RefHandle     handle;
        std::uint32_t     pad0C;
        RE::BSFixedString  nameStr;
        std::uint32_t     type;
    };

    bool LocationBelongsToWorld(RE::BGSLocation* a_loc, RE::TESWorldSpace* a_cur) {
        if (!a_cur) return true;
        auto* root = a_cur->location;
        if (!root || !a_loc) return false;
        for (auto* l = a_loc; l; l = l->parentLoc) {
            if (l == root) return true;
        }
        return false;
    }

    bool InCurrentWorld(RE::TESWorldSpace* a_markerWorld, RE::TESWorldSpace* a_cur) {
        if (!a_cur) return true;
        for (auto* w = a_markerWorld; w; w = w->parentWorld) {
            if (w == a_cur) return true;
        }
        return false;
    }
}

MapStats* MapStats::GetSingleton() {
    static MapStats singleton;
    return &singleton;
}

MapStatsData MapStats::Get() {
    std::lock_guard<std::mutex> lk(_mutex);
    return _data;
}

static std::uint32_t ComputeTotalBounty() {
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) return 0;

    std::int64_t total = 0;
    for (auto* fac : dh->GetFormArray<RE::TESFaction>()) {
        if (!fac || !fac->TracksCrimes()) continue;
        const std::int32_t gold = fac->GetCrimeGold();
        if (gold > 0) total += gold;
    }
    return static_cast<std::uint32_t>(total);
}

void MapStats::Refresh(RE::MapMenu* a_menu, RE::TESWorldSpace* a_currentWorld) {
    const RE::FormID curWorldID = a_currentWorld ? a_currentWorld->GetFormID() : 0;

    {
        const std::uint32_t bounty = ComputeTotalBounty();
        std::lock_guard<std::mutex> lk(_mutex);
        _data.totalBounty = bounty;
    }

    if (Settings::GetSingleton()->mapStatsLog && a_menu) {
        auto  probeBase = reinterpret_cast<std::uintptr_t>(a_menu);
        auto  probeArr  = *reinterpret_cast<std::uintptr_t*>(probeBase + kMarkerArrayOffset);
        const auto probeSize = probeArr
            ? *reinterpret_cast<std::uint32_t*>(probeBase + MarkerCountOffset())
            : 0u;
        if (curWorldID != _probeWorld) {
            _probeWorld = curWorldID;
            _probeFrame = 0;
            _probeLastSize = 0xFFFFFFFFu;
        }
        ++_probeFrame;
        if (probeSize != _probeLastSize || _probeFrame <= 3) {
            logger::info("[MapStats] probe world={:08X} frame={} arraySize={}{}",
                         curWorldID, _probeFrame, probeSize,
                         (curWorldID == _computedForWorld) ? " (already cached)" : "");
            _probeLastSize = probeSize;
        }
    }

    std::uint32_t liveArraySize = 0;
    if (a_menu) {
        auto gBase = reinterpret_cast<std::uintptr_t>(a_menu);
        auto gArr  = *reinterpret_cast<std::uintptr_t*>(gBase + kMarkerArrayOffset);
        liveArraySize = gArr
            ? *reinterpret_cast<std::uint32_t*>(gBase + MarkerCountOffset())
            : 0u;
    }
    if (curWorldID == _computedForWorld && liveArraySize == _computedArraySize) return;

    MapStatsData out;

    int clearableSeen = 0, nullMarker = 0, wrongWorld = 0;
    std::vector<std::string> rejectSamples;
    int rejectTamriel = 0, rejectOther = 0, rejectNullWorld = 0;

    if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
        if (auto* clearKw = RE::TESForm::LookupByID<RE::BGSKeyword>(kLocTypeClearable)) {
            for (auto* loc : dataHandler->GetFormArray<RE::BGSLocation>()) {
                if (!loc || !loc->HasKeyword(clearKw)) continue;
                ++clearableSeen;
                auto markerRef = loc->worldLocMarker.get();
                RE::TESWorldSpace* locWorld = markerRef ? markerRef->GetWorldspace() : nullptr;
                if (!markerRef) ++nullMarker;
                if (!locWorld) {
                    if (!LocationBelongsToWorld(loc, a_currentWorld)) { ++wrongWorld; continue; }
                    ++out.dungeonsTotal;
                    if (loc->IsCleared()) ++out.dungeonsCleared;
                    continue;
                }
                if (!InCurrentWorld(locWorld, a_currentWorld)) {
                    ++wrongWorld;
                    auto* mw = locWorld;
                    const RE::FormID mwID = mw ? mw->GetFormID() : 0;
                    if (mwID == 0)               ++rejectNullWorld;
                    else if (mwID == 0x0000003C) ++rejectTamriel;
                    else                         ++rejectOther;
                    const bool informative =
                        (mwID == curWorldID) || (mwID != 0x0000003C && mwID != 0);
                    if (informative && rejectSamples.size() < 20) {
                        std::string chain;
                        for (auto* w = mw; w; w = w->parentWorld) {
                            chain += std::format("{:08X}", w->GetFormID());
                            if (w->parentWorld) chain += "->";
                        }
                        if (chain.empty()) chain = "<null>";
                        rejectSamples.push_back(std::format(
                            "{}({:08X}) markerWorld={} ",
                            loc->GetFormEditorID() ? loc->GetFormEditorID() : "?",
                            loc->GetFormID(), chain));
                    }
                    continue;
                }
                ++out.dungeonsTotal;
                if (loc->IsCleared()) ++out.dungeonsCleared;
            }
        }
    }

    std::map<std::uint32_t, int> typeHistogram;
    std::uint32_t arrSizeSeen = 0;

    int skipAlias = 0, skipNoHandle = 0, skipLookup = 0,
        skipWorld = 0, skipNoExtra = 0, notDiscovered = 0;

    if (a_menu) {
        auto base   = reinterpret_cast<std::uintptr_t>(a_menu);
        auto arrPtr = *reinterpret_cast<std::uintptr_t*>(base + kMarkerArrayOffset);
        const auto arrSize = arrPtr
            ? *reinterpret_cast<std::uint32_t*>(base + MarkerCountOffset())
            : 0u;
        arrSizeSeen = arrSize;
        for (std::uint32_t i = 0; i < arrSize; ++i) {
            const auto* e = reinterpret_cast<const MarkerEntry*>(
                arrPtr + static_cast<std::uintptr_t>(i) * kMarkerStride);
            if (e->type != 0)    { ++skipAlias;    continue; }
            if (e->handle == 0)  { ++skipNoHandle; continue; }
            RE::NiPointer<RE::TESObjectREFR> refr;
            if (!RE::LookupReferenceByHandle(e->handle, refr) || !refr) { ++skipLookup; continue; }
            if (!InCurrentWorld(refr->GetWorldspace(), a_currentWorld)) { ++skipWorld; continue; }
            auto* extra = refr->extraList.GetByType<RE::ExtraMapMarker>();
            if (!extra || !extra->mapData) { ++skipNoExtra; continue; }
            const bool discovered = (extra->mapData->flags.underlying() & 2) != 0;
            if (discovered) ++out.locationsDiscovered; else ++notDiscovered;
            ++typeHistogram[extra->mapData->type.underlying()];
            if (IsStandingStoneType(extra->mapData->type.underlying())) {
                if (discovered) ++out.stonesFound;
            }
        }
    }

    if (auto* pcell = a_currentWorld ? a_currentWorld->persistentCell : nullptr) {
        pcell->ForEachReference([&](RE::TESObjectREFR* a_refPtr) {
            if (!a_refPtr) return RE::BSContainer::ForEachResult::kContinue;
            auto& a_ref = *a_refPtr;
            auto* extra = a_ref.extraList.GetByType<RE::ExtraMapMarker>();
            if (extra && extra->mapData) {
                ++out.locationsTotal;
                if (IsStandingStoneType(extra->mapData->type.underlying())) {
                    ++out.stonesTotal;
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    out.locationsTotal = std::max(out.locationsTotal, out.locationsDiscovered);
    out.stonesTotal    = std::max(out.stonesTotal, out.stonesFound);

    if (Settings::GetSingleton()->mapStatsLog) {
        logger::info("[MapStats] world={:08X} markerArraySize={}", curWorldID, arrSizeSeen);
        logger::info("[MapStats]   dungeons={}/{}  (clearableSeen={} nullMarker={} wrongWorld={})",
                     out.dungeonsCleared, out.dungeonsTotal, clearableSeen, nullMarker, wrongWorld);
        logger::info("[MapStats]   rejectSplit tamriel={} other={} nullWorld={}",
                     rejectTamriel, rejectOther, rejectNullWorld);
        logger::info("[MapStats]   locations={}/{}  stones={}/{}",
                     out.locationsDiscovered, out.locationsTotal,
                     out.stonesFound, out.stonesTotal);
        std::string hist;
        for (const auto& [type, count] : typeHistogram) {
            hist += std::format("{}:{} ", type, count);
        }
        logger::info("[MapStats]   markerTypes {}", hist);
        logger::info("[MapStats]   walkSplit alias={} noHandle={} lookupFail={} "
                     "wrongWorld={} noExtra={} notDiscovered={} counted={}",
                     skipAlias, skipNoHandle, skipLookup, skipWorld,
                     skipNoExtra, notDiscovered, out.locationsDiscovered);
        for (const auto& r : rejectSamples) {
            logger::info("[MapStats]   reject {}", r);
        }
    }

    std::lock_guard<std::mutex> lk(_mutex);
    out.totalBounty = _data.totalBounty;
    _data = out;
    _computedForWorld  = curWorldID;
    _computedArraySize = liveArraySize;
}
