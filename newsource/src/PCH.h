// File: PCH.h
#pragma once

// ===========================================================
// Core Skyrim / SKSE Includes for CommonLibSSE-NG 3.x
// ===========================================================

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/API.h>
#include <SKSE/SKSE.h>
using namespace std::literals;

// If you are not using trampoline hooks, leave this commented:
// #include <SKSE/Trampoline.h>

// ===========================================================
// Standard Library Includes
// ===========================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// ===========================================================
// spdlog Logging
// ===========================================================

#include <spdlog/spdlog.h>

#ifdef NDEBUG
    #include <spdlog/sinks/basic_file_sink.h>
#else
    #include <spdlog/sinks/msvc_sink.h>
#endif

namespace logger = SKSE::log;

// ===========================================================
// Convenience Macros
// ===========================================================

#define DLLEXPORT __declspec(dllexport)

// NG VariantOffset helper
#define RELOCATION_OFFSET(SE, AE) REL::VariantOffset(SE, AE, 0).offset()

#define RELOCATION_OFFSET2(SE, AE, AE629)                                                                             \
    REL::VariantOffset(                                                                                               \
        SE,                                                                                                           \
        (REL::Module::get().version().compare(SKSE::RUNTIME_SSE_1_6_629) == std::strong_ordering::less) ? AE : AE629, \
        0)                                                                                                            \
        .offset()

// ===========================================================
// Utility
// ===========================================================
namespace util {
    using SKSE::stl::report_and_fail;
}
