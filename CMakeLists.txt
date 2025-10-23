cmake_minimum_required(VERSION 4.0)

project(pireneus)

# Include third-party libraries
add_subdirectory(third-parties)

add_executable(pireneus src/main.c)

target_include_directories(pireneus PUBLIC ${PIRENEUS_THIRDPARTIES_INCLUDES})
target_link_libraries(pireneus SDL3-static)