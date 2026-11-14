#ifndef _UWP_EVENTS_H_
#define _UWP_EVENTS_H_

// The UWP platform layer's native event (see EventCallback in platforms_window.h) : SDL3 has no UWP
// support, so what src/imgui gets from an SDL_Event elsewhere comes in this form here.

typedef enum
{
    UWP_EVENT_MOUSE_MOVE,
    UWP_EVENT_MOUSE_BUTTON,
    UWP_EVENT_MOUSE_WHEEL,
    UWP_EVENT_KEY,
    UWP_EVENT_CHARACTER,
    UWP_EVENT_FOCUS
} UwpEventType;

typedef struct
{
    UwpEventType type;
    float x, y;            // MOUSE_MOVE : position in pixels
    int button;            // MOUSE_BUTTON : 0 left, 1 right, 2 middle
    int down;              // MOUSE_BUTTON, KEY : pressed ; FOCUS : gained
    float wheel;           // MOUSE_WHEEL : in notches
    int virtual_key;       // KEY : Windows::System::VirtualKey
    unsigned int character; // CHARACTER : UTF-16 code unit
} UwpEvent;

#endif // _UWP_EVENTS_H_
