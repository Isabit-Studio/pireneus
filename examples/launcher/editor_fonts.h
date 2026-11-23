#ifndef _EDITOR_FONTS_H_
#define _EDITOR_FONTS_H_

// Dear ImGui's default font with the Lucide icon font merged in, so icons mix with text in any label.
//
// Lucide codepoints (UTF-8) come from lucide-static 1.59.0's lucide.css : they're specific to that
// version, which third-parties/CMakeLists.txt pins.

#include <stdio.h>

#include <SDL3/SDL.h>

#include "imgui.h"

#define ICON_BOX "\xee\x81\xa1"         // U+E061
#define ICON_LIGHTBULB "\xee\x87\x82"   // U+E1C2
#define ICON_FLASHLIGHT "\xee\x83\x93"  // U+E0D3
#define ICON_CONE "\xee\x94\xa3"        // U+E523
#define ICON_CYLINDER "\xee\x94\xa5"    // U+E525
#define ICON_TORUS "\xee\x94\xaf"       // U+E52F
#define ICON_SQUARE "\xee\x85\xa7"      // U+E167
#define ICON_CIRCLE "\xee\x81\xb6"      // U+E076
#define ICON_GLOBE "\xee\x83\xa8"       // U+E0E8
#define ICON_MOVE_3D "\xee\x8b\xa5"     // U+E2E5
#define ICON_ROTATE_3D "\xee\x8b\xaa"   // U+E2EA
#define ICON_SCALE_3D "\xee\x8b\xab"    // U+E2EB
#define ICON_TRASH "\xee\x86\x8e"       // U+E18E
#define ICON_COPY "\xee\x82\x9e"        // U+E09E
#define ICON_SAVE "\xee\x85\x8d"        // U+E14D
#define ICON_FOLDER_OPEN "\xee\x89\x87" // U+E247
#define ICON_UNDO "\xee\x8a\xa1"        // U+E2A1
#define ICON_REDO "\xee\x8a\xa0"        // U+E2A0
#define ICON_MAGNET "\xee\x8a\xb5"      // U+E2B5
#define ICON_CROSSHAIR "\xee\x82\xac"   // U+E0AC
#define ICON_EARTH "\xee\x87\xb3"       // U+E1F3
#define ICON_AXIS_3D "\xee\x8b\xbe"     // U+E2FE
#define ICON_SHAPES "\xee\x92\xb3"      // U+E4B3
#define ICON_ARROW_LEFT "\xee\x81\x88"  // U+E048
#define ICON_PLAY "\xee\x84\xbc" // U+E13C
#define ICON_PAUSE "\xee\x84\xae" // U+E12E
#define ICON_STEP_FORWARD "\xee\x8f\xaa" // U+E3EA
#define ICON_USER "\xee\x86\x9f" // U+E19F
#define ICON_IMAGE "\xee\x83\xb6" // U+E0F6
#define ICON_VOLUME "\xee\x86\xab" // U+E1AB
#define ICON_PLUS "\xee\x84\xbd" // U+E13D
#define ICON_FOLDER "\xee\x83\x97" // U+E0D7
#define ICON_REFRESH "\xee\x85\x85" // U+E145
#define ICON_SEARCH "\xee\x85\x91" // U+E151
#define ICON_GRID "\xee\x83\xa9" // U+E0E9
#define ICON_FILE "\xee\x83\x80" // U+E0C0
#define ICON_FOLDER_PLUS "\xee\x83\x99" // U+E0D9
#define ICON_ROCKET "\xee\x8a\x86" // U+E286
#define ICON_LAYERS "\xee\x94\xa9" // U+E529
#define ICON_EYE "\xee\x82\xba"             // U+E0BA
#define ICON_EYE_OFF "\xee\x82\xbb"         // U+E0BB
#define ICON_SCISSORS "\xee\x85\x8e"        // U+E14E
#define ICON_POINTER "\xee\x87\x83"         // U+E1C3 mouse-pointer-2
#define ICON_VERTEX "\xee\x8d\x85"          // U+E345 circle-dot
#define ICON_EDGE "\xee\x95\x92"            // U+E552 git-commit-vertical
#define ICON_FACE "\xee\x99\xbc"            // U+E67C vector-square
#define ICON_MESH "\xee\x94\xa4"            // U+E524 cuboid
#define ICON_BLOCK "\xee\x93\xba"           // U+E4FA blocks
#define ICON_PAINT "\xee\x8b\xa6"           // U+E2E6 paint-bucket
#define ICON_BRICK "\xee\x96\x81"           // U+E581 brick-wall
#define ICON_VIEWS "\xee\x84\xac"           // U+E12C panels-top-left
#define ICON_MERGE "\xee\x91\x8c"           // U+E44C combine
#define ICON_SPLIT "\xee\x8e\xb6"           // U+E3B6 square-split-horizontal
#define ICON_FLIP "\xee\x8d\xa0"            // U+E360 flip-vertical-2
#define ICON_EXTRUDE "\xee\x91\x8e"         // U+E44E arrow-up-from-dot
#define ICON_INSET "\xee\x88\xa0"           // U+E220 shrink
#define ICON_MINUS "\xee\x84\x9c"           // U+E11C

#define UI_FONT_SIZE 13.0f
#define FONT_PATH "resources/fonts/lucide.ttf"

static inline void launcher_setup_fonts(void)
{
    ImGuiIO& io = ImGui::GetIO();

    // An explicit reference size for the default font : merging a font with glyph offsets/advances
    // into it (below) requires both to have one.
    ImFontConfig default_config;
    default_config.SizePixels = UI_FONT_SIZE;
    io.Fonts->AddFontDefault(&default_config);

    char path[512];
    const char* base_path = SDL_GetBasePath();
    SDL_snprintf(path, sizeof(path), "%s%s", base_path ? base_path : "", FONT_PATH);

    ImFontConfig config;
    config.MergeMode = true;
    config.GlyphMinAdvanceX = UI_FONT_SIZE;
    config.GlyphOffset = ImVec2(0.0f, 2.0f);

    if (!io.Fonts->AddFontFromFileTTF(path, UI_FONT_SIZE, &config))
    {
        printf("Could not load the icon font '%s' : icons won't show.\n", path);
    }
}

#endif // _EDITOR_FONTS_H_
