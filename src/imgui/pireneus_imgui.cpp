#include "imgui/pireneus_imgui.h"

#ifndef PIRENEUS_UWP
#include "imgui_impl_sdl3.h"
#else
#include "platforms/uwp/uwp_events.h"
#endif
#include <SDL3/SDL.h>

#include "platforms/platforms_window.h"
#include "renderer/renderer.h"
#include "debug.h"

// ImGui's vertices are indexed ; draw_vertices() takes a plain triangle list. Indices are expanded
// on the CPU into this scratch buffer, a fixed-size chunk at a time (a multiple of 3, so a chunk
// never splits a triangle).
#define EXPANDED_VERTEX_CHUNK (3 * 4096)

static Vertex expanded_vertices[EXPANDED_VERTEX_CHUNK];

#ifdef PIRENEUS_UWP
// Windows::System::VirtualKey values (the same as Win32's VK_*).
static ImGuiKey to_imgui_key(int key)
{
    if (key >= 'A' && key <= 'Z')
    {
        return (ImGuiKey)(ImGuiKey_A + (key - 'A'));
    }

    if (key >= '0' && key <= '9')
    {
        return (ImGuiKey)(ImGuiKey_0 + (key - '0'));
    }

    if (key >= 0x70 && key <= 0x7B)
    {
        return (ImGuiKey)(ImGuiKey_F1 + (key - 0x70));
    }

    switch (key)
    {
    case 0x08: return ImGuiKey_Backspace;
    case 0x09: return ImGuiKey_Tab;
    case 0x0D: return ImGuiKey_Enter;
    case 0x1B: return ImGuiKey_Escape;
    case 0x20: return ImGuiKey_Space;
    case 0x21: return ImGuiKey_PageUp;
    case 0x22: return ImGuiKey_PageDown;
    case 0x23: return ImGuiKey_End;
    case 0x24: return ImGuiKey_Home;
    case 0x25: return ImGuiKey_LeftArrow;
    case 0x26: return ImGuiKey_UpArrow;
    case 0x27: return ImGuiKey_RightArrow;
    case 0x28: return ImGuiKey_DownArrow;
    case 0x2D: return ImGuiKey_Insert;
    case 0x2E: return ImGuiKey_Delete;
    default: return ImGuiKey_None;
    }
}

static void on_platform_event(const void* native_event)
{
    const UwpEvent* event = (const UwpEvent*)native_event;
    ImGuiIO& io = ImGui::GetIO();

    switch (event->type)
    {
    case UWP_EVENT_MOUSE_MOVE:
        io.AddMousePosEvent(event->x, event->y);
        break;

    case UWP_EVENT_MOUSE_BUTTON:
        io.AddMouseButtonEvent(event->button, event->down != 0);
        break;

    case UWP_EVENT_MOUSE_WHEEL:
        io.AddMouseWheelEvent(0.0f, event->wheel);
        break;

    case UWP_EVENT_CHARACTER:
        io.AddInputCharacterUTF16((ImWchar16)event->character);
        break;

    case UWP_EVENT_FOCUS:
        io.AddFocusEvent(event->down != 0);
        break;

    case UWP_EVENT_KEY:
    {
        // Modifiers arrive as keys of their own.
        if (event->virtual_key == 0x10) { io.AddKeyEvent(ImGuiMod_Shift, event->down != 0); }
        else if (event->virtual_key == 0x11) { io.AddKeyEvent(ImGuiMod_Ctrl, event->down != 0); }
        else if (event->virtual_key == 0x12) { io.AddKeyEvent(ImGuiMod_Alt, event->down != 0); }
        else
        {
            ImGuiKey key = to_imgui_key(event->virtual_key);

            if (key != ImGuiKey_None)
            {
                io.AddKeyEvent(key, event->down != 0);
            }
        }

        break;
    }
    }
}
#else
// Forwarded from handle_events() (see set_event_callback()) : the SDL implementation of the platform
// layer hands over its own SDL_Event, which is exactly what ImGui's SDL3 backend consumes.
static void on_platform_event(const void* native_event)
{
    ImGui_ImplSDL3_ProcessEvent((const SDL_Event*)native_event);
}
#endif

static Texture* to_texture(ImTextureID id)
{
    return (Texture*)(intptr_t)id;
}

static void destroy_imgui_texture(ImTextureData* texture_data)
{
    destroy_texture(to_texture(texture_data->GetTexID()));
    texture_data->SetTexID(ImTextureID_Invalid);
    texture_data->SetStatus(ImTextureStatus_Destroyed);
}

// Services ImGui's texture requests (ImGuiBackendFlags_RendererHasTextures) : today that's only the
// font atlas, created once and occasionally grown/updated when new glyphs get rasterized. Updates
// are rare, so they simply re-upload the whole texture rather than the dirty sub-rectangles : that
// keeps the RHI free of any partial-update entry point. The previous texture may still be in use by
// a frame in flight, hence the wait before destroying anything.
static void update_textures(ImDrawData* draw_data)
{
    if (!draw_data->Textures)
    {
        return;
    }

    for (int i = 0; i < draw_data->Textures->Size; i++)
    {
        ImTextureData* texture_data = (*draw_data->Textures)[i];

        if (texture_data->Status == ImTextureStatus_OK || texture_data->Status == ImTextureStatus_Destroyed)
        {
            continue;
        }

        if (texture_data->Status == ImTextureStatus_WantDestroy)
        {
            wait_renderer_idle();
            destroy_imgui_texture(texture_data);
            continue;
        }

        // WantCreate or WantUpdates : (re)create the texture from ImGui's full, up-to-date pixel copy.
        IM_ASSERT(texture_data->Format == ImTextureFormat_RGBA32);

        Texture* previous = to_texture(texture_data->GetTexID());

        if (previous)
        {
            wait_renderer_idle();
            destroy_texture(previous);
        }

        Texture* texture = create_texture_from_pixels(texture_data->GetPixels(), texture_data->Width, texture_data->Height);
        texture_data->SetTexID(texture ? (ImTextureID)(intptr_t)texture : ImTextureID_Invalid);

        if (!texture)
        {
            // Left in its WantCreate/WantUpdates state (with no TexID) rather than acknowledged : the
            // request is simply retried next frame, and draw_command() skips it meanwhile.
            printf("Could not create a %dx%d Dear ImGui texture.\n", texture_data->Width, texture_data->Height);
            continue;
        }

        texture_data->SetStatus(ImTextureStatus_OK);

        DEBUG_LOG("[imgui] texture %s: %dx%d\n", previous ? "updated" : "created", texture_data->Width, texture_data->Height);
    }
}

static Vertex to_vertex(const ImDrawVert* source, ImVec2 display_pos, ImVec2 scale)
{
    const float inv255 = 1.0f / 255.0f;
    ImU32 color = source->col;

    Vertex vertex;
    vertex.x = (source->pos.x - display_pos.x) * scale.x;
    vertex.y = (source->pos.y - display_pos.y) * scale.y;
    vertex.u = source->uv.x;
    vertex.v = source->uv.y;
    vertex.r = (float)((color >> IM_COL32_R_SHIFT) & 0xFF) * inv255;
    vertex.g = (float)((color >> IM_COL32_G_SHIFT) & 0xFF) * inv255;
    vertex.b = (float)((color >> IM_COL32_B_SHIFT) & 0xFF) * inv255;
    vertex.a = (float)((color >> IM_COL32_A_SHIFT) & 0xFF) * inv255;

    return vertex;
}

static void draw_command(const ImDrawList* draw_list, const ImDrawCmd* command, ImVec2 display_pos, ImVec2 scale)
{
    // TexRef.GetTexID() rather than ImDrawCmd::GetTexID() : the latter asserts on a texture that has no
    // GPU copy yet, which only happens here if its creation failed (see update_textures()) — skipping
    // those draws for a frame beats aborting the whole program.
    Texture* texture = to_texture(command->TexRef.GetTexID());

    if (!texture)
    {
        return;
    }

    const ImDrawIdx* indices = draw_list->IdxBuffer.Data + command->IdxOffset;
    const ImDrawVert* vertices = draw_list->VtxBuffer.Data + command->VtxOffset;
    int count = 0;

    for (unsigned int i = 0; i < command->ElemCount; i++)
    {
        expanded_vertices[count++] = to_vertex(&vertices[indices[i]], display_pos, scale);

        if (count == EXPANDED_VERTEX_CHUNK)
        {
            draw_vertices(expanded_vertices, count, texture);
            count = 0;
        }
    }

    if (count > 0)
    {
        draw_vertices(expanded_vertices, count, texture);
    }
}

bool pireneus_imgui_init(void)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.BackendRendererName = "pireneus";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;

    ImGui::StyleColorsDark();

#ifdef PIRENEUS_UWP
    io.BackendPlatformName = "pireneus_uwp";
#else
    if (!ImGui_ImplSDL3_InitForOther((SDL_Window*)get_window()))
    {
        ImGui::DestroyContext();
        return false;
    }
#endif

    set_event_callback(on_platform_event);

    DEBUG_LOG("[imgui] initialized (Dear ImGui %s)\n", IMGUI_VERSION);

    return true;
}

void pireneus_imgui_shutdown(void)
{
    wait_renderer_idle();

    ImVector<ImTextureData*>& textures = ImGui::GetPlatformIO().Textures;

    for (int i = 0; i < textures.Size; i++)
    {
        if (textures[i]->RefCount == 1)
        {
            destroy_imgui_texture(textures[i]);
        }
    }

    set_event_callback(NULL);

#ifndef PIRENEUS_UWP
    ImGui_ImplSDL3_Shutdown();
#endif

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = NULL;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);

    ImGui::DestroyContext();

    DEBUG_LOG("[imgui] shut down\n");
}

void pireneus_imgui_new_frame(void)
{
#ifdef PIRENEUS_UWP
    static Uint64 last_ticks;
    Uint64 now = SDL_GetTicksNS();
    int width = 0;
    int height = 0;
    get_window_size(&width, &height);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)width, (float)height);
    io.DeltaTime = last_ticks ? (float)((double)(now - last_ticks) / 1e9) : 1.0f / 60.0f;
    last_ticks = now;
#else
    ImGui_ImplSDL3_NewFrame();
#endif

    ImGui::NewFrame();
}

void pireneus_imgui_prepare_frame(void)
{
    ImGui::Render();
    update_textures(ImGui::GetDrawData());
}

void pireneus_imgui_draw(void)
{
    ImDrawData* draw_data = ImGui::GetDrawData();

    if (!draw_data || draw_data->CmdLists.Size == 0)
    {
        return;
    }

    ImVec2 display_pos = draw_data->DisplayPos;
    ImVec2 scale = draw_data->FramebufferScale;
    float framebuffer_width = draw_data->DisplaySize.x * scale.x;
    float framebuffer_height = draw_data->DisplaySize.y * scale.y;

    if (framebuffer_width <= 0.0f || framebuffer_height <= 0.0f)
    {
        return;
    }

    for (int n = 0; n < draw_data->CmdLists.Size; n++)
    {
        const ImDrawList* draw_list = draw_data->CmdLists[n];

        for (int c = 0; c < draw_list->CmdBuffer.Size; c++)
        {
            const ImDrawCmd* command = &draw_list->CmdBuffer[c];

            if (command->UserCallback)
            {
                // No render state of our own to reset : every draw_vertices() call is self-contained.
                if (command->UserCallback != ImGui::GetPlatformIO().DrawCallback_ResetRenderState)
                {
                    command->UserCallback(draw_list, command);
                }

                continue;
            }

            float clip_x0 = SDL_max((command->ClipRect.x - display_pos.x) * scale.x, 0.0f);
            float clip_y0 = SDL_max((command->ClipRect.y - display_pos.y) * scale.y, 0.0f);
            float clip_x1 = SDL_min((command->ClipRect.z - display_pos.x) * scale.x, framebuffer_width);
            float clip_y1 = SDL_min((command->ClipRect.w - display_pos.y) * scale.y, framebuffer_height);

            if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0)
            {
                continue;
            }

            set_scissor((int)clip_x0, (int)clip_y0, (int)(clip_x1 - clip_x0), (int)(clip_y1 - clip_y0));
            draw_command(draw_list, command, display_pos, scale);
        }
    }

    // Leave the pass unclipped for anything drawn after the UI.
    set_scissor(0, 0, (int)framebuffer_width, (int)framebuffer_height);
}
