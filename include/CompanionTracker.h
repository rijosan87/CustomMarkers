#pragma once

#include <RE/Skyrim.h>

#include <string>
#include <vector>

// Spouse, followers left waiting, and dismissed followers, with where each is now.
namespace CompanionTracker {
    struct Entry {
        RE::FormID       actorID = 0;
        std::string      name;
        std::string      place;            // location name; "" while travelling
        RE::BGSLocation* loc     = nullptr; // location of the actor's cell; nullptr in transit
        bool             spouse  = false;
        // Root worldspace frame, so it pans and projects directly on the world map.
        bool            hasMapPos   = false;
        RE::FormID       rootWorldID = 0;
        RE::NiPoint3     mapPos{};
    };

    // Rebuilt once per game-time change. Returns a copy: the stat card (game
    // thread) and the List panel (render thread) both read it.
    std::vector<Entry> Get();
}
