#ifndef _PIRENEUS_IMGUI_H_
#define _PIRENEUS_IMGUI_H_

// Dear ImGui integration for the engine (C++ API : ImGui itself is C++, so is any code using it).
//
// Input comes from ImGui's official SDL3 platform backend (fed through set_event_callback(), see
// platforms_window.h). Rendering does NOT use one of ImGui's official renderer backends : ImGui's
// draw lists are converted to the engine's own draw_vertices()/set_scissor() primitives, and its
// textures (font atlas) are created with create_texture_from_pixels(). It therefore runs on every
// renderer.h backend (Vulkan, DX12, ...) unchanged, and an ImTextureID is simply a Texture* : any
// engine texture can be shown in the UI with ImGui::Image(pireneus_imgui_texture(texture), ...).
//
// Per-frame usage :
//     pireneus_imgui_new_frame();
//     ... ImGui:: calls ...
//     pireneus_imgui_prepare_frame();   // BEFORE begin_frame() : ImGui::Render() + texture uploads
//     if (begin_frame())
//     {
//         ... game/editor drawing ...
//         pireneus_imgui_draw();        // last, so the UI is drawn on top
//         end_frame();
//     }

#include "imgui.h"

struct Texture;

bool pireneus_imgui_init(void);  // after create_window() and create_renderer()
void pireneus_imgui_shutdown(void); // before destroy_renderer()

void pireneus_imgui_new_frame(void);
void pireneus_imgui_prepare_frame(void);
void pireneus_imgui_draw(void);

inline ImTextureID pireneus_imgui_texture(Texture* texture)
{
    return (ImTextureID)(intptr_t)texture;
}

#endif // _PIRENEUS_IMGUI_H_
