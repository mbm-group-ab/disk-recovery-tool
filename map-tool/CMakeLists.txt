add_executable(rescue_map_tool
    main.cpp
    ../common/rescue_map.cpp
)
target_include_directories(rescue_map_tool PRIVATE ../common)
