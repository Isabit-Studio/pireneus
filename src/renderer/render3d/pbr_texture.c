#include "renderer/render3d/pbr_texture.h"

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

// --- DDS ------------------------------------------------------------------------------------------
// Block-compressed textures, as games ship them : BC1 (DXT1), BC3 (DXT5) and BC5 (ATI2), with their mip
// chain. Uploaded as they are where the GPU samples them, decoded to RGBA8 otherwise.

static uint32_t read_u32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void rgb565(uint16_t c, uint8_t out[3])
{
    out[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    out[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    out[2] = (uint8_t)((c & 31) * 255 / 31);
}

// A BC1 color block into 4 x 4 RGBA texels. `four_colors` : BC3's color part never has the 1-bit alpha mode.
static void decode_bc1_block(const uint8_t* block, uint8_t out[16][4], int four_colors)
{
    uint16_t c0 = (uint16_t)(block[0] | (block[1] << 8));
    uint16_t c1 = (uint16_t)(block[2] | (block[3] << 8));
    uint8_t palette[4][4];
    rgb565(c0, palette[0]);
    rgb565(c1, palette[1]);
    palette[0][3] = palette[1][3] = 255;

    if (c0 > c1 || four_colors)
    {
        for (int c = 0; c < 3; c++)
        {
            palette[2][c] = (uint8_t)((2 * palette[0][c] + palette[1][c]) / 3);
            palette[3][c] = (uint8_t)((palette[0][c] + 2 * palette[1][c]) / 3);
        }

        palette[2][3] = palette[3][3] = 255;
    }
    else
    {
        for (int c = 0; c < 3; c++)
        {
            palette[2][c] = (uint8_t)((palette[0][c] + palette[1][c]) / 2);
            palette[3][c] = 0;
        }

        palette[2][3] = 255;
        palette[3][3] = 0;
    }

    uint32_t bits = read_u32(block + 4);

    for (int i = 0; i < 16; i++)
    {
        SDL_memcpy(out[i], palette[(bits >> (i * 2)) & 3], 4);
    }
}

// A BC4 block (BC3's alpha, each of BC5's two channels) into 16 values.
static void decode_bc4_block(const uint8_t* block, uint8_t out[16])
{
    uint8_t a0 = block[0];
    uint8_t a1 = block[1];
    uint8_t palette[8] = { a0, a1 };

    if (a0 > a1)
    {
        for (int i = 1; i < 7; i++)
        {
            palette[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
        }
    }
    else
    {
        for (int i = 1; i < 5; i++)
        {
            palette[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5);
        }

        palette[6] = 0;
        palette[7] = 255;
    }

    uint64_t bits = 0;

    for (int i = 0; i < 6; i++)
    {
        bits |= (uint64_t)block[2 + i] << (8 * i);
    }

    for (int i = 0; i < 16; i++)
    {
        out[i] = palette[(bits >> (i * 3)) & 7];
    }
}

// A level of a BC texture into tightly packed RGBA8.
static uint8_t* decode_bc(GpuFormat format, const uint8_t* data, int width, int height)
{
    uint8_t* pixels = SDL_malloc((size_t)width * (size_t)height * 4);
    int blocks_x = (width + 3) / 4;
    int blocks_y = (height + 3) / 4;
    size_t block_size = (format == GPU_FORMAT_BC1 || format == GPU_FORMAT_BC1_SRGB) ? 8 : 16;

    for (int by = 0; by < blocks_y; by++)
    {
        for (int bx = 0; bx < blocks_x; bx++)
        {
            const uint8_t* block = data + ((size_t)by * (size_t)blocks_x + (size_t)bx) * block_size;
            uint8_t texels[16][4];

            if (format == GPU_FORMAT_BC5)
            {
                uint8_t red[16], green[16];
                decode_bc4_block(block, red);
                decode_bc4_block(block + 8, green);

                for (int i = 0; i < 16; i++)
                {
                    texels[i][0] = red[i];
                    texels[i][1] = green[i];
                    texels[i][2] = 0;
                    texels[i][3] = 255;
                }
            }
            else if (block_size == 16)
            {
                uint8_t alpha[16];
                decode_bc4_block(block, alpha);
                decode_bc1_block(block + 8, texels, 1);

                for (int i = 0; i < 16; i++)
                {
                    texels[i][3] = alpha[i];
                }
            }
            else
            {
                decode_bc1_block(block, texels, 0);
            }

            for (int i = 0; i < 16; i++)
            {
                int x = bx * 4 + (i & 3);
                int y = by * 4 + (i >> 2);

                if (x < width && y < height)
                {
                    SDL_memcpy(&pixels[((size_t)y * (size_t)width + (size_t)x) * 4], texels[i], 4);
                }
            }
        }
    }

    return pixels;
}

// A cutout : some texels clearly transparent, others clearly opaque (rather than an even, glass-like
// alpha, which a cutout would make vanish).
static int alpha_is_mask(const uint8_t* rgba, size_t texel_count)
{
    size_t transparent = 0;
    size_t opaque = 0;

    for (size_t i = 0; i < texel_count; i++)
    {
        uint8_t a = rgba[i * 4 + 3];
        transparent += a < 64;
        opaque += a > 192;
    }

    return transparent > texel_count / 200 && opaque > texel_count / 20;
}

static Texture* load_dds(const char* path, int srgb, PbrTextureInfo* info)
{
    size_t size = 0;
    uint8_t* file = SDL_LoadFile(path, &size);

    if (!file || size < 128 || SDL_memcmp(file, "DDS ", 4) != 0)
    {
        printf("Could not load texture '%s': %s\n", path, file ? "not a DDS file" : SDL_GetError());
        SDL_free(file);
        return NULL;
    }

    int height = (int)read_u32(file + 12);
    int width = (int)read_u32(file + 16);
    int mip_count = (int)SDL_max(1u, read_u32(file + 28));
    uint32_t fourcc = read_u32(file + 84);
    size_t offset = 128;
    GpuFormat format = GPU_FORMAT_NONE;

    if (fourcc == SDL_FOURCC('D', 'X', 'T', '1'))
    {
        format = GPU_FORMAT_BC1;
    }
    else if (fourcc == SDL_FOURCC('D', 'X', 'T', '5'))
    {
        format = GPU_FORMAT_BC3;
    }
    else if (fourcc == SDL_FOURCC('A', 'T', 'I', '2') || fourcc == SDL_FOURCC('B', 'C', '5', 'U'))
    {
        format = GPU_FORMAT_BC5;
    }
    else if (fourcc == SDL_FOURCC('D', 'X', '1', '0') && size >= 148)
    {
        uint32_t dxgi = read_u32(file + 128);
        offset = 148;
        format = (dxgi == 71 || dxgi == 72) ? GPU_FORMAT_BC1 : (dxgi == 77 || dxgi == 78) ? GPU_FORMAT_BC3 : (dxgi == 83) ? GPU_FORMAT_BC5 : GPU_FORMAT_NONE;
    }

    if (format == GPU_FORMAT_NONE || width <= 0 || height <= 0)
    {
        printf("Could not load texture '%s': unsupported DDS format (BC1, BC3 and BC5 are)\n", path);
        SDL_free(file);
        return NULL;
    }

    if (srgb && format != GPU_FORMAT_BC5)
    {
        format = format == GPU_FORMAT_BC1 ? GPU_FORMAT_BC1_SRGB : GPU_FORMAT_BC3_SRGB;
    }

    // The levels the file has, as far as it holds them.
    GpuTextureDesc desc;
    SDL_zero(desc);
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = GPU_USAGE_SAMPLED;

    int w = width, h = height;

    for (int mip = 0; mip < SDL_min(mip_count, GPU_MAX_MIPS); mip++)
    {
        size_t level_size = gpu_format_level_size(format, w, h);

        if (offset + level_size > size)
        {
            break;
        }

        desc.mips[mip] = file + offset;
        desc.mip_count = mip + 1;
        offset += level_size;
        w = SDL_max(1, w / 2);
        h = SDL_max(1, h / 2);
    }

    if (desc.mip_count == 0)
    {
        printf("Could not load texture '%s': truncated DDS file\n", path);
        SDL_free(file);
        return NULL;
    }

    // Whether its alpha cuts out : looked at on a small level.
    if (info)
    {
        int level = 0;
        int lw = width, lh = height;

        while (level + 1 < desc.mip_count && SDL_max(lw, lh) > 256)
        {
            level++;
            lw = SDL_max(1, lw / 2);
            lh = SDL_max(1, lh / 2);
        }

        if (format != GPU_FORMAT_BC5)
        {
            uint8_t* rgba = decode_bc(format, desc.mips[level], lw, lh);
            info->alpha_mask = alpha_is_mask(rgba, (size_t)lw * (size_t)lh);
            SDL_free(rgba);
        }

        info->normal_y_down = 1; // DDS normal maps are DirectX's : green points down
    }

    Texture* texture = NULL;

    // Direct3D needs block-aligned sizes, which tiny textures (1 x 1) aren't : those are decoded too.
    if (gpu_format_supported(format) && width % 4 == 0 && height % 4 == 0)
    {
        texture = gpu_create_texture(&desc);
    }
    else
    {
        uint8_t* rgba = decode_bc(format, desc.mips[0], width, height);
        texture = pbr_texture_from_pixels(rgba, width, height, srgb);
        SDL_free(rgba);
    }

    SDL_free(file);
    return texture;
}

static Texture* create_from_surface(SDL_Surface* loaded, int srgb, PbrTextureInfo* info)
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

    if (info)
    {
        info->alpha_mask = alpha_is_mask(base, (size_t)rgba->w * (size_t)rgba->h);
        info->normal_y_down = 0;
    }

    Texture* texture = pbr_texture_from_pixels(base, rgba->w, rgba->h, srgb);

    SDL_DestroySurface(rgba);
    SDL_free(base);

    return texture;
}

Texture* pbr_texture_load_info(const char* path, int srgb, PbrTextureInfo* out_info)
{
    if (out_info)
    {
        SDL_zerop(out_info);
    }

    size_t length = SDL_strlen(path);

    if (length > 4 && SDL_strcasecmp(path + length - 4, ".dds") == 0)
    {
        return load_dds(path, srgb, out_info);
    }

    SDL_Surface* loaded = IMG_Load(path);

    if (!loaded)
    {
        printf("Could not load texture '%s': %s\n", path, SDL_GetError());
        return NULL;
    }

    return create_from_surface(loaded, srgb, out_info);
}

Texture* pbr_texture_load(const char* path, int srgb)
{
    return pbr_texture_load_info(path, srgb, NULL);
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

    return create_from_surface(loaded, srgb, NULL);
}
