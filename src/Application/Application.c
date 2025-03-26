// MIT License
// 
// Copyright (c) 2024-2025 Isabit Studio
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "Application.h"
#include <string.h>

// Create a new application memory safe
Application* Application_Create(const char* name, int x, int y, int width, int height)
{
	Application* app = malloc(sizeof(Application));
	// Check if the memory was allocated
	NULL_PANIC(app);
	app->name = name;
	app->size.x = x;
	app->size.y = y;
	app->size.width = width;
	app->size.height = height;

	// Window class
	WNDCLASS wc = { 0 };
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = name;
	RegisterClass(&wc);

	// Create window
	app->handle = CreateWindow(name, name, WS_OVERLAPPEDWINDOW, x, y, width, height, NULL, NULL, GetModuleHandle(NULL), NULL);
	// Check if the window was created
	if (app->handle == NULL)
	{
		// Destroy the application
		Application_Destroy(app);
		return NULL;
	}

	// Show the window
	ShowWindow(app->handle, SW_SHOW);

	return app;
}

// Message loop
void Application_Run(Application* app)
{
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
}


// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
		// check if the window is being destroyed
	case WM_CLOSE:
		DestroyWindow(hwnd);
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Destroy an application memory safe
void Application_Destroy(Application* app)
{
	free(app);
}
