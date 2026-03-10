#include "../platforms_window.h"
#include <stdio.h>
#include <SDL3/SDL.h>

SDL_Window* window;

int create_window(const char* title, int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO) == false)
    {
        printf("Unable to initialize SDL: %s\n", SDL_GetError());

        return 0;
    }

    window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE);
    
    if (window == NULL)
    {
        printf("Could not create window: %s\n", SDL_GetError());
        
        SDL_Quit();
        
        return 0;
    }

    return 1;
}

int handle_events()
{
    SDL_Event event;
    SDL_PollEvent(&event);

    if (event.type == SDL_EVENT_QUIT)
    {
        return 0;
    }

    return 1;
}

void destroy_window()
{
    SDL_DestroyWindow(window);
    SDL_Quit();
}