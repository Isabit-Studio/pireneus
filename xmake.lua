-- MIT License
-- 
-- Copyright (c) 2024 Isabit Studio
-- 
-- Permission is hereby granted, free of charge, to any person obtaining a copy
-- of this software and associated documentation files (the "Software"), to deal
-- in the Software without restriction, including without limitation the rights
-- to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
-- copies of the Software, and to permit persons to whom the Software is
-- furnished to do so, subject to the following conditions:
-- 
-- The above copyright notice and this permission notice shall be included in all
-- copies or substantial portions of the Software.
-- 
-- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
-- IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
-- FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
-- AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
-- LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
-- OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
-- SOFTWARE.

-- This is the project configuration file for the pireneus project.

-- Rules
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"}) -- VSCode compile commands for Intellisense and Clangd
add_rules("mode.debug", "mode.release") -- Add debug and release modes

-- Options
option("NoConsole", {default = false, showmenu = true, description = "(Windows Only) Disable console"}) -- Disable console on Windows

-- Repositories
add_repositories("Isabit-Studio_public https://github.com/Isabit-Studio/xmake-repo") -- Isabit Studio public repository

-- Dependencies
add_requires("libsdl", {configs = { shared = true }}) -- SDL2 library

if is_plat("windows") then
    if has_config("NoConsole") then
        add_ldflags("/SUBSYSTEM:WINDOWS") -- Set the subsystem to windows
    else
        add_ldflags("/SUBSYSTEM:CONSOLE") -- Set the subsystem to console
    end
end

-- Project
target("pireneus")
    set_kind("binary")
    add_files("src/*.c")
    add_packages("libsdl") -- Link with SDL2 library
    set_license("MIT") -- Set the license to MIT