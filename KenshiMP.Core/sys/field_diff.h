#pragma once
//
// field_diff — pre/post-call struct snapshot diff for constructor-style
// hooks. Designed to catch subtle struct-field corruption inside a hook body
// that the prologue/callsite analyzers can't see (those only verify arg
// count, not what the function does with the args).
//
// Usage pattern inside a hook body:
//
//   void __fastcall Hook_AICreate(void* aiThis, void* character, ...) {
//       static const FieldExpectation kFields[] = {
//           {0x010, "char->this->charPtr"},
//           {0x310, "ai task table"},
//           {0x318, "stack arg 5 sink"},
//       };
//       Snapshot pre = Capture(aiThis, /*size=*/0x400);
//       s_origAICreate(aiThis, character, ...);
//       VerifyFieldsWritten(pre, aiThis, kFields,
//                           sizeof(kFields)/sizeof(kFields[0]),
//                           "AICreate");
//   }
//
// `VerifyFieldsWritten` warns if any expected field is still zero post-call.
// That's the smoking-gun signal for the AI::create-class bug we hit: the
// function "ran" (no crash) but left this+0x318 null because we didn't
// forward the stack-arg that fed it.
//
// Cheap. Snapshot is a memcpy. Diff is a memcmp + per-field zero check.
// Only invoke when you have specific suspicions; not free for hot-path hooks.

#include <cstdint>
#include <cstddef>

namespace kmp::field_diff {

struct Snapshot {
    bool   valid = false;
    void*  ptr   = nullptr;     // The pointer that was snapshotted (for matching).
    size_t size  = 0;           // Bytes captured.
    uint8_t bytes[1024]{};       // Inline storage; cap at 1KB to keep cheap.
};

struct FieldExpectation {
    int         offset;          // Byte offset from the snapshot base.
    const char* description;     // Human label, e.g. "ai task table".
};

// Capture up to `size` bytes (max 1024) of memory at `ptr`. SEH-safe — bad
// pointers return Snapshot{valid=false}. Cheap.
Snapshot Capture(void* ptr, size_t size);

// Compare the post-call state at `ptr` against `pre.bytes` and log any of
// the named fields that are still zero (uint64) post-call. Logs at warn
// level when a field is unwritten — that's the "hook ran but didn't actually
// initialize" signal.
//
// `fieldCount` is array length. `tag` is included in log messages.
void VerifyFieldsWritten(const Snapshot& pre, void* ptr,
                         const FieldExpectation* fields, size_t fieldCount,
                         const char* tag);

} // namespace kmp::field_diff
