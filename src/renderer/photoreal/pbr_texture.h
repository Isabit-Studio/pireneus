#ifndef _PBR_TEXTURE_H_
#define _PBR_TEXTURE_H_

#include <stddef.h>
#include <stdint.h>

#include "renderer/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Material textures for the photorealistic renderer, with a full mip chain. `srgb` : color data (base
// color, emissive), filtered in linear light and decoded by the sampler ; 0 : data (normals, roughness...).
// `path` is absolute. NULL on failure ; destroy_texture() releases it.
Texture* pbr_texture_load(const char* path, int srgb);
Texture* pbr_texture_load_memory(const void* data, size_t size, int srgb); // an image file's contents
Texture* pbr_texture_from_pixels(const uint8_t* rgba, int width, int height, int srgb); // tightly packed RGBA8

#ifdef __cplusplus
}
#endif

#endif // _PBR_TEXTURE_H_
