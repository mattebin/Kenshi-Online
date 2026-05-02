#pragma once
//
// Prologue analyzer — infers a function's argument count from its x64 prologue
// and early body, before any hook is installed. Designed to catch the class of
// bug we just hit with `AI::create`: typedef said 3 args, function actually
// took 6, the missing 3 left this+0x318 / this+0x10 with garbage and the game
// crashed deferred at `game+0x59820D` ~5s later.
//
// The analyzer is conservative — it returns a confidence value alongside the
// inferred arg count so callers can warn rather than assert. False positives
// (analyzer says "looks like N args" when typedef says M) become startup
// warnings in the log; humans still make the call.
//
// What we detect:
//   * Stack-frame setup:        push rXX  /  sub rsp, imm
//   * Home-space register save: mov [rsp+0x08/0x10/0x18/0x20], RCX/RDX/R8/R9
//                               (MSVC __fastcall spills caller's reg-args here)
//   * Stack-arg reads:          mov rXX, [rsp+0x28+]
//                               (MSVC reads arg 5 at [rsp+0x28], arg 6 at +0x30,
//                                etc. The +0x28 base = 0x20 (caller-allocated
//                                shadow space, callee-relative) + 0x08 (return
//                                address pushed by CALL).)
//
// Inferred arg count = max(spilled register index, stack-arg index) + 1.

#include <cstdint>
#include <string>

namespace kmp::prologue_analyzer {

struct Result {
    // Highest register-arg index spilled to home space, 1-based. 0 if none seen.
    //   1 = RCX spilled at [rsp+0x08]
    //   2 = RDX spilled at [rsp+0x10]
    //   3 = R8  spilled at [rsp+0x18]
    //   4 = R9  spilled at [rsp+0x20]
    int registerArgsSpilled = 0;

    // Highest stack-arg index read, 1-based. 0 if none seen.
    // Stack args start at index 5 (positions [rsp+0x28], [rsp+0x30], ...).
    int highestStackArgRead = 0;

    // Inferred total arg count = max(register-args, stack-args). Conservative —
    // a function may take fewer args than it touches the home space for, but
    // never more than its highest stack read.
    int inferredArgCount = 0;

    // 0..100. High when we saw a clean prologue with explicit home-space spills
    // for all register args + matching stack-arg reads. Low when the function
    // is short or the prologue is unusual (no rsp adjust, tail call, etc.).
    int confidence = 0;

    // Bytes successfully decoded before the analyzer stopped (for diagnostics).
    int bytesScanned = 0;

    // Hex dump of the first 32 prologue bytes — useful when comparing across
    // Kenshi builds to know whether a signature shift means "same function,
    // different bytes" or "scanner found the wrong target."
    std::string prologueHex;

    // Human-readable summary, e.g. "spills RCX/RDX/R8 (3 reg args), reads
    // [rsp+0x28] and [rsp+0x30] (2 stack args), inferred 5 args (75% conf)".
    std::string summary;
};

// Analyze the function at `targetAddr`. Reads up to `scanBytes` bytes (default
// 256 — long enough for any reasonable prologue without false-positiving on
// later code). Wrapped in SEH so a bad address can't crash us.
Result Analyze(uintptr_t targetAddr, int scanBytes = 256);

// Run a synthetic-fixture self-test of the analyzer. Logs PASS/FAIL for each
// fixture. Designed to catch analyzer regressions at startup before any real
// hook decisions get made on the live binary. Cheap (microseconds).
void RunSelfTest();

// Convenience: log the analysis under tag "{tag}" if it disagrees with
// `expectedArgCount`. Returns true when the analyzer's confident inference
// disagrees with the typedef. Caller decides whether to warn or fail.
//
// Logs at info level when it agrees, warn when it disagrees with high
// confidence, debug when it disagrees with low confidence (might just be
// a short function the analyzer couldn't read confidently).
bool VerifyArgCount(const char* tag, uintptr_t targetAddr, int expectedArgCount);

} // namespace kmp::prologue_analyzer
