// =========================================================================
//                          KenshiMP.SafeAddon
// =========================================================================
// Implementation: worker thread + SEH-guarded sampling helpers.
//
// SEH boundary
// ------------
// Every read from game memory funnels through `SafeRead64`, which is a
// file-scope helper.  C++ unwinding and `__try`/`__except` can't share
// a function (C2712), so the pattern is to delegate the bare read to a
// no-RAII helper and let the caller wrap it with anything that needs
// destructors.  A failed read returns false and the caller bails out
// of that sample — no exception ever escapes the worker.
#include "safe_addon.h"

#include <Windows.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <atomic>
#include <chrono>
#include <utility>

namespace kmp::safe {
namespace {

// File-scope SEH-guarded 64-bit pointer read.  Uses MEM_BASIC_INFORMATION
// pre-checks to skip obviously-invalid pages (faster than waiting for
// an AV in the common case where Kenshi handed us a NULL pointer).
__declspec(noinline)
static bool SafeRead64(uintptr_t addr, uintptr_t& outVal) {
    if (addr < 0x10000ULL) return false;
    if (addr > 0x00007FFFFFFFFFFFULL) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) {
        return false;
    }
    __try {
        outVal = *reinterpret_cast<volatile uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline)
static bool SafeReadF32(uintptr_t addr, float& outVal) {
    uintptr_t qword = 0;
    if (!SafeRead64(addr & ~uintptr_t{0x3}, qword)) return false;
    // Recover the 32-bit float at the right alignment.
    __try {
        outVal = *reinterpret_cast<volatile float*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Lightweight per-process logger so the addon doesn't have to share
// anyone else's spdlog default logger.  Log file lives next to all the
// other Kenshi-online logs so KenshiMP.LogTail picks it up by pattern.
static std::shared_ptr<spdlog::logger> EnsureLogger() {
    static std::shared_ptr<spdlog::logger> g_log;
    if (g_log) return g_log;
    try {
        char path[MAX_PATH]{};
        wchar_t wpath[MAX_PATH]{};
        GetModuleFileNameW(GetModuleHandleW(L"KenshiMP.SafeAddon.dll"),
                           wpath, MAX_PATH);
        // Strip filename, append our log filename.
        for (int i = (int)wcslen(wpath) - 1; i >= 0; --i) {
            if (wpath[i] == L'\\' || wpath[i] == L'/') {
                wpath[i] = 0; break;
            }
        }
        wcscat_s(wpath, MAX_PATH, L"\\KenshiOnline_SafeAddon.log");
        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, MAX_PATH, nullptr, nullptr);
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            path, /*truncate=*/true);
        g_log = std::make_shared<spdlog::logger>("safe_addon", sink);
        g_log->set_level(spdlog::level::debug);
        g_log->flush_on(spdlog::level::warn);
    } catch (...) {
        // If logger init fails, fall back to a null logger so call
        // sites don't have to null-check.  Worker thread will see
        // empty messages but otherwise keep running.
        g_log = spdlog::default_logger();
    }
    return g_log;
}

} // unnamed namespace

SafeAddon& SafeAddon::Get() {
    static SafeAddon instance;
    return instance;
}

void SafeAddon::Start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel)) {
        return; // already running
    }
    auto log = EnsureLogger();
    log->info("KenshiMP.SafeAddon starting (sampleInterval={}ms, "
              "heartbeat={}ms, maxEntities={})",
              m_opts.sampleInterval.count(),
              m_opts.heartbeatInterval.count(),
              m_opts.maxEntities);
    m_worker = std::thread([this]() { this->WorkerMain(); });
}

void SafeAddon::Stop() {
    bool expected = true;
    if (!m_running.compare_exchange_strong(expected, false,
                                            std::memory_order_acq_rel)) {
        return; // already stopped
    }
    auto log = EnsureLogger();
    log->info("KenshiMP.SafeAddon stopping");
    if (m_worker.joinable()) m_worker.join();
    log->info("KenshiMP.SafeAddon stopped cleanly");
}

SafeWorldSnapshot SafeAddon::SnapshotCopy() const {
    std::lock_guard lk(m_snapMutex);
    return m_lastSnap;
}

void SafeAddon::WorkerMain() {
    auto log = EnsureLogger();
    auto lastHeartbeat = std::chrono::steady_clock::now();
    uint64_t frameIdx = 0;
    while (m_running.load(std::memory_order_acquire)) {
        SafeWorldSnapshot snap;
        snap.frameIndex = frameIdx++;
        snap.sampledAt = std::chrono::steady_clock::now();

        OnFrameStart(snap);
        // OnFrameStart populates the snapshot.  All sampling work
        // already happened there; OnFrameEnd is for outbound work
        // (network sends, command dispatch, etc.).
        OnFrameEnd(snap);

        // Publish the snapshot so external callers can copy it.
        {
            std::lock_guard lk(m_snapMutex);
            m_lastSnap = std::move(snap);
        }

        // Heartbeat — once every second, log we're alive.  Without
        // this the worker is invisible until something happens.
        auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat >= m_opts.heartbeatInterval) {
            lastHeartbeat = now;
            auto& s = m_lastSnap;
            log->info(
                "heartbeat: frame={}, player=({:.1f},{:.1f},{:.1f}) "
                "name='{}' fac='{}' isPlayer={} | entities sampled={}",
                s.frameIndex,
                s.localPlayer.posX, s.localPlayer.posY, s.localPlayer.posZ,
                s.localPlayer.name, s.localPlayer.factionName,
                s.localPlayer.isPlayer ? 1 : 0,
                s.entities.size());
        }

        std::this_thread::sleep_for(m_opts.sampleInterval);
    }
}

void SafeAddon::OnFrameStart(SafeWorldSnapshot& snap) {
    // Sample the local player.  Failure here is non-fatal — we just
    // keep the snapshot's localPlayer at default and try again next
    // tick.  The worker never gives up until Stop() is called.
    SamplePlayer(snap.localPlayer);

    // Sampling the entity list is the next milestone.  Holding off on
    // it here in the MVP until we wire up ClientConfig + the entity
    // iterator.  Snapshot ships with localPlayer populated but
    // entities empty; OnFrameEnd handles the empty case.
}

void SafeAddon::OnFrameEnd(const SafeWorldSnapshot& /*snap*/) {
    // Outbound work hooks in here:
    //   - send the local player's position to the server
    //   - dispatch any queued commands from the network thread
    //   - drive the spawn pipeline against our own SafeWorldSnapshot
    // MVP is empty so that the loop scaffolding lands first; each
    // outbound subsystem will plug into this method one at a time
    // without needing to touch any game-side instrumentation.
}

bool SafeAddon::SamplePlayer(SafeEntityState& outPlayer) {
    // Resolve `kenshi_x64.exe` once (the lookup is cheap but worth
    // caching).  We reach into the host process via GetModuleHandle
    // because `KenshiMP.SafeAddon.dll` is loaded into Kenshi's address
    // space by Ogre, so all addresses are local.
    static HMODULE s_host = nullptr;
    if (!s_host) s_host = GetModuleHandleW(L"kenshi_x64.exe");
    if (!s_host) return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(s_host);

    // PlayerBase singleton — same RVA the legacy resolver uses; we
    // hard-code it for the MVP and will refactor to a runtime-resolved
    // pointer once the addon owns its own scanner.  (RVA from
    // KenshiOnline_<pid>.log, validated across ten+ test sessions.)
    constexpr uintptr_t kPlayerBaseRva = 0x28FF908;
    uintptr_t playerCharPtr = 0;
    {
        uintptr_t pbAddr = base + kPlayerBaseRva;
        uintptr_t pbVal = 0;
        if (!SafeRead64(pbAddr, pbVal)) return false;
        if (pbVal == 0) return false;
        playerCharPtr = pbVal;
    }

    // RootObject layout (re_kenshi 2/manual_findings/README.md):
    //   +0x10  Faction* owner
    //   +0x18  std::string name
    //   +0x48  Vector3 position
    outPlayer.gameAddr = playerCharPtr;

    SafeReadF32(playerCharPtr + 0x48, outPlayer.posX);
    SafeReadF32(playerCharPtr + 0x4C, outPlayer.posY);
    SafeReadF32(playerCharPtr + 0x50, outPlayer.posZ);

    // Faction probe — read the pointer; if it points at a heap-shaped
    // address, that's our faction.  Don't try to read the faction's
    // name string in the MVP; that involves SSO-aware std::string
    // reading we'll add when the snapshot grows.
    uintptr_t factionPtr = 0;
    if (SafeRead64(playerCharPtr + 0x10, factionPtr) && factionPtr != 0) {
        // PlayerInterface field at Faction+0x250 is the "isPlayer"
        // marker (per manual_findings).  Non-null = player faction.
        uintptr_t isPlayerPtr = 0;
        if (SafeRead64(factionPtr + 0x250, isPlayerPtr)) {
            outPlayer.isPlayer = (isPlayerPtr != 0);
        }
    }

    return true;
}

} // namespace kmp::safe
