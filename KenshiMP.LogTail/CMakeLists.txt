add_executable(KenshiMP.LogTail
    main.cpp
)

target_include_directories(KenshiMP.LogTail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KenshiMP.LogTail PRIVATE
    shlwapi
    shell32
)

target_compile_definitions(KenshiMP.LogTail PRIVATE UNICODE _UNICODE)
