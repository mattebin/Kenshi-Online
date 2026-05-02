#include "prologue_analyzer.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace kmp::prologue_analyzer {

namespace {

// SEH-protected memcpy of N bytes. Returns false if the read AVs.
bool SafeMemcpy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Return offset stored in a 1-byte SIB displacement byte for `[rsp+disp8]`
// or 0 if not actually that mode. Caller already verified opcode + ModR/M.
inline int ReadDisp8(const uint8_t* p) { return static_cast<int8_t>(*p); }

// Most prologue / early-body home-space writes and stack reads use the
// pattern `REX.W [opcode] [ModR/M] [SIB=0x24] [disp8]`:
//   REX.W = 0x48 or 0x4C (depending on src register)
//   opcode = 0x89 (MOV r/m64, r64)  for spills
//          = 0x8B (MOV r64, r/m64)  for reads
//   ModR/M = 0x44/0x4C/0x54/0x5C for ax/cx/dx/bx-as-source-or-dest, low ModR/M
//          = 0x44 RAX, 0x4C RCX, 0x54 RDX, 0x5C RBX with REX bit cleared
//          With REX.R bit set (REX 0x4C), 0x44 = R8, 0x4C = R9, etc.
//   SIB = 0x24 means [rsp+disp]
//   disp8 = signed 8-bit displacement
//
// We also handle disp32 form: opcode reaches `[rsp+disp32]` via different
// ModR/M encoding (`0x84/0x8C/0x94/0x9C` instead of `0x44/...`).

// Identify which physical register a spill or read uses, given the REX prefix
// byte and the ModR/M byte (only the reg field at bits 5:3 matters).
//
// Returns: 0 RAX, 1 RCX, 2 RDX, 3 RBX, 4 RSP/(none), 5 RBP, 6 RSI, 7 RDI.
// REX.R extends to 8..15.
int DecodeRegFromModRM(uint8_t rex, uint8_t modrm) {
    int regBase = (modrm >> 3) & 0x07;
    if (rex & 0x04) regBase += 8;  // REX.R
    return regBase;
}

// Append a hex pair to outHex with optional separator. Caller manages size.
void AppendByte(std::string& outHex, uint8_t b, bool addSpace) {
    char tmp[4];
    sprintf_s(tmp, sizeof(tmp), addSpace ? "%02X " : "%02X", b);
    outHex.append(tmp);
}

// Build the prologue hex string from the buffer.
std::string FormatPrologueHex(const uint8_t* buf, int n) {
    std::string out;
    out.reserve(n * 3);
    for (int i = 0; i < n; ++i) {
        AppendByte(out, buf[i], i + 1 < n);
    }
    return out;
}

// Translate one of the four MSVC home-space spill offsets to a 1-based
// register-arg index. Returns 0 if the offset doesn't match.
int HomeSpaceOffsetToArgIdx(int disp) {
    switch (disp) {
        case 0x08: return 1;  // RCX → arg 1
        case 0x10: return 2;  // RDX → arg 2
        case 0x18: return 3;  // R8  → arg 3
        case 0x20: return 4;  // R9  → arg 4
        default:   return 0;
    }
}

// Translate a stack read at [rsp+disp] to a 1-based stack-arg index.
// Stack arg N (N >= 5) lives at [rsp + 0x28 + 8*(N-5)] in MSVC __fastcall:
//   arg 5 → 0x28
//   arg 6 → 0x30
//   arg 7 → 0x38
//   ...
// disp under 0x28 is locals or saved regs, not args.
int StackReadOffsetToArgIdx(int disp) {
    if (disp < 0x28) return 0;
    if ((disp - 0x28) % 8 != 0) return 0;
    return 5 + (disp - 0x28) / 8;
}

} // namespace

Result Analyze(uintptr_t targetAddr, int scanBytes) {
    Result r;
    if (targetAddr == 0 || scanBytes <= 0) return r;
    scanBytes = (std::min)(scanBytes, 1024);

    // Read a defensive copy of the function bytes — never decode in-place
    // because the page might be unmapped or the function might be relocated
    // mid-analysis by a JIT.
    uint8_t buf[1024];
    if (!SafeMemcpy(buf, reinterpret_cast<void*>(targetAddr),
                    static_cast<size_t>(scanBytes))) {
        r.summary = "memcpy AV at target address";
        return r;
    }

    r.prologueHex = FormatPrologueHex(buf, (std::min)(scanBytes, 32));

    // Walk the buffer looking for our recognized instruction shapes. We don't
    // need a full decoder — we only care about the few patterns that hint at
    // arg count. Anything we don't recognize is skipped a byte at a time.
    int homeSpills[5] = {0};   // count[1..4] of spills observed for arg index 1..4
    int stackReadsByArg[64] = {0}; // up to arg 64

    int rspAdjusted = 0;       // sub rsp, imm seen?
    int recognizedCount = 0;   // count of decoded instructions, for confidence

    // ── mov rax,rsp prologue tracking ──
    // Functions that start with `48 8B C4` (mov rax, rsp) capture the original
    // RSP into RAX, then push N callee-saved regs, then `lea rbp, [rax-imm]`.
    // After that they read everything (including stack args 5+) via `[rbp+disp]`
    // instead of `[rsp+disp]`. We need to compute the rbp→original-rsp delta so
    // we can translate `[rbp+disp]` reads into stack-arg indices.
    //
    // Layout once the prologue completes:
    //   original RSP at function entry      = saved in RAX
    //   pushes consume N*8 bytes             = RSP shrinks by N*8 from RAX
    //   lea rbp, [rax + leaImm]              = RBP relative to ORIGINAL RSP
    //
    // Stack arg N (N >= 5) lives at [original_rsp + 0x28 + 8*(N-5)].
    // So [rbp+disp] is a stack arg iff (disp - leaImm) >= 0x28.
    bool    sawMovRaxRsp     = false;
    int32_t rbpLeaFromRax    = 0;     // signed displacement in `lea rbp, [rax+disp]`
    bool    haveRbpFrame     = false; // set once `lea rbp, [rax+disp]` is decoded

    int i = 0;
    while (i + 5 <= scanBytes) {
        // Stop before walking off-end of recognizable code:
        // 0xC3 = RET, 0xCC = INT3 padding -> end of prologue/body region of interest.
        if (buf[i] == 0xC3 || buf[i] == 0xCC) break;

        // ── mov rax, rsp ── 48 8B C4 ── (must be the very first instruction
        // for the RBP-frame trick to work; we still match it anywhere in the
        // first 16 bytes because compilers sometimes prepend a single `push`).
        if (buf[i] == 0x48 && buf[i+1] == 0x8B && buf[i+2] == 0xC4 && i < 16) {
            sawMovRaxRsp = true;
            i += 3; recognizedCount++;
            continue;
        }

        // ── lea rbp, [rax+disp8] ── 48 8D 68 disp8 (signed 8-bit) ──
        // ── lea rbp, [rax+disp32] ── 48 8D A8 disp32 ──
        // Only meaningful when we already saw `mov rax, rsp`.
        if (sawMovRaxRsp && buf[i] == 0x48 && buf[i+1] == 0x8D) {
            uint8_t modrm = buf[i+2];
            if (modrm == 0x68) {              // lea rbp, [rax+disp8]
                if (i + 4 > scanBytes) break;
                rbpLeaFromRax  = static_cast<int8_t>(buf[i+3]);
                haveRbpFrame   = true;
                i += 4; recognizedCount++;
                continue;
            }
            if (modrm == 0xA8) {              // lea rbp, [rax+disp32]
                if (i + 7 > scanBytes) break;
                rbpLeaFromRax  = static_cast<int32_t>(
                    static_cast<uint32_t>(buf[i+3])       |
                    (static_cast<uint32_t>(buf[i+4]) << 8) |
                    (static_cast<uint32_t>(buf[i+5]) << 16)|
                    (static_cast<uint32_t>(buf[i+6]) << 24));
                haveRbpFrame   = true;
                i += 7; recognizedCount++;
                continue;
            }
        }

        // ── stack-arg read via [rbp+disp] when the rax-rsp+lea-rbp frame is set ──
        // disp32 form is the common case for distant args (because leaImm is
        // typically negative and large, e.g. -0x158, so any positive arg
        // offset puts the displacement out of disp8 range).
        // mov rXX, [rbp+disp32]: REX.W [8B] [ModR/M] [disp32]
        // ModR/M = 0b10 mode | reg<<3 | 0b101 = 0x85 | (reg<<3)
        if (haveRbpFrame && (buf[i] == 0x48 || buf[i] == 0x4C) && buf[i+1] == 0x8B) {
            uint8_t modrm = buf[i+2];
            if ((modrm & 0xC7) == 0x85) {     // [rbp+disp32]
                if (i + 7 > scanBytes) break;
                int32_t disp = static_cast<int32_t>(
                    static_cast<uint32_t>(buf[i+3])       |
                    (static_cast<uint32_t>(buf[i+4]) << 8) |
                    (static_cast<uint32_t>(buf[i+5]) << 16)|
                    (static_cast<uint32_t>(buf[i+6]) << 24));
                // Translate [rbp+disp] back to original-rsp-relative offset:
                //   rbp = rax + leaImm = rsp_orig + leaImm
                //   so [rbp+disp] = rsp_orig + (disp + leaImm)
                int32_t rspRelative = disp + rbpLeaFromRax;
                int argIdx = StackReadOffsetToArgIdx(rspRelative);
                if (argIdx > 0 && argIdx < 64) {
                    stackReadsByArg[argIdx]++;
                }
                i += 7; recognizedCount++;
                continue;
            }
            if ((modrm & 0xC7) == 0x45) {     // [rbp+disp8]
                if (i + 4 > scanBytes) break;
                int32_t disp = static_cast<int8_t>(buf[i+3]);
                int32_t rspRelative = disp + rbpLeaFromRax;
                int argIdx = StackReadOffsetToArgIdx(rspRelative);
                if (argIdx > 0 && argIdx < 64) {
                    stackReadsByArg[argIdx]++;
                }
                i += 4; recognizedCount++;
                continue;
            }
        }

        // ── push reg (40 50..57 with REX) or 50..57 plain ──
        if (buf[i] == 0x40 || buf[i] == 0x41) {
            if ((buf[i + 1] & 0xF0) == 0x50) {
                i += 2; recognizedCount++;
                continue;
            }
        }
        if ((buf[i] & 0xF0) == 0x50) {
            i += 1; recognizedCount++;
            continue;
        }

        // ── sub rsp, imm8: 48 83 EC XX ──
        if (buf[i] == 0x48 && buf[i+1] == 0x83 && buf[i+2] == 0xEC) {
            rspAdjusted = 1;
            i += 4; recognizedCount++;
            continue;
        }
        // ── sub rsp, imm32: 48 81 EC XX XX XX XX ──
        if (buf[i] == 0x48 && buf[i+1] == 0x81 && buf[i+2] == 0xEC) {
            if (i + 7 > scanBytes) break;
            rspAdjusted = 1;
            i += 7; recognizedCount++;
            continue;
        }

        // ── home-space spill: REX.W [89] [ModR/M=44/4C/54/5C] [SIB=24] [disp8] ──
        if ((buf[i] == 0x48 || buf[i] == 0x4C) && buf[i+1] == 0x89) {
            uint8_t rex   = buf[i];
            uint8_t modrm = buf[i+2];
            // [rsp+disp8] addressing requires SIB byte = 0x24, ModR/M low nibble 4
            // and ModR/M mode bits 0b01 (high two bits = 0x40).
            if ((modrm & 0xC7) == 0x44) {
                if (i + 4 > scanBytes) break;
                if (buf[i+3] == 0x24) {
                    int disp = ReadDisp8(&buf[i+4]);
                    int argIdx = HomeSpaceOffsetToArgIdx(disp);
                    int reg    = DecodeRegFromModRM(rex, modrm);
                    // Only count if the source register matches MSVC's
                    // expected arg register for that home-space slot:
                    //   arg 1 → RCX (1) ; arg 2 → RDX (2)
                    //   arg 3 → R8  (8) ; arg 4 → R9  (9)
                    static const int expectedReg[5] = {0, 1, 2, 8, 9};
                    if (argIdx > 0 && argIdx < 5 && reg == expectedReg[argIdx]) {
                        homeSpills[argIdx]++;
                    }
                    i += 5; recognizedCount++;
                    continue;
                }
            }
            // [rsp+disp32] form (rare in prologue, typical in body)
            if ((modrm & 0xC7) == 0x84) {
                if (i + 7 > scanBytes) break;
                if (buf[i+3] == 0x24) {
                    int disp = static_cast<int32_t>(buf[i+4] |
                                                    (buf[i+5] << 8) |
                                                    (buf[i+6] << 16) |
                                                    (buf[i+7] << 24));
                    (void)disp;
                    i += 8; recognizedCount++;
                    continue;
                }
            }
        }

        // ── stack-arg read: REX.W [8B] [ModR/M=44/4C/54/5C] [SIB=24] [disp8] ──
        if ((buf[i] == 0x48 || buf[i] == 0x4C) && buf[i+1] == 0x8B) {
            uint8_t modrm = buf[i+2];
            if ((modrm & 0xC7) == 0x44) {
                if (i + 4 > scanBytes) break;
                if (buf[i+3] == 0x24) {
                    int disp   = ReadDisp8(&buf[i+4]);
                    int argIdx = StackReadOffsetToArgIdx(disp);
                    if (argIdx > 0 && argIdx < 64) {
                        stackReadsByArg[argIdx]++;
                    }
                    i += 5; recognizedCount++;
                    continue;
                }
            }
        }

        // Unknown — advance one byte. Some signal lost but we don't pretend
        // to be a full decoder; we just want the high-confidence patterns.
        i += 1;
    }

    r.bytesScanned = i;

    // Highest spilled register-arg index.
    for (int k = 1; k <= 4; ++k) {
        if (homeSpills[k] > 0) r.registerArgsSpilled = k;
    }
    // Highest stack-arg read.
    for (int k = 5; k < 64; ++k) {
        if (stackReadsByArg[k] > 0) r.highestStackArgRead = k;
    }

    int regArgs   = r.registerArgsSpilled;
    int stackArgs = r.highestStackArgRead;

    // If we saw a stack read, we also know there are AT LEAST 4 register
    // args (you can't take a 5th stack arg without using all 4 reg slots
    // first under MSVC __fastcall).
    if (stackArgs >= 5 && regArgs < 4) regArgs = 4;
    r.inferredArgCount = (std::max)(regArgs, stackArgs);

    // Confidence heuristic.
    int conf = 0;
    if (rspAdjusted)            conf += 20;
    if (haveRbpFrame)           conf += 20;  // mov rax,rsp + lea rbp = strong signal
    if (r.registerArgsSpilled)  conf += 20 + 10 * r.registerArgsSpilled;
    if (r.highestStackArgRead)  conf += 30;
    if (recognizedCount >= 6)   conf += 10;
    if (recognizedCount < 3)    conf  = (std::min)(conf, 30);
    r.confidence = (std::min)(conf, 100);

    // Build the summary string. Distinguish "we recognized the prologue but
    // the function doesn't home-spill or read stack args" from "we recognized
    // nothing" — same inferredArgCount==0 outcome, very different meanings:
    //
    //   recognized prologue (mov-rax-rsp+lea-rbp OR rsp adjust) but no spills/
    //   reads = function uses ≤4 args via registers without home-spill. Honest
    //   limitation; arg count is unprovable from the prologue alone.
    //
    //   no prologue recognized = we couldn't decode the start of the function.
    //   The pattern scanner may have landed on something that isn't a function
    //   entry, or we hit a calling-convention we don't know.
    char tmp[256];
    if (r.inferredArgCount == 0) {
        if (haveRbpFrame || rspAdjusted) {
            const char* shape = haveRbpFrame
                ? "mov-rax-rsp + lea-rbp prologue"
                : "rsp-adjust prologue";
            sprintf_s(tmp, sizeof(tmp),
                      "%s recognized but no register-arg home-spills or "
                      "stack-arg reads in %d bytes — function likely takes "
                      "≤4 args via registers without home-spill (arg count "
                      "unprovable from prologue alone)",
                      shape, r.bytesScanned);
        } else {
            sprintf_s(tmp, sizeof(tmp),
                      "no recognizable __fastcall prologue patterns "
                      "(scanned %d bytes, %d insns recognized) — pattern "
                      "scanner may have landed off-target",
                      r.bytesScanned, recognizedCount);
        }
    } else {
        sprintf_s(tmp, sizeof(tmp),
                  "spills %d reg arg(s), reads up to stack arg %d, "
                  "inferred %d args (%d%% conf)",
                  r.registerArgsSpilled,
                  r.highestStackArgRead,
                  r.inferredArgCount,
                  r.confidence);
    }
    r.summary = tmp;
    return r;
}

bool VerifyArgCount(const char* tag, uintptr_t targetAddr, int expectedArgCount) {
    Result r = Analyze(targetAddr);

    // Always log the prologue hex so cross-build comparisons are easy.
    if (r.inferredArgCount == 0) {
        // Two sub-cases (Result.summary distinguishes them now):
        //   "prologue recognized but no signal" → expected for small register-
        //     only functions. Log at info so the reader sees we DID recognize
        //     something, just couldn't infer arg count from it.
        //   "no prologue recognized" → pattern scanner may have landed wrong.
        //     Log at debug — interesting but not actionable without more info.
        const bool prologueRecognized =
            r.summary.find("prologue recognized") != std::string::npos;
        if (prologueRecognized) {
            spdlog::info("prologue_analyzer[{}] @0x{:X}: {} (prologue {})",
                         tag, targetAddr, r.summary, r.prologueHex);
        } else {
            spdlog::debug("prologue_analyzer[{}] @0x{:X}: {} (prologue {})",
                          tag, targetAddr, r.summary, r.prologueHex);
        }
        return false;
    }

    if (r.inferredArgCount == expectedArgCount) {
        spdlog::info("prologue_analyzer[{}] @0x{:X}: typedef {} args matches inferred — {}",
                     tag, targetAddr, expectedArgCount, r.summary);
        return false;
    }

    if (r.confidence >= 60) {
        spdlog::warn("prologue_analyzer[{}] @0x{:X}: typedef says {} args but inferred {} "
                     "({} conf). Hook may pass garbage in unforwarded slots — investigate. "
                     "Prologue: {}",
                     tag, targetAddr, expectedArgCount, r.inferredArgCount,
                     r.confidence, r.prologueHex);
        return true;
    }

    spdlog::debug("prologue_analyzer[{}] @0x{:X}: typedef says {} args, inferred {} but "
                  "confidence is low ({}%) — likely false positive. Summary: {}",
                  tag, targetAddr, expectedArgCount, r.inferredArgCount,
                  r.confidence, r.summary);
    return false;
}

} // namespace kmp::prologue_analyzer
