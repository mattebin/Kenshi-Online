#pragma once
//
// character_accessors — direct calls into Kenshi's own member functions
// instead of reading offsets we have to keep in sync per build.
//
// Why prefer accessors over offsets:
//   - Symbol-stable: if Kenshi reorders fields, the accessor still reads
//     the right thing. Offsets break silently.
//   - Already SEH-safe in Kenshi (the engine isn't going to AV in its
//     own getter).
//   - Express intent: `IsDead(c)` reads better than `Memory::Read(c+0x???)
//     != 0`.
//
// Costs:
//   - One indirect call per query. Negligible vs cost of the operation
//     it usually feeds.
//   - Per-build RVA may shift; we resolve once at startup and warn if it
//     looks invalid.
//
// All RVAs are for Kenshi 1.0.68 (Steam, Newland), sourced from
// lib/kenshilib/Include/kenshi/Character.h and RootObjectBase.h.

#include <cstdint>
#include <string>

namespace kmp::char_accessors {

// Resolve all accessor function pointers from base + RVA. Call once at
// Core::Init after the scanner has the host module base. Logs each
// resolved address and warns on any that fail validation.
void Resolve();

// True if accessors have been resolved successfully.
bool IsReady();

// Direct calls — return Kenshi's own answer. SEH-protected against the
// rare case of being called on a freed pointer.
bool         IsDead(void* characterPtr);
bool         IsPlayerCharacter(void* characterPtr);
bool         IsUnconcious(void* characterPtr);
float        GetMovementSpeed(void* characterPtr);
std::string  GetName(void* characterPtr);

// Inverse of IsDead — convenience helper, no extra cost.
inline bool IsAlive(void* characterPtr) { return !IsDead(characterPtr); }

} // namespace kmp::char_accessors
