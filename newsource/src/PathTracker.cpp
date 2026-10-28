#include "PathTracker.h"
#include "MenuUtils.h"
#include "Settings.h"
#include <cmath>

PathTracker* PathTracker::GetSingleton() {
    static PathTracker singleton;
    return &singleton;
}

void PathTracker::ToggleTracking() {
    if (!Settings::GetSingleton()->enablePathTracking) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_isTracking = !m_isTracking;
    logger::info("PathTracking toggled: {}", m_isTracking);

    // Toggle-off is pause, not delete. Fat-fingering T mid-dungeon must not
    // wipe hours of walked trail; the player just resumes on the next T.
}

void PathTracker::Update() {
    if (!Settings::GetSingleton()->enablePathTracking) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isTracking) return;

    // Protect against the engine temporarily nullifying the player's cell
    // during Sleep/Wait or Loading menus, which would incorrectly wipe the path.
    if (MenuUtils::IsBlockingMenuOpen()) return;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    auto* cell = player->GetParentCell();
    if (!cell || cell->IsExteriorCell()) {
        // Courtyard-style mega-dungeons transition through exterior worldspace
        // between wings. Don't nuke m_paths here — the interior trails are
        // keyed per cell FormID and must survive the outdoor gap, resuming
        // where they left off when the player re-enters. Just stop growing
        // the "current" trail; GetSegments/GetPoints(0) fall back to
        // m_currentCellID, so zeroing it makes those return empty while
        // outside instead of replaying the last interior's trail.
        if (m_currentCellID != 0) {
            m_currentCellID = 0;
            logger::debug("PathTracker: Left interior, pausing (paths retained).");
        }
        return;
    }

    RE::FormID cellID = cell->GetFormID();
    if (cellID != m_currentCellID) {
        m_currentCellID = cellID;
        logger::debug("PathTracker: Switched cells. Now tracking in cell {:08X}.", cellID);
    }

    // First visit to this cell this session: track insertion order for the
    // cell cap eviction below.
    if (!m_paths.contains(m_currentCellID)) {
        m_cellOrder.push_back(m_currentCellID);
    }

    RE::NiPoint3 pos = player->GetPosition();
    auto& segments = m_paths[m_currentCellID];

    if (m_paths.size() > kMaxTrackedCells) {
        // Evict the oldest tracked cell that isn't the one we're standing in.
        for (auto it = m_cellOrder.begin(); it != m_cellOrder.end(); ++it) {
            if (*it == m_currentCellID) continue;
            logger::debug("PathTracker: Cell cap ({}) reached, evicting cell {:08X}.", kMaxTrackedCells, *it);
            m_paths.erase(*it);
            m_cellOrder.erase(it);
            break;
        }
    }

    if (segments.empty()) {
        segments.push_back({ pos });
        logger::debug("PathTracker: Started new path in cell {:08X}. Total points: 1", m_currentCellID);
        return;
    }

    const RE::NiPoint3& lastPos = segments.back().back();
    float dx = pos.x - lastPos.x;
    float dy = pos.y - lastPos.y;
    float dz = pos.z - lastPos.z;
    float distance = std::sqrt(dx*dx + dy*dy + dz*dz);

    // 100 Skyrim game units between samples. Decimates the path so we only
    // store a point once the player has actually moved a meaningful distance.
    if (distance > 100.0f) {
        // Dedup-skip: if the player is back within the corridor of an
        // existing trail point, don't append — that ground is already
        // mapped. Nothing is ever deleted (an earlier loop-cut variant
        // truncated the trail on revisit and wiped walked routes), circles
        // add no points, and the count stays bounded by cell geometry
        // instead of time spent walking.
        // 3D distance on purpose: multi-level dungeons stack the same x/y
        // on different floors, z keeps those from deduping each other.
        // The newest point (last segment's back) is exempt: it's what the
        // 100-unit decimation above measures against.
        constexpr float kDedupDist = 130.0f;
        bool onMappedGround = false;
        for (const auto& seg : segments) {
            for (const auto& p : seg) {
                if (&seg == &segments.back() && &p == &seg.back()) continue;
                const float lx = pos.x - p.x;
                const float ly = pos.y - p.y;
                const float lz = pos.z - p.z;
                if (lx * lx + ly * ly + lz * lz < kDedupDist * kDedupDist) {
                    onMappedGround = true;
                    break;
                }
            }
            if (onMappedGround) break;
        }
        if (onMappedGround) {
            // Revisit feedback is transient. The renderer highlights this
            // point briefly instead of adding another breadcrumb.
            m_revisits.push_back({ pos, std::chrono::steady_clock::now() });
            if (m_revisits.size() > kMaxRevisits) m_revisits.pop_front();
        }
        if (!onMappedGround) {
            // Segment break: emerging from mapped ground far from where the
            // trail last grew. Connecting across that gap drew phantom
            // straight runs through walls; start a fresh segment instead.
            constexpr float kBreakDist = 400.0f;
            if (distance > kBreakDist) {
                segments.push_back({ pos });
                logger::debug("PathTracker: New segment ({}) in cell {:08X} (gap {:.1f})",
                              segments.size(), m_currentCellID, distance);
            } else {
                segments.back().push_back(pos);
                const std::size_t n = segments.back().size();
                // Log periodically so we can see how fast points accumulate
                if (n % 10 == 0 || n < 5) {
                    logger::debug("PathTracker: Path growing in cell {:08X}. Segment points: {} (Distance from last: {:.1f})", m_currentCellID, n, distance);
                }
            }
        }
    }
}

std::vector<PathTracker::Segment> PathTracker::GetSegments(RE::FormID cellID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cellID == 0) {
        cellID = m_currentCellID;
    }
    auto it = m_paths.find(cellID);
    if (it != m_paths.end()) {
        return it->second;
    }
    return {};
}

std::vector<RE::NiPoint3> PathTracker::GetPoints(RE::FormID cellID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cellID == 0) {
        cellID = m_currentCellID;
    }
    std::vector<RE::NiPoint3> flat;
    auto it = m_paths.find(cellID);
    if (it != m_paths.end()) {
        for (const auto& seg : it->second) {
            flat.insert(flat.end(), seg.begin(), seg.end());
        }
    }
    return flat;
}

std::vector<RevisitHit> PathTracker::GetRecentRevisits(float maxAgeSec) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    while (!m_revisits.empty() &&
           std::chrono::duration<float>(now - m_revisits.front().when).count() > maxAgeSec) {
        m_revisits.pop_front();
    }
    return { m_revisits.begin(), m_revisits.end() };
}

// ==========================================
// SERIALIZATION
// ==========================================

void PathTracker::OnGameSave(SKSE::SerializationInterface* serde) {
    if (!serde->OpenRecord(kSerializationSignature, kSerializationVersion)) {
        logger::error("PathTracker: Failed to open PTRK record for serialization!");
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    std::uint8_t tracking = m_isTracking ? 1 : 0;
    serde->WriteRecordData(&tracking, sizeof(tracking));

    std::uint32_t cellCount = static_cast<std::uint32_t>(m_paths.size());
    serde->WriteRecordData(&cellCount, sizeof(cellCount));

    for (const auto& [cellID, segments] : m_paths) {
        serde->WriteRecordData(&cellID, sizeof(cellID));

        std::uint32_t segCount = static_cast<std::uint32_t>(segments.size());
        serde->WriteRecordData(&segCount, sizeof(segCount));

        for (const auto& seg : segments) {
            std::uint32_t ptCount = static_cast<std::uint32_t>(seg.size());
            serde->WriteRecordData(&ptCount, sizeof(ptCount));
            if (ptCount > 0) {
                serde->WriteRecordData(seg.data(), ptCount * sizeof(RE::NiPoint3));
            }
        }
    }

    logger::info("PathTracker: Saved {} tracked cell(s), tracking={}.", cellCount, m_isTracking);
}

void PathTracker::LoadRecord(SKSE::SerializationInterface* serde, std::uint32_t version, std::uint32_t /*length*/) {
    if (version < 1) {
        logger::warn("PathTracker: Unsupported PTRK record version {}.", version);
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_paths.clear();
    m_cellOrder.clear();

    std::uint8_t tracking = 0;
    serde->ReadRecordData(&tracking, sizeof(tracking));

    std::uint32_t cellCount = 0;
    serde->ReadRecordData(&cellCount, sizeof(cellCount));
    if (cellCount > 100000) {
        logger::warn("PathTracker: PTRK cell count {} looks corrupt, aborting load.", cellCount);
        return;
    }

    for (std::uint32_t i = 0; i < cellCount; ++i) {
        RE::FormID savedCellID = 0;
        serde->ReadRecordData(&savedCellID, sizeof(savedCellID));

        // Cell FormIDs can shift between the save's load order and the
        // current one (light plugin renumbering etc.); resolve like
        // MarkerManager does elsewhere. Every segment/point still has to be
        // read regardless of resolution so the record stream stays aligned
        // for whatever follows.
        RE::FormID resolvedCellID = 0;
        bool resolved = serde->ResolveFormID(savedCellID, resolvedCellID);

        std::uint32_t segCount = 0;
        serde->ReadRecordData(&segCount, sizeof(segCount));
        if (segCount > 100000) {
            logger::warn("PathTracker: PTRK segment count {} looks corrupt, aborting load.", segCount);
            return;
        }

        std::vector<Segment> segments;
        segments.reserve(segCount);
        for (std::uint32_t s = 0; s < segCount; ++s) {
            std::uint32_t ptCount = 0;
            serde->ReadRecordData(&ptCount, sizeof(ptCount));
            if (ptCount > 100000) {
                logger::warn("PathTracker: PTRK point count {} looks corrupt, aborting load.", ptCount);
                return;
            }

            Segment seg;
            if (ptCount > 0) {
                seg.resize(ptCount);
                serde->ReadRecordData(seg.data(), ptCount * sizeof(RE::NiPoint3));
            }
            segments.push_back(std::move(seg));
        }

        if (resolved && resolvedCellID != 0) {
            m_paths[resolvedCellID] = std::move(segments);
            m_cellOrder.push_back(resolvedCellID);
        }
    }

    m_isTracking = (tracking != 0);
    logger::info("PathTracker: Loaded {} tracked cell(s), tracking={}.", m_paths.size(), m_isTracking);
}

void PathTracker::OnRevert() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_paths.clear();
    m_cellOrder.clear();
    m_revisits.clear();
    m_currentCellID = 0;
    m_isTracking = false;
    logger::debug("PathTracker: Reverted.");
}
