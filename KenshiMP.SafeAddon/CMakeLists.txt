# =========================================================================
#                          KenshiMP.SafeAddon
# -------------------------------------------------------------------------
# A clean-room Ogre plugin DLL that interacts with Kenshi through exactly
# two function calls — `dllStartPlugin` and `dllStopPlugin`.  Between
# those, all work happens in our own worker thread reading game memory
# at validated offsets.  Zero MinHook trampolines, zero prologue
# rewrites, zero MovRaxRsp wrappers.  When this addon crashes, the
# game keeps running.
# =========================================================================

add_library(KenshiMP.SafeAddon SHARED
    dllmain.cpp
    safe_addon.cpp
)

target_include_directories(KenshiMP.SafeAddon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(KenshiMP.SafeAddon PRIVATE
    KenshiMP.Common
    enet
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ws2_32
    winmm
)

target_compile_definitions(KenshiMP.SafeAddon PRIVATE UNICODE _UNICODE)

# Force Release-mode container layout, same reasoning as KenshiMP.Core:
# Kenshi's std::string/vector come from a Release-built MyGUI/Ogre and
# any Debug-mode addition would break ABI on container boundaries.
target_compile_definitions(KenshiMP.SafeAddon PRIVATE _ITERATOR_DEBUG_LEVEL=0)

# Match the .dll naming Ogre's plugin loader expects: it loads "Plugin=X"
# entries from Plugins_x64.cfg as `X.dll`, no prefix.  CMake on Windows
# defaults to no `lib` prefix for SHARED targets so we just need to
# confirm the output filename is exactly the target name.
set_target_properties(KenshiMP.SafeAddon PROPERTIES
    PREFIX ""
    OUTPUT_NAME "KenshiMP.SafeAddon"
)

# Auto-deploy to the Kenshi install directory next to KenshiMP.Core.dll
# so users can switch between the two by editing one line in
# Plugins_x64.cfg.  Keeping both DLLs side-by-side; only the one named
# in the config gets loaded.
get_filename_component(KENSHI_DIR "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
if(EXISTS "${KENSHI_DIR}/kenshi_x64.exe")
    add_custom_command(TARGET KenshiMP.SafeAddon POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:KenshiMP.SafeAddon>"
            "${KENSHI_DIR}/KenshiMP.SafeAddon.dll"
        COMMENT "Deploying KenshiMP.SafeAddon.dll to Kenshi directory"
    )
endif()
