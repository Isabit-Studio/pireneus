#ifndef _PLATFORMS_PATHS_H_
#define _PLATFORMS_PATHS_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where a program may keep things, which depends on the platform (a sandboxed UWP app can only write
// to its own folders). Both paths end with a separator.
void platform_user_data_path(char* out, size_t out_size);  // settings and caches of the engine itself
void platform_documents_path(char* out, size_t out_size);  // where new projects go by default

// Native dialogs, asynchronous : the callback is called later, on the main thread, from handle_events().
// `paths` is NULL if the user cancelled. Filters are "name|pattern;pattern" strings, e.g. "Images|png;jpg".
typedef void (*FileDialogCallback)(void* user_data, const char* const* paths, int count);
void platform_show_open_files_dialog(FileDialogCallback callback, void* user_data, const char* filter);
void platform_show_open_folder_dialog(FileDialogCallback callback, void* user_data);

// Moves a file or folder to the system's trash, where it can still be restored. 0 on failure.
int platform_move_to_trash(const char* path);

#ifdef __cplusplus
}
#endif

#endif // _PLATFORMS_PATHS_H_
