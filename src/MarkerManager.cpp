#include "MarkerManager.h"
#include "LocalizationManager.h"
#include "MapMarkerRenderer.h"
#include "PathTracker.h"
#include <chrono>
#include <cmath>
#include <memory>

// Unique signature for the save file (4 bytes)
// "CMMK" = Custom MarKers or Custom Map Markers
const std::uint32_t kSerializationSignature = 'CMMK';
const std::uint32_t kSerializationVersion = 8;

namespace {
    constexpr std::uint32_t kMaxSerializedEntries = 10'000;
    constexpr std::uint32_t kMaxSerializedStringBytes = 4'096;

    class RecordReader {
    public:
        RecordReader(SKSE::SerializationInterface* a_serde, std::uint32_t a_length) :
            serde(a_serde), remaining(a_length) {}

        template <class T>
        bool Read(T& a_value) {
            return ReadBytes(std::addressof(a_value), sizeof(T));
        }

        bool ReadBytes(void* a_dst, std::uint32_t a_size) {
            if (!serde || a_size > remaining) return false;
            const std::uint32_t read = serde->ReadRecordData(a_dst, a_size);
            if (read > remaining) return false;
            remaining -= read;
            return read == a_size;
        }

        bool ReadString(std::string& a_out) {
            std::uint32_t size = 0;
            if (!Read(size) || size > kMaxSerializedStringBytes || size > remaining) return false;
            a_out.resize(size);
            return size == 0 || ReadBytes(a_out.data(), size);
        }

        std::uint32_t Remaining() const { return remaining; }

    private:
        SKSE::SerializationInterface* serde;
        std::uint32_t remaining;
    };

    struct LoadedMarkerState {
        std::vector<MapMarker> markers;
        std::vector<FavoriteMarker> favorites;
        std::unordered_map<RE::FormID, std::string> vanillaNotes;
        VanillaDestination vanillaDest{};
        std::uint32_t savedNextID = 1;
    };

    template <class T>
    bool WriteChecked(SKSE::SerializationInterface* a_serde, const T& a_value) {
        return a_serde && a_serde->WriteRecordData(std::addressof(a_value), sizeof(T));
    }

    bool WriteStringChecked(SKSE::SerializationInterface* a_serde, const std::string& a_str) {
        if (!a_serde) return false;
        const std::uint32_t size = static_cast<std::uint32_t>(a_str.length());
        if (!WriteChecked(a_serde, size)) return false;
        return size == 0 || a_serde->WriteRecordData(a_str.data(), size);
    }

    bool ResolveNonzeroFormID(SKSE::SerializationInterface* a_serde,
                              RE::FormID a_saved,
                              RE::FormID& a_resolved) {
        if (a_saved == 0) {
            a_resolved = 0;
            return false;
        }
        RE::FormID resolved = 0;
        if (a_serde && a_serde->ResolveFormID(a_saved, resolved) && resolved != 0) {
            a_resolved = resolved;
            return true;
        }
        a_resolved = 0;
        return false;
    }

    std::uint32_t RecoverNextIDHint(std::uint32_t a_saved,
                                    const std::vector<MapMarker>& a_markers) {
        std::uint32_t candidate = a_saved;
        if (candidate == 0 || candidate >= kVanillaDestId) {
            candidate = 1;
        }
        const std::uint32_t maxAttempts = static_cast<std::uint32_t>(a_markers.size()) + 2;
        for (std::uint32_t attempts = 0; attempts < maxAttempts; ++attempts) {
            if (candidate == 0 || candidate >= kVanillaDestId) {
                candidate = 1;
            }
            bool occupied = false;
            for (const auto& m : a_markers) {
                if (m.id == candidate) {
                    occupied = true;
                    break;
                }
            }
            if (!occupied) {
                return candidate;
            }
            candidate++;
        }
        return 1;
    }

    void NormalizeDestinationState(LoadedMarkerState& a_loaded) {
        if (a_loaded.vanillaDest.active) {
            for (auto& m : a_loaded.markers) {
                m.isDestination = false;
            }
        } else {
            bool foundFirstDest = false;
            for (auto& m : a_loaded.markers) {
                if (m.isDestination) {
                    if (!foundFirstDest) {
                        foundFirstDest = true;
                    } else {
                        m.isDestination = false;
                    }
                }
            }
        }
    }

    bool ReadCMMKRecord(SKSE::SerializationInterface* a_serde,
                        std::uint32_t a_version,
                        std::uint32_t a_length,
                        LoadedMarkerState& a_out) {
        RecordReader reader(a_serde, a_length);

        // 1. Read Marker Count
        std::uint32_t markerCount = 0;
        if (!reader.Read(markerCount)) {
            logger::error("CMMK v{}: failed to read marker count (length {})", a_version, a_length);
            return false;
        }

        const std::uint32_t minMarkerSize = (a_version >= 8) ? 47 :
                                            ((a_version >= 5) ? 46 : ((a_version >= 2) ? 41 : 40));
        std::uint32_t mandatoryTrailing = 4; // _nextID
        if (a_version >= 3) mandatoryTrailing += 4; // favCount
        if (a_version >= 4) mandatoryTrailing += 4; // noteCount
        if (a_version >= 6) mandatoryTrailing += 1; // destPresent

        if (markerCount > kMaxSerializedEntries ||
            reader.Remaining() < mandatoryTrailing ||
            (reader.Remaining() - mandatoryTrailing) / minMarkerSize < markerCount) {
            logger::error("CMMK v{}: invalid marker count {} (remaining bytes {})",
                          a_version, markerCount, reader.Remaining());
            return false;
        }

        a_out.markers.reserve(markerCount);
        std::uint32_t discardedMarkers = 0;

        // 2. Read Markers
        for (std::uint32_t i = 0; i < markerCount; ++i) {
            std::uint32_t id = 0;
            RE::FormID worldSpaceID = 0;
            RE::FormID cellID = 0;
            RE::NiPoint3 position{};
            std::uint32_t iconType = 0;
            std::uint64_t timestamp = 0;
            std::string note;
            std::uint8_t destByte = 0;
            std::uint8_t autoByte = 0;
            std::uint32_t autoNameNumber = 0;
            std::uint8_t originByte = static_cast<std::uint8_t>(MarkerOrigin::Manual);

            if (!reader.Read(id) ||
                !reader.Read(worldSpaceID) ||
                !reader.Read(cellID) ||
                !reader.Read(position) ||
                !reader.Read(iconType) ||
                !reader.Read(timestamp) ||
                !reader.ReadString(note)) {
                logger::error("CMMK v{}: truncated base marker {} of {}", a_version, i, markerCount);
                return false;
            }

            if (a_version >= 2) {
                if (!reader.Read(destByte)) {
                    logger::error("CMMK v{}: truncated dest byte on marker {} of {}", a_version, i, markerCount);
                    return false;
                }
            }

            if (a_version >= 5) {
                if (!reader.Read(autoByte) || !reader.Read(autoNameNumber)) {
                    logger::error("CMMK v{}: truncated autoName on marker {} of {}", a_version, i, markerCount);
                    return false;
                }
            }

            if (a_version >= 8) {
                if (!reader.Read(originByte)) {
                    logger::error("CMMK v{}: truncated origin on marker {} of {}", a_version, i, markerCount);
                    return false;
                }
            } else if (autoByte == static_cast<std::uint8_t>(AutoName::Pinned)) {
                originByte = static_cast<std::uint8_t>(MarkerOrigin::LookAndPin);
            } else if (autoByte == static_cast<std::uint8_t>(AutoName::QuickMark)) {
                originByte = static_cast<std::uint8_t>(MarkerOrigin::InspectQuickMark);
            }

            // Entry validation
            bool entryValid = true;

            if (destByte > 1) {
                logger::warn("Discarding marker {}: invalid destByte {}", id, destByte);
                entryValid = false;
            }

            if (autoByte > static_cast<std::uint8_t>(AutoName::QuickMark)) {
                logger::warn("Discarding marker {}: invalid autoName {}", id, autoByte);
                entryValid = false;
            }

            if (originByte > static_cast<std::uint8_t>(MarkerOrigin::InspectQuickMark)) {
                logger::warn("Discarding marker {}: invalid origin {}", id, originByte);
                entryValid = false;
            }

            if (id == 0 || id == kVanillaDestId) {
                logger::warn("Discarding marker: reserved or zero id {:08X}", id);
                entryValid = false;
            }

            for (const auto& existing : a_out.markers) {
                if (existing.id == id) {
                    logger::warn("Discarding marker: duplicate id {}", id);
                    entryValid = false;
                    break;
                }
            }

            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
                logger::warn("Discarding marker {}: non-finite position ({}, {}, {})", id, position.x, position.y, position.z);
                entryValid = false;
            }

            RE::FormID resolvedWS = 0;
            RE::FormID resolvedCell = 0;
            if (entryValid) {
                if (worldSpaceID != 0) {
                    if (!ResolveNonzeroFormID(a_serde, worldSpaceID, resolvedWS)) {
                        logger::warn("Discarding exterior marker {}: unresolvable worldSpaceID {:08X}", id, worldSpaceID);
                        entryValid = false;
                    } else if (cellID != 0) {
                        if (!ResolveNonzeroFormID(a_serde, cellID, resolvedCell)) {
                            resolvedCell = 0;
                        }
                    }
                } else {
                    if (!ResolveNonzeroFormID(a_serde, cellID, resolvedCell)) {
                        logger::warn("Discarding interior marker {}: unresolvable cellID {:08X}", id, cellID);
                        entryValid = false;
                    }
                }
            }

            if (!entryValid) {
                discardedMarkers++;
            } else {
                MapMarker m;
                m.id = id;
                m.worldSpaceID = resolvedWS;
                m.cellID = resolvedCell;
                m.position = position;
                m.note = std::move(note);
                m.iconType = iconType;
                m.timestamp = timestamp;
                m.isDestination = (destByte != 0);
                m.autoName = static_cast<AutoName>(autoByte);
                m.autoNameNumber = autoNameNumber;
                m.origin = static_cast<MarkerOrigin>(originByte);
                a_out.markers.push_back(std::move(m));
            }
        }

        // 3. Read Next ID
        if (!reader.Read(a_out.savedNextID)) {
            logger::error("CMMK v{}: failed to read saved _nextID", a_version);
            return false;
        }

        // 4. Read Favorites (v3+)
        std::uint32_t discardedFavorites = 0;
        if (a_version >= 3) {
            std::uint32_t favCount = 0;
            if (!reader.Read(favCount)) {
                logger::error("CMMK v{}: failed to read favorites count", a_version);
                return false;
            }
            constexpr std::uint32_t minFavSize = 28;
            std::uint32_t mandatoryAfterFav = 0;
            if (a_version >= 4) mandatoryAfterFav += 4; // noteCount
            if (a_version >= 6) mandatoryAfterFav += 1; // destPresent

            if (favCount > kMaxSerializedEntries ||
                reader.Remaining() < mandatoryAfterFav ||
                (reader.Remaining() - mandatoryAfterFav) / minFavSize < favCount) {
                logger::error("CMMK v{}: invalid favorites count {} (remaining bytes {})",
                              a_version, favCount, reader.Remaining());
                return false;
            }

            a_out.favorites.reserve(favCount);
            for (std::uint32_t i = 0; i < favCount; ++i) {
                RE::FormID formID = 0;
                RE::FormID worldSpaceID = 0;
                RE::FormID cellID = 0;
                RE::NiPoint3 position{};
                std::string name;

                if (!reader.Read(formID) ||
                    !reader.Read(worldSpaceID) ||
                    !reader.Read(cellID) ||
                    !reader.Read(position) ||
                    !reader.ReadString(name)) {
                    logger::error("CMMK v{}: truncated favorite {} of {}", a_version, i, favCount);
                    return false;
                }

                bool favValid = true;
                if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
                    logger::warn("Discarding favorite {:08X}: non-finite position", formID);
                    favValid = false;
                }

                RE::FormID resolvedFormID = 0;
                if (favValid && !ResolveNonzeroFormID(a_serde, formID, resolvedFormID)) {
                    logger::warn("Discarding favorite: unresolvable formID {:08X}", formID);
                    favValid = false;
                }

                RE::FormID resolvedWS = 0;
                RE::FormID resolvedCell = 0;
                if (favValid) {
                    if (worldSpaceID != 0) {
                        if (!ResolveNonzeroFormID(a_serde, worldSpaceID, resolvedWS)) {
                            logger::warn("Discarding exterior favorite {:08X}: unresolvable worldSpaceID {:08X}",
                                         formID, worldSpaceID);
                            favValid = false;
                        } else if (cellID != 0) {
                            if (!ResolveNonzeroFormID(a_serde, cellID, resolvedCell)) {
                                resolvedCell = 0;
                            }
                        }
                    } else {
                        if (!ResolveNonzeroFormID(a_serde, cellID, resolvedCell)) {
                            logger::warn("Discarding interior favorite {:08X}: unresolvable cellID {:08X}",
                                         formID, cellID);
                            favValid = false;
                        }
                    }
                }

                if (!favValid) {
                    discardedFavorites++;
                } else {
                    FavoriteMarker fav;
                    fav.formID = resolvedFormID;
                    fav.worldSpaceID = resolvedWS;
                    fav.cellID = resolvedCell;
                    fav.position = position;
                    fav.name = std::move(name);
                    a_out.favorites.push_back(std::move(fav));
                }
            }
        }

        // 5. Read Vanilla-marker notes (v4+)
        std::uint32_t discardedNotes = 0;
        if (a_version >= 4) {
            std::uint32_t noteCount = 0;
            if (!reader.Read(noteCount)) {
                logger::error("CMMK v{}: failed to read note count", a_version);
                return false;
            }
            constexpr std::uint32_t minNoteSize = 8;
            std::uint32_t mandatoryAfterNotes = 0;
            if (a_version >= 6) mandatoryAfterNotes += 1; // destPresent

            if (noteCount > kMaxSerializedEntries ||
                reader.Remaining() < mandatoryAfterNotes ||
                (reader.Remaining() - mandatoryAfterNotes) / minNoteSize < noteCount) {
                logger::error("CMMK v{}: invalid note count {} (remaining bytes {})",
                              a_version, noteCount, reader.Remaining());
                return false;
            }

            a_out.vanillaNotes.reserve(noteCount);
            for (std::uint32_t i = 0; i < noteCount; ++i) {
                RE::FormID formID = 0;
                std::string note;
                if (!reader.Read(formID) || !reader.ReadString(note)) {
                    logger::error("CMMK v{}: truncated note {} of {}", a_version, i, noteCount);
                    return false;
                }

                RE::FormID resolvedFormID = 0;
                if (!ResolveNonzeroFormID(a_serde, formID, resolvedFormID) || note.empty()) {
                    logger::warn("Discarding vanilla note for FormID {:08X}", formID);
                    discardedNotes++;
                } else {
                    a_out.vanillaNotes[resolvedFormID] = std::move(note);
                }
            }
        }

        // 6. Read Vanilla Destination (v6+)
        bool discardedDest = false;
        if (a_version >= 6) {
            std::uint8_t destPresent = 0;
            if (!reader.Read(destPresent)) {
                logger::error("CMMK v{}: failed to read destPresent byte", a_version);
                return false;
            }

            if (destPresent != 0 && destPresent != 1) {
                logger::error("CMMK v{}: invalid destPresent byte value {} (must be 0 or 1)", a_version, destPresent);
                return false; // Structural error
            }

            if (destPresent == 1) {
                RE::FormID worldSpaceID = 0;
                RE::FormID cellID = 0;
                RE::NiPoint3 position{};
                std::string name;
                RE::FormID sourceFormID = 0;
                std::uint8_t fromLocalDoor = 0;

                if (!reader.Read(worldSpaceID) ||
                    !reader.Read(cellID) ||
                    !reader.Read(position) ||
                    !reader.ReadString(name)) {
                    logger::error("CMMK v{}: truncated vanilla destination prefix", a_version);
                    return false;
                }

                if (a_version >= 7) {
                    if (!reader.Read(sourceFormID) || !reader.Read(fromLocalDoor)) {
                        logger::error("CMMK v{}: truncated vanilla destination v7 suffix", a_version);
                        return false;
                    }
                }

                bool destValid = true;
                if (a_version >= 7 && fromLocalDoor > 1) {
                    logger::warn("Discarding vanilla destination: invalid fromLocalDoor byte {}", fromLocalDoor);
                    destValid = false;
                }

                if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
                    logger::warn("Discarding vanilla destination: non-finite position");
                    destValid = false;
                }

                RE::FormID resolvedSource = 0;
                if (destValid && sourceFormID != 0) {
                    if (!ResolveNonzeroFormID(a_serde, sourceFormID, resolvedSource)) {
                        logger::warn("Discarding vanilla destination: unresolvable sourceFormID {:08X}", sourceFormID);
                        destValid = false;
                    }
                }

                RE::FormID resolvedWS = 0;
                RE::FormID resolvedCell = 0;
                if (destValid) {
                    if (worldSpaceID != 0) {
                        if (!ResolveNonzeroFormID(a_serde, worldSpaceID, resolvedWS)) {
                            logger::warn("Discarding exterior vanilla destination: unresolvable worldSpaceID {:08X}", worldSpaceID);
                            destValid = false;
                        } else if (cellID != 0) {
                            if (!ResolveNonzeroFormID(a_serde, cellID, resolvedCell)) {
                                resolvedCell = 0;
                            }
                        }
                    } else {
                        if (!ResolveNonzeroFormID(a_serde, cellID, resolvedCell)) {
                            logger::warn("Discarding interior vanilla destination: unresolvable cellID {:08X}", cellID);
                            destValid = false;
                        }
                    }
                }

                if (!destValid) {
                    discardedDest = true;
                } else {
                    a_out.vanillaDest.worldSpaceID = resolvedWS;
                    a_out.vanillaDest.cellID = resolvedCell;
                    a_out.vanillaDest.position = position;
                    a_out.vanillaDest.name = std::move(name);
                    a_out.vanillaDest.sourceFormID = resolvedSource;
                    a_out.vanillaDest.active = true;
                    a_out.vanillaDest.fromLocalDoor = (fromLocalDoor != 0);
                }
            }
        }

        // Check unconsumed trailing bytes
        if (reader.Remaining() != 0) {
            logger::error("CMMK v{} record had {} unconsumed trailing bytes (declared length {})",
                          a_version, reader.Remaining(), a_length);
            return false;
        }

        logger::info("Loaded CMMK v{} record: {} markers ({} discarded), {} favorites ({} discarded), {} notes ({} discarded), vanilla dest active={} ({} discarded). Next ID hint: {}",
                     a_version, a_out.markers.size(), discardedMarkers,
                     a_out.favorites.size(), discardedFavorites,
                     a_out.vanillaNotes.size(), discardedNotes,
                     a_out.vanillaDest.active, discardedDest ? 1 : 0,
                     a_out.savedNextID);
        return true;
    }
}

// Auto-name pattern per scheme. The VALUE carries the placeholder so a
// translator can move it; a value with no "{}" just prints without the number.
static const char* AutoNameKey(AutoName scheme) {
    switch (scheme) {
        case AutoName::Custom:    return "Labels.AutoCustomMarker";
        case AutoName::Pinned:    return "Labels.AutoPinnedLocation";
        case AutoName::QuickMark: return "Labels.AutoQuickMark";
        default:                  return "";
    }
}

std::string ResolveMarkerName(const MapMarker& marker) {
    if (!marker.note.empty()) return marker.note;   // user's own words win
    if (marker.autoName == AutoName::None) return {};
    return LocalizationManager::Format(AutoNameKey(marker.autoName), marker.autoNameNumber);
}

std::uint32_t NextAutoNameNumber(AutoName scheme) {
    std::uint32_t maxN = 0;
    for (const auto& m : MarkerManager::GetSingleton()->GetUserMarkers()) {
        if (m.autoName == scheme) maxN = (std::max)(maxN, m.autoNameNumber);
    }
    return maxN + 1;
}

std::uint32_t MarkerManager::AllocateMarkerIDLocked() {
    if (_markers.size() >= kMaxSerializedEntries) {
        return 0;
    }
    std::uint32_t candidate = _nextID;
    if (candidate == 0 || candidate >= kVanillaDestId) {
        candidate = 1;
    }
    const std::uint32_t maxAttempts = static_cast<std::uint32_t>(_markers.size()) + 2;
    for (std::uint32_t attempts = 0; attempts < maxAttempts; ++attempts) {
        if (candidate == 0 || candidate >= kVanillaDestId) {
            candidate = 1;
        }
        bool occupied = false;
        for (const auto& m : _markers) {
            if (m.id == candidate) {
                occupied = true;
                break;
            }
        }
        if (!occupied) {
            std::uint32_t allocated = candidate;
            candidate++;
            if (candidate == 0 || candidate >= kVanillaDestId) {
                candidate = 1;
            }
            _nextID = candidate;
            return allocated;
        }
        candidate++;
    }
    return 0;
}

std::uint32_t MarkerManager::AddMarker(MapMarker marker) {
    std::unique_lock lock(_mutex);
    if (_markers.size() >= kMaxSerializedEntries) {
        logger::warn("Cannot add marker: marker cap ({}) reached", kMaxSerializedEntries);
        return 0;
    }
    if (marker.note.length() > kMaxSerializedStringBytes) {
        logger::warn("Cannot add marker: note exceeds max bytes ({})", kMaxSerializedStringBytes);
        return 0;
    }
    if (!std::isfinite(marker.position.x) || !std::isfinite(marker.position.y) || !std::isfinite(marker.position.z)) {
        logger::warn("Cannot add marker: non-finite position");
        return 0;
    }
    if (marker.autoName > AutoName::QuickMark) {
        logger::warn("Cannot add marker: invalid autoName scheme");
        return 0;
    }
    if (marker.origin > MarkerOrigin::InspectQuickMark) {
        logger::warn("Cannot add marker: invalid origin");
        return 0;
    }

    const std::uint32_t id = AllocateMarkerIDLocked();
    if (id == 0) {
        logger::warn("Cannot add marker: ID allocation failed");
        return 0;
    }

    marker.id = id;
    if (marker.timestamp == 0) {
        marker.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    }
    _markers.push_back(marker);

    logger::debug("Added marker ID: {} | Note: {} | Pos: {:.2f}, {:.2f}, {:.2f}",
                 marker.id, marker.note, marker.position.x, marker.position.y,
                 marker.position.z);
    return id;
}

std::uint32_t MarkerManager::AddMarker(RE::FormID worldSpaceID, RE::FormID cellID,
                                       const RE::NiPoint3& position,
                                       std::uint32_t iconType,
                                       const std::string& note) {
    MapMarker marker;
    marker.worldSpaceID = worldSpaceID;
    marker.cellID = cellID;
    marker.position = position;
    marker.iconType = iconType;
    marker.note = note;
    marker.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return AddMarker(marker);
}

std::uint32_t MarkerManager::AddAutoNamedMarker(RE::FormID worldSpaceID, RE::FormID cellID,
                                               const RE::NiPoint3& position,
                                               std::uint32_t iconType,
                                               AutoName scheme, std::uint32_t number,
                                               MarkerOrigin origin) {
    MapMarker marker;
    marker.worldSpaceID = worldSpaceID;
    marker.cellID = cellID;
    marker.position = position;
    marker.iconType = iconType;
    marker.note.clear();  // empty note IS the auto-name signal
    marker.autoName = scheme;
    marker.autoNameNumber = number ? number : NextAutoNameNumber(scheme);
    marker.origin = origin;
    marker.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return AddMarker(marker);
}

bool MarkerManager::RemoveMarker(std::uint32_t id) {
    bool removed = false;
    {
        std::unique_lock lock(_mutex);
        auto it = std::remove_if(_markers.begin(), _markers.end(),
                                 [id](const MapMarker &m) { return m.id == id; });

        if (it != _markers.end()) {
            _markers.erase(it, _markers.end());
            removed = true;
        }
    }

    if (removed) {
        logger::debug("Removed marker ID: {}", id);
        return true;
    }
    return false;
}

bool MarkerManager::UpdateMarker(std::uint32_t id, const std::string& note,
                                 std::uint32_t iconType) {
    if (note.length() > kMaxSerializedStringBytes) {
        logger::warn("UpdateMarker {}: note exceeds max bytes ({})", id, kMaxSerializedStringBytes);
        return false;
    }
    std::unique_lock lock(_mutex);
    for (auto &m : _markers) {
        if (m.id == id) {
            m.note = note;
            if (note.empty() && m.autoName == AutoName::None) {
                m.autoName = AutoName::Custom;
                std::uint32_t maxN = 0;
                for (const auto& existing : _markers) {
                    if (existing.autoName == AutoName::Custom) {
                        maxN = (std::max)(maxN, existing.autoNameNumber);
                    }
                }
                m.autoNameNumber = maxN + 1;
            }
            m.iconType = iconType;
            logger::debug("Updated marker ID: {} | Note: {} | iconType: {}", id, note, iconType);
            return true;
        }
    }
    return false;
}

std::vector<MapMarker> MarkerManager::GetUserMarkers() {
    std::unique_lock lock(_mutex);
    return _markers;
}

std::vector<MapMarker> MarkerManager::GetMarkers() {
    std::unique_lock lock(_mutex);
    std::vector<MapMarker> out = _markers;
    if (_vanillaDest.active) {
        MapMarker m;
        m.id            = kVanillaDestId;
        m.worldSpaceID  = _vanillaDest.worldSpaceID;
        m.cellID        = _vanillaDest.cellID;
        m.position      = _vanillaDest.position;
        // The raw name, no prefix. ResolveMarkerName returns note when non-empty, so
        // every existing draw site prints the right text with no strip step.
        m.note          = _vanillaDest.name;
        m.iconType      = 0;
        m.timestamp     = 0;
        m.isDestination = true;
        m.autoName      = AutoName::None;
        out.push_back(m);
    }
    return out;
}

void MarkerManager::SetDestination(std::uint32_t id, bool flag) {
    std::unique_lock lock(_mutex);
    // A real marker taking the destination clears the vanilla record: the two
    // kinds are mutually exclusive, or two destinations would draw at once.
    if (flag) _vanillaDest = {};
    for (auto &m : _markers) {
        if (m.id == id) {
            m.isDestination = flag;
        } else if (flag) {
            // Single-destination rule: clear every other marker when setting.
            m.isDestination = false;
        }
    }
    logger::debug("SetDestination id={} flag={}", id, flag);
}

std::uint32_t MarkerManager::GetDestinationID() {
    std::unique_lock lock(_mutex);
    for (const auto &m : _markers) {
        if (m.isDestination) return m.id;
    }
    // Matches what GetMarkers() hands out: an active vanilla destination reports
    // the reserved synthesized id, not 0.
    return _vanillaDest.active ? kVanillaDestId : 0;
}

bool MarkerManager::SetVanillaDestination(const std::string& name,
                                          RE::FormID sourceFormID,
                                          RE::FormID worldSpaceID,
                                          RE::FormID cellID,
                                          const RE::NiPoint3& position,
                                          bool fromLocalDoor) {
    if (name.length() > kMaxSerializedStringBytes) {
        logger::warn("SetVanillaDestination: name exceeds max bytes ({})", kMaxSerializedStringBytes);
        return false;
    }
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
        logger::warn("SetVanillaDestination: non-finite position");
        return false;
    }
    if (worldSpaceID == 0 && cellID == 0) {
        logger::warn("SetVanillaDestination: both worldSpaceID and cellID are 0");
        return false;
    }

    std::unique_lock lock(_mutex);
    for (auto &m : _markers) {
        m.isDestination = false;
    }

    _vanillaDest.worldSpaceID = worldSpaceID;
    _vanillaDest.cellID       = cellID;
    _vanillaDest.position     = position;
    _vanillaDest.name         = name;
    _vanillaDest.sourceFormID = sourceFormID;
    _vanillaDest.active       = true;
    _vanillaDest.fromLocalDoor = fromLocalDoor;

    logger::debug("SetVanillaDestination '{}' src={:08X} ws={:08X} cell={:08X} fromLocalDoor={} pos=({:.1f},{:.1f},{:.1f})",
                  name, sourceFormID, worldSpaceID, cellID, fromLocalDoor,
                  position.x, position.y, position.z);
    return true;
}

bool MarkerManager::IsVanillaDestination(const std::string& name) const {
    std::unique_lock lock(_mutex);
    return _vanillaDest.active && _vanillaDest.name == name;
}

bool MarkerManager::IsVanillaDestFromLocalDoor() const {
    std::unique_lock lock(_mutex);
    return _vanillaDest.active && _vanillaDest.fromLocalDoor;
}

bool MarkerManager::HasVanillaDestination() const {
    std::unique_lock lock(_mutex);
    return _vanillaDest.active;
}

VanillaDestination MarkerManager::GetVanillaDestination() const {
    std::unique_lock lock(_mutex);
    return _vanillaDest;
}

void MarkerManager::RemoveVanillaDestination() {
    std::unique_lock lock(_mutex);
    logger::debug("RemoveVanillaDestination: was active={} name='{}'",
                  _vanillaDest.active, _vanillaDest.name);
    _vanillaDest = {};
}

void MarkerManager::ClearDestination() {
    std::unique_lock lock(_mutex);
    _vanillaDest = {};
    for (auto &m : _markers) {
        m.isDestination = false;
    }
}

bool MarkerManager::ToggleFavorite(RE::FormID formID, RE::FormID worldSpaceID, RE::FormID cellID, const RE::NiPoint3& position, const std::string& name) {
    if (formID == 0) {
        logger::warn("ToggleFavorite: formID is 0");
        return false;
    }
    std::unique_lock lock(_mutex);
    auto it = std::find_if(_favorites.begin(), _favorites.end(), [formID](const FavoriteMarker& f) {
        return f.formID == formID;
    });

    if (it != _favorites.end()) {
        _favorites.erase(it);
        logger::debug("Removed favorite for FormID {:08X}", formID);
        return true;
    }

    if (_favorites.size() >= kMaxSerializedEntries) {
        logger::warn("ToggleFavorite: favorites cap ({}) reached", kMaxSerializedEntries);
        return false;
    }
    if (name.length() > kMaxSerializedStringBytes) {
        logger::warn("ToggleFavorite: name exceeds max bytes ({})", kMaxSerializedStringBytes);
        return false;
    }
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
        logger::warn("ToggleFavorite: non-finite position");
        return false;
    }
    if (worldSpaceID == 0 && cellID == 0) {
        logger::warn("ToggleFavorite: both worldSpaceID and cellID are 0");
        return false;
    }

    _favorites.push_back({formID, worldSpaceID, cellID, position, name});
    logger::debug("Added favorite for FormID {:08X} ({})", formID, name);
    return true;
}

bool MarkerManager::IsFavorite(RE::FormID formID) const {
    std::unique_lock lock(_mutex);
    return std::find_if(_favorites.begin(), _favorites.end(), [formID](const FavoriteMarker& f) {
        return f.formID == formID;
    }) != _favorites.end();
}

std::vector<FavoriteMarker> MarkerManager::GetFavorites() const {
    std::unique_lock lock(_mutex);
    return _favorites;
}

bool MarkerManager::SetVanillaNote(RE::FormID formID, const std::string& note) {
    if (formID == 0) {
        logger::warn("SetVanillaNote: formID is 0");
        return false;
    }
    if (note.length() > kMaxSerializedStringBytes) {
        logger::warn("SetVanillaNote: note exceeds max bytes ({})", kMaxSerializedStringBytes);
        return false;
    }
    std::unique_lock lock(_mutex);
    if (note.empty()) {
        auto it = _vanillaNotes.find(formID);
        if (it != _vanillaNotes.end()) {
            _vanillaNotes.erase(it);
            logger::debug("Cleared vanilla note for FormID {:08X}", formID);
        }
        return true;
    }

    if (_vanillaNotes.find(formID) == _vanillaNotes.end() && _vanillaNotes.size() >= kMaxSerializedEntries) {
        logger::warn("SetVanillaNote: notes cap ({}) reached", kMaxSerializedEntries);
        return false;
    }

    _vanillaNotes[formID] = note;
    logger::debug("Set vanilla note for FormID {:08X}: {}", formID, note);
    return true;
}

std::string MarkerManager::GetVanillaNote(RE::FormID formID) const {
    std::unique_lock lock(_mutex);
    auto it = _vanillaNotes.find(formID);
    return it != _vanillaNotes.end() ? it->second : std::string{};
}

std::vector<std::pair<RE::FormID, std::string>> MarkerManager::GetVanillaNotes() const {
    std::unique_lock lock(_mutex);
    std::vector<std::pair<RE::FormID, std::string>> out;
    out.reserve(_vanillaNotes.size());
    for (const auto& [formID, note] : _vanillaNotes) {
        out.emplace_back(formID, note);
    }
    return out;
}

void MarkerManager::OnGameSave(SKSE::SerializationInterface *serde) {
    if (!serde) return;

    std::unique_lock lock(_mutex);

    // Invariant prevalidation before OpenRecord
    if (_markers.size() > kMaxSerializedEntries) {
        logger::error("Save aborted: marker count {} exceeds cap {}", _markers.size(), kMaxSerializedEntries);
        return;
    }
    if (_favorites.size() > kMaxSerializedEntries) {
        logger::error("Save aborted: favorites count {} exceeds cap {}", _favorites.size(), kMaxSerializedEntries);
        return;
    }
    if (_vanillaNotes.size() > kMaxSerializedEntries) {
        logger::error("Save aborted: notes count {} exceeds cap {}", _vanillaNotes.size(), kMaxSerializedEntries);
        return;
    }

    for (const auto& marker : _markers) {
        if (marker.id == 0 || marker.id == kVanillaDestId) {
            logger::error("Save aborted: marker id {:08X} is invalid", marker.id);
            return;
        }
        if (marker.note.length() > kMaxSerializedStringBytes) {
            logger::error("Save aborted: marker id {} note size {} exceeds limit", marker.id, marker.note.length());
            return;
        }
        if (!std::isfinite(marker.position.x) || !std::isfinite(marker.position.y) || !std::isfinite(marker.position.z)) {
            logger::error("Save aborted: marker id {} has non-finite position", marker.id);
            return;
        }
        if (marker.autoName > AutoName::QuickMark) {
            logger::error("Save aborted: marker id {} has invalid autoName {}", marker.id, static_cast<std::uint8_t>(marker.autoName));
            return;
        }
        if (marker.origin > MarkerOrigin::InspectQuickMark) {
            logger::error("Save aborted: marker id {} has invalid origin {}", marker.id, static_cast<std::uint8_t>(marker.origin));
            return;
        }
    }

    for (const auto& fav : _favorites) {
        if (fav.formID == 0) {
            logger::error("Save aborted: favorite has formID 0");
            return;
        }
        if (fav.name.length() > kMaxSerializedStringBytes) {
            logger::error("Save aborted: favorite {:08X} name size {} exceeds limit", fav.formID, fav.name.length());
            return;
        }
        if (!std::isfinite(fav.position.x) || !std::isfinite(fav.position.y) || !std::isfinite(fav.position.z)) {
            logger::error("Save aborted: favorite {:08X} has non-finite position", fav.formID);
            return;
        }
    }

    for (const auto& [formID, note] : _vanillaNotes) {
        if (formID == 0) {
            logger::error("Save aborted: vanilla note has formID 0");
            return;
        }
        if (note.length() > kMaxSerializedStringBytes) {
            logger::error("Save aborted: vanilla note {:08X} size {} exceeds limit", formID, note.length());
            return;
        }
    }

    if (_vanillaDest.active) {
        if (_vanillaDest.name.length() > kMaxSerializedStringBytes) {
            logger::error("Save aborted: destination name size {} exceeds limit", _vanillaDest.name.length());
            return;
        }
        if (!std::isfinite(_vanillaDest.position.x) || !std::isfinite(_vanillaDest.position.y) || !std::isfinite(_vanillaDest.position.z)) {
            logger::error("Save aborted: destination has non-finite position");
            return;
        }
    }

    if (!serde->OpenRecord(kSerializationSignature, kSerializationVersion)) {
        logger::error("Failed to open record for serialization!");
        return;
    }

    // 1. Write Count
    const std::uint32_t count = static_cast<std::uint32_t>(_markers.size());
    if (!WriteChecked(serde, count)) {
        logger::error("Save failed: write marker count");
        return;
    }

    // 2. Write Markers
    for (const auto &marker : _markers) {
        if (!WriteChecked(serde, marker.id) ||
            !WriteChecked(serde, marker.worldSpaceID) ||
            !WriteChecked(serde, marker.cellID) ||
            !WriteChecked(serde, marker.position) ||
            !WriteChecked(serde, marker.iconType) ||
            !WriteChecked(serde, marker.timestamp) ||
            !WriteStringChecked(serde, marker.note)) {
            logger::error("Save failed: write marker id {}", marker.id);
            return;
        }

        const std::uint8_t destByte = marker.isDestination ? 1 : 0;
        if (!WriteChecked(serde, destByte)) {
            logger::error("Save failed: write marker destByte");
            return;
        }

        const std::uint8_t autoByte = static_cast<std::uint8_t>(marker.autoName);
        if (!WriteChecked(serde, autoByte) ||
            !WriteChecked(serde, marker.autoNameNumber)) {
            logger::error("Save failed: write marker autoName");
            return;
        }

        const std::uint8_t originByte = static_cast<std::uint8_t>(marker.origin);
        if (!WriteChecked(serde, originByte)) {
            logger::error("Save failed: write marker origin");
            return;
        }
    }

    // 3. Write Next ID
    if (!WriteChecked(serde, _nextID)) {
        logger::error("Save failed: write _nextID");
        return;
    }

    // 4. Write Favorites
    const std::uint32_t favCount = static_cast<std::uint32_t>(_favorites.size());
    if (!WriteChecked(serde, favCount)) {
        logger::error("Save failed: write favCount");
        return;
    }
    for (const auto &fav : _favorites) {
        if (!WriteChecked(serde, fav.formID) ||
            !WriteChecked(serde, fav.worldSpaceID) ||
            !WriteChecked(serde, fav.cellID) ||
            !WriteChecked(serde, fav.position) ||
            !WriteStringChecked(serde, fav.name)) {
            logger::error("Save failed: write favorite {:08X}", fav.formID);
            return;
        }
    }

    // 5. Write Vanilla-marker notes
    const std::uint32_t noteCount = static_cast<std::uint32_t>(_vanillaNotes.size());
    if (!WriteChecked(serde, noteCount)) {
        logger::error("Save failed: write noteCount");
        return;
    }
    for (const auto& [formID, note] : _vanillaNotes) {
        if (!WriteChecked(serde, formID) ||
            !WriteStringChecked(serde, note)) {
            logger::error("Save failed: write vanilla note {:08X}", formID);
            return;
        }
    }

    // 6. Write the vanilla destination (v6+ with v7 suffix)
    const std::uint8_t destPresent = _vanillaDest.active ? 1 : 0;
    if (!WriteChecked(serde, destPresent)) {
        logger::error("Save failed: write destPresent");
        return;
    }
    if (destPresent) {
        if (!WriteChecked(serde, _vanillaDest.worldSpaceID) ||
            !WriteChecked(serde, _vanillaDest.cellID) ||
            !WriteChecked(serde, _vanillaDest.position) ||
            !WriteStringChecked(serde, _vanillaDest.name)) {
            logger::error("Save failed: write destination prefix");
            return;
        }
        // v7 suffix
        const std::uint8_t fromLocalDoorByte = _vanillaDest.fromLocalDoor ? 1 : 0;
        if (!WriteChecked(serde, _vanillaDest.sourceFormID) ||
            !WriteChecked(serde, fromLocalDoorByte)) {
            logger::error("Save failed: write destination v7 suffix");
            return;
        }
    }

    logger::info("Saved {} custom markers (CMMK v{}).", count, kSerializationVersion);
}

void MarkerManager::OnGameLoad(SKSE::SerializationInterface *serde) {
    {
        std::unique_lock lock(_mutex);
        _markers.clear();
        _favorites.clear();
        _vanillaNotes.clear();
        _vanillaDest = {};
        _nextID = 1;
    }

    std::uint32_t type = 0;
    std::uint32_t version = 0;
    std::uint32_t length = 0;

    while (serde->GetNextRecordInfo(type, version, length)) {
        if (type == kSerializationSignature) {
            if (version < 1 || version > kSerializationVersion) {
                logger::warn("Unsupported CMMK version {}.", version);
                continue;  // GetNextRecordInfo skips this record's unread bytes.
            }

            LoadedMarkerState loaded;
            if (!ReadCMMKRecord(serde, version, length, loaded)) {
                logger::error("Rejected malformed CMMK v{} record ({} bytes).", version, length);
                continue;
            }

            loaded.savedNextID = RecoverNextIDHint(loaded.savedNextID, loaded.markers);
            NormalizeDestinationState(loaded);

            std::unique_lock lock(_mutex);
            _markers = std::move(loaded.markers);
            _favorites = std::move(loaded.favorites);
            _vanillaNotes = std::move(loaded.vanillaNotes);
            _vanillaDest = std::move(loaded.vanillaDest);
            _nextID = loaded.savedNextID;
        } else if (type == PathTracker::kSerializationSignature) {
            PathTracker::GetSingleton()->LoadRecord(serde, version, length);
        }
    }
}

void MarkerManager::OnRevert(SKSE::SerializationInterface* /*serde*/) {
    std::unique_lock lock(_mutex);
    _markers.clear();
    _favorites.clear();
    _vanillaNotes.clear();
    _vanillaDest = {};
    _nextID = 1;
    logger::debug("Reverted marker data.");
}
