#pragma once

// Queries active quest objectives and associated map markers.

#include <RE/Skyrim.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct ActiveQuestRow {
    RE::FormID   questID = 0;
    std::string  name;
    std::string  objectiveText;
    std::int32_t questType = 0;
    bool         hasPosition = false;
    RE::NiPoint3 position{};
    RE::FormID   worldSpaceID = 0;
    std::int32_t posSource = 0;
};

class QuestTracker {
public:
    static QuestTracker* GetSingleton();
    void Refresh();
    std::vector<ActiveQuestRow> Get();
private:
    QuestTracker() = default;
    QuestTracker(const QuestTracker&) = delete;
    QuestTracker& operator=(const QuestTracker&) = delete;
    std::mutex                  _mutex;
    std::vector<ActiveQuestRow> _rows;
};
