# Every example in one executable, picked from a Dear ImGui menu (launcher/main.cpp). Each example
# stays in its own folder, exposing an Example descriptor (example.h).
add_executable(pireneus-examples
    launcher/main.cpp
    hello-triangle/main.c
    2d-test/main.c
    pixel-perfect/main.c
    tilemap-editor/main.cpp
    3d-test/main.c
    photoreal/main.c
    scene-editor/main.cpp
    scene-editor/panels.cpp
    scene-editor/asset_browser.cpp
    scene-editor/level_editor.cpp
)
target_include_directories(pireneus-examples PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pireneus-examples PRIVATE pireneus_imgui imguizmo)
pireneus_copy_shaders(pireneus-examples)

# The resources the examples load at runtime, and the Lucide icon font (+ its ISC license notice).
add_custom_command(TARGET pireneus-examples POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/resources
        $<TARGET_FILE_DIR:pireneus-examples>/resources
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:pireneus-examples>/resources/fonts
    COMMAND ${CMAKE_COMMAND} -E copy ${PIRENEUS_LUCIDE_FONT} $<TARGET_FILE_DIR:pireneus-examples>/resources/fonts/lucide.ttf
    COMMAND ${CMAKE_COMMAND} -E copy ${PIRENEUS_LUCIDE_LICENSE} $<TARGET_FILE_DIR:pireneus-examples>/resources/fonts/lucide-LICENSE.txt
)

# On UWP : an AppContainer executable, and its package layout next to it. Deploy that layout with
# Add-AppxPackage -Register (developer mode), or pack it with makeappx and sign it for distribution.
if(PIRENEUS_UWP)
    set_target_properties(pireneus-examples PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(pireneus-examples PRIVATE /APPCONTAINER /ENTRY:wWinMainCRTStartup)

    set(PIRENEUS_UWP_PUBLISHER "CN=Pireneus" CACHE STRING "The package's publisher : the subject of the certificate that signs it")
    set(layout ${CMAKE_BINARY_DIR}/uwp_layout)
    configure_file(${CMAKE_SOURCE_DIR}/cmake/uwp/AppxManifest.xml.in ${CMAKE_BINARY_DIR}/AppxManifest.xml @ONLY)

    add_custom_command(TARGET pireneus-examples POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${layout}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:pireneus-examples> ${layout}/
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/AppxManifest.xml ${layout}/
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/cmake/uwp/Logos ${layout}/Logos
        COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:pireneus-examples>/shaders ${layout}/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_FILE_DIR:pireneus-examples>/resources ${layout}/resources
    )
endif()
