// include/QuestTracker.h
#pragma once
#include <RE/Skyrim.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Queries active quest objectives and associated map markers.
struct ActiveQuestRow {
    RE::FormID    questID = 0;
    std::string   name;                 // GetFullName() for regular; empty for misc
    std::string   objectiveText;        // first kDisplayed objective, or misc objective text
    std::int32_t  questType = 0;        // QUEST_DATA::Type; 6 == kMiscellaneous
    bool          isMiscellaneous = false;
    std::uint32_t instanceID = 0;       // PlayerCharacter::objectives instanceID (misc)
    std::uint16_t objectiveIndex = 0;   // BGSQuestObjective::index (misc)
    bool          hasPosition = false;  // a live position was resolved for this quest/objective
    RE::NiPoint3  position{};           // only meaningful when hasPosition
    RE::FormID    worldSpaceID = 0;
    std::int32_t  posSource = 0;
};

class QuestTracker {
public:
    static QuestTracker* GetSingleton();

    void Refresh();

    // Invalidates cached rows on map closure or game lifecycle changes.
    // A pending task retains its slot until it retires without publishing.
    void Reset();

    // Copy of the cached rows under the mutex, for the draw path.
    std::vector<ActiveQuestRow> Get();

private:
    QuestTracker() = default;
    QuestTracker(const QuestTracker&) = delete;
    QuestTracker& operator=(const QuestTracker&) = delete;

    void RefreshOnGameThread(std::uint64_t a_generation);

    std::mutex                  _mutex;
    std::vector<ActiveQuestRow> _rows;
    bool                       _refreshPending = false;
    bool                       _sessionActive = false;
    std::uint64_t              _generation = 0;
};
