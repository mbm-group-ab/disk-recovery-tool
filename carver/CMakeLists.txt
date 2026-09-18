add_executable(carver
    main.cpp
    ../common/rescue_map.cpp
    ../common/random_access_reader.cpp
    ../common/sha256.cpp
)
target_include_directories(carver PRIVATE ../common)
target_link_libraries(carver PRIVATE bcrypt)
target_compile_definitions(carver PRIVATE RECOVERY_REGULAR_FILES_ONLY)
set_target_properties(carver PROPERTIES OUTPUT_NAME recovery_carver)
