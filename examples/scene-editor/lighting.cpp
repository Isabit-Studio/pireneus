// Baked lighting in the editor (see bake/bake.h) : the Lighting menu, a bake running in the background
// with its progress, and the result, loaded with the scene (<scene>.bake next to it) : lightmaps for the
// editable meshes, the probe volume, occlusion culling, and the reflection probes.
//
// Reflection probes are captured after the rest, by the renderer itself : one cube face per frame, read
// back, then turned into a prefiltered panorama like the sky's.

#include "editor.h"

#include "bake/bake.h"
#include "meshedit/edit_mesh_gpu.h"
#include "renderer/render3d/half_float.h"
#include "renderer/render3d/render3d.h"

typedef struct
{
    uint32_t key;
    int vertex_count;
    GpuBuffer* uvs;
    Render3DLightmap lightmap;
} LightmapEntry;

static BakeJob* job;
static char job_scene[ASSET_PATH_SIZE];  // the scene being baked

static BakeResult* baked;
static char baked_scene[ASSET_PATH_SIZE]; // the scene `baked` belongs to
static Texture** pages;
static int page_count;
static GpuBuffer* probe_buffer;
static LightmapEntry* entries;
static int entry_count;
static int entry_capacity;
static float culling_point[3];

static Environment* probe_environments;   // one per baked reflection probe
static int probe_environment_count;

// A capture in progress : the probes' objects, the current one and face.
static int capturing;
static int capture_objects[SCENE_MAX_OBJECTS];
static int capture_count;
static int capture_index;
static int capture_face;
static int capture_size;
static int face_rendered;                 // this frame rendered a face : read it back next update
static float* capture_faces[6];           // RGB

static void start_capture(void);
static bool active(void);

static void bake_path(const char* scene_path, char* out, size_t size)
{
    char relative[ASSET_PATH_SIZE];
    SDL_snprintf(relative, sizeof(relative), "%s", scene_path);
    char* dot = SDL_strrchr(relative, '.');

    if (dot)
    {
        *dot = '\0';
    }

    SDL_strlcat(relative, ".bake", sizeof(relative));
    asset_absolute_path(relative, out, size);
}

static void unload(void)
{
    if (!baked)
    {
        return;
    }

    wait_renderer_idle();

    for (int i = 0; i < page_count; i++)
    {
        destroy_texture(pages[i]);
    }

    for (int i = 0; i < entry_count; i++)
    {
        gpu_destroy_buffer(entries[i].uvs);
    }

    if (probe_buffer)
    {
        gpu_destroy_buffer(probe_buffer);
    }

    for (int i = 0; i < probe_environment_count; i++)
    {
        environment_destroy(&probe_environments[i]);
    }

    SDL_free(probe_environments);
    probe_environments = NULL;
    probe_environment_count = 0;

    SDL_free(pages);
    pages = NULL;
    page_count = 0;
    entry_count = 0;
    probe_buffer = NULL;
    render3d_set_probe_volume(NULL);

    bake_free(baked);
    baked = NULL;
    baked_scene[0] = '\0';
}

// The reflection probes' panoramas, prefiltered.
static void load_reflection_probes(void)
{
    for (int i = 0; i < probe_environment_count; i++)
    {
        environment_destroy(&probe_environments[i]);
    }

    SDL_free(probe_environments);
    probe_environment_count = bake_reflection_probe_count(baked);
    probe_environments = (Environment*)SDL_calloc((size_t)SDL_max(1, probe_environment_count), sizeof(Environment));

    for (int i = 0; i < probe_environment_count; i++)
    {
        const BakeReflectionProbe* probe = bake_reflection_probe(baked, i);
        size_t texels = (size_t)probe->width * (size_t)probe->height;
        float* rgb = (float*)SDL_malloc(sizeof(float) * 3 * texels);

        for (size_t t = 0; t < texels; t++)
        {
            for (int c = 0; c < 3; c++)
            {
                rgb[t * 3 + c] = half_to_float(probe->pixels[t * 4 + c]);
            }
        }

        environment_create_from_pixels(rgb, probe->width, probe->height, &probe_environments[i]);
        SDL_free(rgb);
    }
}

// The result's GPU resources : its pages and probes now, objects' UVs when first drawn.
static void use(BakeResult* result, const char* scene_path)
{
    unload();
    baked = result;
    SDL_snprintf(baked_scene, sizeof(baked_scene), "%s", scene_path);

    page_count = bake_page_count(result);
    pages = (Texture**)SDL_calloc((size_t)SDL_max(1, page_count), sizeof(Texture*));

    for (int i = 0; i < page_count; i++)
    {
        GpuTextureDesc desc;
        SDL_zero(desc);
        desc.width = desc.height = bake_page_size(result);
        desc.mip_count = 1;
        desc.format = GPU_FORMAT_RGBA16F;
        desc.usage = GPU_USAGE_SAMPLED;
        desc.mips[0] = bake_page_pixels(result, i);
        pages[i] = gpu_create_texture(&desc);
    }

    load_reflection_probes();

    BakeProbeVolume volume;

    if (bake_probe_volume(result, &volume))
    {
        probe_buffer = gpu_create_buffer(volume.data, volume.size);

        Render3DProbeVolume probes;
        probes.buffer = probe_buffer;
        SDL_memcpy(probes.origin, volume.origin, sizeof(probes.origin));
        probes.cell_size = volume.cell_size;
        SDL_memcpy(probes.cells, volume.cells, sizeof(probes.cells));
        probes.table_offset = volume.table_offset;
        probes.coarse_offset = volume.coarse_offset;
        probes.bricks_offset = volume.bricks_offset;
        render3d_set_probe_volume(&probes);
    }
}

void lighting_scene_loaded(void)
{
    if (baked && SDL_strcmp(baked_scene, editor_scene_path) == 0)
    {
        return;
    }

    unload();

    char path[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    bake_path(editor_scene_path, path, sizeof(path));
    BakeResult* result = bake_load(path);

    if (result)
    {
        use(result, editor_scene_path);
    }
}

// Static enough to bake : editable meshes and models that don't move.
static bool bakeable(const SceneObject* object)
{
    return !object->hidden && !object->is_character && (object->body_type == SCENE_BODY_NONE || object->body_type == SCENE_BODY_STATIC) &&
        (object->mesh || object->model[0]) && !object->is_2d;
}

static void start_bake(void)
{
    if (job || editor_scene.count == 0)
    {
        return;
    }

    const ProjectSettings* settings = &editor_project.settings;
    BakeMesh* meshes = (BakeMesh*)SDL_calloc((size_t)editor_scene.count, sizeof(BakeMesh));
    uint32_t** filtered = (uint32_t**)SDL_calloc((size_t)editor_scene.count, sizeof(uint32_t*));
    BakeLight* lights = (BakeLight*)SDL_calloc((size_t)editor_scene.count + 1, sizeof(BakeLight));
    int mesh_count = 0;
    int light_count = 0;

    for (int i = 0; i < editor_scene.count; i++)
    {
        const SceneObject* object = &editor_scene.objects[i];
        Mat4 matrix = scene_object_matrix(object);

        if (object->light_type != SCENE_LIGHT_NONE && !object->hidden && object->light_mode != SCENE_LIGHT_REALTIME)
        {
            BakeLight* light = &lights[light_count++];
            light->type = object->light_type == SCENE_LIGHT_SPOT ? BAKE_LIGHT_SPOT : BAKE_LIGHT_POINT;
            light->position[0] = matrix.m[3];
            light->position[1] = matrix.m[7];
            light->position[2] = matrix.m[11];
            Vec3 down = vec3_normalize(vec3(-matrix.m[1], -matrix.m[5], -matrix.m[9]));
            light->direction[0] = down.x;
            light->direction[1] = down.y;
            light->direction[2] = down.z;

            for (int c = 0; c < 3; c++)
            {
                light->color[c] = object->light_color[c] * object->light_intensity;
            }

            light->range = object->light_range;
            light->spot_angle = object->light_spot_angle;
            light->spot_softness = object->light_spot_softness;
            light->baked = object->light_mode == SCENE_LIGHT_BAKED;
        }

        if (!bakeable(object))
        {
            continue;
        }

        BakeMesh* mesh = &meshes[mesh_count];
        SDL_memcpy(mesh->model, matrix.m, sizeof(mesh->model));
        mesh->key = bake_key(object->name);
        mesh->occludes = 1;
        mesh->albedo[0] = mesh->albedo[1] = mesh->albedo[2] = 1.0f;

        if (object->override_material)
        {
            SDL_memcpy(mesh->albedo, object->base_color, sizeof(mesh->albedo));
            SDL_memcpy(mesh->emissive, object->emissive, sizeof(mesh->emissive));
        }

        if (object->mesh)
        {
            const EditMeshGpu* gpu = edit_mesh_gpu_get(object->mesh);
            const EditMesh* source = edit_mesh_store_get(object->mesh);

            if (!gpu || !gpu->vertices || !source)
            {
                continue;
            }

            // Tool materials (nodraw, clip) are invisible in the game : they neither block nor bounce light.
            uint32_t* indices = (uint32_t*)SDL_malloc(sizeof(uint32_t) * (size_t)SDL_max(1, gpu->index_count));
            int index_count = 0;

            for (int p = 0; p < gpu->part_count; p++)
            {
                if (!asset_is_tool_material(source->materials[gpu->parts[p].material]))
                {
                    SDL_memcpy(indices + index_count, gpu->indices + gpu->parts[p].first_index, sizeof(uint32_t) * (size_t)gpu->parts[p].index_count);
                    index_count += gpu->parts[p].index_count;
                }
            }

            filtered[mesh_count] = indices;
            mesh->vertices = gpu->vertices;
            mesh->vertex_count = gpu->vertex_count;
            mesh->indices = indices;
            mesh->index_count = index_count;
            mesh->vertex_charts = gpu->vertex_faces;
        }
        else
        {
            const LoadedModel* model = asset_get_model(object->model);

            if (!model)
            {
                continue;
            }

            mesh->vertices = model->data.vertices;
            mesh->vertex_count = model->data.vertex_count;
            mesh->indices = model->data.indices;
            mesh->index_count = model->data.index_count;
        }

        mesh_count++;
    }

    // The sun : mixed, its direct light stays realtime, its bounces are baked.
    float sun_direction[3];
    float sun_color[3];
    editor_sun(sun_direction, sun_color);

    BakeLight* sun = &lights[light_count++];
    sun->type = BAKE_LIGHT_DIRECTIONAL;

    for (int c = 0; c < 3; c++)
    {
        sun->direction[c] = -sun_direction[c];
        sun->color[c] = sun_color[c];
    }

    BakeSettings bake = settings->bake;
    const Environment* environment = editor_environment();

    if (environment)
    {
        SDL_memcpy(bake.sky_sh, environment->sh, sizeof(bake.sky_sh));
    }

    bake.sky_intensity = settings->look.sky_intensity;

    job = bake_start(meshes, mesh_count, lights, light_count, &bake);
    SDL_snprintf(job_scene, sizeof(job_scene), "%s", editor_scene_path);

    for (int i = 0; i < mesh_count; i++)
    {
        SDL_free(filtered[i]);
    }

    SDL_free(filtered);
    SDL_free(meshes);
    SDL_free(lights);

    editor_set_status("Baking '%s'...", editor_scene_path);
}

static void finish_bake(void)
{
    BakeResult* result = bake_finish(job);
    job = NULL;

    if (!result)
    {
        editor_set_status("Baking cancelled.");
        return;
    }

    char path[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    bake_path(job_scene, path, sizeof(path));
    bake_save(result, path);

    if (SDL_strcmp(job_scene, editor_scene_path) == 0)
    {
        use(result, job_scene);
        editor_set_status("Baked '%s' : %d lightmap page%s.", job_scene, bake_page_count(result), bake_page_count(result) == 1 ? "" : "s");
        start_capture(); // the reflection probes, now that the rest of the lighting is there to be seen
    }
    else
    {
        bake_free(result); // the scene changed meanwhile : saved for when it's opened again
        editor_set_status("Baked '%s'.", job_scene);
    }
}

// --- Reflection probe captures -------------------------------------------------------------------

static const float face_directions[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
static const float face_ups[6][3] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };

static Vec3 probe_position(const SceneObject* object)
{
    Mat4 matrix = scene_object_matrix(object);
    return vec3(matrix.m[3], matrix.m[7], matrix.m[11]);
}

static Mat4 face_view_projection(Vec3 eye, int face, Mat4* out_view, Mat4* out_projection)
{
    Vec3 direction = vec3(face_directions[face][0], face_directions[face][1], face_directions[face][2]);
    Vec3 up = vec3(face_ups[face][0], face_ups[face][1], face_ups[face][2]);
    *out_view = mat4_look_at(eye, vec3_add(eye, direction), up);
    *out_projection = mat4_perspective(SDL_PI_F * 0.5f, 1.0f, 0.05f, 2000.0f);
    return mat4_multiply(*out_projection, *out_view);
}

static void start_capture(void)
{
    capture_count = 0;

    for (int i = 0; i < editor_scene.count; i++)
    {
        if (editor_scene.objects[i].reflection_probe && !editor_scene.objects[i].hidden)
        {
            capture_objects[capture_count++] = i;
        }
    }

    if (capture_count == 0)
    {
        return;
    }

    if (!baked)
    {
        baked = bake_empty();
        SDL_snprintf(baked_scene, sizeof(baked_scene), "%s", editor_scene_path);
    }

    capturing = 1;
    capture_index = 0;
    capture_face = 0;
    face_rendered = 0;
    capture_size = SDL_clamp(editor_scene.objects[capture_objects[0]].probe_resolution, 32, 1024);
    editor_set_status("Capturing %d reflection probe%s...", capture_count, capture_count == 1 ? "" : "s");
}

int lighting_capture_render(void)
{
    if (!capturing)
    {
        return 0;
    }

    const SceneObject* object = &editor_scene.objects[capture_objects[capture_index]];
    Render3DCamera camera;
    camera.eye = probe_position(object);
    face_view_projection(camera.eye, capture_face, &camera.view, &camera.projection);
    camera.near_plane = 0.05f;
    camera.far_plane = 2000.0f;

    face_rendered = editor_render_capture(&camera, capture_size);

    if (!face_rendered)
    {
        capturing = 0; // nothing to render with
    }

    return 1;
}

// The six faces into an equirectangular panorama (as panorama_uv() reads it), stored with the bake.
static void store_capture(const SceneObject* object)
{
    int width = SDL_min(capture_size * 4, 1024);
    int height = width / 2;
    Vec3 eye = probe_position(object);
    Mat4 face_matrices[6];

    for (int face = 0; face < 6; face++)
    {
        Mat4 view, projection;
        face_matrices[face] = face_view_projection(eye, face, &view, &projection);
    }

    uint16_t* pixels = (uint16_t*)SDL_malloc(sizeof(uint16_t) * 4 * (size_t)width * (size_t)height);

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            float theta = ((float)y + 0.5f) / (float)height * SDL_PI_F;
            float phi = (((float)x + 0.5f) / (float)width - 0.5f) * 2.0f * SDL_PI_F;
            Vec3 d = vec3(SDL_sinf(theta) * SDL_sinf(phi), SDL_cosf(theta), -SDL_sinf(theta) * SDL_cosf(phi));
            float color[3] = { 0.0f, 0.0f, 0.0f };

            // The face whose frustum it's in : projected, then sampled bilinearly.
            for (int face = 0; face < 6; face++)
            {
                float clip[4];
                mat4_transform(face_matrices[face], eye.x + d.x, eye.y + d.y, eye.z + d.z, 1.0f, clip);

                if (clip[3] <= 0.0f || SDL_fabsf(clip[0]) > clip[3] || SDL_fabsf(clip[1]) > clip[3])
                {
                    continue;
                }

                float fx = (clip[0] / clip[3] * 0.5f + 0.5f) * (float)capture_size - 0.5f;
                float fy = (0.5f - clip[1] / clip[3] * 0.5f) * (float)capture_size - 0.5f;
                int x0 = SDL_clamp((int)SDL_floorf(fx), 0, capture_size - 1);
                int y0 = SDL_clamp((int)SDL_floorf(fy), 0, capture_size - 1);
                int x1 = SDL_min(x0 + 1, capture_size - 1);
                int y1 = SDL_min(y0 + 1, capture_size - 1);
                float tx = SDL_clamp(fx - (float)x0, 0.0f, 1.0f);
                float ty = SDL_clamp(fy - (float)y0, 0.0f, 1.0f);
                const float* image = capture_faces[face];

                for (int c = 0; c < 3; c++)
                {
                    float a = image[(y0 * capture_size + x0) * 3 + c] * (1.0f - tx) + image[(y0 * capture_size + x1) * 3 + c] * tx;
                    float b = image[(y1 * capture_size + x0) * 3 + c] * (1.0f - tx) + image[(y1 * capture_size + x1) * 3 + c] * tx;
                    color[c] = a * (1.0f - ty) + b * ty;
                }

                break;
            }

            uint16_t* out = &pixels[((size_t)y * (size_t)width + (size_t)x) * 4];
            out[0] = float_to_half(color[0]);
            out[1] = float_to_half(color[1]);
            out[2] = float_to_half(color[2]);
            out[3] = float_to_half(1.0f);
        }
    }

    BakeReflectionProbe probe;
    SDL_zero(probe);
    probe.key = bake_key(object->name);

    for (int c = 0; c < 3; c++)
    {
        probe.position[c] = (&eye.x)[c];
        probe.box_min[c] = (&eye.x)[c] - object->probe_box[c] * 0.5f;
        probe.box_max[c] = (&eye.x)[c] + object->probe_box[c] * 0.5f;
    }

    probe.blend = object->probe_blend;
    probe.width = width;
    probe.height = height;
    probe.pixels = pixels;
    bake_set_reflection_probe(baked, &probe);
    SDL_free(pixels);
}

// After a frame that rendered a face : read back, and on to the next.
static void continue_capture(void)
{
    if (!face_rendered)
    {
        return;
    }

    face_rendered = 0;

    Texture* image = render3d_hdr_image();
    int w = 0, h = 0;

    if (image)
    {
        texture_get_size(image, &w, &h);
    }

    size_t texels = (size_t)capture_size * (size_t)capture_size;
    uint16_t* halves = (uint16_t*)SDL_malloc(sizeof(uint16_t) * 4 * texels);

    if (!image || w != capture_size || h != capture_size || !gpu_read_texture(image, halves, sizeof(uint16_t) * 4 * texels))
    {
        SDL_free(halves);
        capturing = 0;
        editor_set_status("Could not read a reflection probe's capture back.");
        return;
    }

    capture_faces[capture_face] = (float*)SDL_realloc(capture_faces[capture_face], sizeof(float) * 3 * texels);

    for (size_t t = 0; t < texels; t++)
    {
        for (int c = 0; c < 3; c++)
        {
            capture_faces[capture_face][t * 3 + c] = half_to_float(halves[t * 4 + c]);
        }
    }

    SDL_free(halves);

    if (++capture_face < 6)
    {
        return;
    }

    store_capture(&editor_scene.objects[capture_objects[capture_index]]);
    capture_face = 0;

    if (++capture_index < capture_count)
    {
        capture_size = SDL_clamp(editor_scene.objects[capture_objects[capture_index]].probe_resolution, 32, 1024);
        return;
    }

    // Every probe captured : saved with the rest, and in use.
    capturing = 0;

    char path[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
    bake_path(baked_scene, path, sizeof(path));
    bake_save(baked, path);
    load_reflection_probes();
    editor_set_status("Captured %d reflection probe%s.", capture_count, capture_count == 1 ? "" : "s");
}

void lighting_add_reflection_probes(void)
{
    if (!active() || capturing)
    {
        return;
    }

    for (int i = 0; i < probe_environment_count && i < bake_reflection_probe_count(baked); i++)
    {
        const BakeReflectionProbe* source = bake_reflection_probe(baked, i);

        Render3DReflectionProbe probe;
        probe.environment = &probe_environments[i];
        SDL_memcpy(probe.position, source->position, sizeof(probe.position));
        SDL_memcpy(probe.box_min, source->box_min, sizeof(probe.box_min));
        SDL_memcpy(probe.box_max, source->box_max, sizeof(probe.box_max));
        probe.blend = source->blend;
        render3d_add_reflection_probe(&probe);
    }
}

void lighting_update(void)
{
    if (capturing)
    {
        continue_capture();
        return;
    }

    if (!job)
    {
        return;
    }

    if (bake_done(job))
    {
        finish_bake();
        return;
    }

    const char* stage = "";
    float progress = bake_progress(job, &stage);

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 80.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f));

    if (ImGui::Begin("Baking", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking))
    {
        ImGui::Text("%s : %s", job_scene, stage);
        char label[32];
        SDL_snprintf(label, sizeof(label), "%d %%", (int)(progress * 100.0f));
        ImGui::ProgressBar(progress, ImVec2(-1.0f, 0.0f), label);

        if (ImGui::Button("Cancel", ImVec2(-1.0f, 0.0f)))
        {
            bake_cancel(job);
        }
    }

    ImGui::End();
}

void lighting_draw_menu(void)
{
    ProjectSettings* settings = &editor_project.settings;

    if (ImGui::MenuItem("Bake lighting", NULL, false, !job && !capturing && !editor_is_playing()))
    {
        start_bake();
    }

    ImGui::SetItemTooltip("Lightmaps for the editable meshes, the probe volume, occlusion culling, then the reflection probes");

    if (ImGui::MenuItem("Capture reflection probes", NULL, false, !job && !capturing && !editor_is_playing()))
    {
        start_capture();
    }

    if (ImGui::MenuItem("Clear baked lighting", NULL, false, !job && baked != NULL))
    {
        char path[PROJECT_PATH_SIZE + ASSET_PATH_SIZE];
        bake_path(editor_scene_path, path, sizeof(path));
        unload();
        SDL_RemovePath(path);
        editor_set_status("Baked lighting cleared.");
    }

    ImGui::Separator();

    bool use_baked = settings->use_baked_lighting != 0;

    if (ImGui::MenuItem("Use baked lighting", NULL, &use_baked))
    {
        settings->use_baked_lighting = use_baked;
        editor_save_project();
    }

    bool culling = settings->occlusion_culling != 0;

    if (ImGui::MenuItem("Occlusion culling", NULL, &culling))
    {
        settings->occlusion_culling = culling;
        editor_save_project();
    }

    ImGui::Separator();
    ImGui::TextDisabled(baked ? "Baked : %d lightmap page(s)" : "Not baked", baked ? bake_page_count(baked) : 0);
}

static bool active(void)
{
    return baked && editor_project.settings.use_baked_lighting && SDL_strcmp(baked_scene, editor_scene_path) == 0;
}

void lighting_prepare_frame(Vec3 eye)
{
    culling_point[0] = eye.x;
    culling_point[1] = eye.y;
    culling_point[2] = eye.z;

    BakeProbeVolume volume;

    if (!active() || !bake_probe_volume(baked, &volume) || !probe_buffer)
    {
        render3d_set_probe_volume(NULL);
        return;
    }

    Render3DProbeVolume probes;
    probes.buffer = probe_buffer;
    SDL_memcpy(probes.origin, volume.origin, sizeof(probes.origin));
    probes.cell_size = volume.cell_size;
    SDL_memcpy(probes.cells, volume.cells, sizeof(probes.cells));
    probes.table_offset = volume.table_offset;
    probes.coarse_offset = volume.coarse_offset;
    probes.bricks_offset = volume.bricks_offset;
    render3d_set_probe_volume(&probes);
}

int lighting_light_baked_out(const SceneObject* object)
{
    return active() && object->light_mode == SCENE_LIGHT_BAKED;
}

static const Render3DLightmap* find_lightmap(uint32_t key, int vertex_count)
{
    for (int i = 0; i < entry_count; i++)
    {
        if (entries[i].key == key && entries[i].vertex_count == vertex_count)
        {
            return entries[i].uvs ? &entries[i].lightmap : NULL;
        }
    }

    // First drawn since the bake was loaded : its UVs go to the GPU (or a mark that it has none).
    int page = 0;
    const float* uvs = NULL;

    if (entry_count == entry_capacity)
    {
        entry_capacity = SDL_max(64, entry_capacity * 2);
        entries = (LightmapEntry*)SDL_realloc(entries, sizeof(LightmapEntry) * (size_t)entry_capacity);
    }

    LightmapEntry* entry = &entries[entry_count++];
    SDL_zerop(entry);
    entry->key = key;
    entry->vertex_count = vertex_count;

    if (bake_object_lightmap(baked, key, vertex_count, &page, &uvs) && page < page_count && pages[page])
    {
        entry->uvs = gpu_create_buffer(uvs, sizeof(float) * 2 * (size_t)vertex_count);
        entry->lightmap.texture = pages[page];
        entry->lightmap.uvs = entry->uvs;
        entry->lightmap.scale_offset[0] = 1.0f;
        entry->lightmap.scale_offset[1] = 1.0f;
    }

    return entry->uvs ? &entry->lightmap : NULL;
}

Render3DDrawOptions lighting_draw_options(const SceneObject* object, int vertex_count)
{
    Render3DDrawOptions options;
    SDL_zero(options);

    if (!active() || !bakeable(object))
    {
        return options;
    }

    uint32_t key = bake_key(object->name);

    if (object->mesh)
    {
        options.lightmap = find_lightmap(key, vertex_count);
    }

    options.hidden_from_view = editor_project.settings.occlusion_culling && !bake_object_visible(baked, key, culling_point);

    return options;
}

int lighting_is_baking(void)
{
    return job != NULL || capturing;
}

void lighting_shutdown(void)
{
    if (job)
    {
        bake_cancel(job);
        bake_free(bake_finish(job));
        job = NULL;
    }

    capturing = 0;
    unload();
    SDL_free(entries);
    entries = NULL;
    entry_capacity = 0;

    for (int face = 0; face < 6; face++)
    {
        SDL_free(capture_faces[face]);
        capture_faces[face] = NULL;
    }
}
