#pragma once
//
// Call-site analyzer — finds a `call <target>` instruction in the host
// module's .text section, walks backward, and counts how many MSVC
// __fastcall arg slots the caller sets up before the call.
//
// Cross-checks prologue_analyzer: prologue analysis tells us how many args
// the callee READS, this tells us how many args at least one caller WRITES.
// When they agree we're confident; when they disagree there's something to
// investigate (varargs, tail call, calling-convention violation, or — most
// commonly — our typedef is wrong).
//
// Looks for setup of:
//   * RCX / RDX / R8 / R9     (the 4 register-arg slots — REX-prefixed mov,
//                              mov ax, rXX, lea, xor self-zero, etc.)
//   * [rsp+0x28] / +0x30 / ..  (stack-arg slots — write of any reg or imm
//                              into the post-shadow-space stack region)
//
// Stops walking back when it hits another CALL/RET/JMP, gives up after a
// configurable instruction budget. Conservative: only counts arg-slot
// writes when we're sure the destination is the right slot — better to
// under-count than over-count.

#include <cstdint>
#include <string>

namespace kmp::callsite_analyzer {

struct Result {
    // True iff we found at least one CALL whose displacement resolves to the
    // target address.
    bool callSiteFound = false;

    // The address of the CALL instruction we analyzed (so a human can pull
    // up Ghidra at this RVA for verification).
    uintptr_t callSiteAddr = 0;

    // True iff the function pointer was found inside .rdata (i.e. dispatched
    // via a vtable). We can't infer arg count from a vtable slot alone, but
    // knowing it's vtable-dispatched explains why no `call rel32` exists and
    // is the right hint for "open Ghidra at this address to find callers."
    bool      inVtable     = false;
    uintptr_t vtableSlot   = 0;

    // Bitmask: bit 0 = RCX written, bit 1 = RDX, bit 2 = R8, bit 3 = R9.
    int registerArgsSet = 0;

    // Highest stack-arg index written, 1-based, where 5 = [rsp+0x28].
    // 0 if no stack arg writes seen.
    int highestStackArgSet = 0;

    // Inferred arg count = popcount(registerArgsSet) + (max(0, stack args)).
    // Conservative: we report the highest contiguous slot set, not just the
    // highest set, because non-contiguous writes are usually noise.
    int inferredArgCount = 0;

    // Distance backward we walked (instruction count).
    int instructionsScanned = 0;

    // Human-readable summary.
    std::string summary;
};

// Find ONE caller of `targetAddr` in the host module's .text and analyze
// its arg setup. `searchBytes` caps how much .text we scan looking for the
// CALL (default 16 MiB — most Kenshi functions are reachable).
Result FindAndAnalyzeOneCaller(uintptr_t targetAddr,
                               size_t searchBytes = 16 * 1024 * 1024);

// Convenience: log the call-site analysis under tag "{tag}", noting whether
// it agrees with the typedef arg count. Returns true when the analyzer's
// result disagrees with high confidence.
bool VerifyArgCount(const char* tag, uintptr_t targetAddr, int expectedArgCount);

} // namespace kmp::callsite_analyzer
