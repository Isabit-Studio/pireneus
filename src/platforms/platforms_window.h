#ifndef _PLATFORMS_H_
#define _PLATFORMS_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Window Window; // opaque handle to the platform window
typedef void (*RedrawCallback)(void);

// Receives every native platform event polled by handle_events(), before the engine's own handling.
// `native_event` is the platform layer's own event type (a `const SDL_Event*` for the SDL
// implementation) : only code that already depends on that platform layer (e.g. src/imgui, which
// feeds it to ImGui's SDL3 backend) should cast and read it.
typedef void (*EventCallback)(const void* native_event);

int create_window(const char*, int, int); // title, width, height
int handle_events(); // returns 0 if the window should be closed, 1 otherwise
void destroy_window(); // destroys the window and cleans up resources

Window* get_window(void); // returns an opaque handle to the platform window, for use by the renderer
void get_window_size(int* out_width, int* out_height); // current window size, in pixels
void set_window_title(const char* title);

// Registers the function that draws one frame, and calls it synchronously whenever the
// platform needs the window redrawn immediately (e.g. while the user is dragging its border) :
// on Windows, an interactive resize/move blocks the normal event loop in a modal OS loop, so
// without this the window would otherwise freeze (stop rendering) until the drag ends.
void set_redraw_callback(RedrawCallback callback);

// Registers the (single) function receiving every native event, see EventCallback. NULL removes it.
void set_event_callback(EventCallback callback);

// Registers the function called with each file dragged from the desktop and dropped on the window.
typedef void (*FileDropCallback)(const char* path);
void set_file_drop_callback(FileDropCallback callback);

// Invokes the registered redraw callback, guarded against reentrancy : a nested call (e.g. some
// GPU API pumping window messages internally, from within a call this same callback made) is
// dropped rather than run, since the callback isn't reentrant (it holds renderer state, such as
// which command buffer is currently being recorded, across its own sub-calls). The main loop
// must call this instead of calling its frame function directly, so every path that can trigger
// a redraw (the modal-resize watcher and the normal per-iteration draw alike) shares the same guard.
void call_redraw_callback(void);

#ifdef __cplusplus
}
#endif

#endif // _PLATFORMS_H_