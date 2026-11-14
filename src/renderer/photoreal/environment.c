// The environment of the photorealistic renderer, computed on the CPU : the irradiance's spherical
// harmonics, a GGX prefiltered mip chain of the panorama, and the sun (found in an HDR panorama and
// removed from it, or placed in a procedural Nishita sky).

#include "renderer/photoreal/environment.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <math.h>

#include "assets/hdr_image.h"
#include "renderer/gpu.h"
#include "renderer/photoreal/half_float.h"
#include "debug.h"

#define PANORAMA_MAX_WIDTH 2048
#define PREFILTER_WIDTH 512
#define PREFILTER_MIPS 6
#define PREFILTER_SAMPLES 96
#define SH_WIDTH 256
#define PI_F 3.14159265f

// Panoramas : U around the horizon (-Z at its center), V from straight up to straight down, as
// panorama_uv() in photoreal.slang.
static void panorama_direction(float u, float v, float out[3])
{
    float theta = v * PI_F;
    float phi = (u - 0.5f) * 2.0f * PI_F;
    float sin_theta = sinf(theta);
    out[0] = sin_theta * sinf(phi);
    out[1] = cosf(theta);
    out[2] = -sin_theta * cosf(phi);
}

// --- Parallel loops -----------------------------------------------------------------------------

typedef void (*RowFunction)(int row, void* context);

typedef struct
{
    RowFunction function;
    void* context;
    SDL_AtomicInt next_row;
    int row_count;
} ParallelJob;

static int parallel_worker(void* data)
{
    ParallelJob* job = data;

    for (;;)
    {
        int row = SDL_AddAtomicInt(&job->next_row, 1);

        if (row >= job->row_count)
        {
            return 0;
        }

        job->function(row, job->context);
    }
}

static void parallel_rows(int row_count, RowFunction function, void* context)
{
    ParallelJob job;
    job.function = function;
    job.context = context;
    job.row_count = row_count;
    SDL_SetAtomicInt(&job.next_row, 0);

    SDL_Thread* threads[32];
    int thread_count = SDL_clamp(SDL_GetNumLogicalCPUCores() - 1, 0, 32);
    int started = 0;

    for (int i = 0; i < thread_count; i++)
    {
        threads[started] = SDL_CreateThread(parallel_worker, "environment", &job);

        if (threads[started])
        {
            started++;
        }
    }

    parallel_worker(&job);

    for (int i = 0; i < started; i++)
    {
        SDL_WaitThread(threads[i], NULL);
    }
}

// --- Images -------------------------------------------------------------------------------------

typedef struct
{
    float* pixels; // RGB
    int width;
    int height;
} Image;

static Image image_alloc(int width, int height)
{
    Image image;
    image.width = width;
    image.height = height;
    image.pixels = SDL_malloc(sizeof(float) * 3 * (size_t)width * (size_t)height);
    return image;
}

// Box filter down to `width` x `height`, wrapping around horizontally.
static Image image_resize(const Image* source, int width, int height)
{
    Image out = image_alloc(width, height);

    for (int y = 0; y < height; y++)
    {
        int y0 = y * source->height / height;
        int y1 = SDL_max(y0 + 1, (y + 1) * source->height / height);

        for (int x = 0; x < width; x++)
        {
            int x0 = x * source->width / width;
            int x1 = SDL_max(x0 + 1, (x + 1) * source->width / width);
            float sum[3] = { 0.0f, 0.0f, 0.0f };

            for (int sy = y0; sy < y1; sy++)
            {
                for (int sx = x0; sx < x1; sx++)
                {
                    const float* p = &source->pixels[((size_t)sy * (size_t)source->width + (size_t)sx) * 3];
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }

            float scale = 1.0f / (float)((y1 - y0) * (x1 - x0));
            float* out_pixel = &out.pixels[((size_t)y * (size_t)width + (size_t)x) * 3];
            out_pixel[0] = sum[0] * scale;
            out_pixel[1] = sum[1] * scale;
            out_pixel[2] = sum[2] * scale;
        }
    }

    return out;
}

static void image_sample(const Image* image, float u, float v, float out[3])
{
    u -= floorf(u);
    float fx = u * (float)image->width - 0.5f;
    float fy = SDL_clamp(v, 0.0f, 1.0f) * (float)image->height - 0.5f;
    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;

    int xa = (x0 + image->width) % image->width;
    int xb = (x0 + 1 + image->width) % image->width;
    int ya = SDL_clamp(y0, 0, image->height - 1);
    int yb = SDL_clamp(y0 + 1, 0, image->height - 1);

    const float* p00 = &image->pixels[((size_t)ya * (size_t)image->width + (size_t)xa) * 3];
    const float* p10 = &image->pixels[((size_t)ya * (size_t)image->width + (size_t)xb) * 3];
    const float* p01 = &image->pixels[((size_t)yb * (size_t)image->width + (size_t)xa) * 3];
    const float* p11 = &image->pixels[((size_t)yb * (size_t)image->width + (size_t)xb) * 3];

    for (int c = 0; c < 3; c++)
    {
        float top = p00[c] + (p10[c] - p00[c]) * tx;
        float bottom = p01[c] + (p11[c] - p01[c]) * tx;
        out[c] = top + (bottom - top) * ty;
    }
}

static uint16_t* image_to_rgba16f(const Image* image)
{
    size_t count = (size_t)image->width * (size_t)image->height;
    uint16_t* out = SDL_malloc(sizeof(uint16_t) * 4 * count);
    uint16_t one = float_to_half(1.0f);

    for (size_t i = 0; i < count; i++)
    {
        out[i * 4 + 0] = float_to_half(image->pixels[i * 3 + 0]);
        out[i * 4 + 1] = float_to_half(image->pixels[i * 3 + 1]);
        out[i * 4 + 2] = float_to_half(image->pixels[i * 3 + 2]);
        out[i * 4 + 3] = one;
    }

    return out;
}

// --- Diffuse : spherical harmonics --------------------------------------------------------------

static void project_sh(const Image* image, float out_sh[9][3], float* out_average)
{
    double sh[9][3] = { { 0.0 } };
    double weight_sum = 0.0;
    double luminance_sum = 0.0;

    for (int y = 0; y < image->height; y++)
    {
        float v = ((float)y + 0.5f) / (float)image->height;
        float solid_angle = sinf(v * PI_F);

        for (int x = 0; x < image->width; x++)
        {
            float d[3];
            panorama_direction(((float)x + 0.5f) / (float)image->width, v, d);

            const float basis[9] = {
                0.282095f,
                0.488603f * d[1], 0.488603f * d[2], 0.488603f * d[0],
                1.092548f * d[0] * d[1], 1.092548f * d[1] * d[2],
                0.315392f * (3.0f * d[2] * d[2] - 1.0f),
                1.092548f * d[0] * d[2],
                0.546274f * (d[0] * d[0] - d[1] * d[1])
            };

            const float* p = &image->pixels[((size_t)y * (size_t)image->width + (size_t)x) * 3];

            for (int i = 0; i < 9; i++)
            {
                for (int c = 0; c < 3; c++)
                {
                    sh[i][c] += (double)p[c] * basis[i] * solid_angle;
                }
            }

            luminance_sum += (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) * solid_angle;
            weight_sum += solid_angle;
        }
    }

    // Integrated over the sphere (4 PI / weight_sum), then Ramamoorthi's clamped-cosine convolution
    // (PI, 2 PI / 3, PI / 4 per band) divided by PI : irradiance / PI, ready to multiply an albedo.
    double scale = 4.0 * (double)PI_F / weight_sum;
    static const double band[9] = { 1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25 };

    for (int i = 0; i < 9; i++)
    {
        for (int c = 0; c < 3; c++)
        {
            out_sh[i][c] = (float)(sh[i][c] * scale * band[i]);
        }
    }

    *out_average = (float)(luminance_sum / weight_sum);
}

// --- Specular : GGX prefiltering ----------------------------------------------------------------

static void hammersley(uint32_t i, uint32_t n, float out[2])
{
    uint32_t bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    out[0] = (float)i / (float)n;
    out[1] = (float)bits * 2.3283064365386963e-10f;
}

typedef struct
{
    const Image* pyramid; // source mip chain
    int pyramid_levels;
    Image* out;
    float roughness;
} PrefilterJob;

static void sample_pyramid(const Image* pyramid, int levels, float u, float v, float lod, float out[3])
{
    lod = SDL_clamp(lod, 0.0f, (float)(levels - 1));
    int level = (int)lod;
    float t = lod - (float)level;

    image_sample(&pyramid[level], u, v, out);

    if (t > 0.0f && level + 1 < levels)
    {
        float next[3];
        image_sample(&pyramid[level + 1], u, v, next);

        for (int c = 0; c < 3; c++)
        {
            out[c] += (next[c] - out[c]) * t;
        }
    }
}

static void prefilter_row(int y, void* context)
{
    PrefilterJob* job = context;
    Image* out = job->out;
    float a = job->roughness * job->roughness;
    float a2 = a * a;
    float source_texels = (float)job->pyramid[0].width * (float)job->pyramid[0].height;

    float v = ((float)y + 0.5f) / (float)out->height;

    for (int x = 0; x < out->width; x++)
    {
        float n[3];
        panorama_direction(((float)x + 0.5f) / (float)out->width, v, n);

        // A basis around the normal.
        float up[3] = { fabsf(n[1]) < 0.999f ? 0.0f : 1.0f, fabsf(n[1]) < 0.999f ? 1.0f : 0.0f, 0.0f };
        float t[3] = { up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0] };
        float t_length = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        t[0] /= t_length;
        t[1] /= t_length;
        t[2] /= t_length;
        float b[3] = { n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0] };

        float sum[3] = { 0.0f, 0.0f, 0.0f };
        float weight = 0.0f;

        for (uint32_t i = 0; i < PREFILTER_SAMPLES; i++)
        {
            float xi[2];
            hammersley(i, PREFILTER_SAMPLES, xi);

            // GGX importance sampling of the half vector, with V = N = R (Karis).
            float phi = 2.0f * PI_F * xi[0];
            float cos_theta = sqrtf((1.0f - xi[1]) / (1.0f + (a2 - 1.0f) * xi[1]));
            float sin_theta = sqrtf(SDL_max(0.0f, 1.0f - cos_theta * cos_theta));
            float hx = sin_theta * cosf(phi);
            float hy = sin_theta * sinf(phi);

            float h[3] = {
                t[0] * hx + b[0] * hy + n[0] * cos_theta,
                t[1] * hx + b[1] * hy + n[1] * cos_theta,
                t[2] * hx + b[2] * hy + n[2] * cos_theta
            };

            float n_dot_h = n[0] * h[0] + n[1] * h[1] + n[2] * h[2];
            float l[3] = { 2.0f * n_dot_h * h[0] - n[0], 2.0f * n_dot_h * h[1] - n[1], 2.0f * n_dot_h * h[2] - n[2] };
            float n_dot_l = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];

            if (n_dot_l <= 0.0f)
            {
                continue;
            }

            // Filtered importance sampling (Krivanek & Colbert) : a coarser source where samples are sparse.
            float d = (n_dot_h * a2 - n_dot_h) * n_dot_h + 1.0f;
            float pdf = a2 / (PI_F * d * d) * 0.25f;
            float omega_s = 1.0f / ((float)PREFILTER_SAMPLES * SDL_max(pdf, 1e-6f));
            float omega_p = 4.0f * PI_F / source_texels;
            float lod = 0.5f * log2f(SDL_max(omega_s / omega_p, 1.0f)) + 0.5f;

            float u = 0.5f + atan2f(l[0], -l[2]) / (2.0f * PI_F);
            float vv = acosf(SDL_clamp(l[1], -1.0f, 1.0f)) / PI_F;

            float color[3];
            sample_pyramid(job->pyramid, job->pyramid_levels, u, vv, lod, color);

            sum[0] += color[0] * n_dot_l;
            sum[1] += color[1] * n_dot_l;
            sum[2] += color[2] * n_dot_l;
            weight += n_dot_l;
        }

        float* pixel = &out->pixels[((size_t)y * (size_t)out->width + (size_t)x) * 3];
        float scale = weight > 0.0f ? 1.0f / weight : 0.0f;
        pixel[0] = sum[0] * scale;
        pixel[1] = sum[1] * scale;
        pixel[2] = sum[2] * scale;
    }
}

// --- Assembly -----------------------------------------------------------------------------------

// `lighting` is the panorama used for image-based lighting (without the sun's disk), `display` the one
// drawn as the background.
static int build(const Image* display, const Image* lighting, Environment* environment)
{
    GpuTextureDesc panorama_desc;
    SDL_zero(panorama_desc);
    panorama_desc.width = display->width;
    panorama_desc.height = display->height;
    panorama_desc.mip_count = 1;
    panorama_desc.format = GPU_FORMAT_RGBA16F;
    panorama_desc.usage = GPU_USAGE_SAMPLED;

    uint16_t* panorama_pixels = image_to_rgba16f(display);
    panorama_desc.mips[0] = panorama_pixels;
    environment->panorama = gpu_create_texture(&panorama_desc);
    SDL_free(panorama_pixels);

    if (!environment->panorama)
    {
        return 0;
    }

    // The source pyramid for filtered importance sampling.
    Image pyramid[10];
    int pyramid_levels = 0;

    pyramid[0] = image_resize(lighting, PREFILTER_WIDTH, PREFILTER_WIDTH / 2);
    pyramid_levels = 1;

    while (pyramid_levels < 8 && pyramid[pyramid_levels - 1].height > 2)
    {
        const Image* previous = &pyramid[pyramid_levels - 1];
        pyramid[pyramid_levels] = image_resize(previous, previous->width / 2, previous->height / 2);
        pyramid_levels++;
    }

    Image sh_image = image_resize(lighting, SH_WIDTH, SH_WIDTH / 2);
    project_sh(&sh_image, environment->sh, &environment->average_luminance);
    SDL_free(sh_image.pixels);

    GpuTextureDesc prefiltered_desc;
    SDL_zero(prefiltered_desc);
    prefiltered_desc.width = PREFILTER_WIDTH;
    prefiltered_desc.height = PREFILTER_WIDTH / 2;
    prefiltered_desc.mip_count = PREFILTER_MIPS;
    prefiltered_desc.format = GPU_FORMAT_RGBA16F;
    prefiltered_desc.usage = GPU_USAGE_SAMPLED;

    uint16_t* mip_pixels[PREFILTER_MIPS];

    for (int mip = 0; mip < PREFILTER_MIPS; mip++)
    {
        int width = PREFILTER_WIDTH >> mip;
        int height = (PREFILTER_WIDTH / 2) >> mip;
        Image level = image_alloc(width, height);

        if (mip == 0)
        {
            SDL_memcpy(level.pixels, pyramid[0].pixels, sizeof(float) * 3 * (size_t)width * (size_t)height);
        }
        else
        {
            PrefilterJob job;
            job.pyramid = pyramid;
            job.pyramid_levels = pyramid_levels;
            job.out = &level;
            job.roughness = (float)mip / (float)(PREFILTER_MIPS - 1);
            parallel_rows(height, prefilter_row, &job);
        }

        mip_pixels[mip] = image_to_rgba16f(&level);
        prefiltered_desc.mips[mip] = mip_pixels[mip];
        SDL_free(level.pixels);
    }

    environment->prefiltered = gpu_create_texture(&prefiltered_desc);
    environment->prefiltered_mips = PREFILTER_MIPS;

    for (int mip = 0; mip < PREFILTER_MIPS; mip++)
    {
        SDL_free(mip_pixels[mip]);
    }

    for (int i = 0; i < pyramid_levels; i++)
    {
        SDL_free(pyramid[i].pixels);
    }

    return environment->prefiltered != NULL;
}

static float luminance(const float* p)
{
    return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
}

// Looks for a sun in an HDR panorama : pixels far brighter than the sky around them. They're removed
// from the image used for lighting (the directional light replaces them, with real shadows) and their
// energy becomes that light's color.
static void extract_sun(Image* image, Environment* environment)
{
    size_t count = (size_t)image->width * (size_t)image->height;
    float peak = 0.0f;
    double sum = 0.0;

    for (size_t i = 0; i < count; i++)
    {
        float l = luminance(&image->pixels[i * 3]);
        peak = SDL_max(peak, l);
        sum += l;
    }

    float average = (float)(sum / (double)count);
    float threshold = SDL_max(average * 12.0f, peak * 0.05f);

    if (peak < average * 40.0f || peak < 4.0f)
    {
        return; // no clearly distinct sun : an overcast or tonemapped sky
    }

    double energy[3] = { 0.0, 0.0, 0.0 };
    double direction[3] = { 0.0, 0.0, 0.0 };
    float solid_angle_unit = (2.0f * PI_F / (float)image->width) * (PI_F / (float)image->height);

    for (int y = 0; y < image->height; y++)
    {
        float v = ((float)y + 0.5f) / (float)image->height;
        float solid_angle = solid_angle_unit * sinf(v * PI_F);

        for (int x = 0; x < image->width; x++)
        {
            float* p = &image->pixels[((size_t)y * (size_t)image->width + (size_t)x) * 3];
            float l = luminance(p);

            if (l <= threshold)
            {
                continue;
            }

            float d[3];
            panorama_direction(((float)x + 0.5f) / (float)image->width, v, d);

            float scale = threshold / l;

            for (int c = 0; c < 3; c++)
            {
                float excess = p[c] * (1.0f - scale);
                energy[c] += (double)excess * solid_angle;
                direction[c] += (double)d[c] * (double)(l - threshold) * solid_angle;
                p[c] *= scale;
            }
        }
    }

    double length = sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);

    if (length <= 0.0)
    {
        return;
    }

    environment->has_sun = 1;

    for (int c = 0; c < 3; c++)
    {
        environment->sun_direction[c] = (float)(direction[c] / length);
        environment->sun_color[c] = (float)energy[c];
    }
}

int environment_load(const char* path, Environment* out_environment)
{
    SDL_zerop(out_environment);

    HdrImage hdr;

    if (!hdr_image_load(path, &hdr))
    {
        return 0;
    }

    Image source = { hdr.pixels, hdr.width, hdr.height };

    // Half floats top out at 65504 ; a sun's disk in a good HDRI can exceed even that.
    size_t count = (size_t)hdr.width * (size_t)hdr.height * 3;

    for (size_t i = 0; i < count; i++)
    {
        if (!(source.pixels[i] >= 0.0f))
        {
            source.pixels[i] = 0.0f; // negative or NaN
        }
    }

    int display_width = SDL_min(hdr.width, PANORAMA_MAX_WIDTH);
    Image display = image_resize(&source, display_width, display_width / 2);

    for (size_t i = 0; i < (size_t)display.width * (size_t)display.height * 3; i++)
    {
        display.pixels[i] = SDL_min(display.pixels[i], 60000.0f);
    }

    Image lighting = image_resize(&source, 1024, 512);
    extract_sun(&lighting, out_environment);

    int built = build(&display, &lighting, out_environment);

    SDL_free(display.pixels);
    SDL_free(lighting.pixels);
    hdr_image_free(&hdr);

    if (!built)
    {
        environment_destroy(out_environment);
        return 0;
    }

    DEBUG_LOG("[environment] loaded \"%s\" : average luminance %.2f, sun %s (%.2f, %.2f, %.2f)\n", path, out_environment->average_luminance,
        out_environment->has_sun ? "found" : "none", out_environment->sun_color[0], out_environment->sun_color[1], out_environment->sun_color[2]);

    return 1;
}

// --- Procedural sky : Rayleigh and Mie single scattering (Nishita) --------------------------------

#define EARTH_RADIUS 6360e3f
#define ATMOSPHERE_RADIUS 6420e3f
#define RAYLEIGH_HEIGHT 8e3f
#define MIE_HEIGHT 1.2e3f
#define SUN_INTENSITY 20.0f
#define PRIMARY_SAMPLES 16
#define LIGHT_SAMPLES 6

static const float beta_rayleigh[3] = { 5.8e-6f, 13.5e-6f, 33.1e-6f };
static const float beta_mie = 21e-6f;

// Distance from `height` above the ground, along a direction with this cosine to the zenith, to the top
// of the atmosphere.
static float distance_to_top(float height, float cos_zenith)
{
    float r = EARTH_RADIUS + height;
    float b = r * cos_zenith;
    return -b + sqrtf(SDL_max(0.0f, b * b - r * r + ATMOSPHERE_RADIUS * ATMOSPHERE_RADIUS));
}

// Optical depth (Rayleigh, Mie) from a point toward the sun ; 0 if the ground blocks it.
static int light_optical_depth(const float position[3], const float sun[3], float out[2])
{
    float r = sqrtf(position[0] * position[0] + position[1] * position[1] + position[2] * position[2]);
    float height = r - EARTH_RADIUS;
    float cos_zenith = (position[0] * sun[0] + position[1] * sun[1] + position[2] * sun[2]) / r;
    float length = distance_to_top(height, cos_zenith);
    float step = length / (float)LIGHT_SAMPLES;

    out[0] = out[1] = 0.0f;

    for (int i = 0; i < LIGHT_SAMPLES; i++)
    {
        float t = ((float)i + 0.5f) * step;
        float p[3] = { position[0] + sun[0] * t, position[1] + sun[1] * t, position[2] + sun[2] * t };
        float h = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - EARTH_RADIUS;

        if (h < 0.0f)
        {
            return 0;
        }

        out[0] += expf(-h / RAYLEIGH_HEIGHT) * step;
        out[1] += expf(-h / MIE_HEIGHT) * step;
    }

    return 1;
}

static void scatter(const float direction[3], const float sun[3], float out[3])
{
    const float observer[3] = { 0.0f, EARTH_RADIUS + 2.0f, 0.0f };
    float length = distance_to_top(2.0f, direction[1]);
    float step = length / (float)PRIMARY_SAMPLES;
    float mu = direction[0] * sun[0] + direction[1] * sun[1] + direction[2] * sun[2];
    float phase_r = 3.0f / (16.0f * PI_F) * (1.0f + mu * mu);
    const float g = 0.76f;
    float phase_m = 3.0f / (8.0f * PI_F) * ((1.0f - g * g) * (1.0f + mu * mu)) / ((2.0f + g * g) * powf(1.0f + g * g - 2.0f * g * mu, 1.5f));

    float sum_r[3] = { 0.0f, 0.0f, 0.0f };
    float sum_m[3] = { 0.0f, 0.0f, 0.0f };
    float depth_r = 0.0f;
    float depth_m = 0.0f;

    for (int i = 0; i < PRIMARY_SAMPLES; i++)
    {
        float t = ((float)i + 0.5f) * step;
        float p[3] = { observer[0] + direction[0] * t, observer[1] + direction[1] * t, observer[2] + direction[2] * t };
        float h = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - EARTH_RADIUS;
        float density_r = expf(-h / RAYLEIGH_HEIGHT) * step;
        float density_m = expf(-h / MIE_HEIGHT) * step;

        depth_r += density_r;
        depth_m += density_m;

        float light_depth[2];

        if (!light_optical_depth(p, sun, light_depth))
        {
            continue;
        }

        for (int c = 0; c < 3; c++)
        {
            float tau = beta_rayleigh[c] * (depth_r + light_depth[0]) + beta_mie * 1.1f * (depth_m + light_depth[1]);
            float attenuation = expf(-tau);
            sum_r[c] += density_r * attenuation;
            sum_m[c] += density_m * attenuation;
        }
    }

    for (int c = 0; c < 3; c++)
    {
        out[c] = SUN_INTENSITY * (sum_r[c] * beta_rayleigh[c] * phase_r + sum_m[c] * beta_mie * phase_m);
    }

    // Single scattering alone leaves the horizon too dark and too colored, its long paths having lost
    // their blue. Light scattered several times is what really fills it : approximated by a neutral
    // haze growing toward the horizon, as in Inigo Quilez's skies, scaled to how bright the sun's side is.
    float sun_level = SDL_clamp(sun[1] * 4.0f + 0.3f, 0.0f, 1.0f);
    float haze = powf(1.0f - SDL_max(direction[1], 0.0f), 6.0f) * sun_level;
    static const float haze_color[3] = { 1.0f, 1.08f, 1.2f };

    for (int c = 0; c < 3; c++)
    {
        out[c] += (haze_color[c] * 1.1f - out[c]) * haze * 0.75f;
    }
}

typedef struct
{
    Image* image;
    const float* sun;
    const float* ground; // radiance of the ground, under the horizon
} SkyJob;

static void sky_row(int y, void* context)
{
    SkyJob* job = context;
    Image* image = job->image;
    float v = ((float)y + 0.5f) / (float)image->height;

    for (int x = 0; x < image->width; x++)
    {
        float d[3];
        panorama_direction(((float)x + 0.5f) / (float)image->width, v, d);

        float* pixel = &image->pixels[((size_t)y * (size_t)image->width + (size_t)x) * 3];

        if (d[1] >= 0.0f)
        {
            scatter(d, job->sun, pixel);
            continue;
        }

        // Below the horizon : the ground, seen through the haze of the horizon.
        float horizon[3] = { d[0], 0.0005f, d[2] };
        float n = sqrtf(horizon[0] * horizon[0] + horizon[1] * horizon[1] + horizon[2] * horizon[2]);
        horizon[0] /= n;
        horizon[1] /= n;
        horizon[2] /= n;

        float haze[3];
        scatter(horizon, job->sun, haze);

        float t = SDL_min(1.0f, -d[1] * 12.0f);

        for (int c = 0; c < 3; c++)
        {
            pixel[c] = haze[c] + (job->ground[c] - haze[c]) * t;
        }
    }
}

int environment_create_procedural(const float sun_direction[3], Environment* out_environment)
{
    SDL_zerop(out_environment);

    float length = sqrtf(sun_direction[0] * sun_direction[0] + sun_direction[1] * sun_direction[1] + sun_direction[2] * sun_direction[2]);
    float sun[3] = { sun_direction[0] / length, sun_direction[1] / length, sun_direction[2] / length };

    // The sunlight reaching the ground, reddened by the air it crossed.
    float observer[3] = { 0.0f, EARTH_RADIUS + 2.0f, 0.0f };
    float depth[2];
    float sun_color[3] = { 0.0f, 0.0f, 0.0f };

    if (light_optical_depth(observer, sun, depth))
    {
        for (int c = 0; c < 3; c++)
        {
            sun_color[c] = SUN_INTENSITY * expf(-(beta_rayleigh[c] * depth[0] + beta_mie * 1.1f * depth[1]));
        }
    }

    // The ground's radiance : a mid-grey albedo, lit by the sun and a rough estimate of the sky.
    float ground[3];

    for (int c = 0; c < 3; c++)
    {
        ground[c] = 0.2f * (sun_color[c] * SDL_max(sun[1], 0.0f) / PI_F + 0.8f);
    }

    Image sky = image_alloc(512, 256);
    SkyJob job = { &sky, sun, ground };
    parallel_rows(sky.height, sky_row, &job);

    int built = build(&sky, &sky, out_environment);

    SDL_free(sky.pixels);

    if (!built)
    {
        environment_destroy(out_environment);
        return 0;
    }

    out_environment->has_sun = 1;
    out_environment->show_sun_disk = 1;

    for (int c = 0; c < 3; c++)
    {
        out_environment->sun_direction[c] = sun[c];
        out_environment->sun_color[c] = sun_color[c];
    }

    return 1;
}

void environment_destroy(Environment* environment)
{
    if (environment->panorama)
    {
        destroy_texture(environment->panorama);
    }

    if (environment->prefiltered)
    {
        destroy_texture(environment->prefiltered);
    }

    SDL_zerop(environment);
}
