#include "time_hooks.h"
#include "../core.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <atomic>

namespace kmp::time_hooks {

using TimeUpdateFn = void(__fastcall*)(void* timeManager, float deltaTime);

static TimeUpdateFn s_origTimeUpdate = nullptr;
static float s_serverTimeOfDay = 0.5f;
static float s_serverGameSpeed = 1.0f;
static bool  s_hasServerTime = false;

// Diagnostic only. On Kenshi 1.0.68 this hook has been observed not to fire,
// and the old TimeManager +0x08/+0x10 fields are not a proven live path.
static void* s_timeManager = nullptr;
static std::atomic<bool> s_loggedUnsupportedApply{false};
static std::atomic<bool> s_loggedTimeUpdateFired{false};

void SetServerTime(float timeOfDay, float gameSpeed) {
    s_serverTimeOfDay = timeOfDay;
    s_serverGameSpeed = gameSpeed;
    s_hasServerTime = true;

    bool expected = false;
    if (s_loggedUnsupportedApply.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        spdlog::warn("time_hooks: TimeSync apply disabled on v1.0.68 - no proven live client write path "
                     "(received tod={:.4f}, speed={:.2f})", timeOfDay, gameSpeed);
    }
}

float GetTimeOfDay() {
    return 0.5f;
}

float GetGameSpeed() {
    return 1.0f;
}

bool WriteTimeOfDay(float timeOfDay) {
    (void)timeOfDay;
    return false;
}

bool HasTimeManager() {
    return s_timeManager != nullptr;
}

static void LogTimeUpdateFiredOnce(void* timeManager) {
    bool expected = false;
    if (s_loggedTimeUpdateFired.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        spdlog::warn("time_hooks: TIME_UPDATE fired unexpectedly at manager=0x{:X}; "
                     "legacy TimeManager offsets remain quarantined until runtime-proven live",
                     reinterpret_cast<uintptr_t>(timeManager));
    }
}

static void __fastcall Hook_TimeUpdate(void* timeManager, float deltaTime) {
    if (!s_timeManager) {
        s_timeManager = timeManager;
        LogTimeUpdateFiredOnce(timeManager);
    }

    __try {
        s_origTimeUpdate(timeManager, deltaTime);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        spdlog::error("time_hooks: TimeUpdate trampoline CRASHED!");
        return;
    }

    // Diagnostic only. Present/render hook is the proven OnGameTick driver.
    static int s_timeHookCallCount = 0;
    s_timeHookCallCount++;
    if (s_timeHookCallCount <= 5 || s_timeHookCallCount % 3000 == 0) {
        char buf[160];
        sprintf_s(buf, "KMP: Hook_TimeUpdate diagnostic #%d (dt=%.4f, sync apply disabled)\n",
                  s_timeHookCallCount, deltaTime);
        OutputDebugStringA(buf);
    }
}

bool Install() {
    auto& funcs = Core::Get().GetGameFunctions();
    auto& hookMgr = HookManager::Get();

    if (funcs.TimeUpdate) {
        if (hookMgr.InstallAt("TimeUpdate",
                              reinterpret_cast<uintptr_t>(funcs.TimeUpdate),
                              &Hook_TimeUpdate, &s_origTimeUpdate)) {
            spdlog::info("time_hooks: TimeUpdate hook installed for dead-path diagnostics only; "
                         "not used for time sync or OnGameTick on v1.0.68");
        }
    }

    spdlog::info("time_hooks: Installed (TimeUpdate={})", funcs.TimeUpdate != nullptr);
    return true;
}

void Uninstall() {
    HookManager::Get().Remove("TimeUpdate");
}

} // namespace kmp::time_hooks
