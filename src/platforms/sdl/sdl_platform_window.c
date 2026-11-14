#include "../platforms_window.h"
#include <stdio.h>
#include <SDL3/SDL.h>

#include "debug.h"

SDL_Window* window;
static RedrawCallback redraw_callback;
static EventCallback event_callback;
static FileDropCallback file_drop_callback;

void platform_input_begin_frame(void);                 // sdl_platform_input.c
void platform_input_process_event(const SDL_Event* event);
static int in_redraw_callback;

// Under a fast resize, some DXGI/D3D12 calls (Present in particular) can themselves pump the
// Win32 message queue internally, which would re-enter this — and so `redraw_callback` — while a
// previous call is still mid-frame, corrupting the renderer's shared state (e.g. which command
// buffer is currently being recorded). The guard applies regardless of which of the two call
// sites (the resize watcher below, or the main loop's own per-iteration call) is already running :
// a re-entrant call is simply dropped, the in-progress frame finishes normally, and the next real
// event/iteration still triggers a fresh redraw.
void call_redraw_callback(void)
{
    if (redraw_callback == NULL || in_redraw_callback)
    {
        return;
    }

    in_redraw_callback = 1;
    redraw_callback();
    in_redraw_callback = 0;
}

// SDL still pumps window events from inside the OS's modal resize/move loop, and calls event
// watchers synchronously as it does so (unlike SDL_PollEvent, which only returns once that
// modal loop has ended). Redrawing here is what keeps the window animating live instead of
// freezing while the user drags its border.
static bool redraw_event_watcher(void* userdata, SDL_Event* event)
{
    (void)userdata;

    if (event->type == SDL_EVENT_WINDOW_RESIZED)
    {
        DEBUG_LOG("[window] live resize: %dx%d\n", event->window.data1, event->window.data2);
    }

    if (event->type == SDL_EVENT_WINDOW_RESIZED || event->type == SDL_EVENT_WINDOW_EXPOSED)
    {
        call_redraw_callback();
    }

    return true;
}

int create_window(const char* title, int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO) == false)
    {
        printf("Unable to initialize SDL: %s\n", SDL_GetError());

        return 0;
    }

    window = SDL_CreateWindow(title, width, height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);

    if (window == NULL)
    {
        printf("Could not create window: %s\n", SDL_GetError());

        SDL_Quit();

        return 0;
    }

    SDL_AddEventWatch(redraw_event_watcher, NULL);

    DEBUG_LOG("[window] created \"%s\" (%dx%d)\n", title, width, height);

    return 1;
}

void platform_pump_dialogs(void); // sdl_platform_paths.c

int handle_events()
{
    SDL_Event event;

    platform_pump_dialogs();
    platform_input_begin_frame();

    while (SDL_PollEvent(&event))
    {
        if (event_callback)
        {
            event_callback(&event);
        }

        platform_input_process_event(&event);

        if (event.type == SDL_EVENT_DROP_FILE && file_drop_callback && event.drop.data)
        {
            file_drop_callback(event.drop.data);
        }

        if (event.type == SDL_EVENT_QUIT)
        {
            return 0;
        }
    }

    return 1;
}

void destroy_window()
{
    SDL_RemoveEventWatch(redraw_event_watcher, NULL);
    SDL_DestroyWindow(window);
    SDL_Quit();

    DEBUG_LOG("[window] destroyed\n");
}

Window* get_window(void)
{
    return (Window*)window;
}

void get_window_size(int* out_width, int* out_height)
{
    SDL_GetWindowSizeInPixels(window, out_width, out_height);
}

void set_window_title(const char* title)
{
    SDL_SetWindowTitle(window, title);
}

void set_redraw_callback(RedrawCallback callback)
{
    redraw_callback = callback;
}

void set_event_callback(EventCallback callback)
{
    event_callback = callback;
}

void set_file_drop_callback(FileDropCallback callback)
{
    file_drop_callback = callback;
}