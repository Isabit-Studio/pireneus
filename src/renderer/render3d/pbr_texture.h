#ifndef _PBR_TEXTURE_H_
#define _PBR_TEXTURE_H_

#include <stddef.h>
#include <stdint.h>

#include "renderer/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Material textures for the 3D renderers (render3d.h), with a full mip chain. `srgb` : color data (base
// color, emissive), filtered in linear light and decoded by the sampler ; 0 : data (normals, roughness...).
// `path` is absolute. NULL on failure ; destroy_texture() releases it.
//
// Image files (PNG, JPEG, TGA, BMP) are decoded to RGBA8. DDS files (BC1, BC3, BC5, with their mips)
// stay compressed on the GPU where it samples them (gpu_format_supported()), and are decoded otherwise.
typedef struct
{
    int alpha_mask;     // its alpha cuts out : some texels clearly transparent, others clearly opaque
    int normal_y_down;  // a normal map's green points down (DirectX's convention, a DDS file's) ; else up
} PbrTextureInfo;

Texture* pbr_texture_load(const char* path, int srgb);
Texture* pbr_texture_load_info(const char* path, int srgb, PbrTextureInfo* out_info); // out_info may be NULL
Texture* pbr_texture_load_memory(const void* data, size_t size, int srgb); // an image file's contents
Texture* pbr_texture_from_pixels(const uint8_t* rgba, int width, int height, int srgb); // tightly packed RGBA8

#ifdef __cplusplus
}
#endif

#endif // _PBR_TEXTURE_H_
