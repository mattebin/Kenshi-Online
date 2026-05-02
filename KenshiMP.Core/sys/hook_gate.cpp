#include "hook_gate.h"
#include "../core.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <cstdio>
#include <cstring>

namespace kmp::hook_gate {

namespace {

void Trim(std::string& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' ||
                          s.back()  == '\r' || s.back()  == '\n')) {
        s.pop_back();
    }
}

// Returns the raw config string and populates outSource with FILE/ENV/NONE.
std::string Load(std::string& outSource) {
    // 1) File next to kenshi_x64.exe
    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, sizeof(exePath));
    std::string exeDir = exePath;
    size_t slash = exeDir.find_last_of("\\/");
    if (slash != std::string::npos) exeDir.resize(slash);
    const std::string filePath = exeDir + "\\kmp_disable_hooks.txt";

    FILE* f = nullptr;
    if (fopen_s(&f, filePath.c_str(), "r") == 0 && f) {
        char line[1024] = {};
        std::string content;
        while (fgets(line, sizeof(line), f)) {
            std::string s(line);
            // Strip line comments (anything from '#' onward).
            size_t hash = s.find('#');
            if (hash != std::string::npos) s.resize(hash);
            Trim(s);
            if (s.empty()) continue;
            if (!content.empty()) content += ",";
            content += s;
        }
        fclose(f);
        Trim(content);
        if (!content.empty()) {
            outSource = "FILE: " + filePath;
            return content;
        }
        outSource = "FILE: " + filePath + " (empty)";
        return "";
    }

    // 2) Env-var fallback
    char buf[1024] = {};
    DWORD len = GetEnvironmentVariableA("KMP_DISABLE_HOOKS", buf, sizeof(buf));
    if (len > 0 && len < sizeof(buf)) {
        std::string s(buf, len);
        Trim(s);
        if (!s.empty()) {
            outSource = "ENV: KMP_DISABLE_HOOKS";
            return s;
        }
    }

    outSource = "NONE (file " + filePath + " not found, env var unset)";
    return "";
}

struct Cache {
    std::string list;
    std::string source;
};

const Cache& GetCache() {
    static const Cache cache = []() {
        Cache c;
        c.list = Load(c.source);

        // Loud startup banner — also goes to KenshiOnline_<pid>.log via spdlog.
        spdlog::info("=========================================================");
        spdlog::info("  Hook-disable gate active");
        spdlog::info("    source: {}", c.source);
        spdlog::info("    value : \"{}\"", c.list);
        spdlog::info("=========================================================");

        // OutputDebugString in case the log file isn't being tailed.
        const std::string ods =
            "KMP: HookDisable source=" + c.source + " value=\"" + c.list + "\"\n";
        OutputDebugStringA(ods.c_str());

        // In-game HUD line, visible without leaving the game.
        const std::string hudMsg = c.list.empty()
            ? "no overrides (all hooks enabled)"
            : "DISABLE=" + c.list + " (" + c.source + ")";
        Core::Get().GetNativeHud().LogStep("GATE", hudMsg);

        return c;
    }();
    return cache;
}

} // namespace

void EnsureLoaded() {
    (void)GetCache();
}

bool IsDisabled(const char* name) {
    const Cache& cache = GetCache();
    if (cache.list.empty()) return false;

    // Wildcard: every hook except render.
    if (cache.list == "*" || cache.list == "all") {
        return std::strcmp(name, "render") != 0;
    }

    // Comma-separated whitelist match.
    const std::string target(name);
    size_t pos = 0;
    while (pos < cache.list.size()) {
        const size_t comma = cache.list.find(',', pos);
        std::string token = cache.list.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        Trim(token);
        if (token == target) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

} // namespace kmp::hook_gate
