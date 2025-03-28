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

#ifndef _H_APPLICATION
#define _H_APPLICATION

#include <stdlib.h>
#include <stdio.h>

#include <Windows.h>
// Remove unused functions
#define WIN32_LEAN_AND_MEAN

#include "../Math/Rectangle.h"
#include "../Utils/Macros.h"

// Application struct
typedef struct Application
{
	// Window name
	const char* name;
	// Window size
	struct Rectangle size;

	// Window handle
	HWND handle;
} Application;

// Create a new application
Application* Application_Create(const char* name, int x, int y, int width, int height);

// Message loop
void Application_Run(Application* app);

// Window procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Destroy an application
void Application_Destroy(Application* app);


#endif // !_H_APPLICATION
