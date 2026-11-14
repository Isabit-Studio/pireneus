# Compiles the engine's shared Slang shader sources (src/renderer/shaders/*.slang) once, to
# ${CMAKE_BINARY_DIR}/shaders. Every example target copies that shared output next to itself
# with pireneus_copy_shaders(), instead of each recompiling its own copy.
#
# Each entry point of each source is compiled twice, from the same .slang file :
# - SPIR-V (Vulkan) : Slang's own backend, generated directly from its IR, no extra dependency.
# - DXIL (DX12) : Slang transpiles to HLSL text (its own backend, no dxc/dxcompiler.dll involved at
#   this step), then dxc.exe (located from the Windows SDK) compiles that generated HLSL to DXIL —
#   Slang's release doesn't bundle dxcompiler.dll, so its own DXIL backend isn't used.

# One shader entry point : "<source>,<entry point>,<stage>,<spv output name>,<dxc profile>,<cso output name>".
# The output names are what vulkan_renderer.c / dx12_renderer.c load at runtime.
set(PIRENEUS_SHADER_ENTRY_POINTS
    "sprite.slang,vertexMain,vertex,sprite.vert.spv,vs_6_0,sprite_vs.cso"
    "sprite.slang,fragmentMain,fragment,sprite.frag.spv,ps_6_0,sprite_ps.cso"
    "mesh.slang,shadowVertexMain,vertex,mesh_shadow.vert.spv,vs_6_0,mesh_shadow_vs.cso"
    "mesh.slang,sceneVertexMain,vertex,mesh.vert.spv,vs_6_0,mesh_vs.cso"
    "mesh.slang,sceneFragmentMain,fragment,mesh.frag.spv,ps_6_0,mesh_ps.cso"
    "mesh.slang,skyVertexMain,vertex,sky.vert.spv,vs_6_0,sky_vs.cso"
    "mesh.slang,skyFragmentMain,fragment,sky.frag.spv,ps_6_0,sky_ps.cso"
    "photoreal.slang,prShadowVs,vertex,pr_shadow_vs.spv,vs_6_0,pr_shadow_vs.cso"
    "photoreal.slang,prShadowMaskedVs,vertex,pr_shadow_masked_vs.spv,vs_6_0,pr_shadow_masked_vs.cso"
    "photoreal.slang,prShadowMaskedPs,fragment,pr_shadow_masked_ps.spv,ps_6_0,pr_shadow_masked_ps.cso"
    "photoreal.slang,prVisibilityVs,vertex,pr_visibility_vs.spv,vs_6_0,pr_visibility_vs.cso"
    "photoreal.slang,prVisibilityPs,fragment,pr_visibility_ps.spv,ps_6_0,pr_visibility_ps.cso"
    "photoreal.slang,prScreenVs,vertex,pr_screen_vs.spv,vs_6_0,pr_screen_vs.cso"
    "photoreal.slang,prAoPs,fragment,pr_ao_ps.spv,ps_6_0,pr_ao_ps.cso"
    "photoreal.slang,prAoBlurPs,fragment,pr_ao_blur_ps.spv,ps_6_0,pr_ao_blur_ps.cso"
    "photoreal.slang,prShadePs,fragment,pr_shade_ps.spv,ps_6_0,pr_shade_ps.cso"
    "photoreal.slang,prBloomDownPs,fragment,pr_bloom_down_ps.spv,ps_6_0,pr_bloom_down_ps.cso"
    "photoreal.slang,prBloomUpPs,fragment,pr_bloom_up_ps.spv,ps_6_0,pr_bloom_up_ps.cso"
    "photoreal.slang,prTonemapPs,fragment,pr_tonemap_ps.spv,ps_6_0,pr_tonemap_ps.cso"
    "photoreal.slang,prFxaaPs,fragment,pr_fxaa_ps.spv,ps_6_0,pr_fxaa_ps.cso"
    "photoreal.slang,prPathTracePs,fragment,pr_pathtrace_ps.spv,ps_6_5,pr_pathtrace_ps.cso"
)

function(pireneus_compile_shaders)
    set(shader_binary_dir ${CMAKE_BINARY_DIR}/shaders)
    set(shader_source_dir ${CMAKE_SOURCE_DIR}/src/renderer/shaders)
    set(generated_dir ${CMAKE_BINARY_DIR}/generated_shaders)
    set(outputs "")

    # Slang is only fetched on Windows for now (see third-parties/CMakeLists.txt). This means Vulkan
    # builds on other platforms currently get no compiled shaders at all ; a warning makes that
    # limitation visible rather than silently producing a binary with no shaders.
    if(NOT WIN32)
        message(WARNING "Shader compilation (Slang) is only wired up on Windows for now ; Vulkan builds on this platform will have no compiled shaders.")
        add_custom_target(pireneus_shaders)
        return()
    endif()

    set(slangc_executable ${PIRENEUS_SLANGC_EXECUTABLE})

    file(GLOB dxc_candidates "$ENV{WindowsSdkDir}bin/*/x64/dxc.exe")
    if(NOT dxc_candidates)
        file(GLOB dxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe")
    endif()
    if(NOT dxc_candidates)
        message(FATAL_ERROR "Could not find dxc.exe (DirectX Shader Compiler) from the Windows SDK.")
    endif()
    list(SORT dxc_candidates)
    list(GET dxc_candidates -1 dxc_executable)
    message(STATUS "Using dxc: ${dxc_executable}")

    foreach(entry ${PIRENEUS_SHADER_ENTRY_POINTS})
        string(REPLACE "," ";" entry "${entry}")
        list(GET entry 0 source_name)
        list(GET entry 1 entry_point)
        list(GET entry 2 stage)
        list(GET entry 3 spv_name)
        list(GET entry 4 dxc_profile)
        list(GET entry 5 cso_name)

        set(shader_source ${shader_source_dir}/${source_name})

        # SPIR-V (Vulkan)
        set(spv_output ${shader_binary_dir}/${spv_name})

        add_custom_command(
            OUTPUT ${spv_output}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_binary_dir}
            COMMAND ${slangc_executable} ${shader_source} -entry ${entry_point} -stage ${stage} -target spirv -profile spirv_1_5 -warnings-disable 41012 -o ${spv_output}
            DEPENDS ${shader_source}
            VERBATIM
        )

        # HLSL text (Slang), then DXIL (dxc)
        get_filename_component(cso_base ${cso_name} NAME_WE)
        set(generated_hlsl ${generated_dir}/${cso_base}.gen.hlsl)
        set(cso_output ${shader_binary_dir}/${cso_name})

        add_custom_command(
            OUTPUT ${generated_hlsl}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${generated_dir}
            COMMAND ${slangc_executable} ${shader_source} -entry ${entry_point} -stage ${stage} -target hlsl -o ${generated_hlsl}
            DEPENDS ${shader_source}
            VERBATIM
        )

        add_custom_command(
            OUTPUT ${cso_output}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_binary_dir}
            COMMAND ${dxc_executable} -T ${dxc_profile} -E ${entry_point} ${generated_hlsl} -Fo ${cso_output}
            DEPENDS ${generated_hlsl}
            VERBATIM
        )

        list(APPEND outputs ${spv_output} ${cso_output})
    endforeach()

    add_custom_target(pireneus_shaders DEPENDS ${outputs})
endfunction()

# Copies the shared compiled shaders next to TARGET's own output, after TARGET is built.
function(pireneus_copy_shaders TARGET)
    add_dependencies(${TARGET} pireneus_shaders)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/shaders $<TARGET_FILE_DIR:${TARGET}>/shaders
    )
endfunction()
