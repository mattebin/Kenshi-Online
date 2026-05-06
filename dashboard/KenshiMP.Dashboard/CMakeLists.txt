# =========================================================================
#                       KenshiMP.Dashboard
# =========================================================================
# The unified out-of-game client.  Win32 GUI shell that becomes the user's
# primary entry point — over time, every feature currently living inside
# the legacy KenshiMP.Core.dll migrates to this binary.
#
# Phase 1 (initial): observer + child-process launcher.  No IPC, no
# in-game footprint changes.  See ../docs/ARCHITECTURE_v2.md.
add_executable(KenshiMP.Dashboard WIN32
    main.cpp
    process_status.cpp
    log_aggregator.cpp
    spawn_queue.cpp
)

target_include_directories(KenshiMP.Dashboard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(KenshiMP.Dashboard PRIVATE
    KenshiMP.Common
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    shlwapi
    psapi
    comctl32
    shell32
)

target_compile_definitions(KenshiMP.Dashboard PRIVATE UNICODE _UNICODE)

# Auto-deploy alongside the game.
get_filename_component(KENSHI_DIR "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
if(EXISTS "${KENSHI_DIR}/kenshi_x64.exe")
    add_custom_command(TARGET KenshiMP.Dashboard POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:KenshiMP.Dashboard>"
            "${KENSHI_DIR}/KenshiMP.Dashboard.exe"
        COMMENT "Deploying KenshiMP.Dashboard.exe"
    )
endif()
