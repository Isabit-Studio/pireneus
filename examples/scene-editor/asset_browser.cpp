// The asset browser : what's in the project's Assets folder, with thumbnails ; importing new files through
// a dialog or by dropping them on the window ; dragging assets into the scene and the inspector.

#include "editor.h"

#include "audio/audio.h"
#include "platforms/platforms_paths.h"
#include "platforms/platforms_window.h"
#include "renderer/renderer.h"

#define MAX_PENDING_IMPORTS 64

static char current_folder[ASSET_PATH_SIZE] = "Assets";
static char search[64];
static float tile_size = 96.0f;
static char selected_path[ASSET_PATH_SIZE];

typedef struct
{
    char source[PROJECT_PATH_SIZE];
    int as_type; // AssetType
} PendingImport;

static PendingImport pending_imports[MAX_PENDING_IMPORTS];
static int pending_import_count;
static bool open_import_popup;

static char rename_buffer[96];
static char rename_path[ASSET_PATH_SIZE];
static bool open_rename_popup;

static char trash_path[ASSET_PATH_SIZE];
static bool open_trash_popup;

static char new_folder_name[64];
static bool open_folder_popup;

static AudioVoice* preview_voice;
static char preview_path[ASSET_PATH_SIZE];

static void queue_import(const char* source)
{
    if (pending_import_count == MAX_PENDING_IMPORTS)
    {
        return;
    }

    AssetType type = asset_type_for_file(source);

    if (type == ASSET_OTHER)
    {
        editor_set_status("'%s' isn't a supported file type.", source);
        return;
    }

    PendingImport* import = &pending_imports[pending_import_count++];
    SDL_snprintf(import->source, sizeof(import->source), "%s", source);
    import->as_type = type;
    open_import_popup = true;
}

static void on_files_chosen(void* user_data, const char* const* paths, int count)
{
    (void)user_data;

    for (int i = 0; paths && i < count; i++)
    {
        queue_import(paths[i]);
    }
}

// Files dragged from the desktop onto the window.
static void on_file_dropped(const char* path)
{
    queue_import(path);
}

void editor_asset_browser_init(void)
{
    SDL_snprintf(current_folder, sizeof(current_folder), "Assets");
    search[0] = '\0';
    selected_path[0] = '\0';
    pending_import_count = 0;
    set_file_drop_callback(on_file_dropped);
}

void editor_asset_browser_shutdown(void)
{
    set_file_drop_callback(NULL);
    audio_stop(preview_voice);
    preview_voice = NULL;
}

const char* editor_accept_asset(int type)
{
    static char accepted[ASSET_PATH_SIZE];

    if (!ImGui::BeginDragDropTarget())
    {
        return NULL;
    }

    const char* result = NULL;
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();

    if (payload && payload->IsDataType(ASSET_DRAG_TYPE))
    {
        const char* path = (const char*)payload->Data;
        int matches = (type == ASSET_TEXTURE || type == ASSET_SPRITE) && asset_find_builtin_material(path) != NULL;

        for (int i = 0; i < asset_database_count() && !matches; i++)
        {
            const AssetEntry* entry = asset_database_get(i);

            if (SDL_strcmp(entry->path, path) == 0)
            {
                // Sprites work wherever textures do, and the other way round.
                matches = entry->type == type ||
                    ((type == ASSET_TEXTURE || type == ASSET_SPRITE) && (entry->type == ASSET_TEXTURE || entry->type == ASSET_SPRITE));
            }
        }

        if (matches && ImGui::AcceptDragDropPayload(ASSET_DRAG_TYPE))
        {
            SDL_snprintf(accepted, sizeof(accepted), "%s", path);
            result = accepted;
        }
    }

    ImGui::EndDragDropTarget();

    return result;
}

static const char* type_icon(AssetType type)
{
    switch (type)
    {
    case ASSET_FOLDER: return ICON_FOLDER;
    case ASSET_TEXTURE: return ICON_IMAGE;
    case ASSET_SPRITE: return ICON_IMAGE;
    case ASSET_MODEL: return ICON_BOX;
    case ASSET_AUDIO: return ICON_VOLUME;
    case ASSET_SKYBOX: return ICON_EARTH;
    case ASSET_SCENE: return ICON_SHAPES;
    case ASSET_TILEMAP: return ICON_GRID;
    default: return ICON_FILE;
    }
}

// Whether `path` sits directly in `folder`.
static bool is_in_folder(const char* path, const char* folder)
{
    size_t length = SDL_strlen(folder);

    return SDL_strncmp(path, folder, length) == 0 && path[length] == '/' && !SDL_strchr(path + length + 1, '/');
}

static void draw_folder_tree(const char* folder, const char* label)
{
    bool has_children = false;

    for (int i = 0; i < asset_database_count() && !has_children; i++)
    {
        const AssetEntry* entry = asset_database_get(i);
        has_children = entry->type == ASSET_FOLDER && is_in_folder(entry->path, folder);
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;

    if (!has_children)
    {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    if (SDL_strcmp(folder, current_folder) == 0)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    char text[128];
    SDL_snprintf(text, sizeof(text), ICON_FOLDER "  %s", label);

    bool open = ImGui::TreeNodeEx(folder, flags, "%s", text);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    {
        SDL_snprintf(current_folder, sizeof(current_folder), "%s", folder);
    }

    if (open)
    {
        for (int i = 0; i < asset_database_count(); i++)
        {
            const AssetEntry* entry = asset_database_get(i);

            if (entry->type == ASSET_FOLDER && is_in_folder(entry->path, folder))
            {
                draw_folder_tree(entry->path, entry->name);
            }
        }

        ImGui::TreePop();
    }
}

static void toggle_preview(const AssetEntry* entry)
{
    bool was_this = SDL_strcmp(preview_path, entry->path) == 0 && audio_is_playing(preview_voice);

    audio_stop(preview_voice);
    preview_voice = NULL;
    preview_path[0] = '\0';

    if (was_this)
    {
        return;
    }

    AudioClip* clip = asset_get_audio(entry->path);

    if (clip)
    {
        preview_voice = audio_play(clip, AUDIO_BUS_SOUND, 1.0f, 0);
        SDL_snprintf(preview_path, sizeof(preview_path), "%s", entry->path);
    }
}

static void open_asset(const AssetEntry* entry)
{
    const char* file_name = entry->name;

    switch (entry->type)
    {
    case ASSET_FOLDER:
        SDL_snprintf(current_folder, sizeof(current_folder), "%s", entry->path);
        break;

    case ASSET_MODEL:
        if (!editor_is_playing())
        {
            editor_add_model(entry->path, file_name);
        }

        break;

    case ASSET_SPRITE:
        if (!editor_is_playing())
        {
            editor_add_sprite(entry->path);
        }

        break;

    case ASSET_AUDIO:
        toggle_preview(entry);
        break;

    case ASSET_SKYBOX:
        if (!editor_is_playing())
        {
            editor_begin_edit();
            SDL_snprintf(editor_scene.skybox, sizeof(editor_scene.skybox), "%s", entry->path);
        }

        break;

    case ASSET_SCENE:
        editor_load_scene(entry->path);
        break;

    default:
        break;
    }
}

static void draw_tile(const AssetEntry* entry)
{
    ImGui::PushID(entry->path);
    ImGui::BeginGroup();

    ImVec2 size(tile_size, tile_size);
    ImVec2 position = ImGui::GetCursorScreenPos();
    bool selected = SDL_strcmp(selected_path, entry->path) == 0;

    ImGui::InvisibleButton("tile", size);

    bool hovered = ImGui::IsItemHovered();
    bool activated = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    if (ImGui::IsItemClicked())
    {
        SDL_snprintf(selected_path, sizeof(selected_path), "%s", entry->path);
    }

    if (entry->type != ASSET_FOLDER && ImGui::BeginDragDropSource())
    {
        ImGui::SetDragDropPayload(ASSET_DRAG_TYPE, entry->path, SDL_strlen(entry->path) + 1);
        ImGui::Text("%s  %s", type_icon(entry->type), entry->name);
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginPopupContextItem("context"))
    {
        SDL_snprintf(selected_path, sizeof(selected_path), "%s", entry->path);

        if (ImGui::MenuItem("Rename..."))
        {
            SDL_snprintf(rename_path, sizeof(rename_path), "%s", entry->path);
            SDL_snprintf(rename_buffer, sizeof(rename_buffer), "%s", entry->name);
            open_rename_popup = true;
        }

        if (ImGui::MenuItem("Move to Trash..."))
        {
            SDL_snprintf(trash_path, sizeof(trash_path), "%s", entry->path);
            open_trash_popup = true;
        }

        ImGui::EndPopup();
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImU32 background = selected ? IM_COL32(60, 95, 150, 255) : hovered ? IM_COL32(60, 60, 66, 255) : IM_COL32(40, 40, 44, 255);
    draw_list->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y), background, 4.0f);

    Texture* thumbnail = NULL;

    if (entry->type == ASSET_TEXTURE || entry->type == ASSET_SPRITE || entry->type == ASSET_SKYBOX)
    {
        thumbnail = asset_get_thumbnail(entry->path);
    }

    if (thumbnail)
    {
        int width = 0;
        int height = 0;
        texture_get_size(thumbnail, &width, &height);

        float scale = (tile_size - 8.0f) / (float)SDL_max(1, SDL_max(width, height));
        ImVec2 image((float)width * scale, (float)height * scale);
        ImVec2 corner(position.x + (size.x - image.x) * 0.5f, position.y + (size.y - image.y) * 0.5f);

        draw_list->AddImage(pireneus_imgui_texture(thumbnail), corner, ImVec2(corner.x + image.x, corner.y + image.y));
    }
    else
    {
        // No image to show : the type's icon, large.
        const char* icon = type_icon(entry->type);
        float font_size = tile_size * 0.45f;
        ImFont* font = ImGui::GetFont();
        ImVec2 text = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, icon);
        draw_list->AddText(font, font_size, ImVec2(position.x + (size.x - text.x) * 0.5f, position.y + (size.y - text.y) * 0.5f), IM_COL32(200, 200, 210, 255), icon);
    }

    if (entry->type == ASSET_AUDIO && SDL_strcmp(preview_path, entry->path) == 0 && audio_is_playing(preview_voice))
    {
        draw_list->AddRect(position, ImVec2(position.x + size.x, position.y + size.y), IM_COL32(100, 220, 120, 255), 4.0f, 2.0f);
    }

    // The name, cut to fit.
    char label[96];
    SDL_snprintf(label, sizeof(label), "%s", entry->name);

    while (label[0] && ImGui::CalcTextSize(label).x > tile_size)
    {
        size_t length = SDL_strlen(label);

        if (length < 4)
        {
            break;
        }

        label[length - 4] = '.';
        label[length - 3] = '.';
        label[length - 2] = '\0';
    }

    float label_width = ImGui::CalcTextSize(label).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + SDL_max(0.0f, (tile_size - label_width) * 0.5f));
    ImGui::TextUnformatted(label);

    ImGui::EndGroup();

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s\n%s, %.1f KB", entry->name, asset_type_name(entry->type), (double)entry->size / 1024.0);
    }

    ImGui::PopID();

    if (activated)
    {
        open_asset(entry);
    }
}

static bool matches_search(const AssetEntry* entry)
{
    return !search[0] || SDL_strcasestr(entry->name, search);
}

static void draw_popups(void)
{
    if (open_import_popup)
    {
        ImGui::OpenPopup("Import Assets");
        open_import_popup = false;
    }

    if (ImGui::BeginPopupModal("Import Assets", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        for (int i = 0; i < pending_import_count; i++)
        {
            PendingImport* import = &pending_imports[i];
            const char* name = SDL_strrchr(import->source, '/');
            const char* back = SDL_strrchr(import->source, '\\');
            name = back > name ? back : name;
            name = name ? name + 1 : import->source;

            ImGui::PushID(i);
            ImGui::SetNextItemWidth(140);

            AssetType natural = asset_type_for_file(import->source);

            if (natural == ASSET_TEXTURE && ImGui::BeginCombo("##type", asset_type_name((AssetType)import->as_type)))
            {
                // Images can be textures, sprites or skyboxes ; other files are what their extension says.
                static const AssetType image_kinds[] = { ASSET_TEXTURE, ASSET_SPRITE, ASSET_SKYBOX };

                for (size_t k = 0; k < sizeof(image_kinds) / sizeof(image_kinds[0]); k++)
                {
                    if (ImGui::Selectable(asset_type_name(image_kinds[k]), import->as_type == image_kinds[k]))
                    {
                        import->as_type = image_kinds[k];
                    }
                }

                ImGui::EndCombo();
            }
            else if (natural != ASSET_TEXTURE)
            {
                ImGui::TextUnformatted(asset_type_name((AssetType)import->as_type));
            }

            ImGui::SameLine(160);
            ImGui::TextUnformatted(name);
            ImGui::PopID();
        }

        ImGui::Separator();

        if (ImGui::Button("Import", ImVec2(120, 0)))
        {
            int imported = 0;

            for (int i = 0; i < pending_import_count; i++)
            {
                imported += asset_import(pending_imports[i].source, NULL, (AssetType)pending_imports[i].as_type, NULL, 0);
            }

            editor_set_status("Imported %d file(s).", imported);
            pending_import_count = 0;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            pending_import_count = 0;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (open_rename_popup)
    {
        ImGui::OpenPopup("Rename Asset");
        open_rename_popup = false;
    }

    if (ImGui::BeginPopupModal("Rename Asset", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##name", rename_buffer, sizeof(rename_buffer));

        if (ImGui::Button("Rename", ImVec2(120, 0)))
        {
            wait_renderer_idle();
            asset_forget(rename_path);

            if (!asset_rename(rename_path, rename_buffer))
            {
                editor_set_status("Could not rename : is the name taken ?");
            }

            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (open_trash_popup)
    {
        ImGui::OpenPopup("Move to Trash");
        open_trash_popup = false;
    }

    if (ImGui::BeginPopupModal("Move to Trash", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Move '%s' to the trash ?", trash_path);

        if (ImGui::Button("Move to Trash", ImVec2(140, 0)))
        {
            wait_renderer_idle();
            asset_forget(trash_path);

            if (!asset_trash(trash_path))
            {
                editor_set_status("Could not move it to the trash.");
            }

            selected_path[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (open_folder_popup)
    {
        ImGui::OpenPopup("New Folder");
        open_folder_popup = false;
    }

    if (ImGui::BeginPopupModal("New Folder", NULL, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##folder", new_folder_name, sizeof(new_folder_name));

        if (ImGui::Button("Create", ImVec2(120, 0)) && new_folder_name[0])
        {
            asset_create_folder(current_folder, new_folder_name);
            new_folder_name[0] = '\0';
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void editor_draw_asset_browser(void)
{
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(390, io.DisplaySize.y - 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(SDL_max(300.0f, io.DisplaySize.x - 390 - 320), 290), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin(ICON_FOLDER " Assets"))
    {
        ImGui::End();
        draw_popups();
        return;
    }

    if (ImGui::Button(ICON_PLUS " Import..."))
    {
        platform_show_open_files_dialog(on_files_chosen, NULL, "Assets|png;jpg;jpeg;tga;bmp;fbx;obj;gltf;glb;wav;ogg;mp3;flac;exr;hdr;json");
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_FOLDER " New Folder"))
    {
        open_folder_popup = true;
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_REFRESH " Refresh"))
    {
        asset_database_refresh();
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##search", ICON_SEARCH " Search", search, sizeof(search));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("##size", &tile_size, 48.0f, 192.0f, "");

    ImGui::SameLine();
    ImGui::TextDisabled("%s", current_folder);

    if (ImGui::BeginTable("browser", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("folders", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("files", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::BeginChild("tree");
        draw_folder_tree("Assets", "Assets");
        ImGui::EndChild();

        ImGui::TableNextColumn();
        ImGui::BeginChild("tiles");

        float available = ImGui::GetContentRegionAvail().x;
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        int columns = SDL_max(1, (int)((available + spacing) / (tile_size + spacing)));
        int column = 0;
        int shown = 0;

        for (int i = 0; i < asset_database_count(); i++)
        {
            const AssetEntry* entry = asset_database_get(i);

            // A search looks everywhere ; otherwise, just this folder.
            if (search[0] ? !matches_search(entry) : !is_in_folder(entry->path, current_folder))
            {
                continue;
            }

            if (column > 0)
            {
                ImGui::SameLine();
            }

            draw_tile(entry);
            shown++;
            column = (column + 1) % columns;
        }

        if (shown == 0)
        {
            ImGui::TextDisabled(search[0] ? "Nothing matches." : "Empty folder : import files, or drop them on the window.");
        }

        ImGui::EndChild();
        ImGui::EndTable();
    }

    ImGui::End();

    draw_popups();
}
