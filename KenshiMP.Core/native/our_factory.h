#pragma once
#include <cstdint>
#include "../game/game_types.h"

// ─────────────────────────────────────────────────────────────────────────────
//  OurFactory v2 — KMP-owned character construction via process()
//
//  Architecture: build a `RootObjectFactory::CreatelistItem` ourselves
//  (verified layout from KenshiReclaimer/KenshiLib's RootObjectFactory.h),
//  then call `RootObjectFactory::process(this, item*)` at RVA 0x581770. The
//  process() method is the internal dispatcher that all higher-level create*
//  paths use; it has a stable 2-arg __thiscall signature (RCX=factory,
//  RDX=item*). No HeapSize, no memcpy, no class-size guessing.
//
//  KenshiLib CreatelistItem layout:
//    +0x00  RootObjectContainer* container
//    +0x08  Building*            homeBuilding
//    +0x10  Faction*             faction
//    +0x18  GameData*            data
//    +0x20  Ogre::Vector3        position  (12B)
//    +0x2C  bool                 isFromActiveLevelMod
//    +0x30  Ogre::Quaternion     rotation  (16B, w/x/y/z)
//    +0x40  FactoryCallbackInterface* callbackObject
//    +0x48  GameSaveState*       saveState
//    +0x50  float                age
//   sizeof = 0x54, but we round to 0x60 for alignment safety.
//
//  This file replaces the previous template-capture/memcpy approach which
//  triggered STATUS_HEAP_CORRUPTION at 14:00:54 (HeapSize on Kenshi-allocated
//  blocks isn't safe — Kenshi's allocator is not the process heap).
// ─────────────────────────────────────────────────────────────────────────────

namespace kmp::our_factory {

// Initialize: resolves process() RVA from the game module. Call once at
// mod startup. Returns true if process() was located.
bool Init() noexcept;

// True if process() resolved AND we have at least one valid donor-derived
// (Faction*, GameData*) tuple to pass into a CreatelistItem.
bool IsReady() noexcept;

// True if process() resolved and we have a live Faction*. This is enough when
// the caller supplies a proven mod GameData* template.
bool CanCreateWithGameData() noexcept;

// Update the donor-derived (Faction*, GameData*) pair from a fully-loaded
// Character observed by Hook_AICreate. Filters out characters whose owner
// is in EXE static-data — only heap-resident factions are accepted, because
// process() will mutate the Faction (refcounts, member additions) and
// EXE-section memory is read-only.
//
// `character` is the 5th-arg Character* from CharBody::create.
void RecordPlayerFactionFromCharacter(void* character) noexcept;

// Create a remote character at `position`. Constructs a CreatelistItem on
// the stack, fills it with the recorded (faction, gameData) plus our
// position, calls RootObjectFactory::process(). Returns the new
// RootObjectBase* (which IS the Character pointer for character templates)
// or nullptr if not ready / process() failed.
void* CreateRemoteCharacter(const Vec3& position) noexcept;

// Same as above, but uses an explicit GameData* template supplied by the
// SpawnManager's mod-template cache instead of the donor character's template.
void* CreateRemoteCharacterFromGameData(const Vec3& position, void* gameData) noexcept;

// Diagnostics
int   RemoteCharacterCount() noexcept;
const char* StatusString() noexcept;

} // namespace kmp::our_factory
