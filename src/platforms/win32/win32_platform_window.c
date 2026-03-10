#include "../platforms_window.h"
#include <stdio.h>
#include <windows.h>

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
HWND hwnd;

int create_window(const char* title, int width, int height)
{
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "PireneusWindowClass";

    if (!RegisterClass(&wc))
    {
        printf("Failed to register window class.\n");
        return 0;
    }

    hwnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        NULL, NULL, wc.hInstance, NULL
    );

    if (hwnd == NULL)
    {
        printf("Failed to create window.\n");
        return 0;
    }

    ShowWindow(hwnd, SW_SHOW);
    return 1;
}

int handle_events()
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            return 0;
        }
        
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 1;
}

void destroy_window()
{
    DestroyWindow(hwnd);
}

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}