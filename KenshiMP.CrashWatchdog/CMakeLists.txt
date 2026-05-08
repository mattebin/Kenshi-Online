# =========================================================================
#                      KenshiMP.CrashWatchdog
# =========================================================================
# Out-of-process supervisor for kenshi_x64.exe.  Captures forensic data
# on game exit (clean or crash), writes a timestamped report next to
# the game logs, and answers the recurring question "was that a crash
# or did the user exit" definitively via the process exit code.
#
# No external deps beyond the system runtime — pure Win32 + C++17.
# Standalone exe; the injector spawns it after launching Kenshi.
add_executable(KenshiMP.CrashWatchdog
    main.cpp
)

target_include_directories(KenshiMP.CrashWatchdog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(KenshiMP.CrashWatchdog PRIVATE
    shlwapi
    psapi
)

target_compile_definitions(KenshiMP.CrashWatchdog PRIVATE UNICODE _UNICODE)

# Auto-deploy alongside the game so the injector can find it.
get_filename_component(KENSHI_DIR "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
if(EXISTS "${KENSHI_DIR}/kenshi_x64.exe")
    add_custom_command(TARGET KenshiMP.CrashWatchdog POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:KenshiMP.CrashWatchdog>"
            "${KENSHI_DIR}/KenshiMP.CrashWatchdog.exe"
        COMMENT "Deploying KenshiMP.CrashWatchdog.exe"
    )
endif()
