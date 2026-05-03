#include "character_accessors.h"
#include "../core.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <atomic>
#include <cstring>

namespace kmp::char_accessors {

namespace {

// RVAs for Kenshi 1.0.68 Newland — from KenshiLib Character.h / RootObjectBase.h.
constexpr uintptr_t RVA_IS_DEAD             = 0x620E30;  // Character::isDead()
constexpr uintptr_t RVA_IS_PLAYER_CHAR      = 0x790470;  // Character::isPlayerCharacter()
constexpr uintptr_t RVA_IS_UNCONCIOUS       = 0x5C9690;  // Character::isUnconcious() (override)
constexpr uintptr_t RVA_GET_MOVEMENT_SPEED  = 0x5C7C50;  // Character::getMovementSpeed() (override)
constexpr uintptr_t RVA_GET_NAME            = 0xD3CA0;   // RootObjectBase::getName()

// Function-pointer types. All are member functions; on x64 MSVC this is
// the standard fastcall convention with `this` in RCX.
using FnBoolThis      = bool   (__fastcall*)(void* thisPtr);
using FnFloatThis     = float  (__fastcall*)(void* thisPtr);
// getName returns std::string by value — on x64 MSVC, return-value
// optimisation for non-trivial types passes a pointer to caller-allocated
// storage in RCX, with `this` shifted to RDX. We capture into a
// caller-supplied std::string via the helper below.
using FnGetNameRaw    = std::string* (__fastcall*)(std::string* out, void* thisPtr);

FnBoolThis    s_isDead          = nullptr;
FnBoolThis    s_isPlayerChar    = nullptr;
FnBoolThis    s_isUnconcious    = nullptr;
FnFloatThis   s_getMovementSpd  = nullptr;
FnGetNameRaw  s_getName         = nullptr;

std::atomic<bool> s_ready{false};

bool LooksLikeFunction(uintptr_t addr) {
    if (addr < 0x10000 || addr > 0x00007FFFFFFFFFFF) return false;
    // Quick sanity: first byte should be a plausible x64 prologue opcode.
    // Common starters: 48 (REX.W), 40/41 (REX), 55 (push rbp), 56/57 (push rsi/rdi),
    //                  53 (push rbx), 4C (REX.WR), E9 (jmp — Kenshi often has these).
    __try {
        const uint8_t first = *reinterpret_cast<const uint8_t*>(addr);
        if (first == 0x48 || first == 0x40 || first == 0x41 || first == 0x4C ||
            first == 0x55 || first == 0x56 || first == 0x57 || first == 0x53 ||
            first == 0xE9) {
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH wrapper for accessor calls. C++ destructors of the std::string
// in GetName aren't permitted inside __try, so we keep the SEH calls
// inside no-cleanup helpers and let the RAII string live in the public
// caller.
bool SafeCallBoolThis(FnBoolThis fn, void* thisPtr, bool& out) {
    if (!fn || !thisPtr) return false;
    __try {
        out = fn(thisPtr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCallFloatThis(FnFloatThis fn, void* thisPtr, float& out) {
    if (!fn || !thisPtr) return false;
    __try {
        out = fn(thisPtr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeCallGetName(FnGetNameRaw fn, void* thisPtr, std::string* outBuf) {
    if (!fn || !thisPtr || !outBuf) return false;
    __try {
        fn(outBuf, thisPtr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void Resolve() {
    auto& scanner = Core::Get().GetScanner();
    const uintptr_t base = scanner.GetBase();
    if (!base) {
        spdlog::warn("char_accessors: scanner base unavailable");
        return;
    }

    auto resolve = [&](uintptr_t rva, const char* name, void** outFn) {
        const uintptr_t addr = base + rva;
        if (LooksLikeFunction(addr)) {
            *outFn = reinterpret_cast<void*>(addr);
            spdlog::info("char_accessors: {} = 0x{:X} (RVA 0x{:X})", name, addr, rva);
        } else {
            spdlog::warn("char_accessors: {} = 0x{:X} (RVA 0x{:X}) — first byte "
                         "doesn't match a function prologue, leaving null",
                         name, addr, rva);
        }
    };

    resolve(RVA_IS_DEAD,            "Character::isDead",            reinterpret_cast<void**>(&s_isDead));
    resolve(RVA_IS_PLAYER_CHAR,     "Character::isPlayerCharacter", reinterpret_cast<void**>(&s_isPlayerChar));
    resolve(RVA_IS_UNCONCIOUS,      "Character::isUnconcious",      reinterpret_cast<void**>(&s_isUnconcious));
    resolve(RVA_GET_MOVEMENT_SPEED, "Character::getMovementSpeed",  reinterpret_cast<void**>(&s_getMovementSpd));
    resolve(RVA_GET_NAME,           "RootObjectBase::getName",      reinterpret_cast<void**>(&s_getName));

    s_ready.store(true, std::memory_order_release);
}

bool IsReady() { return s_ready.load(std::memory_order_acquire); }

bool IsDead(void* c) {
    bool r = false;
    return SafeCallBoolThis(s_isDead, c, r) ? r : false;
}

bool IsPlayerCharacter(void* c) {
    bool r = false;
    return SafeCallBoolThis(s_isPlayerChar, c, r) ? r : false;
}

bool IsUnconcious(void* c) {
    bool r = false;
    return SafeCallBoolThis(s_isUnconcious, c, r) ? r : false;
}

float GetMovementSpeed(void* c) {
    float r = 0.f;
    return SafeCallFloatThis(s_getMovementSpd, c, r) ? r : 0.f;
}

std::string GetName(void* c) {
    std::string out;
    SafeCallGetName(s_getName, c, &out);
    return out;
}

} // namespace kmp::char_accessors
