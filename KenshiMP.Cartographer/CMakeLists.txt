add_executable(KenshiMP.Cartographer
    main.cpp
)

target_include_directories(KenshiMP.Cartographer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(KenshiMP.Cartographer PRIVATE
    shlwapi
)

target_compile_definitions(KenshiMP.Cartographer PRIVATE UNICODE _UNICODE)
