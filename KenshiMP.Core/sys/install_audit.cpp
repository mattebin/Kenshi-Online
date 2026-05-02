#include "install_audit.h"
#include "prologue_analyzer.h"
#include "callsite_analyzer.h"
#include "concurrency_watch.h"
#include "leak_watch.h"
#include "kmp/hook_manager.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <cstdio>
#include <string>

namespace kmp::install_audit {

namespace {

uintptr_t HostModuleBase() {
    HMODULE h = GetModuleHandleA(nullptr);
    return h ? reinterpret_cast<uintptr_t>(h) : 0;
}

std::string PrologueHex(const uint8_t* prologue, size_t n) {
    std::string out;
    char tmp[4];
    for (size_t i = 0; i < n; ++i) {
        sprintf_s(tmp, sizeof(tmp), "%02X", prologue[i]);
        out.append(tmp);
        if (i + 1 < n) out.push_back(' ');
    }
    return out;
}

} // namespace

void Emit() {
    // Run analyzer self-tests first so a regression in the analyzer itself
    // shows up at the top of the audit block, before any real hook checks
    // would silently misreport.
    prologue_analyzer::RunSelfTest();

    const uintptr_t hostBase = HostModuleBase();
    auto diags = HookManager::Get().GetDiagnostics();

    spdlog::info("=== KMP HOOK AUDIT BEGIN ===");
    spdlog::info("  hostModuleBase = 0x{:X}", hostBase);
    spdlog::info("  hooks total = {}", diags.size());
    spdlog::info("  ----");

    for (const auto& d : diags) {
        const uintptr_t rva = (hostBase != 0 && d.targetAddr > hostBase)
                                ? d.targetAddr - hostBase
                                : 0;

        // Prologue + callsite analysis are read-only — safe to call even on
        // hooks that aren't currently enabled.
        prologue_analyzer::Result pa{};
        callsite_analyzer::Result ca{};
        if (d.targetAddr != 0) {
            pa = prologue_analyzer::Analyze(d.targetAddr);
            ca = callsite_analyzer::FindAndAnalyzeOneCaller(d.targetAddr);
        }

        spdlog::info(
            "  [{}] addr=0x{:X} rva=0x{:X} installed={} enabled={} "
            "movRaxRsp={} calls={} crashes={}",
            d.name, d.targetAddr, rva,
            d.installed, d.enabled,
            d.hasMovRaxRspFix, d.callCount, d.crashCount);

        spdlog::info("    prologue: {}",
                     PrologueHex(d.prologue, sizeof(d.prologue)));

        if (pa.inferredArgCount > 0) {
            spdlog::info("    prologue-analyzer: {} ({}% conf)",
                         pa.summary, pa.confidence);
        } else {
            spdlog::info("    prologue-analyzer: {}", pa.summary);
        }

        if (ca.callSiteFound) {
            spdlog::info("    callsite-analyzer: {}", ca.summary);
        } else {
            spdlog::info("    callsite-analyzer: {}",
                         ca.summary.empty() ? std::string("not run") : ca.summary);
        }

        spdlog::info("    ----");
    }

    spdlog::info("=== KMP HOOK AUDIT END ===");

    // Concurrency + leak watchers belong in the same bug-report block —
    // one grep gets the maintainer everything they need.
    concurrency_watch::EmitSummary();
    leak_watch::SnapshotNow();
}

} // namespace kmp::install_audit
