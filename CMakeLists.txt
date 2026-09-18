cmake_minimum_required(VERSION 3.20)
project(disk_recovery_tool CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/O2 /W4 /EHsc)
else()
    add_compile_options(-O3 -Wall -Wextra)
    if(MINGW)
        # Produce self-contained executables; this avoids runtime-DLL drift on
        # the recovery workstation and is accepted by stricter App Control setups.
        add_link_options(-static -static-libgcc -static-libstdc++)
    endif()
endif()

add_subdirectory(imager)
add_subdirectory(capacity-probe)
add_subdirectory(exfat-parser)
add_subdirectory(carver)
add_subdirectory(map-tool)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
