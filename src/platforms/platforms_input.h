#ifndef _PLATFORMS_INPUT_H_
#define _PLATFORMS_INPUT_H_

#ifdef __cplusplus
extern "C" {
#endif

// Keyboard keys identified by PHYSICAL POSITION (scancodes), independent of the keyboard layout :
// SCANCODE_W is the key left of E on the top letter row, wherever that is "W" (QWERTY) or "Z"
// (French AZERTY). Use these for anything positional — movement (WASD on QWERTY is ZQSD on AZERTY),
// grouped tool keys — and layout-aware keys (e.g. ImGui's) for mnemonic shortcuts like Ctrl+Z.
//
// Names follow the US QWERTY layout, as is customary. Values are the USB HID keyboard usage IDs,
// which SDL scancodes also use (checked at compile time in the SDL implementation).
typedef enum
{
    SCANCODE_UNKNOWN = 0,

    SCANCODE_A = 4, SCANCODE_B, SCANCODE_C, SCANCODE_D, SCANCODE_E, SCANCODE_F, SCANCODE_G,
    SCANCODE_H, SCANCODE_I, SCANCODE_J, SCANCODE_K, SCANCODE_L, SCANCODE_M, SCANCODE_N,
    SCANCODE_O, SCANCODE_P, SCANCODE_Q, SCANCODE_R, SCANCODE_S, SCANCODE_T, SCANCODE_U,
    SCANCODE_V, SCANCODE_W, SCANCODE_X, SCANCODE_Y, SCANCODE_Z,

    SCANCODE_1 = 30, SCANCODE_2, SCANCODE_3, SCANCODE_4, SCANCODE_5,
    SCANCODE_6, SCANCODE_7, SCANCODE_8, SCANCODE_9, SCANCODE_0,

    SCANCODE_RETURN = 40,
    SCANCODE_ESCAPE = 41,
    SCANCODE_BACKSPACE = 42,
    SCANCODE_TAB = 43,
    SCANCODE_SPACE = 44,
    SCANCODE_LEFTBRACKET = 47,  // AZERTY : ^
    SCANCODE_RIGHTBRACKET = 48, // AZERTY : $

    SCANCODE_RIGHT = 79,
    SCANCODE_LEFT = 80,
    SCANCODE_DOWN = 81,
    SCANCODE_UP = 82,

    SCANCODE_LCTRL = 224,
    SCANCODE_LSHIFT = 225,
    SCANCODE_LALT = 226,
    SCANCODE_RCTRL = 228,
    SCANCODE_RSHIFT = 229,
    SCANCODE_RALT = 230,

    SCANCODE_COUNT = 512
} Scancode;

// Whether the key at this physical position is currently held. Reflects the events last processed
// by handle_events() (see platforms_window.h).
int input_is_key_down(Scancode scancode);

// The label of the key at this physical position on the CURRENT keyboard layout, for UI hints :
// "Z" for SCANCODE_W on a French AZERTY keyboard, "W" on QWERTY. Never NULL (empty if unknown).
// The returned string stays valid for the next 15 calls, so several labels can be combined at once
// (e.g. in one printf-style call).
const char* input_key_label(Scancode scancode);

// Mouse capture, for games : the cursor disappears and only relative motion is reported.
void input_set_mouse_captured(int captured);

// The mouse's motion since the last handle_events(), in pixels.
void input_get_mouse_delta(float* out_dx, float* out_dy);

#ifdef __cplusplus
}
#endif

#endif // _PLATFORMS_INPUT_H_
