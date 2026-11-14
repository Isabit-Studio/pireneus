#include <stddef.h>

#include "renderer/renderer.h"
#include "example.h"

// No SpriteBatch here: this example exercises the low-level draw_vertices() primitive
// directly, with a 1x1 white texture standing in for "no texture".
static Vertex triangle[3] = {
    { 400.0f, 150.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f },
    { 550.0f, 450.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f },
    { 250.0f, 450.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f }
};

static void render(void)
{
    draw_vertices(triangle, 3, white_texture());
}

const Example example_hello_triangle = {
    "hello-triangle", "Hello Triangle",
    "The low-level draw_vertices() primitive, with no sprite batch.",
    NULL, NULL, render, NULL
};
