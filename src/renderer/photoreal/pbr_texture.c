#include "renderer/photoreal/pbr_texture.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <stdio.h>

#include "renderer/gpu.h"

static float srgb_table[256];
static uint8_t linear_to_srgb_table[4096];
static int tables_ready;

static void prepare_tables(void)
{
    if (tables_ready)
    {
        return;
    }

    for (int i = 0; i < 256; i++)
    {
        float c = (float)i / 255.0f;
        srgb_table[i] = c <= 0.04045f ? c / 12.92f : SDL_powf((c + 0.055f) / 1.055f, 2.4f);
    }

    for (int i = 0; i < 4096; i++)
    {
        float c = (float)i / 4095.0f;
        float s = c <= 0.0031308f ? c * 12.92f : 1.055f * SDL_powf(c, 1.0f / 2.4f) - 0.055f;
        linear_to_srgb_table[i] = (uint8_t)(SDL_clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    tables_ready = 1;
}

// One level down : a 2x2 box, in linear light for color data.
static uint8_t* downsample(const uint8_t* source, int width, int height, int srgb, int* out_width, int* out_height)
{
    int w = SDL_max(1, width / 2);
    int h = SDL_max(1, height / 2);
    uint8_t* out = SDL_malloc((size_t)w * (size_t)h * 4);

    for (int y = 0; y < h; y++)
    {
        int y0 = SDL_min(y * 2, height - 1);
        int y1 = SDL_min(y * 2 + 1, height - 1);

        for (int x = 0; x < w; x++)
        {
            int x0 = SDL_min(x * 2, width - 1);
            int x1 = SDL_min(x * 2 + 1, width - 1);

            const uint8_t* p[4] = {
                &source[((size_t)y0 * (size_t)width + (size_t)x0) * 4],
                &source[((size_t)y0 * (size_t)width + (size_t)x1) * 4],
                &source[((size_t)y1 * (size_t)width + (size_t)x0) * 4],
                &source[((size_t)y1 * (size_t)width + (size_t)x1) * 4]
            };

            uint8_t* o = &out[((size_t)y * (size_t)w + (size_t)x) * 4];

            for (int c = 0; c < 3; c++)
            {
                if (srgb)
                {
                    float sum = srgb_table[p[0][c]] + srgb_table[p[1][c]] + srgb_table[p[2][c]] + srgb_table[p[3][c]];
                    o[c] = linear_to_srgb_table[(int)(sum * 0.25f * 4095.0f + 0.5f)];
                }
                else
                {
                    o[c] = (uint8_t)(((int)p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
                }
            }

            o[3] = (uint8_t)(((int)p[0][3] + p[1][3] + p[2][3] + p[3][3] + 2) / 4);
        }
    }

    *out_width = w;
    *out_height = h;

    return out;
}

Texture* pbr_texture_from_pixels(const uint8_t* rgba, int width, int height, int srgb)
{
    prepare_tables();

    GpuTextureDesc desc;
    SDL_zero(desc);
    desc.width = width;
    desc.height = height;
    desc.format = srgb ? GPU_FORMAT_RGBA8_SRGB : GPU_FORMAT_RGBA8;
    desc.usage = GPU_USAGE_SAMPLED;

    uint8_t* levels[GPU_MAX_MIPS];
    int count = 0;

    levels[count++] = (uint8_t*)rgba;

    while ((width > 1 || height > 1) && count < GPU_MAX_MIPS)
    {
        levels[count] = downsample(levels[count - 1], width, height, srgb, &width, &height);
        count++;
    }

    desc.mip_count = count;

    for (int i = 0; i < count; i++)
    {
        desc.mips[i] = levels[i];
    }

    Texture* texture = gpu_create_texture(&desc);

    for (int i = 1; i < count; i++)
    {
        SDL_free(levels[i]);
    }

    return texture;
}

static Texture* create_from_surface(SDL_Surface* loaded, int srgb)
{
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);

    if (!rgba)
    {
        return NULL;
    }

    // The surface's rows may be padded : the base level is copied tightly.
    uint8_t* base = SDL_malloc((size_t)rgba->w * (size_t)rgba->h * 4);

    for (int y = 0; y < rgba->h; y++)
    {
        SDL_memcpy(base + (size_t)y * (size_t)rgba->w * 4, (const uint8_t*)rgba->pixels + (size_t)y * (size_t)rgba->pitch, (size_t)rgba->w * 4);
    }

    Texture* texture = pbr_texture_from_pixels(base, rgba->w, rgba->h, srgb);

    SDL_DestroySurface(rgba);
    SDL_free(base);

    return texture;
}

Texture* pbr_texture_load(const char* path, int srgb)
{
    SDL_Surface* loaded = IMG_Load(path);

    if (!loaded)
    {
        printf("Could not load texture '%s': %s\n", path, SDL_GetError());
        return NULL;
    }

    return create_from_surface(loaded, srgb);
}

Texture* pbr_texture_load_memory(const void* data, size_t size, int srgb)
{
    SDL_IOStream* stream = SDL_IOFromConstMem(data, size);
    SDL_Surface* loaded = stream ? IMG_Load_IO(stream, true) : NULL;

    if (!loaded)
    {
        printf("Could not decode an embedded texture: %s\n", SDL_GetError());
        return NULL;
    }

    return create_from_surface(loaded, srgb);
}
