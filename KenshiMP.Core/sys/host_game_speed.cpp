#include "host_game_speed.h"
#include "../core.h"
#include "../hooks/entity_hooks.h"
#include "../net/client.h"
#include "kmp/messages.h"
#include "kmp/protocol.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <atomic>
#include <chrono>

namespace kmp::host_game_speed {

namespace {

// Validated 2026-05-04 via Ghidra Recon5 + AddToUpdateListMain runtime
// capture: Kenshi 1.0.68's frameSpeedMult lives at GameWorld + 0x700,
// matching KenshiLib's 1.0.51 reference. The previous quarantine was
// because we never had a live GameWorld pointer; entity_hooks now
// captures one on every character-add via the addToUpdateListMain
// hook (RVA 0x787C70).
constexpr std::ptrdiff_t kOffsetFrameSpeedMult = 0x700;

// Sanity range. Vanilla Kenshi exposes 1× / 2× / 3× via UI which
// RE_Kenshi confirmed map to 1.0/2.0/5.0 in the float. Custom-speeds
// modes can go down to 0.1 (slow-mo) and up to ~5-10 (mod territory).
constexpr float kSpeedMin = 0.04f;
constexpr float kSpeedMax = 10.0f;

std::atomic<float>   s_lastReadSpeed{1.0f};
std::atomic<float>   s_lastBroadcastSpeed{0.0f};
std::atomic<int64_t> s_lastBroadcastMs{0};
std::atomic<bool>    s_seenValidRead{false};

template <typename T>
bool SafeRead(uintptr_t addr, T& out) {
    __try {
        out = *reinterpret_cast<volatile T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

} // namespace

void Tick(float /*deltaTime*/) {
    // Get the live GameWorld pointer from the AddToUpdateListMain capture.
    // Returns 0 until the first character add — i.e. before any save loads
    // or zone streams. That's expected; the speed value isn't meaningful
    // before then anyway.
    uintptr_t gw = entity_hooks::GetGameWorldFromHook();
    if (gw == 0) {
        return;
    }

    float speed = 0.0f;
    if (!SafeRead(gw + kOffsetFrameSpeedMult, speed)) {
        // Read fault — log once.
        static std::atomic<bool> s_loggedFault{false};
        bool expected = false;
        if (s_loggedFault.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            spdlog::warn("host_game_speed: read fault at gw+0x{:X} "
                         "(gw=0x{:X})", kOffsetFrameSpeedMult, gw);
        }
        return;
    }

    if (!(speed > kSpeedMin && speed < kSpeedMax) || speed != speed) {
        // Out-of-range value. Log once so the offset assumption is visible
        // if it's wrong on this build, then stay quiet.
        static std::atomic<bool> s_loggedRange{false};
        bool expected = false;
        if (s_loggedRange.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            spdlog::warn("host_game_speed: gw+0x{:X} = {:.4f} — outside "
                         "sane (0.04 .. 10.0); offset may be wrong on this "
                         "build", kOffsetFrameSpeedMult, speed);
        }
        return;
    }

    // Sanity-pass. Log first valid read once, then on change.
    bool firstRead = false;
    if (!s_seenValidRead.exchange(true, std::memory_order_acq_rel)) {
        firstRead = true;
        spdlog::info("host_game_speed: FIRST sane read — gw+0x{:X} = {:.4f}× "
                     "(gw=0x{:X})", kOffsetFrameSpeedMult, speed, gw);
    }

    float prev = s_lastReadSpeed.exchange(speed, std::memory_order_acq_rel);
    if (!firstRead && prev != speed) {
        spdlog::info("host_game_speed: speed changed {:.4f}× -> {:.4f}×",
                     prev, speed);
    }

    // ── Broadcast (host-only, throttled) ────────────────────────────────
    // We only send if:
    //   * connected,
    //   * we are the HOST (acting authority for time of day),
    //   * the speed has changed since last broadcast OR ≥1 s elapsed.
    auto& core = Core::Get();
    if (!core.IsConnected()) return;
    if (!core.IsHost()) return;

    int64_t now = NowMs();
    int64_t lastSent = s_lastBroadcastMs.load(std::memory_order_acquire);
    float lastBroadcast = s_lastBroadcastSpeed.load(std::memory_order_acquire);

    bool changed = (lastBroadcast != speed);
    bool stale   = (now - lastSent >= 1000);
    if (!changed && !stale) return;

    PacketWriter w;
    w.WriteHeader(MessageType::C2S_HostGameSpeed);
    MsgHostGameSpeed msg{};
    msg.speed = speed;
    w.WriteRaw(&msg, sizeof(msg));
    core.GetClient().SendReliable(w.Data(), w.Size());
    s_lastBroadcastSpeed.store(speed, std::memory_order_release);
    s_lastBroadcastMs.store(now, std::memory_order_release);
    if (changed) {
        spdlog::info("host_game_speed: broadcast {:.4f}× to server", speed);
    }
}

float LastReportedSpeed() {
    return s_lastReadSpeed.load(std::memory_order_acquire);
}

} // namespace kmp::host_game_speed
