// The editor's profiler, toggled from the "Profiler" menu :
// - a simple FPS counter, in a corner of the 3D view ;
// - a detailed panel : frame time and its CPU breakdown, GPU time, process CPU usage, RAM and VRAM,
//   draw counts, with graphs of the last frames.
//
// CPU times are measured around the editor's update() (logic and UI) and render() (recording GPU
// commands) ; "other" is the rest of the frame interval : ImGui's own rendering, events, and waiting
// for the display (vertical sync). The GPU time comes from the renderer (renderer_get_stats()).

#include <stdarg.h>

#include "editor.h"

#include "platforms/platforms_system.h"

#define HISTORY_SIZE 240           // frames kept for the graphs and the min / max
#define AVERAGE_PERIOD 0.5         // seconds over which the displayed FPS is averaged
#define PROCESS_STATS_PERIOD 0.5   // seconds between two reads of the process statistics

bool profiler_show_fps;
bool profiler_show_details;

static Uint64 frame_start;         // performance counter at the start of the current frame
static Uint64 phase_start;
static float update_ms;            // of the current frame, until it's pushed to the history
static float render_ms;

static float history_frame[HISTORY_SIZE];
static float history_update[HISTORY_SIZE];
static float history_render[HISTORY_SIZE];
static float history_gpu[HISTORY_SIZE];
static int history_next;
static int history_count;

// The displayed averages, refreshed every AVERAGE_PERIOD so they can be read.
static double average_elapsed;
static int average_frames;
static double average_sum[4];      // frame, update, render, gpu (gpu : over the frames that had one)
static int average_gpu_frames;
static float shown_fps;
static float shown_frame_ms;
static float shown_update_ms;
static float shown_render_ms;
static float shown_gpu_ms = -1.0f;

static ProcessStats process;
static bool process_valid;
static double process_elapsed = PROCESS_STATS_PERIOD;
static double last_cpu_seconds = -1.0;
static float cpu_usage;            // percent of the whole machine

static RendererStats renderer_stats;

static double seconds_between(Uint64 from, Uint64 to)
{
    return (double)(to - from) / (double)SDL_GetPerformanceFrequency();
}

static void push_frame(float frame_ms, float gpu_ms)
{
    history_frame[history_next] = frame_ms;
    history_update[history_next] = update_ms;
    history_render[history_next] = render_ms;
    history_gpu[history_next] = gpu_ms;
    history_next = (history_next + 1) % HISTORY_SIZE;
    history_count = SDL_min(history_count + 1, HISTORY_SIZE);

    average_elapsed += frame_ms * 0.001;
    average_frames++;
    average_sum[0] += frame_ms;
    average_sum[1] += update_ms;
    average_sum[2] += render_ms;

    if (gpu_ms >= 0.0f)
    {
        average_sum[3] += gpu_ms;
        average_gpu_frames++;
    }

    if (average_elapsed >= AVERAGE_PERIOD)
    {
        shown_fps = (float)(average_frames / average_elapsed);
        shown_frame_ms = (float)(average_sum[0] / average_frames);
        shown_update_ms = (float)(average_sum[1] / average_frames);
        shown_render_ms = (float)(average_sum[2] / average_frames);
        shown_gpu_ms = average_gpu_frames > 0 ? (float)(average_sum[3] / average_gpu_frames) : -1.0f;

        average_elapsed = 0.0;
        average_frames = 0;
        average_gpu_frames = 0;
        SDL_zeroa(average_sum);
    }

    // The process' statistics are system calls : a few times per second is plenty.
    process_elapsed += frame_ms * 0.001;

    if (process_elapsed >= PROCESS_STATS_PERIOD)
    {
        process_valid = platform_process_stats(&process) != 0;

        if (process_valid && last_cpu_seconds >= 0.0)
        {
            int cores = SDL_max(1, SDL_GetNumLogicalCPUCores());
            cpu_usage = (float)((process.cpu_seconds - last_cpu_seconds) / process_elapsed / cores * 100.0);
        }

        last_cpu_seconds = process_valid ? process.cpu_seconds : -1.0;
        process_elapsed = 0.0;
    }
}

void profiler_frame_begin(void)
{
    Uint64 now = SDL_GetPerformanceCounter();

    if (frame_start != 0)
    {
        renderer_get_stats(&renderer_stats);
        push_frame((float)(seconds_between(frame_start, now) * 1000.0), renderer_stats.gpu_frame_ms);
    }

    frame_start = now;
    phase_start = now;
    update_ms = 0.0f;
    render_ms = 0.0f;
}

void profiler_update_end(void)
{
    update_ms = (float)(seconds_between(phase_start, SDL_GetPerformanceCounter()) * 1000.0);
}

void profiler_render_begin(void)
{
    phase_start = SDL_GetPerformanceCounter();
}

void profiler_render_end(void)
{
    render_ms = (float)(seconds_between(phase_start, SDL_GetPerformanceCounter()) * 1000.0);
}

void profiler_draw_menu(void)
{
    ImGui::MenuItem("FPS counter", NULL, &profiler_show_fps);
    ImGui::MenuItem("Detailed statistics", NULL, &profiler_show_details);
}

// --- Drawing ---------------------------------------------------------------------------------------

static ImVec4 fps_color(float fps)
{
    return fps >= 55.0f ? ImVec4(0.45f, 0.90f, 0.45f, 1.0f) : fps >= 30.0f ? ImVec4(0.95f, 0.80f, 0.30f, 1.0f) : ImVec4(1.0f, 0.40f, 0.35f, 1.0f);
}

static void format_bytes(char* out, size_t out_size, unsigned long long bytes)
{
    if (bytes >= 1024ull * 1024ull * 1024ull)
    {
        SDL_snprintf(out, out_size, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
    else
    {
        SDL_snprintf(out, out_size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    }
}

// The history in chronological order, oldest first, for ImGui's plots.
static float history_value(void* data, int index)
{
    const float* values = (const float*)data;
    int start = (history_next - history_count + HISTORY_SIZE) % HISTORY_SIZE;
    return SDL_max(0.0f, values[(start + index) % HISTORY_SIZE]);
}

static void history_range(const float* values, float* out_min, float* out_max)
{
    *out_min = 0.0f;
    *out_max = 0.0f;
    int start = (history_next - history_count + HISTORY_SIZE) % HISTORY_SIZE;

    for (int i = 0; i < history_count; i++)
    {
        float value = values[(start + i) % HISTORY_SIZE];

        if (i == 0 || value < *out_min) *out_min = value;
        if (i == 0 || value > *out_max) *out_max = value;
    }
}

static void plot(const char* id, const float* values, const char* overlay, float scale_max)
{
    ImGui::PlotLines(id, history_value, (void*)values, history_count, 0, overlay, 0.0f, scale_max,
        ImVec2(ImGui::GetContentRegionAvail().x, 60.0f));
}

static void draw_fps_counter(void)
{
    ImVec2 view_min;
    ImVec2 view_size;
    editor_view_rect(&view_min, &view_size);

    ImGui::SetNextWindowPos(ImVec2(view_min.x + view_size.x - 10.0f, view_min.y + 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##fps_counter", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
    ImGui::TextColored(fps_color(shown_fps), "%.0f FPS", shown_fps);
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f ms", shown_frame_ms);
    ImGui::End();
}

static void stat_row(const char* label, const char* format, ...) SDL_PRINTF_VARARG_FUNC(2);

static void stat_row(const char* label, const char* format, ...)
{
    char value[160];
    va_list arguments;
    va_start(arguments, format);
    SDL_vsnprintf(value, sizeof(value), format, arguments);
    va_end(arguments);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value);
}

static bool begin_stats_table(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

static void draw_details(void)
{
    ImGui::SetNextWindowPos(ImVec2(420.0f, 60.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(470.0f, 700.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Profiler", &profiler_show_details))
    {
        ImGui::End();
        return;
    }

    float frame_min, frame_max, gpu_min, gpu_max, unused;
    history_range(history_frame, &frame_min, &frame_max);
    history_range(history_gpu, &gpu_min, &gpu_max);
    char overlay[64];

    // --- Frame ---
    ImGui::SeparatorText("Frame");
    ImGui::TextColored(fps_color(shown_fps), "%.0f FPS", shown_fps);
    ImGui::SameLine();
    ImGui::Text("  %.2f ms", shown_frame_ms);

    if (begin_stats_table("frame"))
    {
        stat_row("Min / max", "%.2f / %.2f ms (last %d frames)", frame_min, frame_max, history_count);
        ImGui::EndTable();
    }

    SDL_snprintf(overlay, sizeof(overlay), "frame time, max %.1f ms", frame_max);
    plot("##frame", history_frame, overlay, SDL_max(frame_max * 1.1f, 20.0f));

    // --- CPU ---
    ImGui::SeparatorText("CPU");
    float other_ms = SDL_max(0.0f, shown_frame_ms - shown_update_ms - shown_render_ms);

    if (begin_stats_table("cpu"))
    {
        stat_row("Update", "%.2f ms  (logic, physics, UI)", shown_update_ms);
        stat_row("Render", "%.2f ms  (GPU commands)", shown_render_ms);
        stat_row("Other", "%.2f ms  (UI drawing, events, vsync)", other_ms);
        if (process_valid)
        {
            int cores = SDL_GetNumLogicalCPUCores();
            stat_row("Process usage", "%.1f %% of %d threads (%.0f %% of one)", cpu_usage, cores, cpu_usage * (float)cores);
        }
        else
        {
            stat_row("Process usage", "n/a");
        }
        ImGui::EndTable();
    }

    history_range(history_update, &unused, &frame_max);
    SDL_snprintf(overlay, sizeof(overlay), "update, max %.1f ms", frame_max);
    plot("##update", history_update, overlay, SDL_max(frame_max * 1.1f, 5.0f));

    // --- GPU ---
    ImGui::SeparatorText("GPU");

    if (begin_stats_table("gpu"))
    {
        stat_row("Device", "%s", renderer_stats.gpu_name[0] ? renderer_stats.gpu_name : "unknown");
        stat_row("Backend", "%s", renderer_stats.backend ? renderer_stats.backend : "unknown");

        if (shown_gpu_ms >= 0.0f)
        {
            stat_row("Frame", "%.2f ms   (min %.2f, max %.2f)", shown_gpu_ms, gpu_min, gpu_max);
        }
        else
        {
            stat_row("Frame", "n/a (no GPU timestamps)");
        }

        ImGui::EndTable();
    }

    if (shown_gpu_ms >= 0.0f)
    {
        SDL_snprintf(overlay, sizeof(overlay), "GPU time, max %.1f ms", gpu_max);
        plot("##gpu", history_gpu, overlay, SDL_max(gpu_max * 1.1f, 5.0f));
    }

    ImGui::TextDisabled("Measured around the frame's command list : synchronous\npasses (shadow maps, render targets) aren't included.");

    // --- Memory ---
    ImGui::SeparatorText("Memory");
    char a[32];
    char b[32];

    if (begin_stats_table("memory"))
    {
        if (process_valid)
        {
            format_bytes(a, sizeof(a), process.resident_bytes);
            stat_row("RAM (in use)", "%s", a);

            if (process.private_bytes)
            {
                format_bytes(a, sizeof(a), process.private_bytes);
                stat_row("RAM (committed)", "%s", a);
            }
        }
        else
        {
            stat_row("RAM", "n/a");
        }

        format_bytes(a, sizeof(a), (unsigned long long)SDL_GetSystemRAM() * 1024ull * 1024ull);
        stat_row("System RAM", "%s", a);

        if (renderer_stats.vram_used || renderer_stats.vram_budget)
        {
            format_bytes(a, sizeof(a), renderer_stats.vram_used);
            format_bytes(b, sizeof(b), renderer_stats.vram_budget);
            stat_row("VRAM", "%s / %s", a, b);
        }
        else
        {
            stat_row("VRAM", "n/a");
        }

        ImGui::EndTable();
    }

    if (renderer_stats.vram_budget)
    {
        float fraction = (float)((double)renderer_stats.vram_used / (double)renderer_stats.vram_budget);
        SDL_snprintf(overlay, sizeof(overlay), "VRAM %.0f %% of the budget", fraction * 100.0f);
        ImGui::ProgressBar(SDL_min(fraction, 1.0f), ImVec2(-1.0f, 0.0f), overlay);
    }

    // --- Rendering ---
    ImGui::SeparatorText("Rendering");

    if (begin_stats_table("rendering"))
    {
        int width = 0;
        int height = 0;
        get_window_size(&width, &height);

        stat_row("Draw calls", "%d", renderer_stats.draw_calls);
        stat_row("Mesh draws", "%d", renderer_stats.mesh_draws);
        stat_row("2D / UI vertices", "%d", renderer_stats.vertices_2d);
        stat_row("Scene objects", "%d", editor_scene.count);
        stat_row("Resolution", "%d x %d", width, height);
        ImGui::EndTable();
    }

    ImGui::End();
}

void profiler_draw(void)
{
    if (profiler_show_fps)
    {
        draw_fps_counter();
    }

    if (profiler_show_details)
    {
        draw_details();
    }
}
