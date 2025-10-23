#include <stdio.h>
#include <SDL3/SDL.h>

int main(int argc, char **argv)
{
    if ( SDL_Init(SDL_INIT_VIDEO) == false )
    {
        printf("Unable to initialize SDL: %s\n", SDL_GetError());

        return 1;
    }

    SDL_Window *window;
    
    if (window = SDL_CreateWindow( "Pireneus", 800, 600, SDL_WINDOW_RESIZABLE) == NULL)
    {
        printf("Could not create window: %s\n", SDL_GetError());
        
        SDL_Quit();
        
        return 1;
    }

    SDL_Event event;
    printf( "Hello, Pireneus!\n" );

    while (1)
    {
        SDL_PollEvent( &event );

        if (event.type == SDL_EVENT_QUIT)
        {
            break;
        }
    }

    SDL_DestroyWindow( window );
    SDL_Quit();
    
    return 0;
}
