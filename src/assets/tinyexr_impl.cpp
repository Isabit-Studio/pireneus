// tinyexr's implementation is C++ : compiled once here, against the vendored miniz, behind a C function.

#include <stdlib.h>

#include "miniz.h"

#define TINYEXR_USE_MINIZ 1
#define TINYEXR_USE_THREAD 0
#define TINYEXR_USE_OPENMP 0
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

extern "C" int tinyexr_load_rgba(const char* path, float** out_rgba, int* out_width, int* out_height)
{
    const char* error = NULL;
    int result = LoadEXR(out_rgba, out_width, out_height, path, &error);

    if (result != TINYEXR_SUCCESS)
    {
        if (error)
        {
            FreeEXRErrorMessage(error);
        }

        return 0;
    }

    return 1;
}

extern "C" void tinyexr_free(float* rgba)
{
    free(rgba);
}
