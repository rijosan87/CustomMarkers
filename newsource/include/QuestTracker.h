// include/QuestTracker.h
#pragma once
#include <RE/Skyrim.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// The player's journal-SELECTED quests, for the List panel's Active Quests
// section. See findings/spec-findings-Active-Quests-Section.md.
//
// The selection test is TWO flags together: kActive is the tracked bit but ~15
// background/utility quests also carry it (Delivery, Thane of Falkreath, bounty
// templates), and kDisplayedInHUD is journal-visible but every running journal
// quest carries it whether selected or not. Their intersection was exactly the
// selected set, confirmed in-game 2026-08-19 against the journal's own Flash
// TitleList.entryList[].active.
//
// Ruled out, do not re-attempt: BGSQuestObjective::state (progress only),
// PlayerCharacter::objectives InstanceState (mirrors progress), and
// PlayerCharacter::questTargets (24 quests, not the selection).
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
    // Worldspace the resolved position belongs to. 0 when hasPosition is false.
    // The List panel drops rows whose worldspace isn't the open map's: a
    // Wyrmstooth quest target carries Tamriel coords, and panning to those on
    // the Wyrmstooth map lands on empty terrain that doesn't exist there.
    RE::FormID    worldSpaceID = 0;
    // Diagnostic only, not drawn: which of the resolve paths answered.
    // 0 none, 1 compass buffer, 2 exterior target ref, 3 worldLocMarker,
    // 4 TESQuestTarget teleport-path exterior door.
    // A vanished row and an unclickable row look identical on screen; this is
    // what separates them in the log. Gated by [Logging] QuestRowLog.
    std::int32_t  posSource = 0;
};

class QuestTracker {
public:
    static QuestTracker* GetSingleton();

    // Rebuilds the tracked list. NOT cache-gated on worldspace the way
    // MapStats::Refresh is: journal edits carry no worldspace signal, so gating
    // on it would freeze the list. Walks quests (tens), not refs.
    void Refresh();

    // Copy of the cached rows under the mutex, for the draw path.
    std::vector<ActiveQuestRow> Get();

private:
    QuestTracker() = default;
    QuestTracker(const QuestTracker&) = delete;
    QuestTracker& operator=(const QuestTracker&) = delete;

    std::mutex                  _mutex;
    std::vector<ActiveQuestRow> _rows;
};
