#include "field_diff.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <cstring>
#include <algorithm>

namespace kmp::field_diff {

namespace {

bool SafeMemcpy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadU64Safe(const void* addr, uint64_t& out) {
    __try {
        out = *static_cast<const volatile uint64_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

Snapshot Capture(void* ptr, size_t size) {
    Snapshot s;
    if (!ptr) return s;
    auto addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000 || addr > 0x00007FFFFFFFFFFF) return s;

    size = (std::min)(size, sizeof(s.bytes));
    if (!SafeMemcpy(s.bytes, ptr, size)) return s;

    s.valid = true;
    s.ptr   = ptr;
    s.size  = size;
    return s;
}

void VerifyFieldsWritten(const Snapshot& pre, void* ptr,
                         const FieldExpectation* fields, size_t fieldCount,
                         const char* tag) {
    if (!pre.valid || ptr != pre.ptr) return;
    if (!fields || fieldCount == 0)   return;

    for (size_t i = 0; i < fieldCount; ++i) {
        const auto& f = fields[i];
        if (f.offset < 0 || static_cast<size_t>(f.offset) + 8 > pre.size) continue;

        // Pre-call value (cheap — already in our snapshot buffer).
        uint64_t preVal;
        memcpy(&preVal, pre.bytes + f.offset, sizeof(preVal));

        // Post-call value (read live, SEH-safe).
        auto* postAddr = reinterpret_cast<const uint8_t*>(ptr) + f.offset;
        uint64_t postVal = 0;
        if (!ReadU64Safe(postAddr, postVal)) {
            spdlog::warn("field_diff[{}]: AV reading {} at +0x{:X}",
                         tag, f.description ? f.description : "(unnamed)", f.offset);
            continue;
        }

        // Three failure modes worth logging:
        //   1. Field was zero before AND still zero — function never wrote it.
        //   2. Field had a value before AND is zero now — function ZEROED it
        //      (often a sign that arg forwarding broke and the ctor took the
        //      bail-out path that NULLs everything).
        //   3. Field changed but to something non-pointer-shaped (very small
        //      int that landed in a pointer slot). Caller's job to check.
        if (preVal == 0 && postVal == 0) {
            spdlog::warn("field_diff[{}]: expected field '{}' at +0x{:X} still NULL "
                         "after call — likely arg forwarding bug or ctor took error path",
                         tag, f.description ? f.description : "(unnamed)", f.offset);
        } else if (preVal != 0 && postVal == 0) {
            spdlog::warn("field_diff[{}]: field '{}' at +0x{:X} was 0x{:X} before, "
                         "ZERO after — call zeroed it (arg forwarding break?)",
                         tag, f.description ? f.description : "(unnamed)",
                         f.offset, preVal);
        }
    }
}

} // namespace kmp::field_diff
