#include "../platforms_input.h"

#include <SDL3/SDL.h>

// The engine's Scancode values are SDL's own (both are USB HID usage IDs) : a plain cast at the
// boundary is enough. These checks keep that assumption honest.
SDL_COMPILE_TIME_ASSERT(scancode_a, (int)SCANCODE_A == (int)SDL_SCANCODE_A);
SDL_COMPILE_TIME_ASSERT(scancode_z, (int)SCANCODE_Z == (int)SDL_SCANCODE_Z);
SDL_COMPILE_TIME_ASSERT(scancode_0, (int)SCANCODE_0 == (int)SDL_SCANCODE_0);
SDL_COMPILE_TIME_ASSERT(scancode_space, (int)SCANCODE_SPACE == (int)SDL_SCANCODE_SPACE);
SDL_COMPILE_TIME_ASSERT(scancode_leftbracket, (int)SCANCODE_LEFTBRACKET == (int)SDL_SCANCODE_LEFTBRACKET);
SDL_COMPILE_TIME_ASSERT(scancode_rightbracket, (int)SCANCODE_RIGHTBRACKET == (int)SDL_SCANCODE_RIGHTBRACKET);
SDL_COMPILE_TIME_ASSERT(scancode_up, (int)SCANCODE_UP == (int)SDL_SCANCODE_UP);
SDL_COMPILE_TIME_ASSERT(scancode_lctrl, (int)SCANCODE_LCTRL == (int)SDL_SCANCODE_LCTRL);
SDL_COMPILE_TIME_ASSERT(scancode_ralt, (int)SCANCODE_RALT == (int)SDL_SCANCODE_RALT);
SDL_COMPILE_TIME_ASSERT(scancode_count, (int)SCANCODE_COUNT == (int)SDL_SCANCODE_COUNT);

int input_is_key_down(Scancode scancode)
{
    if (scancode <= SCANCODE_UNKNOWN || scancode >= SCANCODE_COUNT)
    {
        return 0;
    }

    // Indexed by scancode, i.e. physical position : layout-independent by construction.
    int key_count = 0;
    const bool* state = SDL_GetKeyboardState(&key_count);

    return (int)scancode < key_count && state[scancode];
}

const char* input_key_label(Scancode scancode)
{
    // The keycode this position produces on the current layout (no modifiers), then its name :
    // SDL_SCANCODE_W -> SDLK_Z -> "Z" on AZERTY.
    SDL_Keycode key = SDL_GetKeyFromScancode((SDL_Scancode)scancode, SDL_KMOD_NONE, false);
    const char* name = SDL_GetKeyName(key);

    // SDL only keeps the name valid until its next call : copied into a small ring of buffers, so
    // a few labels can be used together (e.g. formatted into one "Z/Q/S/D" hint).
    static char labels[16][32];
    static int next_label;

    char* label = labels[next_label];
    next_label = (next_label + 1) % 16;
    SDL_snprintf(label, sizeof(labels[0]), "%s", name ? name : "");

    return label;
}

static float mouse_dx;
static float mouse_dy;

extern SDL_Window* window;

void platform_input_begin_frame(void)
{
    mouse_dx = 0.0f;
    mouse_dy = 0.0f;
}

void platform_input_process_event(const SDL_Event* event)
{
    if (event->type == SDL_EVENT_MOUSE_MOTION)
    {
        mouse_dx += event->motion.xrel;
        mouse_dy += event->motion.yrel;
    }
}

void input_set_mouse_captured(int captured)
{
    SDL_SetWindowRelativeMouseMode(window, captured != 0);
}

void input_get_mouse_delta(float* out_dx, float* out_dy)
{
    *out_dx = mouse_dx;
    *out_dy = mouse_dy;
}
