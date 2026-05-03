#include "game_world_iter.h"
#include "game_types.h"
#include "kmp/memory.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <algorithm>

namespace kmp::game_world_iter {

namespace {

// GameWorld::charUpdateListMain offset (Kenshi 1.0.68 Newland, KenshiLib
// GameWorld.h:0x750). The set is `ogre_unordered_set<Character*>` which
// is an Ogre-allocated hash-table type. Without the full Ogre allocator
// header we'd need to pin the layout, but the set's externally visible
// shape is consistent with the standard libstdc++ unordered_set used by
// Ogre's STLAllocator template:
//   +0x00: hashpolicy state
//   +0x08: bucket array pointer (Node**)
//   +0x10: bucket count (size_t)
//   +0x18: head node pointer (Node*) — first element via _M_before_begin
//   +0x20: element count (size_t)
//   +0x28: max_load_factor (float)
//   +0x2C: rehash_policy state
//
// Each Node is the standard linked-list shape:
//   +0x00: Node* next
//   +0x08: hash code (size_t)
//   +0x10: stored value (Character* in our case = 8 bytes)
//
// We walk the singly-linked list starting at the head. SEH-protected per
// step so a partial set teardown can't AV us.
constexpr int OFFSET_CHAR_UPDATE_LIST_MAIN = 0x750;

// Offsets within the unordered_set struct (gcc/libstdc++ layout via Ogre's
// STLAllocator wrapper — same one Kenshi was built with).
constexpr int SET_OFFSET_HEAD_NODE = 0x18;
constexpr int SET_OFFSET_ELEM_COUNT = 0x20;
constexpr int NODE_OFFSET_NEXT = 0x00;
constexpr int NODE_OFFSET_VALUE = 0x10;

bool ReadPtrSafe(uintptr_t addr, uintptr_t& out) {
    __try {
        out = *reinterpret_cast<volatile uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadSizeSafe(uintptr_t addr, size_t& out) {
    __try {
        out = *reinterpret_cast<volatile size_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LooksLikePointer(uintptr_t v) {
    return v >= 0x10000 && v <= 0x00007FFFFFFFFFFFULL && (v & 0x7) == 0;
}

void* ResolveGameWorld(void* explicitWorld) {
    if (explicitWorld) return explicitWorld;
    // GetResolvedGameWorld returns the SLOT address; the actual GameWorld
    // instance lives at *(slot). Dereference, sanity-check, return.
    const uintptr_t slot = game::GetResolvedGameWorld();
    if (!slot) return nullptr;
    uintptr_t inst = 0;
    __try {
        inst = *reinterpret_cast<volatile uintptr_t*>(slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (inst < 0x10000 || inst > 0x00007FFFFFFFFFFFULL) return nullptr;
    return reinterpret_cast<void*>(inst);
}

} // namespace

std::vector<void*> SnapshotCharacters(void* gameWorldPtr, size_t capN) {
    std::vector<void*> out;
    void* gw = ResolveGameWorld(gameWorldPtr);
    if (!gw) {
        return out;
    }
    const uintptr_t setAddr = reinterpret_cast<uintptr_t>(gw) +
                              OFFSET_CHAR_UPDATE_LIST_MAIN;

    // Read element count first as a sanity ceiling — even if iteration
    // walks off the end of a torn list, we won't reserve gigabytes.
    size_t elemCount = 0;
    if (!ReadSizeSafe(setAddr + SET_OFFSET_ELEM_COUNT, elemCount)) {
        return out;
    }
    if (elemCount == 0 || elemCount > 100000) {
        // 0 is fine, > 100k is suspicious (typical Kenshi save has up to
        // a few thousand active chars across all zones).
        if (elemCount > 100000) {
            spdlog::warn("game_world_iter: charUpdateListMain elemCount = {} — "
                         "looks corrupt, refusing to walk", elemCount);
        }
        return out;
    }

    const size_t reserveTarget = capN > 0 ? std::min(capN, elemCount) : elemCount;
    out.reserve(reserveTarget);

    // Walk the linked list starting at head. The head pointer in the
    // libstdc++ representation is at SET_OFFSET_HEAD_NODE; each node's
    // `next` is at NODE_OFFSET_NEXT and its stored value is at NODE_OFFSET_VALUE.
    uintptr_t node = 0;
    if (!ReadPtrSafe(setAddr + SET_OFFSET_HEAD_NODE, node)) {
        return out;
    }

    // Hard cap on iterations — protects against a node->next cycle.
    const size_t hardCap = elemCount * 2 + 16;
    size_t walked = 0;
    while (LooksLikePointer(node) && walked < hardCap) {
        ++walked;
        uintptr_t value = 0;
        if (!ReadPtrSafe(node + NODE_OFFSET_VALUE, value)) break;
        if (LooksLikePointer(value)) {
            out.push_back(reinterpret_cast<void*>(value));
            if (capN > 0 && out.size() >= capN) break;
        }
        uintptr_t nextNode = 0;
        if (!ReadPtrSafe(node + NODE_OFFSET_NEXT, nextNode)) break;
        if (nextNode == node) break; // self-loop
        node = nextNode;
    }
    return out;
}

size_t Count(void* gameWorldPtr) {
    void* gw = ResolveGameWorld(gameWorldPtr);
    if (!gw) return 0;
    const uintptr_t setAddr = reinterpret_cast<uintptr_t>(gw) +
                              OFFSET_CHAR_UPDATE_LIST_MAIN;
    size_t elemCount = 0;
    if (!ReadSizeSafe(setAddr + SET_OFFSET_ELEM_COUNT, elemCount)) return 0;
    if (elemCount > 100000) return 0;
    return elemCount;
}

} // namespace kmp::game_world_iter
