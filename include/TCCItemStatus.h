#pragma once

#include <RE/Skyrim.h>

// Optional Curator's Companion data source. Keep its forms and state semantics
// out of the loot-beam renderer.
namespace TCCItemStatus {
    enum class Status { None, New, Found, Displayed };

    void Init();
    Status Get(RE::FormID a_itemFormID);
    const char* Name(Status a_status);
}
