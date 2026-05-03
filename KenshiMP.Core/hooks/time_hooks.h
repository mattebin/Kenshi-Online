#pragma once
namespace kmp::time_hooks {
    bool Install();
    void Uninstall();

    // v1.0.68 quarantine: this records incoming server time for diagnostics
    // only. The old TimeManager write path is not proven live and is not used.
    void SetServerTime(float timeOfDay, float gameSpeed);

    // Legacy diagnostics only. These return sentinels unless TIME_UPDATE ever
    // fires; callers must not use them for authoritative sync on v1.0.68.
    float GetTimeOfDay();
    float GetGameSpeed();
    bool  WriteTimeOfDay(float timeOfDay);
    bool  HasTimeManager();
}
