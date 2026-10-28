#include "HUDMarkerHook.h"
#include "Settings.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <intrin.h>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <polyhook2/Detour/x64Detour.hpp>
#include <Psapi.h>

namespace {
    // AddMarker: SE 50851 / AE 51728.
    constexpr REL::RelocationID kAddMarkerID{ 50851, 51728 };

    // HUDMarker::FrameOffsets singleton (icon goto-frame table). SE 519587 / AE 406118.
    struct FrameOffsets {
        std::uint32_t quest;                 // 00
        std::uint32_t questDoor;             // 04
        std::uint32_t playerSet;             // 08
        std::uint32_t enemy;                 // 0C
        std::uint32_t location;              // 10
        std::uint32_t undiscoveredLocation;  // 14
    };
    static_assert(sizeof(FrameOffsets) == 0x18);

    FrameOffsets* GetFrameOffsets() {
        REL::Relocation<FrameOffsets*> singleton{ REL::RelocationID(519587, 406118) };
        return singleton.get();
    }

    class DiagnosticX64Detour final : public PLH::x64Detour {
    public:
        using PLH::x64Detour::x64Detour;

        detour_scheme_t ChosenScheme() const { return m_chosen_scheme; }
        std::uint64_t ResolvedTargetAddress() const { return m_fnAddress; }
        std::uint32_t OverwrittenSize() const { return m_hookSize; }
        std::uint16_t TrampolineSize() const { return m_trampolineSz; }

        std::string OriginalInstructions() const {
            return PLH::instsToStr(m_originalInsts);
        }

        std::string TrampolineInstructions() {
            const auto instructions = m_disasm.disassemble(
                m_trampoline, m_trampoline, m_trampoline + m_trampolineSz, *this);
            return PLH::instsToStr(instructions);
        }

        std::uint64_t ResumeAddress() const {
            if (m_originalInsts.empty()) return 0;
            const auto& last = m_originalInsts.back();
            return last.getAddress() + last.size();
        }
    };

    constexpr std::size_t kTargetDiagnosticBytes = 32;
    constexpr std::size_t kTrampolineDiagnosticBytes = 64;

    std::uint64_t                                  g_targetAddr = 0;
    std::uint64_t                                  g_trampolineAddr = 0;
    std::unique_ptr<DiagnosticX64Detour>            g_detour;
    std::vector<std::uint8_t>                       g_installedTargetBytes;
    std::vector<std::uint8_t>                       g_installedTrampolineBytes;

    struct HookBreadcrumb {
        std::uint64_t sequence;
        std::uint64_t caller;
        std::uint64_t returnSlot;
        std::uint64_t manager;
        std::uint64_t markerData;
        std::uint64_t param3;
        std::uint64_t refHandlePtr;
        std::uint32_t refHandle;
        std::int32_t  gotoFrame;
        std::uint32_t threadID;
    };

    thread_local HookBreadcrumb  g_hookBreadcrumb{};
    std::atomic<std::uint64_t>    g_hookSequence{ 0 };
    std::atomic_bool              g_crashRecordWritten{ false };
    HANDLE                        g_crashDiagnosticFile = INVALID_HANDLE_VALUE;
    PVOID                         g_vectoredHandler = nullptr;

    using AddMarkerFn = bool(*)(const void*, void*, void*, const std::uint32_t*, std::int32_t);

    bool DiagnosticEnabled() {
        return REL::Module::IsSE() && Settings::GetSingleton()->hudMarkerLog;
    }

    std::vector<std::uint8_t> ReadBytes(std::uint64_t a_address, std::size_t a_size) {
        if (!a_address || !a_size) return {};
        const auto* first = reinterpret_cast<const std::uint8_t*>(a_address);
        return { first, first + a_size };
    }

    std::string FormatBytes(const std::vector<std::uint8_t>& a_bytes) {
        std::ostringstream out;
        out << std::hex << std::uppercase << std::setfill('0');
        for (std::size_t i = 0; i < a_bytes.size(); ++i) {
            if (i != 0) out << ' ';
            out << std::setw(2) << static_cast<unsigned>(a_bytes[i]);
        }
        return out.str();
    }

    bool IsReadableRange(std::uint64_t a_address, std::size_t a_size) noexcept {
        if (!a_address || !a_size) return false;

        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(a_address), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }

        const auto regionStart = reinterpret_cast<std::uint64_t>(info.BaseAddress);
        const auto regionEnd = regionStart + info.RegionSize;
        return a_address >= regionStart && a_address <= regionEnd && a_size <= regionEnd - a_address;
    }

    bool ReadCrashBytes(std::uint64_t a_address, void* a_out, std::size_t a_size) noexcept {
        // VirtualQuery alone leaves a race with protection/unmapping changes.
        // ReadProcessMemory reports failure instead of faulting in our observer.
        if (!IsReadableRange(a_address, a_size)) return false;
        SIZE_T copied = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a_address),
                                 a_out, a_size, &copied) && copied == a_size;
    }

    void WriteCrashLine(const char* a_line, int a_length, std::size_t a_capacity) noexcept {
        if (a_length <= 0) return;
        DWORD written = 0;
        WriteFile(g_crashDiagnosticFile, a_line,
                  static_cast<DWORD>(std::min<std::size_t>(a_length, a_capacity - 1)),
                  &written, nullptr);
    }

    void LogCrashAddress(const char* a_label, std::uint64_t a_address) noexcept {
        MEMORY_BASIC_INFORMATION info{};
        const bool queried = VirtualQuery(reinterpret_cast<const void*>(a_address),
                                          &info, sizeof(info)) == sizeof(info);
        // Query the mapping, avoiding loader enumeration/loader locks in a VEH.
        // MEM_PRIVATE trampolines have no module owner; keep their allocation base.
        char path[1024]{};
        if (queried && info.Type == MEM_IMAGE) {
            GetMappedFileNameA(GetCurrentProcess(), reinterpret_cast<void*>(a_address),
                               path, static_cast<DWORD>(sizeof(path) - 1));
        }
        char hex[64 * 3 + 1]{};
        std::uint8_t bytes[64]{};
        std::size_t count = 0;
        // Retain a readable prefix even at a region boundary.
        while (count < sizeof(bytes) && ReadCrashBytes(a_address + count, &bytes[count], 1)) {
            std::snprintf(hex + count * 3, sizeof(hex) - count * 3, "%02X ",
                          static_cast<unsigned>(bytes[count]));
            ++count;
        }
        char line[2048]{};
        const int length = std::snprintf(line, sizeof(line),
            "%s address=%016llX query=%s base=%p allocation=%p regionSize=%llu "
            "state=%08lX protect=%08lX type=%08lX imageOffset=%016llX mappedImage=%s "
            "readBytes=%llu bytes=%s\r\n",
            a_label, static_cast<unsigned long long>(a_address), queried ? "ok" : "failed",
            info.BaseAddress, info.AllocationBase, static_cast<unsigned long long>(info.RegionSize),
            info.State, info.Protect, info.Type,
            static_cast<unsigned long long>(queried && info.Type == MEM_IMAGE ?
                a_address - reinterpret_cast<std::uint64_t>(info.AllocationBase) : 0),
            path[0] ? path : "<unavailable>", static_cast<unsigned long long>(count), hex);
        WriteCrashLine(line, length, sizeof(line));
    }

    void LogCrashJumpChain() noexcept {
        std::uint64_t address = g_targetAddr;
        std::uint64_t visited[8]{};
        for (std::size_t hop = 0; hop < 8; ++hop) {
            for (std::size_t previous = 0; previous < hop; ++previous) {
                if (visited[previous] == address) {
                    constexpr char cycle[] = "entryChain stop=cycle\r\n";
                    WriteCrashLine(cycle, sizeof(cycle) - 1, sizeof(cycle));
                    return;
                }
            }
            visited[hop] = address;
            LogCrashAddress("entryChain", address);
            std::uint8_t code[16]{};
            std::size_t count = 0;
            while (count < sizeof(code) && ReadCrashBytes(address + count, &code[count], 1)) ++count;
            std::uint64_t destination = 0;
            std::uint64_t slot = 0;
            const char* kind = "unrecognized-or-unreadable";
            if (count >= 6 && code[0] == 0xFF && code[1] == 0x25) {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, code + 2, sizeof(displacement));
                slot = address + 6 + static_cast<std::int64_t>(displacement);
                kind = "jmp-rip-indirect";
                LogCrashAddress("indirectSlot", slot);
                if (!ReadCrashBytes(slot, &destination, sizeof(destination))) {
                    destination = 0;
                    kind = "unreadable-slot";
                }
            } else if (count >= 5 && code[0] == 0xE9) {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, code + 1, sizeof(displacement));
                destination = address + 5 + static_cast<std::int64_t>(displacement);
                kind = "jmp-rel32";
            } else if (count >= 2 && code[0] == 0xEB) {
                destination = address + 2 + static_cast<std::int64_t>(static_cast<std::int8_t>(code[1]));
                kind = "jmp-rel8";
            } else if (count >= 12 && code[0] == 0x48 && code[1] == 0xB8 &&
                       code[10] == 0xFF && code[11] == 0xE0) {
                std::memcpy(&destination, code + 2, sizeof(destination));
                kind = "mov-rax-jmp-rax";
            } else if (count >= 13 && code[0] == 0x49 && code[1] == 0xBB &&
                       code[10] == 0x41 && code[11] == 0xFF && code[12] == 0xE3) {
                std::memcpy(&destination, code + 2, sizeof(destination));
                kind = "mov-r11-jmp-r11";
            }
            char line[256]{};
            const int length = std::snprintf(line, sizeof(line),
                "entryChain hop=%llu kind=%s slot=%016llX destination=%016llX\r\n",
                static_cast<unsigned long long>(hop), kind,
                static_cast<unsigned long long>(slot), static_cast<unsigned long long>(destination));
            WriteCrashLine(line, length, sizeof(line));
            // Unknown code is preserved above for offline disassembly, never executed.
            if (!destination) return;
            address = destination;
        }
        LogCrashAddress("entryChain-limit-destination", address);
        constexpr char limit[] = "entryChain stop=hop-limit\r\n";
        WriteCrashLine(limit, sizeof(limit) - 1, sizeof(limit));
    }

    LONG CALLBACK AddMarkerExceptionHandler(EXCEPTION_POINTERS* a_exception) noexcept {
        if (!a_exception || !a_exception->ExceptionRecord || !a_exception->ContextRecord ||
            a_exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
            g_crashDiagnosticFile == INVALID_HANDLE_VALUE) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const auto rip = static_cast<std::uint64_t>(a_exception->ContextRecord->Rip);
        // The affected reports fail at AddMarker+0x8F. Keep the handler narrow so
        // unrelated game exceptions never produce a misleading hook breadcrumb.
        if (!g_targetAddr || rip < g_targetAddr || rip >= g_targetAddr + 0x200 ||
            g_crashRecordWritten.exchange(true, std::memory_order_relaxed)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const auto& breadcrumb = g_hookBreadcrumb;
        std::uint8_t targetBytes[kTargetDiagnosticBytes]{};
        std::uint8_t trampolineBytes[kTrampolineDiagnosticBytes]{};
        const bool targetReadable = !g_installedTargetBytes.empty() &&
            g_installedTargetBytes.size() <= sizeof(targetBytes) &&
            ReadCrashBytes(g_targetAddr, targetBytes, g_installedTargetBytes.size());
        const bool trampolineReadable = !g_installedTrampolineBytes.empty() &&
            g_installedTrampolineBytes.size() <= sizeof(trampolineBytes) &&
            ReadCrashBytes(g_trampolineAddr, trampolineBytes, g_installedTrampolineBytes.size());
        const bool targetUnchanged = targetReadable &&
            std::memcmp(targetBytes,
                        g_installedTargetBytes.data(), g_installedTargetBytes.size()) == 0;
        const bool trampolineUnchanged = trampolineReadable &&
            std::memcmp(trampolineBytes,
                        g_installedTrampolineBytes.data(), g_installedTrampolineBytes.size()) == 0;

        const auto* record = a_exception->ExceptionRecord;
        const auto* context = a_exception->ContextRecord;
        const auto accessKind = record->NumberParameters >= 1 ? record->ExceptionInformation[0] : 0;
        const auto accessAddress = record->NumberParameters >= 2 ? record->ExceptionInformation[1] : 0;

        char line[1536]{};
        const int length = std::snprintf(
            line, sizeof(line),
            "AddMarker access violation | code=%08lX exception=%p accessKind=%llu accessAddress=%p "
            "tid=%lu rip=%016llX rsp=%016llX r11=%016llX rbp=%016llX "
            "hookSequence=%llu hookTid=%lu hookThreadMatches=%s caller=%016llX returnSlot=%016llX "
            "manager=%016llX markerData=%016llX param3=%016llX refHandlePtr=%016llX "
            "refHandle=%08X gotoFrame=%d target=%016llX trampoline=%016llX "
            "targetUnchanged=%s trampolineUnchanged=%s targetReadable=%s trampolineReadable=%s\r\n",
            record->ExceptionCode, record->ExceptionAddress,
            static_cast<unsigned long long>(accessKind), reinterpret_cast<void*>(accessAddress),
            GetCurrentThreadId(),
            static_cast<unsigned long long>(context->Rip),
            static_cast<unsigned long long>(context->Rsp),
            static_cast<unsigned long long>(context->R11),
            static_cast<unsigned long long>(context->Rbp),
            static_cast<unsigned long long>(breadcrumb.sequence), breadcrumb.threadID,
            breadcrumb.threadID == GetCurrentThreadId() ? "true" : "false",
            static_cast<unsigned long long>(breadcrumb.caller),
            static_cast<unsigned long long>(breadcrumb.returnSlot),
            static_cast<unsigned long long>(breadcrumb.manager),
            static_cast<unsigned long long>(breadcrumb.markerData),
            static_cast<unsigned long long>(breadcrumb.param3),
            static_cast<unsigned long long>(breadcrumb.refHandlePtr), breadcrumb.refHandle,
            breadcrumb.gotoFrame,
            static_cast<unsigned long long>(g_targetAddr),
            static_cast<unsigned long long>(g_trampolineAddr),
            targetUnchanged ? "true" : "false",
            trampolineUnchanged ? "true" : "false",
            targetReadable ? "true" : "false", trampolineReadable ? "true" : "false");

        if (length > 0) {
            const DWORD bytesToWrite = static_cast<DWORD>(
                std::min<std::size_t>(static_cast<std::size_t>(length), sizeof(line) - 1));
            DWORD bytesWritten = 0;
            WriteFile(g_crashDiagnosticFile, line, bytesToWrite, &bytesWritten, nullptr);
            FlushFileBuffers(g_crashDiagnosticFile);
        }
        LogCrashJumpChain();
        LogCrashAddress("customMarkersTrampoline", g_trampolineAddr);
        FlushFileBuffers(g_crashDiagnosticFile);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void ArmCrashDiagnostic() {
        if (g_vectoredHandler || !DiagnosticEnabled()) return;

        const auto logDirectory = SKSE::log::log_directory();
        if (!logDirectory) {
            logger::error("HUDMarkerHook crash diagnostic: SKSE log directory unavailable");
            return;
        }

        const auto path = *logDirectory / "CustomMarkers-HUDMarkerCrash.log";
        g_crashDiagnosticFile = CreateFileW(
            path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (g_crashDiagnosticFile == INVALID_HANDLE_VALUE) {
            logger::error("HUDMarkerHook crash diagnostic: failed to create {} (error {})",
                          path.string(), GetLastError());
            return;
        }

        constexpr char header[] =
            "HUDMarker crash diagnostic v2 (entry-chain capture).\r\n"
            "Waiting for an access violation inside HUDMarkerManager::AddMarker.\r\n";
        DWORD bytesWritten = 0;
        WriteFile(g_crashDiagnosticFile, header, sizeof(header) - 1, &bytesWritten, nullptr);
        FlushFileBuffers(g_crashDiagnosticFile);

        g_vectoredHandler = AddVectoredExceptionHandler(1, AddMarkerExceptionHandler);
        if (!g_vectoredHandler) {
            logger::error("HUDMarkerHook crash diagnostic: AddVectoredExceptionHandler failed (error {})",
                          GetLastError());
            CloseHandle(g_crashDiagnosticFile);
            g_crashDiagnosticFile = INVALID_HANDLE_VALUE;
            return;
        }
        logger::info("HUDMarkerHook crash-only diagnostic armed | file={}", path.string());
    }

    std::mutex                                g_mutex;
    std::vector<HUDMarkerHook::QuestMarker>   g_pending;    // accumulating the current pass
    std::vector<HUDMarkerHook::QuestMarker>   g_published;  // last completed pass, read by renderers

    std::unordered_map<std::uint32_t, RE::TESQuest*> g_handleToQuest;
    std::chrono::steady_clock::time_point            g_lastAliasRebuild{};

    bool IsJournalVisible(const RE::TESQuest* a_quest) {
        return a_quest && a_quest->data.flags.any(RE::QuestFlag::kDisplayedInHUD);
    }

    void RebuildHandleToQuestMap() {
        g_handleToQuest.clear();

        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return;

        for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
            if (!quest || !quest->IsRunning()) continue;

            RE::BSReadLockGuard lock(quest->aliasAccessLock);
            for (const auto& entry : quest->refAliasMap) {
                const std::uint32_t handle = entry.second.native_handle();
                if (!handle) continue;

                auto [it, inserted] = g_handleToQuest.try_emplace(handle, quest);
                if (!inserted && IsJournalVisible(quest) && !IsJournalVisible(it->second)) {
                    it->second = quest;
                }
            }
        }
    }

    // 0 when the handle belongs to no quest alias. Common and not an error:
    // questDoor markers are engine-retargeted load doors carrying no alias.
    RE::FormID ResolveQuestFromHandle(std::uint32_t a_handle) {
        const auto it = g_handleToQuest.find(a_handle);
        if (it == g_handleToQuest.end() || !it->second) return 0;
        return it->second->GetFormID();
    }

    bool HookedAddMarker(const void* a_mgr, void* a_markerData, void* a_param3,
                         const std::uint32_t* a_refHandle, std::int32_t a_gotoFrame) {
        if (DiagnosticEnabled()) {
            g_hookBreadcrumb.sequence = g_hookSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            g_hookBreadcrumb.caller = reinterpret_cast<std::uint64_t>(_ReturnAddress());
            g_hookBreadcrumb.returnSlot = reinterpret_cast<std::uint64_t>(_AddressOfReturnAddress());
            g_hookBreadcrumb.manager = reinterpret_cast<std::uint64_t>(a_mgr);
            g_hookBreadcrumb.markerData = reinterpret_cast<std::uint64_t>(a_markerData);
            g_hookBreadcrumb.param3 = reinterpret_cast<std::uint64_t>(a_param3);
            g_hookBreadcrumb.refHandlePtr = reinterpret_cast<std::uint64_t>(a_refHandle);
            // Do not dereference solely for diagnostics. A bad pointer must still
            // fault in the original path so the recorded RIP remains meaningful.
            g_hookBreadcrumb.refHandle = 0;
            g_hookBreadcrumb.gotoFrame = a_gotoFrame;
            g_hookBreadcrumb.threadID = GetCurrentThreadId();
        }

        const auto* frames = GetFrameOffsets();
        const bool isQuest = frames && a_gotoFrame == static_cast<std::int32_t>(frames->quest);
        const bool isDoor = frames && a_gotoFrame == static_cast<std::int32_t>(frames->questDoor);
        if ((isQuest || isDoor) && a_refHandle) {
            if (auto refPtr = RE::TESObjectREFR::LookupByHandle(*a_refHandle)) {
                HUDMarkerHook::QuestMarker entry;
                entry.position = refPtr->GetPosition();
                entry.questID  = ResolveQuestFromHandle(*a_refHandle);
                entry.isDoor   = isDoor;

                std::lock_guard lock(g_mutex);
                g_pending.push_back(entry);
            }
        }
        return reinterpret_cast<AddMarkerFn>(g_trampolineAddr)(
            a_mgr, a_markerData, a_param3, a_refHandle, a_gotoFrame);
    }
}

namespace HUDMarkerHook {
    void Install() {
        REL::Relocation<std::uintptr_t> target{ kAddMarkerID };
        const std::uint64_t targetAddr = target.address();
        const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedAddMarker);
        g_targetAddr = targetAddr;

        std::vector<std::uint8_t> originalTargetBytes;
        if (DiagnosticEnabled()) {
            originalTargetBytes = ReadBytes(targetAddr, kTargetDiagnosticBytes);
            logger::info(
                "HUDMarkerHook SE diagnostic install | runtime={} | target={:X} | originalTargetBytes={}",
                REL::Module::get().version().string("."), targetAddr,
                FormatBytes(originalTargetBytes));
        }

        g_detour = std::make_unique<DiagnosticX64Detour>(targetAddr, hookAddr, &g_trampolineAddr);

        if (!g_detour->hook()) {
            logger::error("HUDMarkerHook: PolyHook x64Detour::hook() returned false");
            g_detour.reset();
            return;
        }

        logger::info("HUDMarkerHook installed | target={:X} | trampoline={:X}",
                     targetAddr, g_trampolineAddr);

        if (DiagnosticEnabled()) {
            // PolyHook may follow an entry jump before patching. Monitor the
            // address it actually detoured, not only the Address Library input.
            g_targetAddr = g_detour->ResolvedTargetAddress();
            const auto trampolineBytes = std::min<std::size_t>(
                kTrampolineDiagnosticBytes, g_detour->TrampolineSize());
            g_installedTargetBytes = ReadBytes(g_targetAddr, kTargetDiagnosticBytes);
            g_installedTrampolineBytes = ReadBytes(g_trampolineAddr, trampolineBytes);

            logger::info(
                "HUDMarkerHook SE diagnostic hooked | hookTarget={:X} | scheme={} | overwrittenBytes={} | resume={:X} | trampolineSize={}",
                g_targetAddr,
                PLH::x64Detour::printDetourScheme(g_detour->ChosenScheme()),
                g_detour->OverwrittenSize(), g_detour->ResumeAddress(),
                g_detour->TrampolineSize());
            logger::info("HUDMarkerHook SE diagnostic targetBytes={}",
                         FormatBytes(g_installedTargetBytes));
            logger::info("HUDMarkerHook SE diagnostic trampolineBytes={}",
                         FormatBytes(g_installedTrampolineBytes));
            logger::info("HUDMarkerHook SE diagnostic copiedInstructions:\n{}",
                         g_detour->OriginalInstructions());
            logger::info("HUDMarkerHook SE diagnostic trampolineInstructions:\n{}",
                         g_detour->TrampolineInstructions());
            ArmCrashDiagnostic();
        }
    }

    void LogPostLoadState() {
        if (!DiagnosticEnabled() || !g_detour || !g_targetAddr || !g_trampolineAddr) {
            return;
        }

        const auto targetBytes = ReadBytes(g_targetAddr, g_installedTargetBytes.size());
        const auto trampolineBytes = ReadBytes(
            g_trampolineAddr, g_installedTrampolineBytes.size());
        logger::info(
            "HUDMarkerHook SE diagnostic post-load | targetUnchanged={} | trampolineUnchanged={} | target={:X} | trampoline={:X} | resume={:X}",
            targetBytes == g_installedTargetBytes,
            trampolineBytes == g_installedTrampolineBytes,
            g_targetAddr, g_trampolineAddr, g_detour->ResumeAddress());
        logger::info("HUDMarkerHook SE diagnostic post-load targetBytes={}",
                     FormatBytes(targetBytes));
        logger::info("HUDMarkerHook SE diagnostic post-load trampolineBytes={}",
                     FormatBytes(trampolineBytes));
    }

    void EndPass() {
        {
            std::lock_guard lock(g_mutex);
            g_published.swap(g_pending);
            g_pending.clear();
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastAliasRebuild >= std::chrono::seconds(5)) {
            g_lastAliasRebuild = now;
            RebuildHandleToQuestMap();
        }
    }

    std::vector<QuestMarker> GetQuestMarkers() {
        std::lock_guard lock(g_mutex);
        return g_published;
    }
}
