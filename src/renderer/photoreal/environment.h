#ifndef _ENVIRONMENT_H_
#define _ENVIRONMENT_H_

#include "renderer/renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

// What lights a photorealistic scene from afar : a sky, loaded from an HDR panorama or computed from
// the sun's position. Everything is derived on the CPU when it's created, identically on every backend.
typedef struct
{
    Texture* panorama;      // RGBA16F equirectangular image, the background
    Texture* prefiltered;   // RGBA16F, one mip per roughness : GGX-convolved reflections
    int prefiltered_mips;
    float sh[9][3];         // irradiance / PI, as spherical harmonics : diffuse light
    float average_luminance;
    int has_sun;            // a sun was found in the panorama, or computed
    float sun_direction[3]; // toward the sun
    float sun_color[3];     // its irradiance at normal incidence
    int show_sun_disk;      // the panorama doesn't contain the sun's disk : the renderer draws it
} Environment;

// An equirectangular panorama (.exr, .hdr, or an 8-bit image). `path` is absolute. If an HDR panorama
// contains a sun, it's taken out of the image and becomes the directional light, whose shadows it
// casts. 0 on failure.
int environment_load(const char* path, Environment* out_environment);

// A physically based sky (Rayleigh and Mie single scattering) for a sun in this direction.
int environment_create_procedural(const float sun_direction[3], Environment* out_environment);

void environment_destroy(Environment* environment); // the GPU must be done with it

#ifdef __cplusplus
}
#endif

#endif // _ENVIRONMENT_H_
