// A simple Tiled tilemap editor built on Dear ImGui (see src/imgui/pireneus_imgui.h).
//
// The map itself is drawn by the engine (sprite batch + tiled_map_draw_layer) in the window's
// background ; ImGui panels (docked around it) handle the tileset palette, layers and files.
//
// Controls, over the map :
//   Left click / drag         paint the selected tile on the current layer
//   Right click / drag        erase (GID 0)
//   Shift + left click        pick the tile under the cursor (eyedropper)
//   Middle drag, Space + drag pan
//   Mouse wheel               zoom around the cursor
//   Ctrl+Z / Ctrl+Y           undo / redo (one step per stroke)
//   Ctrl+S                    save

#include <stdio.h>

#include <SDL3/SDL.h>

#include "imgui/pireneus_imgui.h"
#include "platforms/platforms_window.h"
#include "renderer/renderer.h"
#include "renderer/sprite_batch.h"
#include "tiled/tiled_map.h"
#include "tiled/tiled_map_render.h"
#include "debug.h"
#include "example.h"

#define DEFAULT_MAP_PATH "resources/2D-test/sample_map.json"

#define MAX_LAYERS 16
#define MAX_UNDO_STEPS 64
#define MIN_ZOOM 1.0f
#define MAX_ZOOM 8.0f

// One undo/redo step : the whole content of the layer a stroke modified, from before (undo stack)
// or after (redo stack) that stroke. Maps are small (100x100 tiles = 40 KB per layer), so full-layer
// snapshots are far simpler than per-tile diffs, for a negligible cost.
typedef struct
{
    int layer;
    uint32_t* tiles;
} Snapshot;

typedef struct
{
    Snapshot steps[MAX_UNDO_STEPS];
    int count;
} SnapshotStack;

static TiledMap* map;
static Texture* tileset_texture;
static int tileset_width;
static int tileset_height;

static char map_path[256];
static char save_path[256];
static char status_message[256];
static int dirty;

static int current_layer;
static bool layer_visible[MAX_LAYERS];
static bool dim_other_layers = true;
static bool show_grid = true;

static uint32_t selected_gid = 1; // GID painted by the brush (tile index + 1)
static float zoom = 2.0f;
static float palette_zoom = 2.0f;
static float camera_x; // window pixels : the map pixel at the window's top-left corner, times zoom
static float camera_y;

static SnapshotStack undo_stack;
static SnapshotStack redo_stack;

typedef enum
{
    STROKE_NONE,
    STROKE_PAINT,
    STROKE_ERASE,
    STROKE_PAN
} StrokeKind;

static StrokeKind stroke;

static void set_status(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    SDL_vsnprintf(status_message, sizeof(status_message), format, args);
    va_end(args);
}

// --- Undo / redo ------------------------------------------------------------------------------

static Snapshot take_snapshot(int layer)
{
    size_t size = sizeof(uint32_t) * (size_t)(tiled_map_width(map) * tiled_map_height(map));

    Snapshot snapshot;
    snapshot.layer = layer;
    snapshot.tiles = (uint32_t*)SDL_malloc(size);
    SDL_memcpy(snapshot.tiles, tiled_map_layer_tiles(map, layer), size);

    return snapshot;
}

static void restore_snapshot(const Snapshot* snapshot)
{
    int width = tiled_map_width(map);
    int height = tiled_map_height(map);

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            tiled_map_set_tile(map, snapshot->layer, x, y, snapshot->tiles[y * width + x]);
        }
    }
}

static void stack_clear(SnapshotStack* stack)
{
    for (int i = 0; i < stack->count; i++)
    {
        SDL_free(stack->steps[i].tiles);
    }

    stack->count = 0;
}

static void stack_push(SnapshotStack* stack, Snapshot snapshot)
{
    if (stack->count == MAX_UNDO_STEPS)
    {
        // Full : drop the oldest step.
        SDL_free(stack->steps[0].tiles);
        SDL_memmove(&stack->steps[0], &stack->steps[1], sizeof(Snapshot) * (MAX_UNDO_STEPS - 1));
        stack->count--;
    }

    stack->steps[stack->count++] = snapshot;
}

// Moves one step from `from` to `to` : restores the layer to `from`'s snapshot, after saving the
// layer's current content onto `to` (so the same step can be travelled back).
static void travel(SnapshotStack* from, SnapshotStack* to)
{
    if (from->count == 0)
    {
        return;
    }

    Snapshot snapshot = from->steps[--from->count];
    stack_push(to, take_snapshot(snapshot.layer));
    restore_snapshot(&snapshot);
    SDL_free(snapshot.tiles);

    current_layer = snapshot.layer;
    dirty = 1;
}

// --- Files ------------------------------------------------------------------------------------

static void edited_path_for(const char* path, char* out, size_t out_size)
{
    const char* extension = SDL_strrchr(path, '.');
    size_t stem_length = extension ? (size_t)(extension - path) : SDL_strlen(path);

    SDL_snprintf(out, out_size, "%.*s_edited%s", (int)stem_length, path, extension ? extension : ".json");
}

// The tileset image path is relative to the map file, both relative to the executable.
static void tileset_path_for(const char* path, const char* image, char* out, size_t out_size)
{
    const char* slash = SDL_strrchr(path, '/');
    const char* backslash = SDL_strrchr(path, '\\');
    const char* separator = slash > backslash ? slash : backslash;
    int directory_length = separator ? (int)(separator - path + 1) : 0;

    SDL_snprintf(out, out_size, "%.*s%s", directory_length, path, image);
}

static int load_map(const char* path)
{
    TiledMap* loaded = tiled_map_load(path);

    if (!loaded)
    {
        set_status("Could not load '%s'.", path);
        return 0;
    }

    char image_path[512];
    tileset_path_for(path, tiled_map_tileset_image(loaded), image_path, sizeof(image_path));

    int width = 0;
    int height = 0;
    Texture* texture = create_texture(image_path, &width, &height);

    if (!texture)
    {
        set_status("Could not load the tileset '%s'.", image_path);
        tiled_map_destroy(loaded);
        return 0;
    }

    if (map)
    {
        wait_renderer_idle(); // the previous tileset may still be used by a frame in flight
        destroy_texture(tileset_texture);
        tiled_map_destroy(map);
    }

    map = loaded;
    tileset_texture = texture;
    tileset_width = width;
    tileset_height = height;

    stack_clear(&undo_stack);
    stack_clear(&redo_stack);
    dirty = 0;
    current_layer = 0;
    camera_x = 0.0f;
    camera_y = 0.0f;

    for (int i = 0; i < MAX_LAYERS; i++)
    {
        layer_visible[i] = true;
    }

    if (path != map_path)
    {
        SDL_snprintf(map_path, sizeof(map_path), "%s", path);
    }

    edited_path_for(map_path, save_path, sizeof(save_path));
    set_status("Loaded '%s' (%dx%d tiles, %d layers).", map_path, tiled_map_width(map), tiled_map_height(map), tiled_map_layer_count(map));

    return 1;
}

static void save_map(void)
{
    if (tiled_map_save(map, save_path))
    {
        dirty = 0;
        set_status("Saved '%s'.", save_path);
    }
    else
    {
        set_status("Could not save '%s'.", save_path);
    }
}

// --- Map viewport -----------------------------------------------------------------------------

static float tile_screen_width(void) { return (float)tiled_map_tile_width(map) * zoom; }
static float tile_screen_height(void) { return (float)tiled_map_tile_height(map) * zoom; }

// Tile coordinates under a window-pixel position ; returns 0 when outside the map.
static int tile_at(float pixel_x, float pixel_y, int* out_x, int* out_y)
{
    int x = (int)SDL_floorf((pixel_x + camera_x) / tile_screen_width());
    int y = (int)SDL_floorf((pixel_y + camera_y) / tile_screen_height());

    *out_x = x;
    *out_y = y;

    return x >= 0 && y >= 0 && x < tiled_map_width(map) && y < tiled_map_height(map);
}

static uint32_t tile_gid(int layer, int x, int y)
{
    return tiled_map_layer_tiles(map, layer)[y * tiled_map_width(map) + x];
}

static void apply_brush(int x, int y, uint32_t gid)
{
    if (tile_gid(current_layer, x, y) != gid)
    {
        tiled_map_set_tile(map, current_layer, x, y, gid);
        dirty = 1;
    }
}

// Eyedropper : the current layer's tile if it has one, else the topmost visible non-empty one.
static void pick_tile(int x, int y)
{
    uint32_t gid = tile_gid(current_layer, x, y);

    for (int layer = tiled_map_layer_count(map) - 1; gid == 0 && layer >= 0; layer--)
    {
        if (layer_visible[layer])
        {
            gid = tile_gid(layer, x, y);
        }
    }

    if (gid != 0)
    {
        selected_gid = gid;
    }
}

static void handle_viewport_input(void)
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 scale = io.DisplayFramebufferScale;
    float mouse_x = io.MousePos.x * scale.x;
    float mouse_y = io.MousePos.y * scale.y;

    // A stroke only starts over the map itself (not over a panel), but then continues until its
    // button is released, even if the cursor wanders over a panel meanwhile.
    if (stroke == STROKE_NONE && !io.WantCaptureMouse)
    {
        bool space = ImGui::IsKeyDown(ImGuiKey_Space) && !io.WantTextInput;

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (space && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))
        {
            stroke = STROKE_PAN;
        }
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyShift)
        {
            int x, y;

            if (tile_at(mouse_x, mouse_y, &x, &y))
            {
                pick_tile(x, y);
            }
        }
        else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            stroke = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ? STROKE_PAINT : STROKE_ERASE;
            stack_push(&undo_stack, take_snapshot(current_layer));
            stack_clear(&redo_stack);
        }

        if (io.MouseWheel != 0.0f)
        {
            float new_zoom = SDL_clamp(zoom * (io.MouseWheel > 0.0f ? 1.25f : 0.8f), MIN_ZOOM, MAX_ZOOM);

            // Keep the map point under the cursor fixed while zooming.
            camera_x = (mouse_x + camera_x) * (new_zoom / zoom) - mouse_x;
            camera_y = (mouse_y + camera_y) * (new_zoom / zoom) - mouse_y;
            zoom = new_zoom;
        }
    }

    if (stroke == STROKE_PAN)
    {
        camera_x -= io.MouseDelta.x * scale.x;
        camera_y -= io.MouseDelta.y * scale.y;

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            stroke = STROKE_NONE;
        }
    }
    else if (stroke == STROKE_PAINT || stroke == STROKE_ERASE)
    {
        ImGuiMouseButton button = stroke == STROKE_PAINT ? ImGuiMouseButton_Left : ImGuiMouseButton_Right;
        int x, y;

        if (tile_at(mouse_x, mouse_y, &x, &y))
        {
            apply_brush(x, y, stroke == STROKE_PAINT ? selected_gid : 0);
        }

        if (!ImGui::IsMouseDown(button))
        {
            stroke = STROKE_NONE;

            // A click that changed nothing shouldn't leave a no-op undo step behind.
            Snapshot* last = &undo_stack.steps[undo_stack.count - 1];
            size_t size = sizeof(uint32_t) * (size_t)(tiled_map_width(map) * tiled_map_height(map));

            if (SDL_memcmp(last->tiles, tiled_map_layer_tiles(map, last->layer), size) == 0)
            {
                SDL_free(last->tiles);
                undo_stack.count--;
            }
        }
    }

    if (!io.WantTextInput)
    {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
        {
            travel(&undo_stack, &redo_stack);
        }

        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
        {
            travel(&redo_stack, &undo_stack);
        }

        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
        {
            save_map();
        }
    }
}

// Grid, map bounds and hovered-tile highlight, drawn with ImGui's background draw list : on top of
// the map (the UI is drawn after it), but below every panel. ImGui works in logical coordinates,
// hence the division by the framebuffer scale.
static void draw_viewport_overlay(void)
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 scale = io.DisplayFramebufferScale;
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    float cell_w = tile_screen_width() / scale.x;
    float cell_h = tile_screen_height() / scale.y;
    float origin_x = -camera_x / scale.x;
    float origin_y = -camera_y / scale.y;
    float map_w = cell_w * (float)tiled_map_width(map);
    float map_h = cell_h * (float)tiled_map_height(map);

    if (show_grid)
    {
        ImU32 grid_color = IM_COL32(255, 255, 255, 28);
        float view_w = io.DisplaySize.x;
        float view_h = io.DisplaySize.y;

        int first_col = SDL_max(0, (int)(-origin_x / cell_w));
        int last_col = SDL_min(tiled_map_width(map), (int)((view_w - origin_x) / cell_w) + 1);
        int first_row = SDL_max(0, (int)(-origin_y / cell_h));
        int last_row = SDL_min(tiled_map_height(map), (int)((view_h - origin_y) / cell_h) + 1);

        for (int col = first_col; col <= last_col; col++)
        {
            float x = origin_x + (float)col * cell_w;
            draw_list->AddLine(ImVec2(x, SDL_max(origin_y, 0.0f)), ImVec2(x, SDL_min(origin_y + map_h, view_h)), grid_color);
        }

        for (int row = first_row; row <= last_row; row++)
        {
            float y = origin_y + (float)row * cell_h;
            draw_list->AddLine(ImVec2(SDL_max(origin_x, 0.0f), y), ImVec2(SDL_min(origin_x + map_w, view_w), y), grid_color);
        }
    }

    draw_list->AddRect(ImVec2(origin_x, origin_y), ImVec2(origin_x + map_w, origin_y + map_h), IM_COL32(255, 255, 255, 110));

    int x, y;

    if (!io.WantCaptureMouse && tile_at(io.MousePos.x * scale.x, io.MousePos.y * scale.y, &x, &y))
    {
        ImVec2 min(origin_x + (float)x * cell_w, origin_y + (float)y * cell_h);
        ImVec2 max(min.x + cell_w, min.y + cell_h);

        draw_list->AddRect(min, max, IM_COL32(255, 220, 64, 255), 0.0f, 2.0f);
    }
}

typedef struct
{
    Color tint;
} LayerDrawContext;

static void draw_tile(Texture* texture, int texture_width, int texture_height,
    float dest_x, float dest_y, float dest_w, float dest_h,
    float src_x, float src_y, float src_w, float src_h, void* user_data)
{
    const LayerDrawContext* context = (const LayerDrawContext*)user_data;

    sprite_batch_draw_region(texture, texture_width, texture_height,
        dest_x, dest_y, dest_w, dest_h, src_x, src_y, src_w, src_h, context->tint);
}

static void draw_map(int window_width, int window_height)
{
    sprite_batch_begin();

    // Map backdrop : tells "empty tile" apart from "outside the map".
    sprite_batch_draw(white_texture(), -camera_x, -camera_y,
        tile_screen_width() * (float)tiled_map_width(map), tile_screen_height() * (float)tiled_map_height(map),
        color_rgba(0.16f, 0.16f, 0.19f, 1.0f));

    for (int layer = 0; layer < tiled_map_layer_count(map) && layer < MAX_LAYERS; layer++)
    {
        if (!layer_visible[layer])
        {
            continue;
        }

        LayerDrawContext context;
        context.tint = (dim_other_layers && layer != current_layer) ? color_rgba(1.0f, 1.0f, 1.0f, 0.35f) : color_white();

        tiled_map_draw_layer(map, layer, tileset_texture, tileset_width, tileset_height,
            camera_x, camera_y, window_width, window_height, zoom, draw_tile, &context);
    }

    sprite_batch_end();
}

// --- Panels -----------------------------------------------------------------------------------

static void tile_uv(uint32_t gid, ImVec2* uv0, ImVec2* uv1)
{
    int tile_w = tiled_map_tile_width(map);
    int tile_h = tiled_map_tile_height(map);
    int spacing = tiled_map_tileset_spacing(map);
    int margin = tiled_map_tileset_margin(map);
    int columns = tiled_map_tileset_columns(map);
    int index = (int)gid - 1;

    float x = (float)(margin + (index % columns) * (tile_w + spacing));
    float y = (float)(margin + (index / columns) * (tile_h + spacing));

    *uv0 = ImVec2(x / (float)tileset_width, y / (float)tileset_height);
    *uv1 = ImVec2((x + (float)tile_w) / (float)tileset_width, (y + (float)tile_h) / (float)tileset_height);
}

static void draw_map_panel(void)
{
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin("Map");

    ImGui::SeparatorText("File");
    ImGui::SetNextItemWidth(-70);
    ImGui::InputText("##map_path", map_path, sizeof(map_path));
    ImGui::SameLine();

    if (ImGui::Button("Load", ImVec2(-1, 0)))
    {
        load_map(map_path);
    }

    ImGui::SetNextItemWidth(-70);
    ImGui::InputText("##save_path", save_path, sizeof(save_path));
    ImGui::SameLine();

    if (ImGui::Button("Save", ImVec2(-1, 0)))
    {
        save_map();
    }

    ImGui::TextDisabled("Paths are relative to the executable.");
    ImGui::Text("%dx%d tiles of %dx%d px%s", tiled_map_width(map), tiled_map_height(map),
        tiled_map_tile_width(map), tiled_map_tile_height(map), dirty ? "  (modified)" : "");

    ImGui::SeparatorText("Layers");

    for (int layer = tiled_map_layer_count(map) - 1; layer >= 0; layer--) // topmost first, like Tiled
    {
        if (layer >= MAX_LAYERS)
        {
            continue;
        }

        ImGui::PushID(layer);
        ImGui::Checkbox("##visible", &layer_visible[layer]);
        ImGui::SameLine();

        if (ImGui::Selectable(tiled_map_layer_name(map, layer), current_layer == layer))
        {
            current_layer = layer;
        }

        ImGui::PopID();
    }

    ImGui::SeparatorText("View");
    ImGui::Checkbox("Dim other layers", &dim_other_layers);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &show_grid);
    ImGui::SliderFloat("Zoom", &zoom, MIN_ZOOM, MAX_ZOOM, "%.2fx");

    ImGui::SeparatorText("Brush");
    ImVec2 uv0, uv1;
    tile_uv(selected_gid, &uv0, &uv1);
    ImGui::Image(pireneus_imgui_texture(tileset_texture),
        ImVec2((float)tiled_map_tile_width(map) * 3.0f, (float)tiled_map_tile_height(map) * 3.0f), uv0, uv1);
    ImGui::SameLine();
    ImGui::Text("Tile #%u", selected_gid - 1);

    ImGui::SeparatorText("History");
    ImGui::BeginDisabled(undo_stack.count == 0);

    if (ImGui::Button("Undo"))
    {
        travel(&undo_stack, &redo_stack);
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redo_stack.count == 0);

    if (ImGui::Button("Redo"))
    {
        travel(&redo_stack, &undo_stack);
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d", undo_stack.count, redo_stack.count);

    if (ImGui::CollapsingHeader("Controls"))
    {
        ImGui::TextUnformatted(
            "Left drag         paint\n"
            "Right drag        erase\n"
            "Shift + click     pick tile\n"
            "Middle drag       pan (or Space + drag)\n"
            "Wheel             zoom\n"
            "Ctrl+Z / Ctrl+Y   undo / redo\n"
            "Ctrl+S            save");
    }

    if (status_message[0])
    {
        ImGui::Separator();
        ImGui::TextWrapped("%s", status_message);
    }

    ImGui::End();
}

static void draw_tileset_panel(void)
{
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12 - 520, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin("Tileset");

    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("Zoom##palette", &palette_zoom, 1.0f, 4.0f, "%.0fx");

    ImGui::BeginChild("palette", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(pireneus_imgui_texture(tileset_texture),
        ImVec2((float)tileset_width * palette_zoom, (float)tileset_height * palette_zoom));

    int tile_w = tiled_map_tile_width(map);
    int tile_h = tiled_map_tile_height(map);
    int spacing = tiled_map_tileset_spacing(map);
    int margin = tiled_map_tileset_margin(map);
    int columns = tiled_map_tileset_columns(map);
    int tile_count = tiled_map_tileset_tile_count(map);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    if (ImGui::IsItemHovered())
    {
        float local_x = (io.MousePos.x - origin.x) / palette_zoom - (float)margin;
        float local_y = (io.MousePos.y - origin.y) / palette_zoom - (float)margin;
        int col = (int)SDL_floorf(local_x / (float)(tile_w + spacing));
        int row = (int)SDL_floorf(local_y / (float)(tile_h + spacing));
        int index = row * columns + col;

        if (col >= 0 && row >= 0 && col < columns && (tile_count <= 0 || index < tile_count))
        {
            ImVec2 min(origin.x + (float)(margin + col * (tile_w + spacing)) * palette_zoom,
                origin.y + (float)(margin + row * (tile_h + spacing)) * palette_zoom);
            ImVec2 max(min.x + (float)tile_w * palette_zoom, min.y + (float)tile_h * palette_zoom);

            draw_list->AddRect(min, max, IM_COL32(255, 255, 255, 160));
            ImGui::SetTooltip("Tile #%d", index);

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                selected_gid = (uint32_t)index + 1;
            }
        }
    }

    int selected_index = (int)selected_gid - 1;
    ImVec2 selected_min(origin.x + (float)(margin + (selected_index % columns) * (tile_w + spacing)) * palette_zoom,
        origin.y + (float)(margin + (selected_index / columns) * (tile_h + spacing)) * palette_zoom);
    ImVec2 selected_max(selected_min.x + (float)tile_w * palette_zoom, selected_min.y + (float)tile_h * palette_zoom);
    draw_list->AddRect(selected_min, selected_max, IM_COL32(255, 220, 64, 255), 0.0f, 2.0f);

    ImGui::EndChild();
    ImGui::End();
}

// --- Frame ------------------------------------------------------------------------------------

static void update(void)
{
    // Panels can be docked to the window edges ; the central node stays transparent (and lets the
    // mouse through), which is where the map shows.
    ImGui::DockSpaceOverViewport(0, NULL, ImGuiDockNodeFlags_PassthruCentralNode);

    handle_viewport_input();
    draw_viewport_overlay();
    draw_map_panel();
    draw_tileset_panel();
}

static void render(void)
{
    int window_width = 0;
    int window_height = 0;
    get_window_size(&window_width, &window_height);

    draw_map(window_width, window_height);
}

static int init(void)
{
    SDL_snprintf(map_path, sizeof(map_path), "%s", DEFAULT_MAP_PATH);

    if (!load_map(map_path))
    {
        printf("%s\n", status_message);
        return 0;
    }

    return 1;
}

static void shutdown_example(void)
{
    stack_clear(&undo_stack);
    stack_clear(&redo_stack);
    destroy_texture(tileset_texture);
    tiled_map_destroy(map);
    tileset_texture = NULL;
    map = NULL;
}

extern "C" const Example example_tilemap_editor = {
    "tilemap-editor", "Tilemap Editor",
    "Paint Tiled maps : layers, tile palette, undo/redo.",
    init, update, render, shutdown_example
};
