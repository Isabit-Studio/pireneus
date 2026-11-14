// The Universal Windows Platform layer : a real UWP application (CoreApplication, CoreWindow, running in an
// AppContainer), implementing the same platform API as src/platforms/sdl. SDL3 removed WinRT support, so
// here it only provides what doesn't depend on a platform layer (surfaces, file I/O, strings...).
//
// The engine's programs have a blocking main loop ; CoreApplication hands control to IFrameworkView::Run()
// on the UI thread, which simply calls that main, and handle_events() pumps the CoreWindow's dispatcher.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <stdio.h>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "platforms/platforms_input.h"
#include "platforms/platforms_paths.h"
#include "platforms/platforms_window.h"
#include "platforms/uwp/uwp_events.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Devices::Input;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::ViewManagement;

extern "C" int pireneus_app_main(int argc, char** argv);

static CoreWindow core_window{ nullptr };
static bool window_closed;
static RedrawCallback redraw_callback;
static EventCallback event_callback;
static FileDropCallback file_drop_callback;
static int in_redraw_callback;
static float dpi_scale = 1.0f;

static bool keys[SCANCODE_COUNT];
static float mouse_dx;
static float mouse_dy;
static bool mouse_captured;
static event_token mouse_moved_token;

static void send_event(const UwpEvent& event)
{
    if (event_callback)
    {
        event_callback(&event);
    }
}

// PS/2 set 1 scancodes, what CoreWindow reports, to USB HID usage IDs, the engine's Scancode.
static const unsigned char ps2_to_hid[0x3A] = {
    0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,
    20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40, 224, 4, 22,
    7, 9, 10, 11, 13, 14, 15, 51, 52, 53, 225, 49, 29, 27, 6, 25,
    5, 17, 16, 54, 55, 56, 229, 0, 226, 44
};

static int to_scancode(unsigned int code, bool extended)
{
    if (extended)
    {
        switch (code)
        {
        case 0x1D: return SCANCODE_RCTRL;
        case 0x38: return SCANCODE_RALT;
        case 0x48: return SCANCODE_UP;
        case 0x50: return SCANCODE_DOWN;
        case 0x4B: return SCANCODE_LEFT;
        case 0x4D: return SCANCODE_RIGHT;
        default: return 0;
        }
    }

    return code < sizeof(ps2_to_hid) ? ps2_to_hid[code] : 0;
}

static void on_key(KeyEventArgs const& args, bool down)
{
    CorePhysicalKeyStatus status = args.KeyStatus();
    int scancode = to_scancode(status.ScanCode, status.IsExtendedKey);

    if (scancode > 0 && scancode < SCANCODE_COUNT)
    {
        keys[scancode] = down;
    }

    UwpEvent event = {};
    event.type = UWP_EVENT_KEY;
    event.down = down;
    event.virtual_key = (int)args.VirtualKey();
    send_event(event);
}

static void on_pointer(PointerEventArgs const& args)
{
    auto point = args.CurrentPoint();
    auto position = point.Position();

    UwpEvent event = {};
    event.type = UWP_EVENT_MOUSE_MOVE;
    event.x = position.X * dpi_scale;
    event.y = position.Y * dpi_scale;
    send_event(event);

    static bool buttons[3];
    auto properties = point.Properties();
    const bool now[3] = { properties.IsLeftButtonPressed(), properties.IsRightButtonPressed(), properties.IsMiddleButtonPressed() };

    for (int i = 0; i < 3; i++)
    {
        if (now[i] != buttons[i])
        {
            buttons[i] = now[i];

            UwpEvent button = {};
            button.type = UWP_EVENT_MOUSE_BUTTON;
            button.button = i;
            button.down = now[i];
            send_event(button);
        }
    }
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
    IFrameworkView CreateView() { return *this; }
    void Initialize(CoreApplicationView const&) {}
    void Load(hstring const&) {}
    void Uninitialize() {}

    void SetWindow(CoreWindow const& window)
    {
        core_window = window;

        window.Closed([](auto&&, auto&&) { window_closed = true; });
        window.KeyDown([](auto&&, KeyEventArgs const& args) { on_key(args, true); });
        window.KeyUp([](auto&&, KeyEventArgs const& args) { on_key(args, false); });

        window.CharacterReceived([](auto&&, CharacterReceivedEventArgs const& args)
        {
            UwpEvent event = {};
            event.type = UWP_EVENT_CHARACTER;
            event.character = args.KeyCode();
            send_event(event);
        });

        window.PointerMoved([](auto&&, PointerEventArgs const& args) { on_pointer(args); });
        window.PointerPressed([](auto&&, PointerEventArgs const& args) { on_pointer(args); });
        window.PointerReleased([](auto&&, PointerEventArgs const& args) { on_pointer(args); });

        window.PointerWheelChanged([](auto&&, PointerEventArgs const& args)
        {
            UwpEvent event = {};
            event.type = UWP_EVENT_MOUSE_WHEEL;
            event.wheel = (float)args.CurrentPoint().Properties().MouseWheelDelta() / 120.0f;
            send_event(event);
        });

        window.Activated([](auto&&, WindowActivatedEventArgs const& args)
        {
            UwpEvent event = {};
            event.type = UWP_EVENT_FOCUS;
            event.down = args.WindowActivationState() != CoreWindowActivationState::Deactivated;
            send_event(event);
        });

        window.SizeChanged([](auto&&, auto&&) { call_redraw_callback(); });

        DisplayInformation display = DisplayInformation::GetForCurrentView();
        dpi_scale = (float)display.RawPixelsPerViewPixel();
        display.DpiChanged([](DisplayInformation const& information, auto&&) { dpi_scale = (float)information.RawPixelsPerViewPixel(); });
    }

    void Run()
    {
        core_window.Activate();

        // Nothing is attached to stdout in an AppContainer : the engine's messages go to a file.
        std::wstring log = std::wstring(ApplicationData::Current().LocalFolder().Path().c_str()) + L"\\log.txt";
        FILE* unused = NULL;
        _wfreopen_s(&unused, log.c_str(), L"w", stdout);
        setvbuf(stdout, NULL, _IONBF, 0);

        pireneus_app_main(0, NULL);
    }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    init_apartment();
    CoreApplication::Run(make<App>());
    return 0;
}

// --- platforms_window.h -------------------------------------------------------------------------

extern "C" {

void platform_uwp_pump_results(void);

int create_window(const char* title, int width, int height)
{
    ApplicationView view = ApplicationView::GetForCurrentView();
    view.Title(to_hstring(title));
    view.SetPreferredMinSize(Size(320.0f, 240.0f));
    view.TryResizeView(Size((float)width / dpi_scale, (float)height / dpi_scale));

    // Let the window come up with its real size before the swapchain is created.
    core_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

    return 1;
}

int handle_events(void)
{
    mouse_dx = 0.0f;
    mouse_dy = 0.0f;

    platform_uwp_pump_results();
    core_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

    return window_closed ? 0 : 1;
}

void destroy_window(void)
{
    core_window = nullptr;
}

Window* get_window(void)
{
    return reinterpret_cast<Window*>(winrt::get_abi(core_window));
}

void get_window_size(int* out_width, int* out_height)
{
    Rect bounds = core_window.Bounds();
    *out_width = (int)(bounds.Width * dpi_scale + 0.5f);
    *out_height = (int)(bounds.Height * dpi_scale + 0.5f);
}

void set_window_title(const char* title)
{
    ApplicationView::GetForCurrentView().Title(to_hstring(title));
}

void set_redraw_callback(RedrawCallback callback)
{
    redraw_callback = callback;
}

void set_event_callback(EventCallback callback)
{
    event_callback = callback;
}

void set_file_drop_callback(FileDropCallback callback)
{
    file_drop_callback = callback;
}

void call_redraw_callback(void)
{
    if (redraw_callback == NULL || in_redraw_callback)
    {
        return;
    }

    in_redraw_callback = 1;
    redraw_callback();
    in_redraw_callback = 0;
}

// --- platforms_input.h --------------------------------------------------------------------------

int input_is_key_down(Scancode scancode)
{
    return scancode > SCANCODE_UNKNOWN && scancode < SCANCODE_COUNT && keys[scancode];
}

const char* input_key_label(Scancode scancode)
{
    // The US layout's name : CoreWindow has no way to ask what a physical key is labeled.
    static char labels[16][8];
    static int next_label;

    char* label = labels[next_label];
    next_label = (next_label + 1) % 16;

    if (scancode >= SCANCODE_A && scancode <= SCANCODE_Z)
    {
        SDL_snprintf(label, sizeof(labels[0]), "%c", 'A' + (int)(scancode - SCANCODE_A));
    }
    else
    {
        label[0] = '\0';
    }

    return label;
}

void input_set_mouse_captured(int captured)
{
    if (captured != 0 == mouse_captured)
    {
        return;
    }

    mouse_captured = captured != 0;

    MouseDevice device = MouseDevice::GetForCurrentView();

    if (mouse_captured)
    {
        core_window.PointerCursor(nullptr);
        mouse_moved_token = device.MouseMoved([](auto&&, MouseEventArgs const& args)
        {
            mouse_dx += (float)args.MouseDelta().X;
            mouse_dy += (float)args.MouseDelta().Y;
        });
    }
    else
    {
        device.MouseMoved(mouse_moved_token);
        core_window.PointerCursor(CoreCursor(CoreCursorType::Arrow, 0));
    }
}

void input_get_mouse_delta(float* out_dx, float* out_dy)
{
    *out_dx = mouse_dx;
    *out_dy = mouse_dy;
}

} // extern "C"

// --- platforms_paths.h --------------------------------------------------------------------------

static std::string local_folder(void)
{
    return to_string(ApplicationData::Current().LocalFolder().Path());
}

// Results of the asynchronous pickers, delivered from handle_events().
static FileDialogCallback pending_callback;
static void* pending_user_data;
static std::vector<std::string> pending_paths;
static bool pending_ready;

extern "C" void platform_uwp_pump_results(void)
{
    if (!pending_ready)
    {
        return;
    }

    pending_ready = false;

    std::vector<const char*> pointers;

    for (const std::string& path : pending_paths)
    {
        pointers.push_back(path.c_str());
    }

    if (pending_callback)
    {
        pending_callback(pending_user_data, pointers.empty() ? NULL : pointers.data(), (int)pointers.size());
    }

    pending_paths.clear();
}

// The picked files can only be reached through their StorageFile : copied to the app's own folder, from
// where the engine's file functions can read them.
static fire_and_forget pick_files(void)
{
    FileOpenPicker picker;
    picker.FileTypeFilter().Append(L"*");

    auto files = co_await picker.PickMultipleFilesAsync();

    StorageFolder imports = co_await ApplicationData::Current().TemporaryFolder().CreateFolderAsync(L"Imports", CreationCollisionOption::OpenIfExists);

    for (StorageFile const& file : files)
    {
        StorageFile copy = co_await file.CopyAsync(imports, file.Name(), NameCollisionOption::ReplaceExisting);
        pending_paths.push_back(to_string(copy.Path()));
    }

    pending_ready = true;
}

extern "C" {

void platform_user_data_path(char* out, size_t out_size)
{
    SDL_snprintf(out, out_size, "%s\\", local_folder().c_str());
}

void platform_documents_path(char* out, size_t out_size)
{
    SDL_snprintf(out, out_size, "%s\\Projects\\", local_folder().c_str());
    SDL_CreateDirectory(out);
}

void platform_show_open_files_dialog(FileDialogCallback callback, void* user_data, const char* filter)
{
    (void)filter;

    pending_callback = callback;
    pending_user_data = user_data;
    pending_paths.clear();
    pick_files();
}

// A sandboxed app can only keep projects in its own folder : there's no other place to pick.
void platform_show_open_folder_dialog(FileDialogCallback callback, void* user_data)
{
    pending_callback = callback;
    pending_user_data = user_data;
    pending_paths.clear();
    pending_ready = true;
}

int platform_move_to_trash(const char* path)
{
    return SDL_RemovePath(path);
}

} // extern "C"
