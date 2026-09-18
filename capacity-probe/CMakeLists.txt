add_executable(capacity_probe main.cpp)
if(MINGW)
    target_link_options(capacity_probe PRIVATE -municode)
endif()
