# Force static linking of SDL2
set(SDL_SHARED OFF)
set(SDL_STATIC ON)

set(PIRENEUS_THIRDPARTIES_INCLUDES SDL/include)

add_subdirectory(SDL)