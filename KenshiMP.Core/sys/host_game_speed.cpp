#include "host_game_speed.h"
#include <spdlog/spdlog.h>
#include <atomic>

namespace kmp::host_game_speed {

namespace {
std::atomic<bool> s_loggedDisabled{false};
}

void Tick(float /*deltaTime*/) {
    bool expected = false;
    if (s_loggedDisabled.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        spdlog::warn("host_game_speed: disabled on v1.0.68 - no proven live host time/speed source yet");
    }
}

float LastReportedSpeed() {
    return 1.0f;
}

} // namespace kmp::host_game_speed
