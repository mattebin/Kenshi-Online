#pragma once
//
// Install audit — dumps a comprehensive snapshot of every installed hook
// in one block. Designed to land in the log file at a known, greppable
// boundary so anyone debugging a problem can copy that block as a
// bug-report attachment without grepping the whole session log.
//
// What it logs per hook:
//   * Name, target address, RVA
//   * Enabled / installed / has-MovRaxRsp-fix flags
//   * Prologue bytes (32 hex)
//   * Prologue-analyzer arg-count inference + confidence
//   * Call-site analyzer arg-count inference + confidence (if a caller is
//     reachable in .text)
//   * Live counters (callCount, crashCount) at the time of the audit
//
// Surrounded by `=== KMP HOOK AUDIT BEGIN ===` / `=== END ===` markers so
// it's easy to extract programmatically.

namespace kmp::install_audit {

// Emit one audit block to spdlog at info level. Safe to call any time
// after HookManager has installed at least the early hooks. Cheap (no
// I/O outside spdlog), but does iterate every hook so don't spam.
void Emit();

} // namespace kmp::install_audit
