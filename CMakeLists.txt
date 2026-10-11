cmake_minimum_required(VERSION 3.16)
project(fpeb C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(raylib)

add_executable(fpeb
    src/main.c
    src/lr_host.c
    src/rom_check.c
    src/eb_world.c
    src/world3d.c
    src/blueprints.c
    src/tree_match.c
    src/viewer.c
    src/log.c)
target_include_directories(fpeb PRIVATE src third_party/libretro)
target_link_libraries(fpeb PRIVATE raylib)
set_target_properties(fpeb PROPERTIES OUTPUT_NAME FPEB)

if(MSVC)
    target_compile_options(fpeb PRIVATE /W3)
    target_compile_definitions(fpeb PRIVATE _CRT_SECURE_NO_WARNINGS)
    # No console window for players; logs go to fpeb.log.
    set_target_properties(fpeb PROPERTIES WIN32_EXECUTABLE TRUE LINK_FLAGS "/ENTRY:mainCRTStartup")
else()
    target_compile_options(fpeb PRIVATE -Wall -Wextra -Wno-unused-parameter)
    if(UNIX)
        target_link_libraries(fpeb PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()

enable_testing()
add_executable(test_rom_check tests/test_rom_check.c src/rom_check.c)
target_include_directories(test_rom_check PRIVATE src)
add_test(NAME rom_check COMMAND test_rom_check)

# Developer tool: top-down render of the map, to check the ROM decoder.
add_executable(ebmapdump tools/ebmapdump.c src/eb_world.c)
target_include_directories(ebmapdump PRIVATE src)
target_link_libraries(ebmapdump PRIVATE raylib)
