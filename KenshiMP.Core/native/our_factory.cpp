#include "our_factory.h"
#include "../core.h"
#include "../game/game_types.h"
#include "../hooks/entity_hooks.h"
#include "../hooks/ai_hooks.h"
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <atomic>

namespace kmp::our_factory {

// ─────────────────────────────────────────────────────────────────────────────
//  CreatelistItem layout (KenshiReclaimer/KenshiLib RootObjectFactory.h)
// ─────────────────────────────────────────────────────────────────────────────
//
// NOTE: The layout is from KenshiLib's reverse-engineering of an older Kenshi
// version. Member offsets within the struct are stable across patches because
// the engine doesn't randomize struct layout. Function RVAs DO drift between
// patches; we resolve at runtime using master_index_1.0.68 values.

#pragma pack(push, 1)
struct CreatelistItem {
    void*  container;             // 0x00 RootObjectContainer*
    void*  homeBuilding;          // 0x08 Building*
    void*  faction;               // 0x10 Faction*
    void*  data;                  // 0x18 GameData*
    float  position_x;            // 0x20  ┐
    float  position_y;            // 0x24  ├ Ogre::Vector3 (12B, packed)
    float  position_z;            // 0x28  ┘
    bool   isFromActiveLevelMod;  // 0x2C
    char   _pad1[3];              // 0x2D-0x2F (align Quat to 0x30)
    float  rotation_w;            // 0x30  ┐
    float  rotation_x;            // 0x34  ├ Ogre::Quaternion w/x/y/z
    float  rotation_y;            // 0x38  │
    float  rotation_z;            // 0x3C  ┘
    void*  callbackObject;        // 0x40 FactoryCallbackInterface*
    void*  saveState;             // 0x48 GameSaveState*
    float  age;                   // 0x50
    char   _pad2[12];             // 0x54-0x5F (round up to 0x60 for safety)
};
#pragma pack(pop)
static_assert(sizeof(CreatelistItem) == 0x60, "CreatelistItem must be 0x60 bytes");
static_assert(offsetof(CreatelistItem, faction)        == 0x10, "faction offset");
static_assert(offsetof(CreatelistItem, data)           == 0x18, "data offset");
static_assert(offsetof(CreatelistItem, position_x)     == 0x20, "position offset");
static_assert(offsetof(CreatelistItem, callbackObject) == 0x40, "callback offset");
static_assert(offsetof(CreatelistItem, saveState)      == 0x48, "saveState offset");
static_assert(offsetof(CreatelistItem, age)            == 0x50, "age offset");

// RootObjectFactory::process(CreatelistItem*) — RVA from master_index_1.0.68
// entry 4775 (BINDIFF_EXACT). Returns RootObjectBase*; for character templates
// the returned pointer IS a Character*.
constexpr uintptr_t kProcessRva = 0x581770;

using ProcessFn = void* (__fastcall*)(void* factory, CreatelistItem* item);

// ─────────────────────────────────────────────────────────────────────────────
//  State
// ─────────────────────────────────────────────────────────────────────────────

namespace {
std::atomic<ProcessFn> s_processFn{nullptr};
std::atomic<void*>     s_recordedFaction{nullptr};   // heap-resident only
std::atomic<void*>     s_recordedGameData{nullptr};  // donor's character template
std::atomic<int>       s_remoteCount{0};
std::atomic<int>       s_lastErrorCode{0};
const char*            s_statusString = "uninitialized";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Init / state accessors
// ─────────────────────────────────────────────────────────────────────────────

bool Init() noexcept {
    if (s_processFn.load(std::memory_order_relaxed)) return true;
    HMODULE mod = GetModuleHandleW(nullptr);
    if (!mod) {
        s_statusString = "GetModuleHandle failed";
        return false;
    }
    auto modBase = reinterpret_cast<uintptr_t>(mod);
    auto fn = reinterpret_cast<ProcessFn>(modBase + kProcessRva);
    s_processFn.store(fn, std::memory_order_relaxed);
    s_statusString = "process() resolved";
    spdlog::info("OurFactory: process() resolved at modBase+0x{:X} = 0x{:X}",
                 kProcessRva, reinterpret_cast<uintptr_t>(fn));
    return true;
}

bool IsReady() noexcept {
    return s_processFn.load(std::memory_order_relaxed) != nullptr
        && s_recordedFaction.load(std::memory_order_relaxed) != nullptr
        && s_recordedGameData.load(std::memory_order_relaxed) != nullptr;
}

bool CanCreateWithGameData() noexcept {
    return s_processFn.load(std::memory_order_relaxed) != nullptr
        && s_recordedFaction.load(std::memory_order_relaxed) != nullptr;
}

const char* StatusString() noexcept {
    return s_statusString;
}

int RemoteCharacterCount() noexcept {
    return s_remoteCount.load(std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Record donor (Faction*, GameData*) tuple
// ─────────────────────────────────────────────────────────────────────────────
//
// SEH-safe reads of Character.owner (+0x10) and Character.gameData (+0x40).
// Both must pass IsHeapResidentPtr — process() will mutate the Faction
// (refcount adjustments, member list inserts) so it cannot be in the
// EXE's read-only static data section.
//
// Once both fields are recorded the factory is ready for use.

namespace {
bool TryReadDonorFields(void* character,
                        uintptr_t& outFaction,
                        uintptr_t& outGameData) noexcept {
    if (!character) return false;
    if (!entity_hooks::IsHeapResidentPtr(reinterpret_cast<uintptr_t>(character))) {
        return false;
    }
    __try {
        outFaction  = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uintptr_t>(character) + 0x10);
        outGameData = *reinterpret_cast<uintptr_t*>(
            reinterpret_cast<uintptr_t>(character) + 0x40);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}

void RecordPlayerFactionFromCharacter(void* character) noexcept {
    // Already have both fields? Done.
    if (s_recordedFaction.load(std::memory_order_relaxed)
        && s_recordedGameData.load(std::memory_order_relaxed)) {
        return;
    }

    uintptr_t faction = 0, gameData = 0;
    if (!TryReadDonorFields(character, faction, gameData)) return;

    // Faction filter: IsValidVAPtr (not IsHeapResidentPtr). Kenshi's Faction
    // objects live in the EXE's writable .data section — they're inside the
    // module range but ARE mutable. The strict filter was rejecting all of
    // them and starving OurFactory (see test log 14:25). process() handles
    // both .data and heap factions; let it through.
    if (entity_hooks::IsValidVAPtr(faction)) {
        void* expected = nullptr;
        if (s_recordedFaction.compare_exchange_strong(expected,
                reinterpret_cast<void*>(faction),
                std::memory_order_relaxed)) {
            spdlog::info("OurFactory: recorded Faction* = 0x{:X}", faction);
        }
    }

    // GameData* (Character template) — heap-resident OR module-resident is OK
    // because process() only READS this (it's the source template).
    if (gameData != 0) {
        void* expected = nullptr;
        if (s_recordedGameData.compare_exchange_strong(expected,
                reinterpret_cast<void*>(gameData),
                std::memory_order_relaxed)) {
            spdlog::info("OurFactory: recorded GameData* = 0x{:X}", gameData);
        }
    }

    if (IsReady()) {
        s_statusString = "ready";
        spdlog::info("OurFactory: ready — process()=0x{:X} faction=0x{:X} gameData=0x{:X}",
                     reinterpret_cast<uintptr_t>(s_processFn.load()),
                     reinterpret_cast<uintptr_t>(s_recordedFaction.load()),
                     reinterpret_cast<uintptr_t>(s_recordedGameData.load()));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  CreateRemoteCharacter
// ─────────────────────────────────────────────────────────────────────────────

// Helper isolated for the SEH __try (can't mix with C++ unwinding objects).
// Returns the result pointer or nullptr on AV.
static void* InvokeProcess(ProcessFn fn,
                           void* factory,
                           CreatelistItem* item) noexcept {
    void* result = nullptr;
    __try {
        result = fn(factory, item);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = nullptr;
    }
    return result;
}

void* CreateRemoteCharacter(const Vec3& position) noexcept {
    return CreateRemoteCharacterFromGameData(
        position, s_recordedGameData.load(std::memory_order_relaxed));
}

void* CreateRemoteCharacterFromGameData(const Vec3& position, void* suppliedGameData) noexcept {
    auto fn = s_processFn.load(std::memory_order_relaxed);
    if (!fn) {
        spdlog::warn("OurFactory: CreateRemoteCharacter — process() not resolved");
        return nullptr;
    }

    void* faction  = s_recordedFaction.load(std::memory_order_relaxed);
    void* gameData = suppliedGameData;
    if (!faction || !gameData) {
        spdlog::warn("OurFactory: CreateRemoteCharacter — donor fields not recorded yet "
                     "(faction/template not ready; faction=0x{:X} gameData=0x{:X})",
                     reinterpret_cast<uintptr_t>(faction),
                     reinterpret_cast<uintptr_t>(gameData));
        return nullptr;
    }

    // Look up the live RootObjectFactory pointer (already discovered by
    // SpawnManager via the mod+0x21345B0 deterministic global read).
    auto& sm = Core::Get().GetSpawnManager();
    void* factory = sm.GetFactory();
    if (!entity_hooks::IsHeapResidentPtr(reinterpret_cast<uintptr_t>(factory))) {
        spdlog::warn("OurFactory: factory not yet captured");
        return nullptr;
    }

    // Build the CreatelistItem on the stack — process() copies what it needs.
    CreatelistItem item = {};
    item.container             = nullptr;          // null = global container
    item.homeBuilding          = nullptr;          // not housed
    item.faction               = faction;
    item.data                  = gameData;
    item.position_x            = position.x;
    item.position_y            = position.y;
    item.position_z            = position.z;
    item.isFromActiveLevelMod  = false;
    item.rotation_w            = 1.0f;             // identity quaternion
    item.rotation_x            = 0.0f;
    item.rotation_y            = 0.0f;
    item.rotation_z            = 0.0f;
    item.callbackObject        = nullptr;
    item.saveState             = nullptr;          // live spawn (no save load)
    item.age                   = 20.0f;            // adult default

    void* result = InvokeProcess(fn, factory, &item);
    if (!result) {
        spdlog::error("OurFactory: process() returned null or AV'd "
                      "(factory=0x{:X} faction=0x{:X} gd=0x{:X} pos={:.1f},{:.1f},{:.1f})",
                      reinterpret_cast<uintptr_t>(factory),
                      reinterpret_cast<uintptr_t>(faction),
                      reinterpret_cast<uintptr_t>(gameData),
                      position.x, position.y, position.z);
        return nullptr;
    }

    // Validate result looks like a real heap pointer
    if (!entity_hooks::IsHeapResidentPtr(reinterpret_cast<uintptr_t>(result))) {
        spdlog::warn("OurFactory: process() returned suspicious pointer 0x{:X}",
                     reinterpret_cast<uintptr_t>(result));
        return nullptr;
    }

    // Mark remote-controlled so local AI doesn't drive the new character.
    ai_hooks::MarkRemoteControlled(result);

    s_remoteCount.fetch_add(1, std::memory_order_relaxed);
    spdlog::info("OurFactory: CreateRemoteCharacter SUCCESS — char 0x{:X} at "
                 "({:.1f},{:.1f},{:.1f}) (remote count = {})",
                 reinterpret_cast<uintptr_t>(result),
                 position.x, position.y, position.z,
                 s_remoteCount.load());
    return result;
}

} // namespace kmp::our_factory
