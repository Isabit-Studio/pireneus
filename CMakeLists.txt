cmake_minimum_required(VERSION 4.0)

set(PIRENEUS_PLATFORM "win32") # sdl, win32

project(pireneus)

# Export compile commands for tools like clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include third-party libraries
add_subdirectory(third-parties)

add_executable(pireneus src/main.c src/platforms/${PIRENEUS_PLATFORM}/${PIRENEUS_PLATFORM}_platform_window.c)

target_include_directories(pireneus PUBLIC ${PIRENEUS_THIRDPARTIES_INCLUDES})
target_link_libraries(pireneus SDL3-static)