#ifndef _HDR_IMAGE_H_
#define _HDR_IMAGE_H_

#ifdef __cplusplus
extern "C" {
#endif

// A floating-point RGB image in linear light : what the renderer needs from a skybox.
typedef struct
{
    float* pixels; // width * height * 3 floats, top row first
    int width;
    int height;
} HdrImage;

// Reads an OpenEXR (.exr), Radiance (.hdr), or ordinary 8-bit image (PNG, JPEG...), the latter
// converted from sRGB. `path` is absolute. 0 on failure.
int hdr_image_load(const char* path, HdrImage* out_image);
void hdr_image_free(HdrImage* image);

#ifdef __cplusplus
}
#endif

#endif // _HDR_IMAGE_H_
