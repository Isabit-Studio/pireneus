#include <stdio.h>
#include <SDL3/SDL.h>

#include "platforms/platforms_window.h"

int main(int argc, char **argv)
{
    if (!create_window("Pireneus Engine", 800, 600))
    {
        printf("Failed to create window.\n");
        return -1;
    }

    while (1)
    {
        if (!handle_events())
        {
            break;
        }
    }
    
    destroy_window();

    return 0;
}
