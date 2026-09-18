add_executable(exfat_parser
    main.cpp
    ../common/rescue_map.cpp
    ../common/random_access_reader.cpp
    ../common/sha256.cpp
)
target_include_directories(exfat_parser PRIVATE ../common)
target_link_libraries(exfat_parser PRIVATE bcrypt)
set_target_properties(exfat_parser PROPERTIES OUTPUT_NAME exfat_recovery)
