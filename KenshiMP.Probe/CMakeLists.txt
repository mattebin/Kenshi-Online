# =========================================================================
#                          KenshiMP.Probe
# =========================================================================
# Out-of-process memory-layout validator.  Attaches to kenshi_x64.exe
# read-only via OpenProcess + ReadProcessMemory and continuously walks
# the data structures our addons depend on (PlayerBase, GameWorld,
# theFactory).  Reports validity to a log file every 2 s.
#
# Complements KenshiMP.Cartographer (static, byte-pattern analysis) by
# verifying the same structures at runtime.  Where the Cartographer
# says "this offset COULD work given the binary's layout", the Probe
# answers "is this offset actually valid right now in the running
# process".
#
# No external dependencies.
add_executable(KenshiMP.Probe
    main.cpp
)

target_include_directories(KenshiMP.Probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(KenshiMP.Probe PRIVATE
    shlwapi
    psapi
)

target_compile_definitions(KenshiMP.Probe PRIVATE UNICODE _UNICODE)

get_filename_component(KENSHI_DIR "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
if(EXISTS "${KENSHI_DIR}/kenshi_x64.exe")
    add_custom_command(TARGET KenshiMP.Probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:KenshiMP.Probe>"
            "${KENSHI_DIR}/KenshiMP.Probe.exe"
        COMMENT "Deploying KenshiMP.Probe.exe"
    )
endif()
