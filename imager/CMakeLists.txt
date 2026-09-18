add_executable(imager
    main.cpp
    ../common/rescue_map.cpp
    ../common/chunked_image_writer.cpp
    ../common/sha256.cpp
)
target_include_directories(imager PRIVATE ../common)
target_link_libraries(imager PRIVATE bcrypt)
if(MINGW)
    target_link_options(imager PRIVATE -municode)
endif()
