// The editor's panels : main menu and play controls, primitives, hierarchy, inspector, project settings.

#include "editor.h"

#include "example.h"
#include "platforms/platforms_input.h"
#include "renderer/render3d/render3d.h"

#define PALETTE_ICON_SIZE 30.0f

static bool toggle_button(const char* label, bool active, const char* tooltip)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }

    bool pressed = ImGui::Button(label);

    if (active)
    {
        ImGui::PopStyleColor();
    }

    if (tooltip && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", tooltip);
    }

    return pressed;
}

// --- Main menu and toolbar --------------------------------------------------------------------

void editor_draw_main_menu(void)
{
    bool playing = editor_is_playing();

    // A tint while playing : the unmistakable sign that changes won't be kept.
    if (playing)
    {
        ImGui::PushStyleColor(ImGuiCol_MenuBarBg, ImVec4(0.45f, 0.20f, 0.10f, 1.0f));
    }

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Scene", NULL, false, !playing))
            {
                editor_new_scene();
            }

            if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !playing))
            {
                editor_save_scene();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Project Settings..."))
            {
                editor_show_project_settings = true;
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Back to Menu"))
            {
                example_return_to_menu();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, editor_can_undo()))
            {
                editor_undo();
            }

            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, editor_can_redo()))
            {
                editor_redo();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editor_scene.selected >= 0 && !playing))
            {
                editor_duplicate_selected();
            }

            if (ImGui::MenuItem("Delete", "Del", false, editor_scene.selected >= 0 && !playing))
            {
                editor_delete_selected();
            }

            if (ImGui::MenuItem("Select All", "Ctrl+A", false, !playing))
            {
                level_mesh_operation(MESH_OP_SELECT_ALL, 0.0f);
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Hide Selection", "H", false, !playing))
            {
                level_hide_selected(false);
            }

            if (ImGui::MenuItem("Hide Others", "Shift+H", false, !playing))
            {
                level_hide_selected(true);
            }

            if (ImGui::MenuItem("Show All", "U", false, !playing))
            {
                level_unhide_all();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("2D Views (top, front, side)", NULL, &editor_show_2d_views);
            ImGui::MenuItem("Grid", NULL, &editor_show_grid);
            ImGui::Separator();

            for (int mode = 0; mode < SELECTION_MODE_COUNT; mode++)
            {
                char shortcut[8];
                SDL_snprintf(shortcut, sizeof(shortcut), "%d", mode + 1);

                if (ImGui::MenuItem(level_selection_mode_name(mode), shortcut, editor_selection_mode == mode))
                {
                    level_set_selection_mode(mode);
                }
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Block Tool", "Shift+B", editor_tool == TOOL_BLOCK, !playing))
            {
                level_set_tool(editor_tool == TOOL_BLOCK ? TOOL_SELECT : TOOL_BLOCK);
            }

            if (ImGui::MenuItem("Clipping Tool", "Shift+X", editor_tool == TOOL_CLIP, !playing))
            {
                level_set_tool(editor_tool == TOOL_CLIP ? TOOL_SELECT : TOOL_CLIP);
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Object", !playing))
        {
            if (ImGui::MenuItem("Empty"))
            {
                editor_add_object("Empty");
            }

            if (ImGui::MenuItem(ICON_LIGHTBULB "  Point Light"))
            {
                editor_add_light(SCENE_LIGHT_POINT);
            }

            if (ImGui::MenuItem(ICON_FLASHLIGHT "  Spot Light"))
            {
                editor_add_light(SCENE_LIGHT_SPOT);
            }

            if (ImGui::MenuItem(ICON_GLOBE "  Reflection Probe"))
            {
                editor_add_reflection_probe();
            }

            ImGui::Separator();

            if (ImGui::BeginMenu(ICON_BRICK "  Mesh (level geometry)"))
            {
                for (int shape = 0; shape < EDIT_SHAPE_COUNT; shape++)
                {
                    if (ImGui::MenuItem(edit_mesh_shape_name(shape)))
                    {
                        level_add_block(shape);
                    }
                }

                ImGui::EndMenu();
            }

            ImGui::Separator();

            for (int kind = 0; kind < PRIMITIVE_COUNT; kind++)
            {
                char label[64];
                SDL_snprintf(label, sizeof(label), "%s  %s", primitive_infos[kind].icon, primitive_infos[kind].label);

                if (ImGui::MenuItem(label))
                {
                    char path[64];
                    SDL_snprintf(path, sizeof(path), SCENE_BUILTIN_PREFIX "%s", primitive_infos[kind].id);
                    editor_add_model(path, primitive_infos[kind].label);
                }
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Lighting"))
        {
            lighting_draw_menu();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Profiler"))
        {
            profiler_draw_menu();
            ImGui::EndMenu();
        }

        // Play controls, centered in the bar.
        float buttons_width = 150.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - buttons_width) * 0.5f);

        if (toggle_button(playing ? ICON_SQUARE "##play" : ICON_PLAY "##play", playing, playing ? "Stop (Ctrl+P)" : "Play (Ctrl+P)"))
        {
            if (playing)
            {
                editor_stop();
            }
            else
            {
                editor_play();
            }
        }

        ImGui::BeginDisabled(!playing);

        if (toggle_button(ICON_PAUSE "##pause", editor_is_paused(), "Pause"))
        {
            editor_toggle_pause();
        }

        ImGui::BeginDisabled(!editor_is_paused());

        if (toggle_button(ICON_STEP_FORWARD "##step", false, "One step"))
        {
            editor_step_once();
        }

        ImGui::EndDisabled();
        ImGui::EndDisabled();

        // Status, on the right.
        char right[320];
        SDL_snprintf(right, sizeof(right), "%s%s   %s", editor_status(), editor_dirty ? " (modified)" : "", editor_project.name);
        float width = ImGui::CalcTextSize(right).x;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - width - 12.0f);
        ImGui::TextDisabled("%s", right);

        ImGui::EndMainMenuBar();
    }

    if (playing)
    {
        ImGui::PopStyleColor();
    }
}

void editor_draw_primitives_panel(void)
{
    ImGui::SetNextWindowPos(ImVec2(12, 50), ImGuiCond_FirstUseEver);
    ImGui::Begin(ICON_SHAPES " Primitives", NULL, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::BeginDisabled(editor_is_playing());

    for (int kind = 0; kind < PRIMITIVE_COUNT; kind++)
    {
        if (kind % 4 != 0)
        {
            ImGui::SameLine();
        }

        ImGui::PushID(kind);
        ImGui::BeginGroup();

        // 1.92 fonts can be drawn at any size : big icons without a second font.
        ImGui::PushFont(NULL, PALETTE_ICON_SIZE);
        bool clicked = ImGui::Button(primitive_infos[kind].icon, ImVec2(64, 52));
        ImGui::PopFont();

        float label_width = ImGui::CalcTextSize(primitive_infos[kind].label).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + SDL_max(0.0f, (64.0f - label_width) * 0.5f));
        ImGui::TextUnformatted(primitive_infos[kind].label);

        ImGui::EndGroup();
        ImGui::PopID();

        if (clicked)
        {
            char path[64];
            SDL_snprintf(path, sizeof(path), SCENE_BUILTIN_PREFIX "%s", primitive_infos[kind].id);
            editor_add_model(path, primitive_infos[kind].label);
        }
    }

    ImGui::EndDisabled();
    ImGui::End();
}

void editor_draw_view_panel(void)
{
    ImGui::SetNextWindowPos(ImVec2(12, 240), ImGuiCond_FirstUseEver);
    ImGui::Begin("Tools", NULL, ImGuiWindowFlags_AlwaysAutoResize);

    // The objects mode's own snapping (the meshes modes snap to the grid, see the Level panel).
    bool shift_held = ImGui::GetIO().KeyShift && editor_selection_mode == SELECT_OBJECTS;

    if (toggle_button(ICON_MAGNET " Snap objects", editor_snap_enabled || shift_held, "Objects mode : snap to these increments (or hold Shift)"))
    {
        editor_snap_enabled = !editor_snap_enabled;
    }

    if (editor_snap_enabled || shift_held)
    {
        ImGui::SetNextItemWidth(70);
        ImGui::DragFloat("##snap_t", &editor_snap_translation, 0.05f, 0.05f, 10.0f, "%.2f m");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::DragFloat("##snap_r", &editor_snap_rotation, 1.0f, 1.0f, 90.0f, "%.0f deg");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::DragFloat("##snap_s", &editor_snap_scale, 0.05f, 0.05f, 2.0f, "x%.2f");
    }

    ImGui::Checkbox("Grid", &editor_show_grid);
    ImGui::SameLine();
    ImGui::Checkbox("2D view", &editor_view_2d);
    ImGui::SameLine();
    ImGui::Checkbox("2D panels", &editor_show_2d_views);

    if (ImGui::CollapsingHeader("Sky & sun"))
    {
        Scene* scene = &editor_scene;
        bool edited = false;

        char label[SCENE_PATH_SIZE + 32];
        SDL_snprintf(label, sizeof(label), "%s", scene->skybox[0] ? scene->skybox : "(procedural sky)");
        ImGui::Button(label, ImVec2(240, 0));

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Drop a skybox from the asset browser here");
        }

        const char* dropped = editor_accept_asset(ASSET_SKYBOX);

        if (dropped)
        {
            editor_begin_edit();
            SDL_snprintf(scene->skybox, sizeof(scene->skybox), "%s", dropped);
        }

        if (scene->skybox[0])
        {
            ImGui::SameLine();

            if (ImGui::Button(ICON_TRASH "##no_sky"))
            {
                editor_begin_edit();
                scene->skybox[0] = '\0';
            }

            bool from_environment = scene->use_environment_sun != 0;

            if (ImGui::Checkbox("Sun from the skybox", &from_environment))
            {
                editor_begin_edit();
                scene->use_environment_sun = from_environment;
            }
        }

        ImGui::SliderAngle("Sun yaw", &scene->sun_yaw, -180.0f, 180.0f);
        edited |= ImGui::IsItemActivated();
        ImGui::SliderAngle("Sun elevation", &scene->sun_pitch, -5.0f, 89.0f);
        edited |= ImGui::IsItemActivated();
        ImGui::SliderFloat("Sun intensity", &scene->sun_intensity, 0.0f, 3.0f, "%.2f");
        edited |= ImGui::IsItemActivated();

        ImGui::SeparatorText("Clouds");
        SceneClouds* clouds = &scene->clouds;
        ImGui::SliderFloat("Coverage", &clouds->coverage, 0.0f, 1.0f, "%.2f");
        edited |= ImGui::IsItemActivated();
        ImGui::BeginDisabled(clouds->coverage <= 0.0f);
        ImGui::SliderFloat("Density", &clouds->density, 0.05f, 2.0f, "%.2f");
        edited |= ImGui::IsItemActivated();
        ImGui::DragFloat("Altitude", &clouds->altitude, 10.0f, 50.0f, 10000.0f, "%.0f m");
        edited |= ImGui::IsItemActivated();
        ImGui::DragFloat("Scale", &clouds->scale, 5.0f, 20.0f, 20000.0f, "%.0f m");
        edited |= ImGui::IsItemActivated();
        ImGui::DragFloat2("Wind", clouds->wind, 0.1f, -100.0f, 100.0f, "%.1f m/s");
        edited |= ImGui::IsItemActivated();
        ImGui::SliderFloat("Shadows", &clouds->shadows, 0.0f, 1.0f, "%.2f");
        edited |= ImGui::IsItemActivated();
        ImGui::EndDisabled();

        if (edited)
        {
            editor_begin_edit();
        }
    }

    if (ImGui::CollapsingHeader("Controls"))
    {
        // Positional keys, labeled as on the current layout (Z / Q / A... on AZERTY).
        ImGui::Text(
            "Left click        select (Ctrl : add / remove)\n"
            "Left drag         box selection (on nothing)\n"
            "Right drag        fly (noclip) :\n"
            "  %s %s %s %s         forward / left / back / right\n"
            "  %s / %s           down / up\n"
            "  Shift           faster\n"
            "Alt + left drag   orbit\n"
            "Middle drag       pan\n"
            "Wheel             zoom\n"
            "F                 focus selection\n"
            "%s %s %s %s %s         vertices / edges / faces / meshes / objects\n"
            "%s / %s / %s         move / rotate / scale\n"
            "Shift + drag      objects : snap ; meshes : clone ;\n"
            "                  faces : extrude ; edges : extend\n"
            "Shift+B, Shift+X  block tool, clipping tool\n"
            "Enter / Escape    apply / cancel a tool\n"
            "%s / %s             finer / coarser grid\n"
            "Arrows, PgUp/PgDn nudge by a grid step\n"
            "M, L, G           merge, edge loop, edge ring\n"
            "H, Shift+H, U     hide, hide others, show all\n"
            "Delete, Ctrl+D    delete / duplicate\n"
            "Ctrl+Z / Ctrl+Y   undo / redo\n"
            "Ctrl+S            save\n"
            "Ctrl+P            play / stop",
            input_key_label(SCANCODE_W), input_key_label(SCANCODE_A), input_key_label(SCANCODE_S), input_key_label(SCANCODE_D),
            input_key_label(SCANCODE_Q), input_key_label(SCANCODE_E),
            input_key_label(SCANCODE_1), input_key_label(SCANCODE_2), input_key_label(SCANCODE_3), input_key_label(SCANCODE_4), input_key_label(SCANCODE_5),
            input_key_label(SCANCODE_T), input_key_label(SCANCODE_R), input_key_label(SCANCODE_E),
            input_key_label(SCANCODE_LEFTBRACKET), input_key_label(SCANCODE_RIGHTBRACKET));
    }

    ImGui::End();
}

static const char* object_icon(const SceneObject* object)
{
    if (object->is_character)
    {
        return ICON_USER;
    }

    if (object->mesh)
    {
        return ICON_BRICK;
    }

    if (object->sprite[0])
    {
        return ICON_IMAGE;
    }

    if (object->model[0])
    {
        return ICON_BOX;
    }

    if (object->light_type != SCENE_LIGHT_NONE)
    {
        return object->light_type == SCENE_LIGHT_SPOT ? ICON_FLASHLIGHT : ICON_LIGHTBULB;
    }

    if (object->reflection_probe)
    {
        return ICON_GLOBE;
    }

    return object->audio[0] ? ICON_VOLUME : ICON_CIRCLE;
}

void editor_draw_hierarchy(void)
{
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 312, 40), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 300), ImGuiCond_FirstUseEver);
    ImGui::Begin("Hierarchy");

    ImGui::BeginDisabled(editor_scene.selected < 0 || editor_is_playing());

    if (ImGui::Button(ICON_COPY " Duplicate"))
    {
        editor_duplicate_selected();
    }

    ImGui::SameLine();

    if (ImGui::Button(ICON_TRASH " Delete"))
    {
        editor_delete_selected();
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(editor_scene.selected < 0);

    if (ImGui::Button(ICON_CROSSHAIR " Focus"))
    {
        editor_focus_selected();
    }

    ImGui::EndDisabled();
    ImGui::Separator();

    ImGui::BeginChild("objects");

    for (int i = 0; i < editor_scene.count; i++)
    {
        SceneObject* object = &editor_scene.objects[i];
        ImGui::PushID(i);

        // Visibility (the editor's only : the game shows everything).
        bool hidden = object->hidden != 0;

        if (ImGui::SmallButton(hidden ? ICON_EYE_OFF : ICON_EYE))
        {
            editor_begin_edit();
            object->hidden = !hidden;

            if (object->hidden && object->selected)
            {
                editor_select_object(i, SELECT_TOGGLE);
            }
        }

        ImGui::SameLine();

        char label[96];
        SDL_snprintf(label, sizeof(label), "%s  %s", object_icon(object), object->name);

        if (hidden)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        }

        if (ImGui::Selectable(label, object->selected != 0))
        {
            editor_select_object(i, ImGui::GetIO().KeyCtrl ? SELECT_TOGGLE : SELECT_REPLACE);
        }

        if (hidden)
        {
            ImGui::PopStyleColor();
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            editor_focus_selected();
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
}

// --- Inspector --------------------------------------------------------------------------------

// A slot for an asset : shows its name, accepts a drop, and can be cleared.
static bool asset_slot(const char* label, char* path, size_t path_size, int type)
{
    bool changed = false;

    ImGui::PushID(label);

    const char* name = SDL_strrchr(path, '/');
    name = name ? name + 1 : (path[0] ? path : "(none)");

    ImGui::Button(name, ImVec2(ImGui::CalcItemWidth() - 28.0f, 0.0f));

    const char* dropped = editor_accept_asset(type);

    if (dropped)
    {
        editor_begin_edit();
        SDL_snprintf(path, path_size, "%s", dropped);
        changed = true;
    }

    ImGui::SameLine(0.0f, 4.0f);
    ImGui::BeginDisabled(path[0] == '\0');

    if (ImGui::Button(ICON_TRASH "##clear"))
    {
        editor_begin_edit();
        path[0] = '\0';
        changed = true;
    }

    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted(label);

    ImGui::PopID();

    return changed;
}

static bool component_header(const char* label, bool* remove)
{
    float right_edge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    ImGui::SameLine(right_edge - 22.0f);
    ImGui::PushID(label);

    if (ImGui::SmallButton(ICON_TRASH))
    {
        *remove = true;
    }

    ImGui::PopID();

    return open;
}

void editor_draw_inspector(void)
{
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 312, 350), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 500), ImGuiCond_FirstUseEver);
    ImGui::Begin("Inspector");

    if (editor_scene.selected < 0)
    {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::End();
        return;
    }

    int selection = editor_selection_count();

    if (selection > 1)
    {
        ImGui::TextDisabled("%d objects selected : this is the last one clicked.", selection);
    }

    SceneObject* object = &editor_scene.objects[editor_scene.selected];
    const ProjectSettings* settings = &editor_project.settings;
    bool edit_started = false;

    ImGui::PushItemWidth(170);

    ImGui::InputText("Name", object->name, sizeof(object->name));
    edit_started |= ImGui::IsItemActivated();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::DragFloat3("Position", object->position, 0.05f, 0.0f, 0.0f, "%.2f");
        edit_started |= ImGui::IsItemActivated();
        ImGui::DragFloat3("Rotation", object->rotation, 0.5f, 0.0f, 0.0f, "%.1f");
        edit_started |= ImGui::IsItemActivated();
        ImGui::DragFloat3("Scale", object->scale, 0.01f, 0.01f, 1000.0f, "%.2f");
        edit_started |= ImGui::IsItemActivated();
    }

    const EditMesh* mesh = object->mesh ? edit_mesh_store_get(object->mesh) : NULL;

    if (mesh)
    {
        if (ImGui::CollapsingHeader(ICON_BRICK " Mesh", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("%d vertices, %d edges, %d faces", mesh->vertex_count, mesh->edge_count, mesh->face_count);

            // The materials its faces use.
            for (int m = 0; m < mesh->material_count; m++)
            {
                int faces = 0;

                for (int f = 0; f < mesh->face_count; f++)
                {
                    faces += mesh->faces[f].style.material == m;
                }

                if (faces > 0)
                {
                    const char* name = SDL_strrchr(mesh->materials[m], '/');
                    ImGui::BulletText("%s : %d face%s", name ? name + 1 : mesh->materials[m], faces, faces == 1 ? "" : "s");
                }
            }

            ImGui::TextDisabled("Edit it with the Level panel : 1 2 3 = vertices, edges, faces.");
        }
    }
    else if (object->model[0] || (!object->sprite[0] && !object->is_2d && object->light_type == SCENE_LIGHT_NONE && !object->reflection_probe))
    {
        if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen))
        {
            asset_slot("Model", object->model, sizeof(object->model), ASSET_MODEL);
        }
    }

    if (object->sprite[0] || object->is_2d)
    {
        if (ImGui::CollapsingHeader("Sprite", ImGuiTreeNodeFlags_DefaultOpen))
        {
            asset_slot("Image", object->sprite, sizeof(object->sprite), ASSET_SPRITE);
            ImGui::DragFloat2("Size", object->sprite_size, 0.01f, 0.01f, 1000.0f, "%.2f m");
            edit_started |= ImGui::IsItemActivated();
            ImGui::DragFloat4("Region", object->sprite_region, 1.0f, 0.0f, 65536.0f, "%.0f");
            edit_started |= ImGui::IsItemActivated();

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("x, y, width, height in pixels ; width 0 = the whole image");
            }

            ImGui::ColorEdit4("Tint", object->base_color);
            edit_started |= ImGui::IsItemActivated();
        }
    }

    bool remove = false;

    if (object->override_material && !(object->sprite[0] || object->is_2d))
    {
        if (component_header("Material", &remove))
        {
            ImGui::ColorEdit4("Base color", object->base_color);
            edit_started |= ImGui::IsItemActivated();
            ImGui::SliderFloat("Metallic", &object->metallic, 0.0f, 1.0f);
            edit_started |= ImGui::IsItemActivated();
            ImGui::SliderFloat("Roughness", &object->roughness, 0.0f, 1.0f);
            edit_started |= ImGui::IsItemActivated();
            ImGui::DragFloat3("Emissive", object->emissive, 0.05f, 0.0f, 1000.0f, "%.2f");
            edit_started |= ImGui::IsItemActivated();
            ImGui::DragFloat2("UV scale", object->uv_scale, 0.05f, 0.01f, 1000.0f, "%.2f");
            edit_started |= ImGui::IsItemActivated();
            asset_slot("Albedo", object->albedo_texture, sizeof(object->albedo_texture), ASSET_TEXTURE);
            asset_slot("Normal", object->normal_texture, sizeof(object->normal_texture), ASSET_TEXTURE);
            asset_slot("Metal/Rough", object->metallic_roughness_texture, sizeof(object->metallic_roughness_texture), ASSET_TEXTURE);
        }

        if (remove)
        {
            editor_begin_edit();
            object->override_material = 0;
            remove = false;
        }
    }

    if (object->body_type != SCENE_BODY_NONE && !object->is_character)
    {
        if (component_header(object->is_2d ? "Rigid Body 2D (Box2D)" : "Rigid Body (Box3D)", &remove))
        {
            static const char* const body_types[] = { "None", "Static", "Kinematic", "Dynamic" };
            static const char* const colliders[] = { "Auto", "Box", "Sphere", "Capsule", "Convex hull", "Triangle mesh" };

            int body_type = object->body_type;
            int collider = object->collider;

            if (ImGui::Combo("Type", &body_type, body_types, 4))
            {
                editor_begin_edit();
                object->body_type = body_type;
            }

            if (ImGui::Combo("Collider", &collider, colliders, object->is_2d ? 4 : 6))
            {
                editor_begin_edit();
                object->collider = collider;
            }

            ImGui::DragFloat("Density", &object->density, 0.05f, 0.001f, 1000.0f, "%.2f");
            edit_started |= ImGui::IsItemActivated();
            ImGui::SliderFloat("Friction", &object->friction, 0.0f, 2.0f);
            edit_started |= ImGui::IsItemActivated();
            ImGui::SliderFloat("Restitution", &object->restitution, 0.0f, 1.0f);
            edit_started |= ImGui::IsItemActivated();
        }

        if (remove)
        {
            editor_begin_edit();
            object->body_type = SCENE_BODY_NONE;
            remove = false;
        }
    }

    if (object->is_character)
    {
        if (component_header("Character Controller", &remove))
        {
            ImGui::TextWrapped("A dynamic capsule : it falls, pushes and gets pushed. Its speed and jump are in the Project Settings.");
        }

        if (remove)
        {
            editor_begin_edit();
            object->is_character = 0;
            remove = false;
        }
    }

    if (object->body_type != SCENE_BODY_NONE || object->is_character)
    {
        if (ImGui::BeginCombo("Layer", settings->layer_names[SDL_clamp(object->layer, 0, PROJECT_MAX_LAYERS - 1)]))
        {
            for (int i = 0; i < PROJECT_MAX_LAYERS; i++)
            {
                if (settings->layer_names[i][0] && ImGui::Selectable(settings->layer_names[i], object->layer == i))
                {
                    editor_begin_edit();
                    object->layer = i;
                }
            }

            ImGui::EndCombo();
        }

        bool is_2d = object->is_2d != 0;

        if (ImGui::Checkbox("2D physics", &is_2d))
        {
            editor_begin_edit();
            object->is_2d = is_2d;
        }
    }

    if (object->light_type != SCENE_LIGHT_NONE)
    {
        if (component_header("Light", &remove))
        {
            static const char* const types[] = { "Point", "Spot" };
            int type = object->light_type - SCENE_LIGHT_POINT;

            if (ImGui::Combo("Type", &type, types, 2))
            {
                editor_begin_edit();
                object->light_type = SCENE_LIGHT_POINT + type;
            }

            ImGui::ColorEdit3("Color", object->light_color);
            edit_started |= ImGui::IsItemActivated();
            ImGui::DragFloat("Intensity", &object->light_intensity, 0.1f, 0.0f, 100000.0f, "%.2f");
            edit_started |= ImGui::IsItemActivated();
            ImGui::SetItemTooltip("The irradiance at 1 m, in the sun's units");
            ImGui::DragFloat("Range", &object->light_range, 0.05f, 0.1f, 1000.0f, "%.2f m");
            edit_started |= ImGui::IsItemActivated();

            if (object->light_type == SCENE_LIGHT_SPOT)
            {
                ImGui::SliderFloat("Angle", &object->light_spot_angle, 1.0f, 170.0f, "%.0f deg");
                edit_started |= ImGui::IsItemActivated();
                ImGui::SliderFloat("Softness", &object->light_spot_softness, 0.0f, 1.0f);
                edit_started |= ImGui::IsItemActivated();
                ImGui::TextDisabled("Points along the object's -Y.");
            }

            bool shadows = object->light_shadows != 0;

            if (ImGui::Checkbox("Cast shadows", &shadows))
            {
                editor_begin_edit();
                object->light_shadows = shadows;
            }

            static const char* const modes[] = { "Realtime", "Mixed", "Baked" };

            if (ImGui::Combo("Mode", &object->light_mode, modes, 3))
            {
                editor_begin_edit();
            }

            ImGui::SetItemTooltip("Realtime : nothing baked. Mixed : direct light live, bounces baked.\n"
                "Baked : all in the lightmaps and probes (Lighting > Bake), free at runtime.");
        }

        if (remove)
        {
            editor_begin_edit();
            object->light_type = SCENE_LIGHT_NONE;
            remove = false;
        }
    }

    if (object->reflection_probe)
    {
        if (component_header("Reflection Probe", &remove))
        {
            ImGui::DragFloat3("Box size", object->probe_box, 0.05f, 0.1f, 1000.0f, "%.2f m");
            edit_started |= ImGui::IsItemActivated();
            ImGui::SetItemTooltip("What reflects it, and the walls its reflections are projected on (centered, not rotated)");
            ImGui::DragFloat("Blend", &object->probe_blend, 0.02f, 0.01f, 50.0f, "%.2f m");
            edit_started |= ImGui::IsItemActivated();
            static const char* const resolutions[] = { "64", "128", "256", "512" };
            int resolution = object->probe_resolution <= 64 ? 0 : object->probe_resolution <= 128 ? 1 : object->probe_resolution <= 256 ? 2 : 3;

            if (ImGui::Combo("Resolution", &resolution, resolutions, 4))
            {
                editor_begin_edit();
                object->probe_resolution = 64 << resolution;
            }

            ImGui::TextDisabled("Captured by Lighting > Bake lighting.");
        }

        if (remove)
        {
            editor_begin_edit();
            object->reflection_probe = 0;
            remove = false;
        }
    }

    // An Audio Source added from the menu has no clip yet : it stays visible to receive one.
    static const SceneObject* audio_without_clip;

    if (object->audio[0] || object == audio_without_clip)
    {
        if (component_header("Audio Source", &remove))
        {
            asset_slot("Clip", object->audio, sizeof(object->audio), ASSET_AUDIO);
            ImGui::SliderFloat("Volume", &object->audio_volume, 0.0f, 1.0f);
            edit_started |= ImGui::IsItemActivated();

            bool loop = object->audio_loop != 0;
            bool autoplay = object->audio_autoplay != 0;
            bool music = object->audio_is_music != 0;
            bool spatial = object->audio_3d != 0;

            if (ImGui::Checkbox("Loop", &loop)) { editor_begin_edit(); object->audio_loop = loop; }
            ImGui::SameLine();
            if (ImGui::Checkbox("Play on start", &autoplay)) { editor_begin_edit(); object->audio_autoplay = autoplay; }
            if (ImGui::Checkbox("Music", &music)) { editor_begin_edit(); object->audio_is_music = music; }
            ImGui::SameLine();
            if (ImGui::Checkbox("3D", &spatial)) { editor_begin_edit(); object->audio_3d = spatial; }
        }

        if (remove)
        {
            editor_begin_edit();
            object->audio[0] = '\0';
            audio_without_clip = NULL;
            remove = false;
        }
    }

    ImGui::Separator();

    if (ImGui::Button(ICON_PLUS " Add Component", ImVec2(-1.0f, 0.0f)))
    {
        ImGui::OpenPopup("add_component");
    }

    if (ImGui::BeginPopup("add_component"))
    {
        if (ImGui::MenuItem("Rigid Body", NULL, false, object->body_type == SCENE_BODY_NONE && !object->is_character))
        {
            editor_begin_edit();
            object->body_type = SCENE_BODY_DYNAMIC;
        }

        if (ImGui::MenuItem("Static Collider", NULL, false, object->body_type == SCENE_BODY_NONE && !object->is_character))
        {
            editor_begin_edit();
            object->body_type = SCENE_BODY_STATIC;
        }

        if (ImGui::MenuItem("Character Controller", NULL, false, !object->is_character))
        {
            editor_begin_edit();
            object->is_character = 1;
            object->body_type = SCENE_BODY_NONE;
            object->layer = 1;
        }

        if (ImGui::MenuItem("Material", NULL, false, !object->override_material))
        {
            editor_begin_edit();
            object->override_material = 1;
        }

        if (ImGui::MenuItem("Light", NULL, false, object->light_type == SCENE_LIGHT_NONE))
        {
            editor_begin_edit();
            object->light_type = SCENE_LIGHT_POINT;
        }

        if (ImGui::MenuItem("Reflection Probe", NULL, false, !object->reflection_probe))
        {
            editor_begin_edit();
            object->reflection_probe = 1;
        }

        if (ImGui::MenuItem("Audio Source", NULL, false, !object->audio[0] && object != audio_without_clip))
        {
            audio_without_clip = object;
        }

        ImGui::EndPopup();
    }

    // Dropping an asset on the inspector's button area assigns it by type.
    const char* audio = editor_accept_asset(ASSET_AUDIO);

    if (audio)
    {
        editor_begin_edit();
        SDL_snprintf(object->audio, sizeof(object->audio), "%s", audio);
    }

    ImGui::PopItemWidth();

    if (edit_started)
    {
        editor_begin_edit();
    }

    ImGui::End();
}

// --- Project settings -------------------------------------------------------------------------

static void draw_collision_matrix(ProjectSettings* settings, bool* changed)
{
    // Layers in use : a matrix of checkboxes, symmetric, like Unity's.
    int layers[PROJECT_MAX_LAYERS];
    int count = 0;

    for (int i = 0; i < PROJECT_MAX_LAYERS; i++)
    {
        if (settings->layer_names[i][0])
        {
            layers[count++] = i;
        }
    }

    if (count == 0 || !ImGui::BeginTable("matrix", count + 1, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Borders))
    {
        return;
    }

    ImGui::TableSetupColumn("");

    for (int c = count - 1; c >= 0; c--)
    {
        ImGui::TableSetupColumn(settings->layer_names[layers[c]], ImGuiTableColumnFlags_AngledHeader);
    }

    ImGui::TableAngledHeadersRow();

    for (int r = 0; r < count; r++)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(settings->layer_names[layers[r]]);

        for (int c = count - 1; c >= r; c--)
        {
            ImGui::TableNextColumn();
            ImGui::PushID(r * PROJECT_MAX_LAYERS + c);

            int a = layers[r];
            int b = layers[c];
            bool collides = (settings->layer_masks[a] >> b) & 1;

            if (ImGui::Checkbox("##cell", &collides))
            {
                if (collides)
                {
                    settings->layer_masks[a] |= (uint16_t)(1u << b);
                    settings->layer_masks[b] |= (uint16_t)(1u << a);
                }
                else
                {
                    settings->layer_masks[a] &= (uint16_t)~(1u << b);
                    settings->layer_masks[b] &= (uint16_t)~(1u << a);
                }

                *changed = true;
            }

            ImGui::PopID();
        }
    }

    ImGui::EndTable();
}

void editor_draw_project_settings(void)
{
    if (!editor_show_project_settings)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(560, 560), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Project Settings", &editor_show_project_settings))
    {
        ImGui::End();
        return;
    }

    ProjectSettings* s = &editor_project.settings;
    bool changed = false;

    if (ImGui::BeginTabBar("settings_tabs"))
    {
        if (ImGui::BeginTabItem("Physics"))
        {
            changed |= ImGui::DragFloat3("Gravity (3D)", s->gravity_3d, 0.05f, -100.0f, 100.0f, "%.2f");
            changed |= ImGui::DragFloat2("Gravity (2D)", s->gravity_2d, 0.05f, -100.0f, 100.0f, "%.2f");
            changed |= ImGui::SliderInt("Steps per second", &s->physics_rate, 15, 240);
            changed |= ImGui::SliderInt("Substeps", &s->physics_substeps, 1, 8);
            changed |= ImGui::SliderFloat("Default friction", &s->default_friction, 0.0f, 2.0f);
            changed |= ImGui::SliderFloat("Default restitution", &s->default_restitution, 0.0f, 1.0f);

            ImGui::SeparatorText("Character controller");
            changed |= ImGui::DragFloat("Speed", &s->character_speed, 0.05f, 0.0f, 100.0f, "%.2f m/s");
            changed |= ImGui::DragFloat("Sprint", &s->character_sprint_multiplier, 0.01f, 1.0f, 5.0f, "x%.2f");
            changed |= ImGui::DragFloat("Jump speed", &s->character_jump_speed, 0.05f, 0.0f, 50.0f, "%.2f m/s");
            changed |= ImGui::DragFloat("Acceleration", &s->character_acceleration, 0.5f, 0.0f, 500.0f, "%.1f m/s2");
            changed |= ImGui::SliderFloat("Air control", &s->character_air_control, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Max slope", &s->character_max_slope, 0.0f, 89.0f, "%.0f deg");
            changed |= ImGui::DragFloat("Mass", &s->character_mass, 0.5f, 1.0f, 1000.0f, "%.1f kg");

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Collision"))
        {
            ImGui::SeparatorText("Which layers collide");
            draw_collision_matrix(s, &changed);

            ImGui::SeparatorText("Layers");

            for (int i = 0; i < PROJECT_MAX_LAYERS; i++)
            {
                ImGui::PushID(i);
                char label[16];
                SDL_snprintf(label, sizeof(label), "Layer %d", i);
                changed |= ImGui::InputText(label, s->layer_names[i], sizeof(s->layer_names[i]));
                ImGui::PopID();
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Rendering"))
        {
            static const char* const paths[] = { "Deferred+", "Forward+", "2D" };
            static const char* const tonemappers[] = { "ACES", "Khronos Neutral", "Reinhard", "None" };
            int tonemapper = (int)s->look.tonemapper;

            if (ImGui::Combo("Render path", &s->render_path, paths, 3))
            {
                // A preset means different things to each pipeline (MSAA or TAA...) : applied again.
                if (s->quality_preset != QUALITY_CUSTOM)
                {
                    project_apply_quality_preset(s, (QualityPreset)s->quality_preset);
                }

                changed = true;
            }

            if (s->render_path == RENDER_PATH_2D)
            {
                ImGui::TextDisabled("Sprites only : 3D objects aren't drawn.");

                ImGui::SeparatorText("2D");
                bool pixel_perfect = s->render_2d.pixel_perfect != 0;

                if (ImGui::Checkbox("Pixel perfect", &pixel_perfect))
                {
                    s->render_2d.pixel_perfect = pixel_perfect;
                    changed = true;
                }

                ImGui::SetItemTooltip("Renders at a fixed height in pixels, scaled up by a whole factor (orthographic views).");

                ImGui::BeginDisabled(!pixel_perfect);
                changed |= ImGui::SliderInt("Pixel height", &s->render_2d.pixel_height, 90, 1080);
                ImGui::EndDisabled();

                changed |= ImGui::ColorEdit4("Background", s->render_2d.background);
            }
            else
            {
                ImGui::SeparatorText("Image");
                changed |= ImGui::SliderFloat("Exposure (EV)", &s->look.exposure_ev, -5.0f, 5.0f, "%.2f");

                if (ImGui::Combo("Tone mapper", &tonemapper, tonemappers, 4))
                {
                    s->look.tonemapper = (Render3DTonemapper)tonemapper;
                    changed = true;
                }

                changed |= ImGui::SliderFloat("Bloom", &s->look.bloom_strength, 0.0f, 0.3f, "%.3f");
                changed |= ImGui::SliderFloat("Contrast", &s->look.contrast, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Saturation", &s->look.saturation, 0.0f, 2.0f);
                changed |= ImGui::SliderFloat("Vignette", &s->look.vignette, 0.0f, 1.0f);

                ImGui::SeparatorText("Light");
                changed |= ImGui::SliderFloat("Sky intensity", &s->look.sky_intensity, 0.0f, 4.0f);
                changed |= ImGui::SliderFloat("Bounce light", &s->look.bounce_strength, 0.0f, 1.0f);
                changed |= ImGui::SliderFloat("Sun softness", &s->look.sun_softness, 0.5f, 20.0f);
                changed |= ImGui::SliderFloat("AO radius", &s->look.ao_radius, 0.05f, 5.0f, "%.2f m");
                changed |= ImGui::SliderFloat("AO power", &s->look.ao_power, 0.2f, 4.0f);

                ImGui::SeparatorText("Fog");
                changed |= ImGui::SliderFloat("Density", &s->look.fog_density, 0.0f, 0.2f, "%.4f");
                changed |= ImGui::SliderFloat("Height falloff", &s->look.fog_height_falloff, 0.001f, 1.0f, "%.3f");
                changed |= ImGui::DragFloat("Base height", &s->look.fog_base_height, 0.1f);
                changed |= ImGui::ColorEdit3("Color", s->look.fog_color);
                changed |= ImGui::ColorEdit3("Toward the sun", s->look.fog_sun_color);
                changed |= ImGui::SliderFloat("Sun power", &s->look.fog_sun_power, 1.0f, 64.0f);
                ImGui::TextDisabled("With volumetrics (Quality), the fog scatters the lights.");
                changed |= ImGui::SliderFloat("Volumetric range", &s->look.volumetric_distance, 5.0f, 500.0f, "%.0f m");
                changed |= ImGui::SliderFloat("Anisotropy", &s->look.volumetric_anisotropy, -0.9f, 0.95f, "%.2f");
                ImGui::SetItemTooltip("0 : light scatters evenly ; toward 1 : mostly forward, a glow around the lights and the sun");
                changed |= ImGui::SliderFloat("Scattering", &s->look.volumetric_intensity, 0.0f, 4.0f, "%.2f");

                if (s->render_path == RENDER_PATH_DEFERRED_PLUS)
                {
                    ImGui::SeparatorText("Reflections");
                    changed |= ImGui::SliderFloat("Ray length", &s->look.ssr_distance, 1.0f, 200.0f, "%.1f m");
                    changed |= ImGui::SliderFloat("Thickness", &s->look.ssr_thickness, 0.02f, 2.0f, "%.2f m");
                }
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Quality"))
        {
            static const char* const presets[] = { "Low", "Medium", "High", "Ultra", "Custom" };

            if (ImGui::Combo("Preset", &s->quality_preset, presets, QUALITY_PRESET_COUNT))
            {
                project_apply_quality_preset(s, (QualityPreset)s->quality_preset);
                changed = true;
            }

            bool quality_changed = false;
            Render3DQuality* q = &s->quality;
            bool ao = q->ao_enabled != 0;
            bool bloom = q->bloom_enabled != 0;
            bool fxaa = q->fxaa_enabled != 0;

            ImGui::SeparatorText("Resolution");
            quality_changed |= ImGui::SliderFloat("Render scale", &q->render_scale, 0.25f, 2.0f, "%.2f");

            ImGui::SeparatorText("Shadows");
            static const char* const sizes[] = { "512", "1024", "2048", "4096" };
            int size_index = q->shadow_resolution <= 512 ? 0 : q->shadow_resolution <= 1024 ? 1 : q->shadow_resolution <= 2048 ? 2 : 3;

            if (ImGui::Combo("Resolution", &size_index, sizes, 4))
            {
                q->shadow_resolution = 512 << size_index;
                quality_changed = true;
            }

            quality_changed |= ImGui::SliderInt("Cascades", &q->shadow_cascades, 1, 4);
            quality_changed |= ImGui::SliderInt("Samples", &q->shadow_samples, 4, 64);
            quality_changed |= ImGui::DragFloat("Distance", &q->shadow_distance, 1.0f, 10.0f, 2000.0f, "%.0f m");

            ImGui::SeparatorText("Point and spot lights");
            static const char* const atlases[] = { "1024", "2048", "4096", "8192" };
            int atlas_index = q->local_shadow_atlas <= 1024 ? 0 : q->local_shadow_atlas <= 2048 ? 1 : q->local_shadow_atlas <= 4096 ? 2 : 3;

            if (ImGui::Combo("Shadow atlas", &atlas_index, atlases, 4))
            {
                q->local_shadow_atlas = 1024 << atlas_index;
                quality_changed = true;
            }

            ImGui::SetItemTooltip("8 x 8 tiles : one per spot light, six per point light");
            quality_changed |= ImGui::SliderInt("Shadowed lights", &q->local_shadow_lights, 0, 32);
            ImGui::SetItemTooltip("At most this many lights cast shadows at once : the most important in view");

            ImGui::SeparatorText("Effects");

            if (ImGui::Checkbox("Ambient occlusion", &ao)) { q->ao_enabled = ao; quality_changed = true; }
            quality_changed |= ImGui::SliderInt("AO slices", &q->ao_slices, 1, 8);
            quality_changed |= ImGui::SliderInt("AO steps", &q->ao_steps, 1, 16);
            if (ImGui::Checkbox("Bloom", &bloom)) { q->bloom_enabled = bloom; quality_changed = true; }
            if (ImGui::Checkbox("FXAA", &fxaa)) { q->fxaa_enabled = fxaa; quality_changed = true; }

            bool volumetrics = q->volumetrics_enabled != 0;
            bool volumetric_lights = q->volumetric_local_lights != 0;
            if (ImGui::Checkbox("Volumetric fog", &volumetrics)) { q->volumetrics_enabled = volumetrics; quality_changed = true; }
            ImGui::SetItemTooltip("Light shafts and halos in the fog (Rendering > Fog : density above 0)");
            quality_changed |= ImGui::SliderInt("Volumetric steps", &q->volumetric_steps, 4, 128);
            if (ImGui::Checkbox("Lights in the fog", &volumetric_lights)) { q->volumetric_local_lights = volumetric_lights; quality_changed = true; }

            bool taa = q->taa_enabled != 0;
            if (ImGui::Checkbox("TAA", &taa)) { q->taa_enabled = taa; quality_changed = true; }
            ImGui::SetItemTooltip("Temporal anti-aliasing : sub-pixel jitter, accumulated over frames");
            quality_changed |= ImGui::SliderFloat("Sharpening", &q->sharpness, 0.0f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("Contrast adaptive sharpening of the final image (0 = off)");

            if (s->render_path == RENDER_PATH_FORWARD_PLUS)
            {
                ImGui::SeparatorText("Forward+");
                static const char* const msaa_modes[] = { "Off", "2x", "4x", "8x" };
                int msaa_index = q->msaa_samples >= 8 ? 3 : q->msaa_samples >= 4 ? 2 : q->msaa_samples >= 2 ? 1 : 0;

                if (ImGui::Combo("MSAA", &msaa_index, msaa_modes, 4))
                {
                    q->msaa_samples = msaa_index == 0 ? 1 : 1 << msaa_index;
                    quality_changed = true;
                }
            }
            else if (s->render_path == RENDER_PATH_DEFERRED_PLUS)
            {
                ImGui::SeparatorText("Deferred+");
                bool ssr = q->ssr_enabled != 0;
                if (ImGui::Checkbox("Screen-space reflections", &ssr)) { q->ssr_enabled = ssr; quality_changed = true; }
                quality_changed |= ImGui::SliderInt("Reflection steps", &q->ssr_steps, 4, 128);
            }

            if (s->render_path != RENDER_PATH_2D)
            {
                ImGui::SeparatorText("Realtime global illumination");
                bool gi = q->gi_enabled != 0;
                bool supported = render3d_realtime_gi_supported() != 0;

                ImGui::BeginDisabled(!supported);
                if (ImGui::Checkbox("Realtime GI", &gi)) { q->gi_enabled = gi; quality_changed = true; }
                ImGui::EndDisabled();
                ImGui::SetItemTooltip(supported ? "Ray-traced probes, every frame : in place of the baked lighting" : "Needs a GPU with ray tracing");
                quality_changed |= ImGui::SliderInt("Rays per probe", &q->gi_rays, 16, 256);
                quality_changed |= ImGui::SliderFloat("Probe spacing", &q->gi_spacing, 0.25f, 8.0f, "%.2f m");
                quality_changed |= ImGui::SliderInt("Bouncing lights", &q->gi_lights, 0, 64);
            }

            if (quality_changed)
            {
                s->quality_preset = QUALITY_CUSTOM;
                changed = true;
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Lighting"))
        {
            BakeSettings* b = &s->bake;
            ImGui::TextWrapped("Baked with Lighting > Bake lighting : lightmaps on the editable meshes, probes for the rest, occlusion culling.");
            ImGui::SeparatorText("Lightmaps");
            changed |= ImGui::SliderFloat("Texels per meter", &b->texels_per_meter, 0.5f, 32.0f, "%.1f");
            static const char* const page_sizes[] = { "256", "512", "1024", "2048", "4096" };
            int page_index = b->page_size <= 256 ? 0 : b->page_size <= 512 ? 1 : b->page_size <= 1024 ? 2 : b->page_size <= 2048 ? 3 : 4;

            if (ImGui::Combo("Page size", &page_index, page_sizes, 5))
            {
                b->page_size = 256 << page_index;
                changed = true;
            }

            changed |= ImGui::SliderInt("Samples", &b->samples, 8, 1024);
            changed |= ImGui::SliderInt("Bounces", &b->bounces, 1, 6);
            ImGui::SeparatorText("Probe volume");
            changed |= ImGui::SliderFloat("Spacing", &b->probe_spacing, 0.0f, 8.0f, "%.2f m");
            ImGui::SetItemTooltip("Where there's geometry ; four times coarser elsewhere. 0 : no probes");
            changed |= ImGui::SliderInt("Rays per probe", &b->probe_rays, 16, 1024);
            ImGui::SeparatorText("Occlusion culling");
            changed |= ImGui::SliderFloat("Cell size", &b->culling_cell, 0.0f, 32.0f, "%.1f m");
            ImGui::SetItemTooltip("0 : no culling data");

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Audio"))
        {
            changed |= ImGui::SliderFloat("Master", &s->master_volume, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Music", &s->music_volume, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Sounds", &s->sound_volume, 0.0f, 1.0f);

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Application"))
        {
            ImGui::InputText("Project name", editor_project.name, sizeof(editor_project.name));
            changed |= ImGui::IsItemDeactivatedAfterEdit();

            ImGui::InputText("Startup scene", s->startup_scene, sizeof(s->startup_scene));
            changed |= ImGui::IsItemDeactivatedAfterEdit();

            const char* dropped = editor_accept_asset(ASSET_SCENE);

            if (dropped)
            {
                SDL_snprintf(s->startup_scene, sizeof(s->startup_scene), "%s", dropped);
                changed = true;
            }

            ImGui::TextDisabled("%s", editor_project.root);

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    if (changed)
    {
        editor_apply_settings();
        editor_save_project();
    }

    ImGui::End();
}
