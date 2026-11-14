#include "assets/hdr_image.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <math.h>
#include <stdio.h>

int tinyexr_load_rgba(const char* path, float** out_rgba, int* out_width, int* out_height);
void tinyexr_free(float* rgba);

static int has_extension(const char* path, const char* extension)
{
    size_t path_length = SDL_strlen(path);
    size_t extension_length = SDL_strlen(extension);

    return path_length >= extension_length && SDL_strcasecmp(path + path_length - extension_length, extension) == 0;
}

static int load_exr(const char* path, HdrImage* out_image)
{
    float* rgba = NULL;
    int width = 0;
    int height = 0;

    if (!tinyexr_load_rgba(path, &rgba, &width, &height))
    {
        printf("Could not read EXR '%s'.\n", path);
        return 0;
    }

    out_image->width = width;
    out_image->height = height;
    out_image->pixels = SDL_malloc(sizeof(float) * 3 * (size_t)width * (size_t)height);

    for (size_t i = 0; i < (size_t)width * (size_t)height; i++)
    {
        out_image->pixels[i * 3 + 0] = rgba[i * 4 + 0];
        out_image->pixels[i * 3 + 1] = rgba[i * 4 + 1];
        out_image->pixels[i * 3 + 2] = rgba[i * 4 + 2];
    }

    tinyexr_free(rgba);

    return 1;
}

static void rgbe_to_float(const uint8_t rgbe[4], float* out)
{
    if (rgbe[3] == 0)
    {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }

    float scale = ldexpf(1.0f, (int)rgbe[3] - (128 + 8));
    out[0] = (float)rgbe[0] * scale;
    out[1] = (float)rgbe[1] * scale;
    out[2] = (float)rgbe[2] * scale;
}

// Radiance RGBE, with its adaptive run-length encoding.
static int load_radiance(const char* path, HdrImage* out_image)
{
    size_t size = 0;
    uint8_t* file = SDL_LoadFile(path, &size);

    if (!file)
    {
        printf("Could not read '%s'.\n", path);
        return 0;
    }

    // The header ends with an empty line, followed by the resolution line.
    size_t position = 0;

    while (position + 1 < size && !(file[position] == '\n' && file[position + 1] == '\n'))
    {
        position++;
    }

    position += 2;

    int width = 0;
    int height = 0;
    char resolution[64];
    size_t line = 0;

    while (position < size && file[position] != '\n' && line + 1 < sizeof(resolution))
    {
        resolution[line++] = (char)file[position++];
    }

    resolution[line] = '\0';
    position++;

    if (SDL_sscanf(resolution, "-Y %d +X %d", &height, &width) != 2 || width <= 0 || height <= 0)
    {
        printf("'%s' : unsupported Radiance orientation ('%s').\n", path, resolution);
        SDL_free(file);
        return 0;
    }

    out_image->width = width;
    out_image->height = height;
    out_image->pixels = SDL_malloc(sizeof(float) * 3 * (size_t)width * (size_t)height);

    uint8_t* scanline = SDL_malloc((size_t)width * 4);

    for (int y = 0; y < height; y++)
    {
        float* row = &out_image->pixels[(size_t)y * (size_t)width * 3];

        if (position + 4 > size)
        {
            break;
        }

        if (width >= 8 && width < 32768 && file[position] == 2 && file[position + 1] == 2 && !(file[position + 2] & 0x80))
        {
            position += 4;

            for (int channel = 0; channel < 4; channel++)
            {
                int x = 0;

                while (x < width && position < size)
                {
                    uint8_t count = file[position++];

                    if (count > 128)
                    {
                        count -= 128;
                        uint8_t value = position < size ? file[position++] : 0;

                        for (int i = 0; i < count && x < width; i++)
                        {
                            scanline[x++ * 4 + channel] = value;
                        }
                    }
                    else
                    {
                        for (int i = 0; i < count && x < width && position < size; i++)
                        {
                            scanline[x++ * 4 + channel] = file[position++];
                        }
                    }
                }
            }

            for (int x = 0; x < width; x++)
            {
                rgbe_to_float(&scanline[x * 4], &row[x * 3]);
            }
        }
        else
        {
            // Flat, uncompressed scanline.
            for (int x = 0; x < width && position + 4 <= size; x++)
            {
                rgbe_to_float(&file[position], &row[x * 3]);
                position += 4;
            }
        }
    }

    SDL_free(scanline);
    SDL_free(file);

    return 1;
}

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : SDL_powf((c + 0.055f) / 1.055f, 2.4f);
}

static int load_ldr(const char* path, HdrImage* out_image)
{
    SDL_Surface* loaded = IMG_Load(path);

    if (!loaded)
    {
        printf("Could not load image '%s': %s\n", path, SDL_GetError());
        return 0;
    }

    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);

    if (!rgba)
    {
        return 0;
    }

    out_image->width = rgba->w;
    out_image->height = rgba->h;
    out_image->pixels = SDL_malloc(sizeof(float) * 3 * (size_t)rgba->w * (size_t)rgba->h);

    static float table[256];
    static int table_ready;

    if (!table_ready)
    {
        for (int i = 0; i < 256; i++)
        {
            table[i] = srgb_to_linear((float)i / 255.0f);
        }

        table_ready = 1;
    }

    for (int y = 0; y < rgba->h; y++)
    {
        const uint8_t* row = (const uint8_t*)rgba->pixels + (size_t)y * (size_t)rgba->pitch;
        float* out = &out_image->pixels[(size_t)y * (size_t)rgba->w * 3];

        for (int x = 0; x < rgba->w; x++)
        {
            out[x * 3 + 0] = table[row[x * 4 + 0]];
            out[x * 3 + 1] = table[row[x * 4 + 1]];
            out[x * 3 + 2] = table[row[x * 4 + 2]];
        }
    }

    SDL_DestroySurface(rgba);

    return 1;
}

int hdr_image_load(const char* path, HdrImage* out_image)
{
    SDL_zerop(out_image);

    if (has_extension(path, ".exr"))
    {
        return load_exr(path, out_image);
    }

    if (has_extension(path, ".hdr"))
    {
        return load_radiance(path, out_image);
    }

    return load_ldr(path, out_image);
}

void hdr_image_free(HdrImage* image)
{
    SDL_free(image->pixels);
    SDL_zerop(image);
}
