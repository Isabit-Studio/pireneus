#ifndef _EXAMPLE_H_
#define _EXAMPLE_H_

// One example of the single examples executable (see launcher/main.cpp). The launcher owns the window,
// the renderer and Dear ImGui ; an example only provides what's specific to it, split around the frame :
//
//     pireneus_imgui_new_frame();
//     example->update();            // input, ImGui panels, self-contained offscreen passes
//     pireneus_imgui_prepare_frame();
//     begin_frame();
//     example->render();            // drawing into the window
//     pireneus_imgui_draw();
//     end_frame();
//
// Every example gets its own Dear ImGui context, so docked layouts don't leak from one to the other.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const char* id;          // what the --example command line option takes
    const char* title;
    const char* description;
    int (*init)(void);       // optional ; 0 on failure
    void (*update)(void);    // optional
    void (*render)(void);    // optional
    void (*shutdown)(void);  // optional ; the GPU is idle when it's called
    int has_own_menu;        // its UI has a way back to the launcher : no "Menu" button is added
} Example;

extern const Example example_hello_triangle;
extern const Example example_2d_test;
extern const Example example_pixel_perfect;
extern const Example example_tilemap_editor;
extern const Example example_forward_plus;
extern const Example example_deferred_plus;
extern const Example example_scene_editor;

// Leaves the running example at the end of the current frame, back to the launcher's menu.
void example_return_to_menu(void);

// The project the editor opens next (scene-editor/main.cpp).
void editor_request_project(const char* path);

#ifdef __cplusplus
}
#endif

#endif // _EXAMPLE_H_
