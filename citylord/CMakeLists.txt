cmake_minimum_required(VERSION 3.16)
project(citylord C)

set(CMAKE_C_STANDARD 99)
include(FetchContent)
FetchContent_Declare(raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(raylib)

add_executable(citylord
    src/main.c src/citylord_data.c src/cl_assets.c src/cl_render.c src/cl_physics.c src/cl_aim.c)
target_link_libraries(citylord PRIVATE raylib)
if(UNIX AND NOT APPLE)
    target_link_libraries(citylord PRIVATE m)
endif()
