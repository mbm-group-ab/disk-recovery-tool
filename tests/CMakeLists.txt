add_executable(rescue_map_tests
    rescue_map_tests.cpp
    ../common/rescue_map.cpp
)
target_include_directories(rescue_map_tests PRIVATE ../common)
add_executable(chunk_store_tests
    chunk_store_tests.cpp
    ../common/chunked_image_writer.cpp
    ../common/random_access_reader.cpp
    ../common/sha256.cpp
)
target_include_directories(chunk_store_tests PRIVATE ../common)
target_link_libraries(chunk_store_tests PRIVATE bcrypt)
add_executable(exfat_file_test_runner
    ../exfat-parser/main.cpp
    ../common/rescue_map.cpp
    ../common/random_access_reader.cpp
    ../common/sha256.cpp
)
target_include_directories(exfat_file_test_runner PRIVATE ../common)
target_link_libraries(exfat_file_test_runner PRIVATE bcrypt)
target_compile_definitions(exfat_file_test_runner PRIVATE RECOVERY_REGULAR_FILES_ONLY)
set_target_properties(exfat_file_test_runner PROPERTIES OUTPUT_NAME metadata_recovery_test)
add_executable(imager_file_test_runner
    ../imager/main.cpp
    ../common/rescue_map.cpp
    ../common/chunked_image_writer.cpp
    ../common/sha256.cpp
)
target_include_directories(imager_file_test_runner PRIVATE ../common)
target_link_libraries(imager_file_test_runner PRIVATE bcrypt)
target_compile_definitions(imager_file_test_runner PRIVATE RECOVERY_REGULAR_FILES_ONLY)
set_target_properties(imager_file_test_runner PROPERTIES OUTPUT_NAME image_copy_test)
if(MINGW)
    target_link_options(imager_file_test_runner PRIVATE -municode)
endif()
add_test(NAME rescue_map_tests COMMAND rescue_map_tests)
add_test(NAME chunk_store_tests COMMAND chunk_store_tests)
add_test(NAME exfat_parser_integration
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/exfat_parser_integration.ps1"
            -Parser "$<TARGET_FILE:exfat_file_test_runner>")
add_test(NAME imager_integration
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/imager_integration.ps1"
            -Imager "$<TARGET_FILE:imager_file_test_runner>")
add_test(NAME carver_integration
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/carver_integration.ps1"
            -Carver "$<TARGET_FILE:carver>")
add_test(NAME map_tool_integration
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/map_tool_integration.ps1"
            -MapTool "$<TARGET_FILE:rescue_map_tool>")
if(MINGW)
    get_filename_component(MINGW_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(rescue_map_tests chunk_store_tests exfat_parser_integration imager_integration carver_integration map_tool_integration PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${MINGW_RUNTIME_DIR}")
endif()
