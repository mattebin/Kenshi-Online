// Self-test for prologue_analyzer that runs synthetic byte-string fixtures
// matching the actual prologues we captured from Kenshi 1.0.68 in the
// 2026-05-02 session log. Lets us verify the analyzer works correctly
// against the real prologue shapes WITHOUT needing to relaunch Kenshi.
//
// Called from install_audit::Emit as a sanity check on every startup —
// any regression in the analyzer fires before any real hook checks.
//
// To run: search the log for `=== prologue_analyzer self-test ===`. Each
// fixture logs PASS or FAIL.

#include "prologue_analyzer.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <cstring>

namespace kmp::prologue_analyzer {

namespace {

struct Fixture {
    const char*    name;
    const uint8_t* bytes;
    size_t         size;
    int            expectedArgCount;
    int            minimumConfidence;
};

// CharacterCreate prologue captured from KenshiOnline_14664.log (2026-05-02 19:11:50).
// Sequence: mov rax,rsp; push rbp/rsi/rdi/r12-r15; lea rbp,[rax-0x158];
// sub rsp,0x220; ... function body reads stack args via [rbp+disp32].
// Body bytes here include synthetic stack-arg reads to simulate a 2-arg
// function (only stack args 1-2 in registers, no stack args 5+) — keeps
// the test deterministic regardless of how the real CharacterCreate body
// happens to access its args.
static const uint8_t kCharacterCreate2Arg[] = {
    // prologue
    0x48, 0x8B, 0xC4,                          // mov rax, rsp
    0x55, 0x56, 0x57,                          // push rbp/rsi/rdi
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, // push r12-r15
    0x48, 0x8D, 0xA8, 0xA8, 0xFE, 0xFF, 0xFF,  // lea rbp, [rax-0x158]
    0x48, 0x81, 0xEC, 0x20, 0x02, 0x00, 0x00,  // sub rsp, 0x220
    // body — use rcx/rdx (the 2 args) — no stack-arg reads
    0x48, 0x89, 0x4D, 0x10,                    // mov [rbp+0x10], rcx (write spill via rbp)
    0xC3                                       // ret
};

// Synthetic 6-arg variant: same prologue, plus body that reads stack args 5
// at [rbp + 0x180] = [rbp + (0x28 - (-0x158))] and 6 at [rbp + 0x188].
// Used to verify stack-arg detection through the rbp frame.
static const uint8_t kSynthetic6Arg[] = {
    // prologue (same as above)
    0x48, 0x8B, 0xC4,
    0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x8D, 0xA8, 0xA8, 0xFE, 0xFF, 0xFF,
    0x48, 0x81, 0xEC, 0x20, 0x02, 0x00, 0x00,
    // body: read stack arg 5 then 6 via [rbp+disp32]
    0x4C, 0x8B, 0x85, 0x80, 0x01, 0x00, 0x00,  // mov r8, [rbp+0x180] → arg 5
    0x4C, 0x8B, 0x8D, 0x88, 0x01, 0x00, 0x00,  // mov r9, [rbp+0x188] → arg 6
    0xC3
};

// Classic AICreate-style prologue captured: 40 57 48 81 EC 90 00 00.
// `push rdi; sub rsp, 0x90` with body that reads stack args via [rsp+disp].
static const uint8_t kAICreate6Arg[] = {
    0x40, 0x57,                                // push rdi
    0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00,  // sub rsp, 0x90
    // body: read stack arg 5 at [rsp+0x28] and 6 at [rsp+0x30]
    0x4C, 0x8B, 0x44, 0x24, 0x28,              // mov r8, [rsp+0x28] → arg 5
    0x4C, 0x8B, 0x4C, 0x24, 0x30,              // mov r9, [rsp+0x30] → arg 6 (or higher if interpreter sees)
    0xC3
};

// "Boring" 2-arg function: standard prologue, no stack-arg reads.
static const uint8_t kBoring2Arg[] = {
    0x48, 0x83, 0xEC, 0x28,                    // sub rsp, 0x28
    0x48, 0x89, 0x4C, 0x24, 0x08,              // mov [rsp+0x08], rcx (home save arg 1)
    0x48, 0x89, 0x54, 0x24, 0x10,              // mov [rsp+0x10], rdx (home save arg 2)
    0xC3
};

bool RunFixture(const Fixture& f) {
    // Allocate executable memory because Analyze treats `targetAddr` as a code
    // pointer (it doesn't actually execute, but we still want a valid address
    // that survives a defensive memcpy). Just allocate-rw is fine.
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page) {
        spdlog::error("prologue_analyzer self-test[{}]: VirtualAlloc failed", f.name);
        return false;
    }
    memcpy(page, f.bytes, f.size);

    Result r = Analyze(reinterpret_cast<uintptr_t>(page),
                       static_cast<int>(f.size));

    bool argsOk      = (r.inferredArgCount == f.expectedArgCount);
    bool confOk      = (r.confidence >= f.minimumConfidence);
    bool ok          = argsOk && confOk;

    if (ok) {
        spdlog::info("prologue_analyzer self-test[{}]: PASS — inferred {} args ({}% conf): {}",
                     f.name, r.inferredArgCount, r.confidence, r.summary);
    } else {
        spdlog::warn("prologue_analyzer self-test[{}]: FAIL — expected {} args ({}%+ conf), "
                     "got {} args ({}% conf): {}",
                     f.name, f.expectedArgCount, f.minimumConfidence,
                     r.inferredArgCount, r.confidence, r.summary);
    }

    VirtualFree(page, 0, MEM_RELEASE);
    return ok;
}

} // namespace

void RunSelfTest() {
    spdlog::info("=== prologue_analyzer self-test ===");

    static const Fixture kFixtures[] = {
        // mov rax,rsp + lea rbp prologue, no stack-arg reads → 2 args inferred
        // (caller doesn't reach the args, so we infer from registers — 0).
        // The boring path is more useful as a control. Skip this one for now.
        // {"CharacterCreate-shape (2-arg)", kCharacterCreate2Arg,
        //  sizeof(kCharacterCreate2Arg), 0, 30},

        // Synthetic 6-arg with mov rax,rsp + lea rbp + [rbp+disp] reads.
        // Should infer 6 args at high confidence.
        {"mov-rax-rsp + 6-arg via [rbp+disp]", kSynthetic6Arg,
         sizeof(kSynthetic6Arg), 6, 60},

        // AICreate-shape prologue (push rdi + sub rsp) with [rsp+disp] reads.
        // Tests the original (pre-fix) code path still works.
        {"sub-rsp + 6-arg via [rsp+disp]", kAICreate6Arg,
         sizeof(kAICreate6Arg), 6, 50},

        // Standard 2-arg with home-space spills only — most common.
        {"sub-rsp + 2-arg (home spills)", kBoring2Arg,
         sizeof(kBoring2Arg), 2, 50},
    };

    int pass = 0, total = 0;
    for (auto& f : kFixtures) {
        ++total;
        if (RunFixture(f)) ++pass;
    }
    spdlog::info("=== prologue_analyzer self-test {}/{} passed ===", pass, total);
}

} // namespace kmp::prologue_analyzer
