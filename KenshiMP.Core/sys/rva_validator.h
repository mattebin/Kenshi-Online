#pragma once
//
// rva_validator — survey every documented RVA at startup, classify what's
// actually at that address in the running binary, log a per-RVA verdict.
//
// Why we need this:
//   KenshiLib documents 1.0.51-era RVAs in its headers; RE_Kenshi ships
//   .br RVA files for up to Kenshi 1.0.65; the user is on 1.0.68. None of
//   the documented RVAs are guaranteed to point at function entries in our
//   binary. Hard-coding RVAs and silently failing at install (Bugs 1, 2, 6
//   of the 2026-05-03 verification round) is the wrong default.
//
// What this module does:
//   At startup, walk a fixed table of (name, rva, expected-shape) entries.
//   For each, read the first ~16 bytes from base + rva, classify into one
//   of {function-entry, jump-thunk, mid-instruction, INT3-padding,
//   unreadable}, and log per-entry. Roll-up at the end:
//     "rva_validator: 8 verified, 4 wrong-target, 1 unreadable — see warns"
//
// The result is the same survey that took us a manual byte-dump to do
// after the failed test. Now it runs at every Kenshi launch automatically.

#include <cstdint>
#include <string>

namespace kmp::rva_validator {

enum class Verdict {
    Unknown,         // not yet checked / dispatch error
    Unreadable,      // memcpy failed at base + rva
    IntPadding,      // first byte CC (or all CCs) — wrong target
    LikelyFunction,  // first byte matches a plausible function-prologue opcode
    JumpThunk,       // first byte E9/EB — jump, possibly to the real body
    MidInstruction,  // first byte doesn't fit either category
};

struct Entry {
    const char* name;        // "Character::isDead"
    uintptr_t   rva;         // documented RVA (e.g. 0x620E30)
    const char* sourceTag;   // "KenshiLib v1.0.51 header"
};

struct Result {
    const Entry* entry;
    Verdict      verdict;
    uint8_t      firstBytes[16];
    uintptr_t    resolvedAddr;  // base + rva
    uintptr_t    jumpTarget;    // 0 unless verdict == JumpThunk
};

// Validate every entry in the built-in table. Logs each result and a
// roll-up. Returns count of LikelyFunction + JumpThunk verdicts as a
// rough "how many usable RVAs do we have".
int RunBuiltinSurvey();

// Single-entry helper for ad-hoc validation outside the survey table.
// Logs the result the same way the survey does.
Result ValidateOne(const Entry& e);

} // namespace kmp::rva_validator
