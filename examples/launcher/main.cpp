// The one executable for every example : a Dear ImGui menu to pick an example, then that example
// running in the same window, with a button back to the menu.
//
// --example <id> (or the PIRENEUS_EXAMPLE environment variable) starts directly in an example.

#include <stdio.h>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include "imgui/pireneus_imgui.h"

#include "platforms/platforms_paths.h"
#include "platforms/platforms_window.h"
#include "project/project.h"
#include "renderer/renderer.h"
#include "debug.h"
#include "example.h"
#include "launcher/editor_fonts.h"

#define WINDOW_WIDTH 1600
#define WINDOW_HEIGHT 900
#define WINDOW_TITLE "Pireneus Engine"

#define LOGO_PATH "resources/logo_light.png" // the white logo, for the dark theme
#define LOGO_HEIGHT 96                           // in pixels, on screen
#define COPYRIGHT "Copyright \xC2\xA9 2025-2026 Isabit Studio"

static const Example* const examples[] = {
    &example_hello_triangle,
    &example_2d_test,
    &example_pixel_perfect,
    &example_tilemap_editor,
    &example_3d_test,
    &example_photoreal
};

#define EXAMPLE_COUNT ((int)(sizeof(examples) / sizeof(examples[0])))

static const Example* current; // NULL : the menu
static const Example* pending;
static int switch_pending;
static char ini_path[512];
static char failure_message[256];
static int recent_count = -1; // projects recently opened ; -1 : not read yet

void example_return_to_menu(void)
{
    pending = NULL;
    switch_pending = 1;
}

// A fresh Dear ImGui context for each example, whose window layout is saved under its own name.
static void reset_imgui(const char* name)
{
    pireneus_imgui_shutdown();
    pireneus_imgui_init();
    launcher_setup_fonts();

    const char* base_path = SDL_GetBasePath();
    SDL_snprintf(ini_path, sizeof(ini_path), "%simgui_%s.ini", base_path ? base_path : "", name);
    ImGui::GetIO().IniFilename = ini_path;
    ImGui::LoadIniSettingsFromDisk(ini_path);
}

static void enter(const Example* next)
{
    wait_renderer_idle();

    if (current && current->shutdown)
    {
        current->shutdown();
    }

    current = NULL;
    reset_imgui(next ? next->id : "menu");

    if (next && next->init && !next->init())
    {
        SDL_snprintf(failure_message, sizeof(failure_message), "'%s' failed to start : see the console.", next->title);
        next = NULL;
    }

    current = next;

    if (!next || !next->has_own_menu)
    {
        char title[128];
        SDL_snprintf(title, sizeof(title), next ? WINDOW_TITLE " - %s" : WINDOW_TITLE, next ? next->title : "");
        set_window_title(title);
    }

    recent_count = -1; // the list may have changed
}

// A small button in a corner, always on top, for the examples that have no UI of their own.
static void draw_back_button(void)
{
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f, io.DisplaySize.y - 10.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##back", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

    if (ImGui::Button(ICON_ARROW_LEFT " Menu"))
    {
        example_return_to_menu();
    }

    ImGui::End();
}

// --- The studio's logo ------------------------------------------------------------------------

static Texture* logo;
static int logo_width;
static int logo_height;
static int logo_tried;

// The logo, cropped to what isn't transparent and reduced to LOGO_HEIGHT by averaging every source pixel
// a destination pixel covers : textures are sampled without filtering, so a 1024 px image drawn ten
// times smaller would alias badly.
static void load_logo(void)
{
    logo_tried = 1;

    char path[512];
    const char* base_path = SDL_GetBasePath();
    SDL_snprintf(path, sizeof(path), "%s%s", base_path ? base_path : "", LOGO_PATH);

    SDL_Surface* loaded = IMG_Load(path);
    SDL_Surface* image = loaded ? SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32) : NULL;
    SDL_DestroySurface(loaded);

    if (!image)
    {
        printf("Could not load the logo '%s'.\n", path);
        return;
    }

    const uint8_t* pixels = (const uint8_t*)image->pixels;
    int min_x = image->w, min_y = image->h, max_x = -1, max_y = -1;

    for (int y = 0; y < image->h; y++)
    {
        for (int x = 0; x < image->w; x++)
        {
            if (pixels[y * image->pitch + x * 4 + 3] > 8)
            {
                min_x = SDL_min(min_x, x);
                min_y = SDL_min(min_y, y);
                max_x = SDL_max(max_x, x);
                max_y = SDL_max(max_y, y);
            }
        }
    }

    if (max_x < 0)
    {
        SDL_DestroySurface(image);
        return;
    }

    int source_width = max_x - min_x + 1;
    int source_height = max_y - min_y + 1;
    logo_height = SDL_min(LOGO_HEIGHT, source_height);
    logo_width = SDL_max(1, (int)((float)source_width * (float)logo_height / (float)source_height + 0.5f));

    uint8_t* reduced = (uint8_t*)SDL_malloc((size_t)logo_width * (size_t)logo_height * 4);

    for (int y = 0; y < logo_height; y++)
    {
        int y0 = min_y + y * source_height / logo_height;
        int y1 = SDL_max(y0 + 1, min_y + (y + 1) * source_height / logo_height);

        for (int x = 0; x < logo_width; x++)
        {
            int x0 = min_x + x * source_width / logo_width;
            int x1 = SDL_max(x0 + 1, min_x + (x + 1) * source_width / logo_width);

            // Colors weighted by alpha, so transparent pixels don't darken the edges.
            float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

            for (int sy = y0; sy < y1; sy++)
            {
                for (int sx = x0; sx < x1; sx++)
                {
                    const uint8_t* p = &pixels[sy * image->pitch + sx * 4];
                    float a = (float)p[3];
                    sum[0] += p[0] * a;
                    sum[1] += p[1] * a;
                    sum[2] += p[2] * a;
                    sum[3] += a;
                }
            }

            uint8_t* out = &reduced[(y * logo_width + x) * 4];
            float count = (float)((x1 - x0) * (y1 - y0));

            for (int c = 0; c < 3; c++)
            {
                out[c] = sum[3] > 0.0f ? (uint8_t)(sum[c] / sum[3] + 0.5f) : 0;
            }

            out[3] = (uint8_t)(sum[3] / count + 0.5f);
        }
    }

    logo = create_texture_from_pixels(reduced, logo_width, logo_height);
    SDL_free(reduced);
    SDL_DestroySurface(image);
}

// --- Projects ---------------------------------------------------------------------------------

static char recent_projects[PROJECT_MAX_RECENT][PROJECT_PATH_SIZE];
static char new_project_name[64] = "My Game";
static char new_project_location[PROJECT_PATH_SIZE];
static int new_project_template;
static char project_error[256];

static void open_in_editor(const char* path)
{
    editor_request_project(path);
    pending = &example_scene_editor;
    switch_pending = 1;
}

static void on_location_chosen(void* user_data, const char* const* paths, int count)
{
    (void)user_data;

    if (paths && count > 0)
    {
        SDL_snprintf(new_project_location, sizeof(new_project_location), "%s", paths[0]);
    }
}

static void on_project_chosen(void* user_data, const char* const* paths, int count)
{
    (void)user_data;

    if (paths && count > 0)
    {
        Project probe;

        if (project_open(paths[0], &probe))
        {
            open_in_editor(probe.root);
        }
        else
        {
            SDL_snprintf(project_error, sizeof(project_error), "No project in this folder.");
        }
    }
}

static void draw_projects(void)
{
    if (recent_count < 0)
    {
        recent_count = project_recent_list(recent_projects);
        project_default_location(new_project_location, sizeof(new_project_location));
    }

    ImGui::SeparatorText("Projects");

    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Name", new_project_name, sizeof(new_project_name));
    ImGui::SameLine();
    ImGui::RadioButton("3D", &new_project_template, PROJECT_TEMPLATE_3D);
    ImGui::SameLine();
    ImGui::RadioButton("2D", &new_project_template, PROJECT_TEMPLATE_2D);

    ImGui::SetNextItemWidth(440);
    ImGui::InputText("##location", new_project_location, sizeof(new_project_location));
    ImGui::SameLine();

    if (ImGui::Button(ICON_FOLDER " Browse"))
    {
        platform_show_open_folder_dialog(on_location_chosen, NULL);
    }

    if (ImGui::Button(ICON_FOLDER_PLUS " Create Project", ImVec2(180.0f, 0.0f)))
    {
        Project created;

        if (project_create(new_project_location, new_project_name, (ProjectTemplate)new_project_template, &created))
        {
            project_error[0] = '\0';
            open_in_editor(created.root);
        }
        else
        {
            SDL_snprintf(project_error, sizeof(project_error), "Could not create the project : does it already exist ?");
        }
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_FOLDER " Open Project...", ImVec2(180.0f, 0.0f)))
    {
        platform_show_open_folder_dialog(on_project_chosen, NULL);
    }

    if (project_error[0])
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", project_error);
    }

    if (recent_count > 0)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Recent");

        for (int i = 0; i < recent_count; i++)
        {
            ImGui::PushID(i);

            if (ImGui::Selectable(recent_projects[i]))
            {
                open_in_editor(recent_projects[i]);
            }

            ImGui::PopID();
        }
    }
}

static void draw_menu(void)
{
    ImGuiIO& io = ImGui::GetIO();

    ImVec2 size(SDL_min(760.0f, io.DisplaySize.x - 40.0f), SDL_min(720.0f, io.DisplaySize.y - 40.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin("Pireneus", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);

    if (!logo_tried)
    {
        load_logo();
    }

    if (logo)
    {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - (float)logo_width) * 0.5f);
        ImGui::Image(pireneus_imgui_texture(logo), ImVec2((float)logo_width, (float)logo_height));
        ImGui::Spacing();
    }

    draw_projects();

    ImGui::Spacing();
    ImGui::SeparatorText("Examples");

    for (int i = 0; i < EXAMPLE_COUNT; i++)
    {
        ImGui::PushID(i);

        if (ImGui::Button(examples[i]->title, ImVec2(180.0f, 0.0f)))
        {
            pending = examples[i];
            switch_pending = 1;
        }

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextWrapped("%s", examples[i]->description);
        ImGui::PopID();
    }

    if (failure_message[0])
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", failure_message);
    }

    // The copyright, centered at the bottom of the window.
    float text_width = ImGui::CalcTextSize(COPYRIGHT).x;
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - text_width) * 0.5f,
        ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - ImGui::GetTextLineHeight()));
    ImGui::TextDisabled("%s", COPYRIGHT);

    ImGui::End();
}

static void frame(void)
{
    int window_width = 0;
    int window_height = 0;
    get_window_size(&window_width, &window_height);

    if (window_width <= 0 || window_height <= 0)
    {
        return;
    }

    pireneus_imgui_new_frame();

    if (current)
    {
        if (current->update)
        {
            current->update();
        }

        if (!current->has_own_menu)
        {
            draw_back_button();
        }
    }
    else
    {
        draw_menu();
    }

    pireneus_imgui_prepare_frame();

    if (begin_frame())
    {
        if (current && current->render)
        {
            current->render();
        }

        pireneus_imgui_draw();
        end_frame();
    }

    if (switch_pending)
    {
        switch_pending = 0;
        failure_message[0] = '\0';
        enter(pending);
    }
}

static const Example* find_example(const char* id)
{
    for (int i = 0; id && i < EXAMPLE_COUNT; i++)
    {
        if (SDL_strcmp(examples[i]->id, id) == 0)
        {
            return examples[i];
        }
    }

    return NULL;
}

// On UWP, the platform layer owns the real entry point and calls this from its view's Run().
#ifdef PIRENEUS_UWP
extern "C" int pireneus_app_main(int argc, char** argv)
#else
int main(int argc, char** argv)
#endif
{
    const char* requested = SDL_getenv("PIRENEUS_EXAMPLE");
    const char* project = NULL;

    for (int i = 1; i + 1 < argc; i++)
    {
        if (SDL_strcmp(argv[i], "--example") == 0)
        {
            requested = argv[i + 1];
        }
        else if (SDL_strcmp(argv[i], "--project") == 0)
        {
            project = argv[i + 1];
        }
    }

    if (!create_window(WINDOW_TITLE, WINDOW_WIDTH, WINDOW_HEIGHT))
    {
        printf("Failed to create window.\n");
        return -1;
    }

    if (!create_renderer(get_window()))
    {
        printf("Failed to initialize the renderer.\n");
        destroy_window();
        return -1;
    }

    if (!pireneus_imgui_init())
    {
        printf("Failed to initialize Dear ImGui.\n");
        destroy_renderer();
        destroy_window();
        return -1;
    }

    if (project)
    {
        editor_request_project(project);
        enter(&example_scene_editor);
    }
    else
    {
        enter(find_example(requested));
    }

    set_redraw_callback(frame);

    DEBUG_LOG("[launcher] entering main loop\n");

    while (handle_events())
    {
        call_redraw_callback();
    }

    DEBUG_LOG("[launcher] shutting down\n");

    wait_renderer_idle();

    if (current && current->shutdown)
    {
        current->shutdown();
    }

    if (logo)
    {
        destroy_texture(logo);
    }

    pireneus_imgui_shutdown();
    destroy_renderer();
    destroy_window();

    return 0;
}
