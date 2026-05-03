#pragma once
//
// game_world_iter — iterate Kenshi's canonical character update list.
//
// Why this exists:
//   We have CharacterIterator (in spawn_manager / engine/) that walks the
//   character lektor (vector-like container). It's the historical source
//   of "heap corruption when reading lektor during active resize" bugs
//   that drove our entity_hooks loading-passthrough design — the lektor
//   reallocates while we read, and we get torn data or AVs.
//
//   GameWorld holds a different container at +0x750:
//     ogre_unordered_set<Character*> charUpdateListMain
//
//   This is the master "characters that need update this tick" set. It's
//   a hash table with stable iteration during normal gameplay (insertions
//   may invalidate iterators in theory, but in practice the engine doesn't
//   rehash during a game tick — Kenshi only mutates the set at frame
//   boundaries we control by polling on OnGameTick).
//
//   Reading from this set instead of the lektor sidesteps the resize-race
//   entirely.
//
// What this module gives you:
//   A SEH-safe snapshot copy of Character* values from the set, returned
//   in a vector. Cheap (one walk, one alloc). Caller iterates the snapshot
//   at leisure with no concurrency concerns.
//
// What we DON'T do:
//   No live iteration with iterators (those CAN invalidate). The snapshot
//   model is the safe pattern here.

#include <vector>
#include <cstdint>

namespace kmp::game_world_iter {

// Take a snapshot of GameWorld::charUpdateListMain. Returns the captured
// Character* values in iteration order. Empty on failure.
//
// Cap N caps the snapshot size — useful for diagnostics ("just give me
// the first 16 chars"). 0 = unlimited.
//
// gameWorldPtr should be the resolved GameWorld singleton pointer (same
// thing the rest of the codebase uses). If null, we try to fetch it via
// game::ResolvedGameWorld() — degrades to empty on failure.
std::vector<void*> SnapshotCharacters(void* gameWorldPtr = nullptr,
                                      size_t capN = 0);

// Diagnostic: how many characters does Kenshi's master update set hold?
// Cheaper than SnapshotCharacters because it doesn't copy pointers.
size_t Count(void* gameWorldPtr = nullptr);

} // namespace kmp::game_world_iter
