#include "MapStats.h"
#include "HoldBounty.h"
#include "Settings.h"

#include <map>

// ============================================================================
// MapStats — worldspace-scoped completionist stats (Map Progress panel row)
// ----------------------------------------------------------------------------
// STATUS: shipped 2026-07-15 per findings/spec-findings-Map-Stats.md. Two
//   enumeration sources, on purpose (Decision 4 in the finding): dungeons
//   cleared/total come from GetFormArray<BGSLocation>() gated on
//   LocTypeClearable + IsCleared() (cleared is a location fact); locations
//   DISCOVERED and standing stones FOUND come from the vanilla marker-array
//   walk (MapMenu+0x30470) the hover-card takeover uses (discovered is a
//   marker fact, flags & 2 = kCanTravelTo, the known misnomer already used
//   everywhere else in this plugin — not fixed here). Their TOTALS come from
//   the worldspace persistentCell instead; see the note further down.
//   Bounty is player state (crime gold), NOT worldspace-scoped; summed once
//   over the shared HoldBounty::kHoldCrimeFactions table.
//   Refresh() is cache-gated on _computedForWorld + _computedArraySize, and
//   that check is the FIRST thing it does, so DrawListPanel can call it every
//   frame for free. It re-walks on a map switch (Tamriel -> Solstheim) OR when
//   the marker array grows, which is how a discovery made WITHOUT leaving the
//   map reaches the counts.
//   Locations/stones NUMERATORS come from the marker array; their TOTALS come
//   from the worldspace persistentCell, because that array is discovery-gated
//   and using it for both halves makes the row read N/N forever.
//   MarkerEntry here is a trimmed VIEW of the same struct HoverImageRenderer
//   uses (stride 0x38, verified via Ghidra decompile of the card-population
//   function; static_assert would read 0x30 because of the unused trailing
//   questObj field, same trap noted there). Only namePtr/handle/type are read;
//   nothing here needs the name/nextIndex/questObj fields the hover card uses.
// ============================================================================

namespace {
    // LocTypeClearable keyword (Skyrim.esm KYWD), confirmed via forms MCP.
    constexpr RE::FormID kLocTypeClearable = 0x000F5E80;
    // Standing-stone MARKER_TYPEs. Deliberately a SET, not one id: Skyrim's
    // Doomstones are kDoomstone (18) but Solstheim's All-Maker Stones are
    // kDLC02StandingStone (55), so a single-id test read 0/0 on Solstheim while
    // the map plainly showed five stones (measured 2026-08-26: the marker-type
    // histogram logged `55:5` there and `18:11` on Tamriel).
    // MarkerFilterHook already buckets BOTH into FocusMode::kLandmarks, so the
    // Legend counted them while these stats did not -- two files disagreeing on
    // what a standing stone is. Keep this list and that case list in step.
    constexpr std::uint16_t kStandingStoneTypes[] = { 18, 55 };

    bool IsStandingStoneType(std::uint16_t a_type) {
        for (const auto t : kStandingStoneTypes) {
            if (t == a_type) return true;
        }
        return false;
    }
    constexpr std::uintptr_t kMarkerArrayOffset = 0x30470;
    constexpr std::size_t    kMarkerStride      = 0x38;

    // Marker-array BSTArray count field. AE +0x30478, SE +0x30480. Same offset
    // pair as HoverImageRenderer::MarkerArrayCountOffset(); duplicated rather
    // than shared because it's a single REL::Relocate call, not worth a header
    // for two call sites (see findings-SE-MapMenu-Offsets.md).
    inline std::ptrdiff_t MarkerCountOffset() {
        return REL::Relocate<std::ptrdiff_t>(0x30480, 0x30478);
    }

    // Trimmed view of one MapMenu+0x30470 entry. Only the fields this walk
    // needs; layout must match HoverImageRenderer.cpp's MarkerEntry up to
    // +0x18 (type) or this reads garbage. Stride is 0x38, NOT sizeof(this).
    struct MarkerEntry {
        void*             namePtr;   // +0x00 (unused here)
        RE::RefHandle     handle;    // +0x08
        std::uint32_t     pad0C;     // +0x0C
        RE::BSFixedString  nameStr;  // +0x10 (unused here)
        std::uint32_t     type;      // +0x18  0 = base MapMarker, 1 = stacked alias
    };

    // Same parent-world walk as MapMarkerRenderer::DrawListPanel's
    // belongsToMap lambda. Copied, not shared: it's four lines and the two
    // call sites (list panel filtering vs stats scoping) have different
    // fallback-on-null-world semantics worth keeping separate.
    // Handle-free scoping fallback. A worldspace carries its own root location
    // (TESWorldSpace::location, XLCN at +0x228); every location in that world
    // chains up to it through parentLoc. Used only when worldLocMarker did not
    // resolve, which is the normal case for a worldspace that is not loaded.
    bool LocationBelongsToWorld(RE::BGSLocation* a_loc, RE::TESWorldSpace* a_cur) {
        if (!a_cur) return true;          // no map worldspace: same fallback as InCurrentWorld
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

// Bounty is live PLAYER crime gold; it changes every time you commit a crime,
// with the map still open on the same worldspace. It must NOT sit behind the
// world-changed guard or the panel goes stale (owe 40, still shows 0). Cheap:
// 9 form lookups, safe to run every call.
static std::uint32_t ComputeTotalBounty() {
    // Enumerate ALL crime factions instead of a hardcoded hold table. The
    // faction itself carries the player's outstanding bounty (GetCrimeGold*),
    // so we don't need the hold->faction map here at all. TracksCrimes() is
    // true only for crime factions. This auto-covers vanilla, DLC (Raven
    // Rock), AND mod-added holds/crime factions with zero maintenance. The
    // hold->faction table in HoldBounty.h stays for the PER-MARKER hover card
    // (which starts from a hold LCTN and needs the faction key); the TOTAL
    // does not, so it reads the complete truth.
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) return 0;

    std::int64_t total = 0;
    for (auto* fac : dh->GetFormArray<RE::TESFaction>()) {
        if (!fac || !fac->TracksCrimes()) continue;
        const std::int32_t gold = fac->GetCrimeGold();  // violent + non-violent
        if (gold > 0) total += gold;
    }
    return static_cast<std::uint32_t>(total);
}

void MapStats::Refresh(RE::MapMenu* a_menu, RE::TESWorldSpace* a_currentWorld) {
    const RE::FormID curWorldID = a_currentWorld ? a_currentWorld->GetFormID() : 0;

    // Bounty recomputes every call (live player state, not world-scoped).
    // Cheap enough for the per-frame draw path.
    {
        const std::uint32_t bounty = ComputeTotalBounty();
        std::lock_guard<std::mutex> lk(_mutex);
        _data.totalBounty = bounty;
    }

    // Marker-array settling probe. Sits BEFORE the world-changed guard so it
    // samples every frame, which the cached walk below cannot do. Wyrmstooth
    // logged Locations 3/3 off markerArraySize=8 while its Legend showed 46
    // markers, so the question is whether the engine array is still filling
    // when the first (and only) walk snapshots it. Logs on CHANGE plus the
    // first few frames per worldspace, not every frame.
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

    // Recompute when the worldspace changes OR when the marker array grows.
    // The array is discovery-gated, so finding a new location while STAYING on
    // the same map grows it -- and a pure worldspace guard froze the numbers
    // until the player switched maps and came back (measured 2026-08-26: a 5th
    // Wyrmstooth discovery still showed 4/4). The array walk is tens of entries
    // and only reruns when the size actually moved, so this stays cheap.
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

    // --- Dungeons: BGSLocation forms with LocTypeClearable. BGSLocation has no
    //     worldspace field of its own, so scoping goes through the marker when
    //     its handle resolves and through the parentLoc chain when it does not.
    // Diagnostic tallies, kept after the fix as regression canaries:
    // nullMarker is now informational (an unresolved handle takes the fallback
    // path instead of dropping the location), and wrongWorld should stay near
    // the count of locations genuinely belonging to other worldspaces.
    int clearableSeen = 0, nullMarker = 0, wrongWorld = 0;
    std::vector<std::string> rejectSamples;
    int rejectTamriel = 0, rejectOther = 0, rejectNullWorld = 0;

    if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
        if (auto* clearKw = RE::TESForm::LookupByID<RE::BGSKeyword>(kLocTypeClearable)) {
            for (auto* loc : dataHandler->GetFormArray<RE::BGSLocation>()) {
                if (!loc || !loc->HasKeyword(clearKw)) continue;
                ++clearableSeen;

                // Scope the location to the current map WITHOUT depending on a
                // runtime handle. worldLocMarker is an ObjectRefHandle that only
                // resolves while its own worldspace is loaded, so on Solstheim
                // every Solstheim location returned null and was dropped from the
                // TOTAL, not just from the cleared count -- the panel read 0/0 on
                // a map with 35 clearable locations (measured 2026-08-26:
                // nullMarker=71 constant across all three maps, rejectSplit
                // other=0 proving the 35 never reached the worldspace test).
                // The marker's worldspace is static record data, so prefer the
                // resolved ref when it IS loaded and fall back to the location's
                // own parent chain against the worldspace's XLCN location.
                auto markerRef = loc->worldLocMarker.get();  // NiPointer, not raw
                RE::TESWorldSpace* locWorld = markerRef ? markerRef->GetWorldspace() : nullptr;
                if (!markerRef) ++nullMarker;   // diagnostic only; no longer a skip

                if (!locWorld) {
                    // Handle did not resolve. Walk the location's parent chain and
                    // match against the current worldspace's own location record.
                    if (!LocationBelongsToWorld(loc, a_currentWorld)) { ++wrongWorld; continue; }
                    ++out.dungeonsTotal;
                    if (loc->IsCleared()) ++out.dungeonsCleared;
                    continue;
                }

                if (!InCurrentWorld(locWorld, a_currentWorld)) {
                    ++wrongWorld;
                    // Sample the first few rejects with the marker's ACTUAL
                    // worldspace and its parent chain. wrongWorld=196 on both
                    // Solstheim and Wyrmstooth (vs 1 on Tamriel) means the walk
                    // rejects locations against their OWN map, so the question
                    // is what GetWorldspace() returns there -- null, Tamriel, or
                    // a child world whose parent chain does not reach the map.
                    // Sampling every reject is useless: the first 12 are always
                    // vanilla Tamriel locations correctly rejected on a DLC map.
                    // Only two cases are informative, so filter to those:
                    //   (a) the marker's world IS the current map -- a location
                    //       rejected against its own worldspace, the actual bug;
                    //   (b) the marker's world is neither the map nor Tamriel --
                    //       i.e. a DLC/mod location landing somewhere unexpected.
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

    // --- Locations + standing stones: vanilla marker array walk. Reuses the
    //     same MapMenu+0x30470 array the hover-card takeover reads.
    // MARKER_TYPE -> count for the whole current worldspace. The stones counter
    // matches a single hardcoded type, so seeing which types a map ACTUALLY
    // uses is the only way to know what it should match instead.
    std::map<std::uint32_t, int> typeHistogram;
    std::uint32_t arrSizeSeen = 0;

    // Attrition counters for the marker walk. Measured 2026-08-26: after a
    // `tmm 1` on Wyrmstooth the array grew 8 -> 64 while the count stayed at
    // 5/50, so 59 entries are being dropped somewhere in this loop and the
    // question is WHICH skip eats them.
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
            // Stacked City/Quest aliases (type==1) share the base entry's
            // handle; skip them here so each marker is only counted once.
            if (e->type != 0)    { ++skipAlias;    continue; }
            if (e->handle == 0)  { ++skipNoHandle; continue; }

            RE::NiPointer<RE::TESObjectREFR> refr;
            if (!RE::LookupReferenceByHandle(e->handle, refr) || !refr) { ++skipLookup; continue; }
            if (!InCurrentWorld(refr->GetWorldspace(), a_currentWorld)) { ++skipWorld; continue; }

            auto* extra = refr->extraList.GetByType<RE::ExtraMapMarker>();
            if (!extra || !extra->mapData) { ++skipNoExtra; continue; }

            // flags & 2 = kCanTravelTo, the project-wide "discovered" misnomer
            // (CLAUDE.md, HoverImageRenderer.cpp:557). Stay consistent.
            const bool discovered = (extra->mapData->flags.underlying() & 2) != 0;
            // NUMERATOR ONLY. The totals come from the persistentCell walk
            // below: this array is DISCOVERY-GATED, so counting it for both
            // halves makes numerator and denominator the same set and the row
            // becomes a tautology that can only ever read N/N. Measured
            // 2026-08-26: Wyrmstooth read 4/4 with 46 markers on the island,
            // and Tamriel's healthy-looking 346/346 was the same bug hidden by
            // a `tmm 1` that had discovered everything.
            if (discovered) ++out.locationsDiscovered; else ++notDiscovered;

            ++typeHistogram[extra->mapData->type.underlying()];

            if (IsStandingStoneType(extra->mapData->type.underlying())) {
                if (discovered) ++out.stonesFound;
            }
        }
    }

    // Denominators from static record data, independent of discovery state.
    // Map markers are PERSISTENT refs, so they live in the worldspace's
    // persistentCell (+0x088) rather than in any loaded exterior cell -- which
    // is exactly what keeps this total from changing as the player walks around.
    // One pass yields both the locations and the stones total.
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
    // A discovered marker must never exceed the static total. If the walk came
    // up short (no persistentCell, say), clamp rather than render a nonsense
    // fraction like 46/0.
    out.locationsTotal = std::max(out.locationsTotal, out.locationsDiscovered);
    out.stonesTotal    = std::max(out.stonesTotal, out.stonesFound);

    // Fires on worldspace change or marker-array growth, so this is a handful
    // of lines per map open, not spam.
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
        // Where the array entries went. These must sum to arraySize.
        logger::info("[MapStats]   walkSplit alias={} noHandle={} lookupFail={} "
                     "wrongWorld={} noExtra={} notDiscovered={} counted={}",
                     skipAlias, skipNoHandle, skipLookup, skipWorld,
                     skipNoExtra, notDiscovered, out.locationsDiscovered);
        for (const auto& r : rejectSamples) {
            logger::info("[MapStats]   reject {}", r);
        }
    }

    std::lock_guard<std::mutex> lk(_mutex);
    out.totalBounty = _data.totalBounty;  // keep the bounty computed above; out clobbers _data
    _data = out;
    _computedForWorld  = curWorldID;
    _computedArraySize = liveArraySize;
}
