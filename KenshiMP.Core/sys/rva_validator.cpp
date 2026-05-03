#include "rva_validator.h"
#include "../core.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <cstdio>
#include <cstring>

namespace kmp::rva_validator {

namespace {

// Documented RVAs we care about. Source tag lets the rollup tell us
// whether mismatches correlate with a particular doc source (so far they
// all do — KenshiLib v1.0.51 RVAs vs our 1.0.68 binary).
//
// IMPORTANT: this table is the SOURCE OF TRUTH for what we expect the
// binary to look like. Adding a new hardcoded RVA anywhere in the codebase
// should also add an entry here.
const Entry kBuiltinTable[] = {
    // Functions we tried to hook in 2026-05-03 verification — most failed.
    { "GameWorld::_DESTRUCTOR",       0x86C0C0, "KenshiLib header (v1.0.51)" },
    { "LoadingWindow::hide",          0x911C10, "KenshiLib header (v1.0.51)" },
    { "MainBarGUI::_CONSTRUCTOR",     0x72C1E0, "KenshiLib header (v1.0.51)" },
    { "Character::isDead",            0x620E30, "KenshiLib header (v1.0.51)" },
    { "Character::isPlayerCharacter", 0x790470, "KenshiLib header (v1.0.51)" },
    { "Character::isUnconcious",      0x5C9690, "KenshiLib header (v1.0.51)" },
    { "Character::getMovementSpeed",  0x5C7C50, "KenshiLib header (v1.0.51)" },
    { "RootObjectBase::getName",      0xD3CA0,  "KenshiLib header (v1.0.51)" },
    { "InputHandler::keyDownEvent",   0x360680, "andperks6 input_hooks (v1.0.68)" },
    { "InputHandler::keyUpEvent",     0x3608F0, "andperks6 input_hooks (v1.0.68)" },
};

bool SafeMemcpy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

Verdict Classify(const uint8_t* bytes, size_t n, uintptr_t addr, uintptr_t& outJumpTarget) {
    outJumpTarget = 0;
    if (n < 4) return Verdict::Unknown;

    const uint8_t b0 = bytes[0];

    // INT3 padding (CC) — reliably wrong target. If the first 4 bytes are
    // all CC we're in alignment slop between functions.
    if (b0 == 0xCC && bytes[1] == 0xCC && bytes[2] == 0xCC && bytes[3] == 0xCC) {
        return Verdict::IntPadding;
    }

    // E9 disp32 — relative JMP. Common for jmp-table thunks; the real
    // function body is at addr+5+disp.
    if (b0 == 0xE9) {
        int32_t disp = static_cast<int32_t>(
            static_cast<uint32_t>(bytes[1]) |
            (static_cast<uint32_t>(bytes[2]) << 8) |
            (static_cast<uint32_t>(bytes[3]) << 16) |
            (static_cast<uint32_t>(bytes[4]) << 24));
        outJumpTarget = addr + 5 + static_cast<intptr_t>(disp);
        return Verdict::JumpThunk;
    }
    // EB disp8 — short JMP (rare for function entries but possible).
    if (b0 == 0xEB) {
        int32_t disp = static_cast<int8_t>(bytes[1]);
        outJumpTarget = addr + 2 + disp;
        return Verdict::JumpThunk;
    }

    // Plausible function-prologue starters. This list is intentionally
    // broad — we want "could be a function entry" not "definitely is one".
    //   48        REX.W (sub rsp, mov rax,rsp, etc.)
    //   40 / 41   REX prefix variants
    //   4C        REX.WR
    //   50..57    push rax/rcx/rdx/rbx/rsp/rbp/rsi/rdi
    //   53        push rbx (subset of above, listed for clarity)
    //   55        push rbp (subset of above)
    //   B8..BF    mov reg32, imm32 (common for trivial getters)
    //   33        xor (zero idiom for "return false" getters)
    //   8B        mov reg, r/m (small accessors)
    //   E8        call (tail-call style entry — uncommon but valid)
    if (b0 == 0x48 || b0 == 0x40 || b0 == 0x41 || b0 == 0x4C ||
        (b0 >= 0x50 && b0 <= 0x57) ||
        (b0 >= 0xB8 && b0 <= 0xBF) ||
        b0 == 0x33 || b0 == 0x8B || b0 == 0xE8) {
        return Verdict::LikelyFunction;
    }

    // Anything else (24, FF, 28, 50/08 sequence, etc.) is mid-instruction.
    return Verdict::MidInstruction;
}

const char* VerdictName(Verdict v) {
    switch (v) {
        case Verdict::LikelyFunction:  return "function-entry";
        case Verdict::JumpThunk:       return "jump-thunk";
        case Verdict::IntPadding:      return "INT3-padding";
        case Verdict::MidInstruction:  return "mid-instruction";
        case Verdict::Unreadable:      return "unreadable";
        default:                       return "unknown";
    }
}

void HexDump(const uint8_t* b, size_t n, char* out, size_t outSize) {
    out[0] = '\0';
    for (size_t i = 0; i < n; ++i) {
        char tmp[4];
        sprintf_s(tmp, sizeof(tmp), "%02X ", b[i]);
        strcat_s(out, outSize, tmp);
    }
}

} // namespace

Result ValidateOne(const Entry& e) {
    Result r{};
    r.entry = &e;
    auto& scanner = Core::Get().GetScanner();
    const uintptr_t base = scanner.GetBase();
    if (!base) {
        r.verdict = Verdict::Unreadable;
        spdlog::warn("rva_validator[{}]: scanner base unavailable", e.name);
        return r;
    }

    r.resolvedAddr = base + e.rva;
    if (!SafeMemcpy(r.firstBytes, reinterpret_cast<void*>(r.resolvedAddr),
                    sizeof(r.firstBytes))) {
        r.verdict = Verdict::Unreadable;
        spdlog::warn("rva_validator[{}]: AV reading at 0x{:X} (RVA 0x{:X}, source: {})",
                     e.name, r.resolvedAddr, e.rva, e.sourceTag);
        return r;
    }

    r.verdict = Classify(r.firstBytes, sizeof(r.firstBytes),
                         r.resolvedAddr, r.jumpTarget);

    char hex[64];
    HexDump(r.firstBytes, 8, hex, sizeof(hex));

    switch (r.verdict) {
        case Verdict::LikelyFunction:
            spdlog::info("rva_validator[{}]: PASS at 0x{:X} (RVA 0x{:X}, src={}) — {} bytes={}",
                         e.name, r.resolvedAddr, e.rva, e.sourceTag,
                         VerdictName(r.verdict), hex);
            break;
        case Verdict::JumpThunk:
            spdlog::info("rva_validator[{}]: JMP-THUNK at 0x{:X} -> 0x{:X} (RVA 0x{:X}, src={}) — {} bytes={}",
                         e.name, r.resolvedAddr, r.jumpTarget, e.rva, e.sourceTag,
                         VerdictName(r.verdict), hex);
            break;
        default:
            spdlog::warn("rva_validator[{}]: FAIL at 0x{:X} (RVA 0x{:X}, src={}) — {} bytes={}",
                         e.name, r.resolvedAddr, e.rva, e.sourceTag,
                         VerdictName(r.verdict), hex);
            break;
    }
    return r;
}

int RunBuiltinSurvey() {
    spdlog::info("=== rva_validator: surveying {} documented RVAs ===",
                 sizeof(kBuiltinTable) / sizeof(kBuiltinTable[0]));

    int usable = 0, padding = 0, mid = 0, unreadable = 0, jmp = 0;
    for (auto& e : kBuiltinTable) {
        Result r = ValidateOne(e);
        switch (r.verdict) {
            case Verdict::LikelyFunction: ++usable; break;
            case Verdict::JumpThunk:      ++usable; ++jmp; break;
            case Verdict::IntPadding:     ++padding; break;
            case Verdict::MidInstruction: ++mid; break;
            case Verdict::Unreadable:     ++unreadable; break;
            default: break;
        }
    }
    spdlog::info("=== rva_validator: usable={} (jmp-thunks={}) padding={} mid-instruction={} "
                 "unreadable={} — see [warning] lines for FAILs ===",
                 usable, jmp, padding, mid, unreadable);
    return usable;
}

} // namespace kmp::rva_validator
