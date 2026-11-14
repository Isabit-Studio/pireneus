#include "../platforms_paths.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

void platform_user_data_path(char* out, size_t out_size)
{
    char* path = SDL_GetPrefPath("Pireneus", "Engine");
    SDL_snprintf(out, out_size, "%s", path ? path : "");
    SDL_free(path);
}

void platform_documents_path(char* out, size_t out_size)
{
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    SDL_snprintf(out, out_size, "%sPireneus Projects/", documents ? documents : "");
    SDL_CreateDirectory(out);
}

// SDL calls its dialog callbacks from another thread on some platforms : the result is parked here
// and delivered from the main loop (see platform_pump_dialogs()).
typedef struct
{
    FileDialogCallback callback;
    void* user_data;
    char** paths;
    int count;
    int ready;
} PendingDialog;

static PendingDialog pending;
static SDL_Mutex* pending_mutex;

static void SDLCALL on_dialog_result(void* user_data, const char* const* file_list, int filter)
{
    (void)user_data;
    (void)filter;

    SDL_LockMutex(pending_mutex);

    pending.count = 0;
    pending.paths = NULL;

    if (file_list)
    {
        int count = 0;

        while (file_list[count])
        {
            count++;
        }

        if (count > 0)
        {
            pending.paths = SDL_malloc(sizeof(char*) * (size_t)count);

            for (int i = 0; i < count; i++)
            {
                pending.paths[i] = SDL_strdup(file_list[i]);
            }

            pending.count = count;
        }
    }

    pending.ready = 1;

    SDL_UnlockMutex(pending_mutex);
}

void platform_pump_dialogs(void)
{
    if (!pending_mutex)
    {
        return;
    }

    SDL_LockMutex(pending_mutex);

    if (!pending.ready)
    {
        SDL_UnlockMutex(pending_mutex);
        return;
    }

    PendingDialog result = pending;
    SDL_zero(pending);

    SDL_UnlockMutex(pending_mutex);

    if (result.callback)
    {
        result.callback(result.user_data, result.count ? (const char* const*)result.paths : NULL, result.count);
    }

    for (int i = 0; i < result.count; i++)
    {
        SDL_free(result.paths[i]);
    }

    SDL_free(result.paths);
}

static void prepare(FileDialogCallback callback, void* user_data)
{
    if (!pending_mutex)
    {
        pending_mutex = SDL_CreateMutex();
    }

    SDL_LockMutex(pending_mutex);
    SDL_zero(pending);
    pending.callback = callback;
    pending.user_data = user_data;
    SDL_UnlockMutex(pending_mutex);
}

extern SDL_Window* window;

void platform_show_open_files_dialog(FileDialogCallback callback, void* user_data, const char* filter)
{
    prepare(callback, user_data);

    static char name[128];
    static char pattern[256];
    static SDL_DialogFileFilter filters[2];
    int filter_count = 0;

    if (filter)
    {
        const char* separator = SDL_strchr(filter, '|');

        if (separator)
        {
            SDL_snprintf(name, sizeof(name), "%.*s", (int)(separator - filter), filter);
            SDL_snprintf(pattern, sizeof(pattern), "%s", separator + 1);
            filters[0].name = name;
            filters[0].pattern = pattern;
            filter_count = 1;
        }
    }

    filters[filter_count].name = "All files";
    filters[filter_count].pattern = "*";
    filter_count++;

    SDL_ShowOpenFileDialog(on_dialog_result, NULL, window, filters, filter_count, NULL, true);
}

void platform_show_open_folder_dialog(FileDialogCallback callback, void* user_data)
{
    prepare(callback, user_data);
    SDL_ShowOpenFolderDialog(on_dialog_result, NULL, window, NULL, false);
}

int platform_move_to_trash(const char* path)
{
#ifdef _WIN32
    // The shell's API takes a double-null-terminated list of backslashed paths.
    wchar_t wide[1024];
    int length = MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1022);

    if (length <= 0)
    {
        return 0;
    }

    for (wchar_t* c = wide; *c; c++)
    {
        if (*c == L'/')
        {
            *c = L'\\';
        }
    }

    wide[length] = 0;

    SHFILEOPSTRUCTW operation;
    SDL_zero(operation);
    operation.wFunc = FO_DELETE;
    operation.pFrom = wide;
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted;
#else
    return SDL_RemovePath(path);
#endif
}
