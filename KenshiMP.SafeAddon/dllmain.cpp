// =========================================================================
//                   KenshiMP.SafeAddon — Ogre plugin entry
// =========================================================================
// The two-call game-facing surface.  Ogre's plugin loader calls
// `dllStartPlugin` exactly once at boot and `dllStopPlugin` exactly
// once at shutdown.  Everything else lives in our worker thread.
//
// We intentionally do NOT export anything else — no global hooks, no
// game-side callbacks, no message-pump subscriptions.  If a future
// game version drops the plugin-cfg mechanism, the addon's start/stop
// path can move to a manual `LoadLibrary` shim, and nothing else
// changes.
#include "safe_addon.h"
#include <Windows.h>

extern "C" {

__declspec(dllexport) void dllStartPlugin() {
    // The "first call" — game-side surface entry.  Boot the addon
    // and return.  `Start` itself is non-blocking; the worker thread
    // it spawns owns everything from here.
    kmp::safe::SafeAddon::Get().Start();
}

__declspec(dllexport) void dllStopPlugin() {
    // The "end call" — game-side surface exit.  Stop the worker
    // thread (joins it) and release resources.  Returning from this
    // function tells Ogre we're done; it then unloads the DLL.
    kmp::safe::SafeAddon::Get().Stop();
}

} // extern "C"

// Standard DLL entry.  No work happens here — Ogre drives lifetime via
// the two exports above.  We disable the per-thread `DllMain` callbacks
// because the worker thread we spawn doesn't need them and it cuts
// overhead during games that spawn many threads (Kenshi's renderer
// alone has half a dozen).
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        // Ogre should call dllStopPlugin first, but defensively stop
        // the worker if it's still running so we don't leak the
        // thread on emergency teardown paths.
        if (kmp::safe::SafeAddon::Get().IsRunning()) {
            kmp::safe::SafeAddon::Get().Stop();
        }
        break;
    }
    return TRUE;
}
