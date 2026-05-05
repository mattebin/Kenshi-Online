add_executable(KenshiMP.Server
    main.cpp
    server.cpp
    upnp.cpp
    game_state.cpp
    player_manager.cpp
    entity_manager.cpp
    zone_manager.cpp
    combat_resolver.cpp
    world_persistence.cpp
)

target_include_directories(KenshiMP.Server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KenshiMP.Server PRIVATE
    KenshiMP.Common
    enet
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ws2_32
    winmm
    ole32
    oleaut32
    iphlpapi
)

# Copy server exe to Kenshi directory so the Host button can find it.
# Use the same KENSHI_DIR cache variable that KenshiMP.Core sets (it points
# at the live Steam install). The previous version of this rule used
# ${CMAKE_SOURCE_DIR}/.. which only works when the project sits inside the
# Kenshi folder — broken on most dev setups, silently skipped the deploy.
set(KENSHI_DIR "${CMAKE_SOURCE_DIR}/.." CACHE PATH "Path to Kenshi install directory")
get_filename_component(KENSHI_DIR_ABS "${KENSHI_DIR}" ABSOLUTE)
if(EXISTS "${KENSHI_DIR_ABS}/kenshi_x64.exe")
    add_custom_command(TARGET KenshiMP.Server POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:KenshiMP.Server>"
            "${KENSHI_DIR_ABS}/KenshiMP.Server.exe"
        COMMENT "Copying KenshiMP.Server.exe to Kenshi directory"
    )
else()
    message(WARNING "KENSHI_DIR does not contain kenshi_x64.exe: ${KENSHI_DIR_ABS}. Skipping KenshiMP.Server auto-deploy.")
endif()
