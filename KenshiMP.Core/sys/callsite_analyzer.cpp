#include "callsite_analyzer.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <Psapi.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

#pragma comment(lib, "Psapi.lib")

namespace kmp::callsite_analyzer {

namespace {

// Get a named PE section's bounds.
bool GetSection(const char* name, uintptr_t& outBase, size_t& outSize) {
    HMODULE h = GetModuleHandleA(nullptr);
    if (!h) return false;

    auto base = reinterpret_cast<uintptr_t>(h);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const size_t nameLen = strlen(name);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, name, nameLen) == 0) {
            outBase = base + sec->VirtualAddress;
            outSize = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

bool GetTextSection(uintptr_t& outBase, size_t& outSize) {
    return GetSection(".text", outBase, outSize);
}

bool GetRDataSection(uintptr_t& outBase, size_t& outSize) {
    return GetSection(".rdata", outBase, outSize);
}

// Scan .rdata looking for QWORDS that equal `targetAddr`. Each match is a
// likely vtable slot pointing at our function. Returns the first match's
// address (the address of the slot itself, not the function), or 0 when
// nothing is found.
//
// SEH-safe — bad reads return 0 (we just stop scanning).
uintptr_t FindVtableSlotPointingAt(uintptr_t rdataBase, size_t rdataSize,
                                   uintptr_t targetAddr) {
    if (rdataBase == 0 || rdataSize < sizeof(uintptr_t)) return 0;
    auto* p = reinterpret_cast<const uintptr_t*>(rdataBase);
    const size_t count = rdataSize / sizeof(uintptr_t);
    for (size_t i = 0; i < count; ++i) {
        __try {
            if (p[i] == targetAddr) {
                return reinterpret_cast<uintptr_t>(&p[i]);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Hit an unmapped page — stop scanning, give up.
            return 0;
        }
    }
    return 0;
}

// SEH-safe scan for a relative CALL whose target equals `targetAddr`.
// CALL rel32 = 0xE8, displacement is signed 32-bit relative to NEXT instruction.
// We scan byte-by-byte (could miss some matches if data appears mid-instruction
// but works for most cases — the signal we want is "find at least one caller").
uintptr_t FindOneCallTo(uintptr_t textBase, size_t textSize, uintptr_t targetAddr,
                        size_t searchLimit) {
    const size_t scanLimit = (std::min)(searchLimit, textSize);
    auto* p = reinterpret_cast<const uint8_t*>(textBase);

    // Skip first 16 bytes to avoid PE header garbage.
    for (size_t i = 16; i + 5 < scanLimit; ++i) {
        if (p[i] != 0xE8) continue;  // not CALL rel32

        __try {
            int32_t disp = *reinterpret_cast<const int32_t*>(p + i + 1);
            uintptr_t nextInsn = textBase + i + 5;
            uintptr_t resolved = nextInsn + static_cast<intptr_t>(disp);
            if (resolved == targetAddr) {
                return textBase + i;  // address of the 0xE8 byte
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // VirtualProtect race or unmapped page — keep scanning.
        }
    }
    return 0;
}

// Walk back from the call site decoding instruction shapes that look like
// arg setup. Returns:
//   regArgsMask: bit 0 = RCX, 1 = RDX, 2 = R8, 3 = R9
//   stackArgsHigh: highest stack-arg index written (5..N)
//
// Heuristic only — not a real disassembler. We only recognize the most
// common arg-setup encodings (mov reg, ?), (mov [rsp+disp], reg), (lea reg, ?),
// (xor reg, reg) — and stop when we hit something we can't make sense of.
void ScanBackArgSetup(const uint8_t* callBytePtr, int instructionBudget,
                      int& outRegMask, int& outStackHigh, int& outInsnsScanned) {
    outRegMask = 0;
    outStackHigh = 0;
    outInsnsScanned = 0;

    // We don't know exact instruction boundaries walking back, so we use a
    // simple sliding-window heuristic: look at the last ~1KB of bytes before
    // the CALL, scan forward from a starting point and count arg-setup
    // patterns just before the CALL.
    constexpr int LOOKBACK = 512;
    const uint8_t* start = callBytePtr - LOOKBACK;

    // Walk forward. At each byte try to recognize an arg-setup pattern.
    // If we see one, advance by its length; otherwise advance by 1 (lossy).
    int i = 0;
    while (i < LOOKBACK) {
        const uint8_t* p = start + i;

        // ── mov RCX, ??? ── 48 8B C9/CA/etc, or 48 89 .. ──
        // We just check: is this a write to RCX/RDX/R8/R9?
        // mov reg64, reg64:  REX 8B ModRM       (destination = reg in ModRM bits 5:3)
        // mov reg64, imm32:  REX C7 ModRM imm32 (destination = ModRM r/m, mode 0b11)
        // lea reg64, [...]:  REX 8D ModRM ...   (destination in ModRM bits 5:3)
        // xor reg, reg:      REX 33 ModRM       (zero idiom)

        // mov rcx, ... (REX.W=0x48, opcode=0x8B/8D, dest field encoded)
        if (i + 3 <= LOOKBACK && (p[0] == 0x48 || p[0] == 0x4C) &&
            (p[1] == 0x8B || p[1] == 0x8D || p[1] == 0x33 || p[1] == 0x89)) {
            uint8_t rex = p[0];
            uint8_t modrm = p[2];
            int reg = ((modrm >> 3) & 0x07) + ((rex & 0x04) ? 8 : 0);
            // 1=RCX, 2=RDX, 8=R8, 9=R9
            if (p[1] == 0x8B || p[1] == 0x8D) {
                // dest is `reg` field
                if (reg == 1) outRegMask |= 0x01;
                else if (reg == 2) outRegMask |= 0x02;
                else if (reg == 8) outRegMask |= 0x04;
                else if (reg == 9) outRegMask |= 0x08;
            }
            // Try to advance by typical operand length.
            i += 3;
            outInsnsScanned++;
            continue;
        }

        // ── mov [rsp+disp8], reg64 ── 48 89 ?? 24 disp ──
        if (i + 5 <= LOOKBACK && (p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89) {
            uint8_t modrm = p[2];
            if ((modrm & 0xC7) == 0x44 && p[3] == 0x24) {
                int disp = static_cast<int8_t>(p[4]);
                if (disp >= 0x28 && (disp - 0x28) % 8 == 0) {
                    int argIdx = 5 + (disp - 0x28) / 8;
                    if (argIdx > outStackHigh) outStackHigh = argIdx;
                }
                i += 5;
                outInsnsScanned++;
                continue;
            }
        }

        // ── mov [rsp+disp8], imm32 ── 48 C7 44 24 disp imm32 ──
        if (i + 8 <= LOOKBACK && p[0] == 0x48 && p[1] == 0xC7 &&
            p[2] == 0x44 && p[3] == 0x24) {
            int disp = static_cast<int8_t>(p[4]);
            if (disp >= 0x28 && (disp - 0x28) % 8 == 0) {
                int argIdx = 5 + (disp - 0x28) / 8;
                if (argIdx > outStackHigh) outStackHigh = argIdx;
            }
            i += 8;
            outInsnsScanned++;
            continue;
        }

        // ── xor RCX/RDX/R8/R9, same ── 33 C9/D2/etc ──
        if (i + 2 <= LOOKBACK && (p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x33) {
            uint8_t rex = p[0];
            uint8_t modrm = p[2];
            int reg = ((modrm >> 3) & 0x07) + ((rex & 0x04) ? 8 : 0);
            if (reg == 1) outRegMask |= 0x01;
            else if (reg == 2) outRegMask |= 0x02;
            else if (reg == 8) outRegMask |= 0x04;
            else if (reg == 9) outRegMask |= 0x08;
            i += 3;
            outInsnsScanned++;
            continue;
        }

        // Couldn't decode — advance one byte.
        i += 1;
        if (instructionBudget > 0 && outInsnsScanned >= instructionBudget) break;
    }
}

// SEH wrapper around ScanBackArgSetup — extracted to a function with no
// non-trivially-destructible locals so MSVC will accept the __try block.
void SafeScanBackArgSetup(const uint8_t* callBytePtr, int instructionBudget,
                          int& outRegMask, int& outStackHigh, int& outInsnsScanned) {
    __try {
        ScanBackArgSetup(callBytePtr, instructionBudget,
                         outRegMask, outStackHigh, outInsnsScanned);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Best effort — drop whatever partial data we have.
    }
}

int PopCount4(int mask) {
    int c = 0;
    if (mask & 0x01) c++;
    if (mask & 0x02) c++;
    if (mask & 0x04) c++;
    if (mask & 0x08) c++;
    return c;
}

} // namespace

Result FindAndAnalyzeOneCaller(uintptr_t targetAddr, size_t searchBytes) {
    Result r;
    if (targetAddr == 0) return r;

    uintptr_t textBase = 0;
    size_t textSize = 0;
    if (!GetTextSection(textBase, textSize)) {
        r.summary = "could not locate .text section";
        return r;
    }

    uintptr_t callAddr = FindOneCallTo(textBase, textSize, targetAddr, searchBytes);
    if (callAddr == 0) {
        // No direct call rel32. Could be vtable-dispatched. Check .rdata.
        uintptr_t rdataBase = 0; size_t rdataSize = 0;
        if (GetRDataSection(rdataBase, rdataSize)) {
            uintptr_t slot = FindVtableSlotPointingAt(rdataBase, rdataSize, targetAddr);
            if (slot != 0) {
                r.inVtable    = true;
                r.vtableSlot  = slot;
                char tmp[160];
                sprintf_s(tmp, sizeof(tmp),
                          "no `call rel32` to target — dispatched via vtable "
                          "(slot at .rdata 0x%llX). Caller-side arg count cannot "
                          "be inferred from vtable alone; cross-check via "
                          "prologue_analyzer instead.",
                          (unsigned long long)slot);
                r.summary = tmp;
                return r;
            }
        }
        r.summary = "no `call rel32` to target found in .text and not in any vtable";
        return r;
    }

    r.callSiteFound = true;
    r.callSiteAddr = callAddr;

    int regMask = 0, stackHigh = 0, insns = 0;
    SafeScanBackArgSetup(reinterpret_cast<const uint8_t*>(callAddr),
                         /*instructionBudget=*/256, regMask, stackHigh, insns);

    r.registerArgsSet     = regMask;
    r.highestStackArgSet  = stackHigh;
    r.instructionsScanned = insns;

    int regArgs = PopCount4(regMask);
    if (stackHigh >= 5 && regArgs < 4) regArgs = 4;
    r.inferredArgCount = (std::max)(regArgs, stackHigh);

    char tmp[256];
    sprintf_s(tmp, sizeof(tmp),
              "callsite @0x%llX sets reg-mask=0x%X (popcount %d), "
              "max stack-arg %d, inferred %d args (%d insns scanned)",
              (unsigned long long)callAddr, regMask, regArgs, stackHigh,
              r.inferredArgCount, insns);
    r.summary = tmp;
    return r;
}

bool VerifyArgCount(const char* tag, uintptr_t targetAddr, int expectedArgCount) {
    Result r = FindAndAnalyzeOneCaller(targetAddr);
    if (!r.callSiteFound) {
        // Promote vtable-dispatched cases to info — they're not a failure, just
        // a "this isn't analyzable from the caller side" signal that helps the
        // reader interpret the prologue-analyzer result on its own.
        if (r.inVtable) {
            spdlog::info("callsite_analyzer[{}] @0x{:X}: {}",
                         tag, targetAddr, r.summary);
        } else {
            spdlog::debug("callsite_analyzer[{}] @0x{:X}: {}",
                          tag, targetAddr, r.summary);
        }
        return false;
    }

    if (r.inferredArgCount == expectedArgCount) {
        spdlog::info("callsite_analyzer[{}] @0x{:X}: typedef {} args matches caller — {}",
                     tag, targetAddr, expectedArgCount, r.summary);
        return false;
    }

    if (r.inferredArgCount > expectedArgCount) {
        spdlog::warn("callsite_analyzer[{}] @0x{:X}: typedef {} but caller sets {} — {}",
                     tag, targetAddr, expectedArgCount, r.inferredArgCount, r.summary);
        return true;
    }

    // Caller sets fewer args than typedef claims — caller might be passing
    // through pre-existing register state, common in tail calls. Debug only.
    spdlog::debug("callsite_analyzer[{}] @0x{:X}: typedef {} but caller only sets {} — "
                  "may be passthrough, {}",
                  tag, targetAddr, expectedArgCount, r.inferredArgCount, r.summary);
    return false;
}

} // namespace kmp::callsite_analyzer
