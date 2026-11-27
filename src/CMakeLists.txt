cmake_minimum_required(VERSION 3.24)
project(omsi-f4-engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OMSI_BUILD_VULKAN "Build Vulkan backend" OFF)
set(OMSI_ULTRALIGHT_ROOT "" CACHE PATH "Ultralight SDK root (include/lib/bin)")

find_package(glfw3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_library(omsi_core
    src/core/logger.cpp
    src/platform/win32_shell_open.cpp
    src/core/http_client.cpp
    src/core/options_loader.cpp
    src/cfg/cfg_parser.cpp
    src/o3d/o3d_loader.cpp
    src/o3d/o3d_writer.cpp
    src/map/omsi_map_loader.cpp
    src/map/map_attachment.cpp
    src/map/map_tile_layout.cpp
    src/map/map_tile_editor.cpp
    src/map/map_geo_tools.cpp
    src/map/dem_geotiff.cpp
    src/map/editor_map_workspace.cpp
    src/tt/tt_loader.cpp
    src/tt/tt_man.cpp
    src/tt/tt_editor_ui.cpp
    src/sco/sco_loader.cpp
    src/sco/oft_font.cpp
    src/sco/crossing_height_deform.cpp
    src/x/x_mesh_loader.cpp
    src/model/model_cfg_loader.cpp
    src/model/visible_runtime.cpp
    src/model/mesh_animator.cpp
    src/script/var_registry.cpp
    src/script/constfile_loader.cpp
    src/script/osc_runtime.cpp
    src/script/osc_vm.cpp
    src/script/map_scenery_script.cpp
    src/script/vehicle_state_logger.cpp
    src/script/vehicle_runtime.cpp
    src/sound/sound_runtime.cpp
    src/sound/editor_sfx.cpp
    src/test/vehicle_test_runner.cpp
    src/vehicle/bus_loader.cpp
    src/vehicle/bus_catalog.cpp
    src/vehicle/cabin_paths_loader.cpp
    src/vehicle/cti_loader.cpp
    src/input/vehicle_input_bindings.cpp
    src/spline/sli_loader.cpp
    src/spline/sli_writer.cpp
    src/spline/spline_geometry.cpp
    src/world/world_loader.cpp
    src/world/world_load_job.cpp
    src/world/world_height.cpp
    src/input/fly_camera.cpp
    src/input/editor_camera.cpp
    src/input/orbit_camera.cpp
    src/input/luna_camera.cpp
    src/input/omsi_bus_camera.cpp
    src/input/omsi_inputs_loader.cpp
    src/input/omsi_view_input_bindings.cpp
    src/editor/editor_sli_bake.cpp
    src/editor/editor_ground.cpp
    src/editor/editor_place_fx.cpp
    src/editor/terrain_polycut.cpp
    src/editor/editor_omsi_tools.cpp
    src/editor/editor_tree_billboard.cpp
    src/gfx/gfx_factory.cpp
    src/gfx/gfx_backend.cpp
    src/gfx/debug_mesh.cpp
    src/gfx/dx12/common/dx12_common.cpp
    src/gfx/dx12/cube/device.cpp
    src/gfx/dx12/cube/frame.cpp
    src/gfx/dx12/cube/geometry.cpp
    src/gfx/dx12/cube/init.cpp
    src/gfx/dx12/cube/pipeline.cpp
    src/gfx/dx12/o3d/chunk.cpp
    src/gfx/dx12/o3d/csm.cpp
    src/gfx/dx12/o3d/device.cpp
    src/gfx/dx12/o3d/draw.cpp
    src/gfx/dx12/o3d/frame.cpp
    src/gfx/dx12/o3d/fxaa_post.cpp
    src/gfx/dx12/o3d/ground.cpp
    src/gfx/dx12/o3d/init.cpp
    src/gfx/dx12/o3d/mesh.cpp
    src/gfx/dx12/o3d/pick.cpp
    src/gfx/dx12/o3d/pipeline.cpp
    src/gfx/dx12/o3d/screenshot.cpp
    src/gfx/dx12/o3d/sky.cpp
    src/gfx/dx12/o3d/visibility.cpp
    src/gfx/dx12/renderer/command.cpp
    src/gfx/dx12/renderer/depth.cpp
    src/gfx/dx12/renderer/device.cpp
    src/gfx/dx12/renderer/draw.cpp
    src/gfx/dx12/renderer/frame.cpp
    src/gfx/dx12/renderer/init.cpp
    src/gfx/dx12/renderer/mesh_upload.cpp
    src/gfx/dx12/renderer/overlay.cpp
    src/gfx/dx12/renderer/pipeline.cpp
    src/gfx/dx12/renderer/screenshot.cpp
    src/gfx/dx12/renderer/shutdown.cpp
    src/gfx/dx12/renderer/srv_heap.cpp
    src/gfx/dx12/renderer/swapchain.cpp
    src/gfx/dx12/texture/dds_decode.cpp
    src/gfx/dx12/texture/load_dds.cpp
    src/gfx/dx12/texture/load_file.cpp
    src/gfx/dx12/texture/load_tga.cpp
    src/gfx/dx12/texture/load_wic.cpp
    src/gfx/dx12/texture/load_cpu.cpp
    src/gfx/dx12/texture/resolve.cpp
    src/gfx/dx12/texture/upload.cpp

    src/ui/launcher.cpp
    src/ui/bus_picker.cpp
    src/ui/loading_screen.cpp
    src/ui/editor_side_panel_build.cpp
    src/ui/editor_side_panel_views.cpp
    src/ui/editor_side_panel_handlers.cpp
    src/ui/editor_side_panel_win32.cpp
    src/ui/editor_prop_dialogs.cpp
    src/ui/spline_creator_dialog.cpp
    src/ui/editor_adjust_hud.cpp
    src/ui/editor_map_text_editor.cpp
    src/ui/editor_session_log.cpp
    src/ui/imgui_overlay.cpp
    src/ui/editor_side_panel_imgui.cpp
    src/ui/map_editor_imgui_panel.cpp
    src/ui/path_editor_panel.cpp
    src/ui/win32_dark.cpp
    src/ui/editor_shortcuts.cpp
    src/ui/ultralight_overlay.cpp
    src/ui/ultralight_html_dialog.cpp
    src/ui/map_editor_ultralight_panel.cpp
    src/ui/detail/imgui_impl_win32.cpp
)

target_include_directories(omsi_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniz
)

add_library(omsi_miniz STATIC ../third_party/miniz/miniz.c)
target_include_directories(omsi_miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniz)

target_link_libraries(omsi_core PUBLIC glfw imgui::imgui omsi_miniz)

if(WIN32)
    target_link_libraries(omsi_core PUBLIC d3d12 dxgi d3dcompiler comctl32 winhttp windowscodecs ole32 winmm)
    if(EXISTS "${OMSI_ULTRALIGHT_ROOT}/include/Ultralight/Ultralight.h")
        target_compile_definitions(omsi_core PUBLIC OMSI_ULTRALIGHT_UI=1)
        # Hot-reloadable HTML/JS UI (edit assets, press F5 — no C++ rebuild).
        string(REPLACE "\\" "/" OMSI_UI_ASSETS_DIR_NORM "${CMAKE_SOURCE_DIR}/assets/ui/map_editor")
        target_compile_definitions(omsi_core PUBLIC "OMSI_UI_ASSETS_DIR=\"${OMSI_UI_ASSETS_DIR_NORM}\"")
        target_include_directories(omsi_core PUBLIC "${OMSI_ULTRALIGHT_ROOT}/include")
        target_link_directories(omsi_core PUBLIC "${OMSI_ULTRALIGHT_ROOT}/lib")
        target_link_libraries(omsi_core PUBLIC AppCore Ultralight UltralightCore WebCore)
    else()
        message(WARNING "Ultralight SDK not found at ${OMSI_ULTRALIGHT_ROOT} — map editor web UI disabled")
    endif()
endif()

if(OMSI_BUILD_VULKAN)
    find_package(Vulkan REQUIRED)
    target_compile_definitions(omsi_core PUBLIC OMSI_GFX_VULKAN)
    target_sources(omsi_core PRIVATE src/gfx/vulkan/vk_renderer.cpp)
    target_link_libraries(omsi_core PUBLIC Vulkan::Vulkan)
endif()

add_executable(omsi_f4_viewer
    src/app/crash_report.cpp
    src/app/main.cpp
    src/app/hello_cube_app.cpp
    src/app/hello_o3d_app.cpp
    src/app/map_editor_app.cpp
    src/app/map_editor_math.cpp
    src/app/spline_creator_app.cpp
    src/app/seh_invoke.cpp
)
target_link_libraries(omsi_f4_viewer PRIVATE omsi_core)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" OMSI_F4_SRC_DIR)
target_compile_definitions(omsi_f4_viewer PRIVATE "OMSI_F4_SOURCE_DIR=\"${OMSI_F4_SRC_DIR}\"")
if(WIN32)
    target_sources(omsi_f4_viewer PRIVATE src/app/omsi_f4_viewer.manifest)
endif()

add_executable(omsi_spline_creator WIN32
    src/app/spline_creator_main.cpp
    src/app/spline_creator_app.cpp
)
target_link_libraries(omsi_spline_creator PRIVATE omsi_core)
if(WIN32)
    target_sources(omsi_spline_creator PRIVATE src/app/omsi_f4_viewer.manifest)
endif()

if(MSVC)
    target_compile_options(omsi_core PRIVATE /W4 "/permissive-" /utf-8 /MP /Zc:inline)
    target_compile_options(omsi_f4_viewer PRIVATE /W4 "/permissive-" /utf-8 /MP /Zc:inline)
    target_compile_options(omsi_spline_creator PRIVATE /W4 "/permissive-" /utf-8 /MP /Zc:inline)
else()
    target_compile_options(omsi_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(omsi_f4_viewer PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(omsi_spline_creator PRIVATE -Wall -Wextra -Wpedantic)
endif()

foreach(_spline_ui_tgt IN ITEMS omsi_f4_viewer omsi_spline_creator)
    add_custom_command(TARGET ${_spline_ui_tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_spline_ui_tgt}>/assets"
        COMMENT "Deploy spline tool icons for ${_spline_ui_tgt}")
    foreach(_icon IN ITEMS spline_pencil_tool.png spline_icon_espejar.png spline_icon_flip.png spline_icon_delete.png
            spline_icon_select.png spline_icon_texture.png spline_icon_path.webp)
        add_custom_command(TARGET ${_spline_ui_tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/assets/${_icon}"
                "$<TARGET_FILE_DIR:${_spline_ui_tgt}>/assets/${_icon}")
    endforeach()
    if(EXISTS "${OMSI_ULTRALIGHT_ROOT}/bin/Ultralight.dll")
        add_custom_command(TARGET ${_spline_ui_tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${OMSI_ULTRALIGHT_ROOT}/bin"
                "$<TARGET_FILE_DIR:${_spline_ui_tgt}>"
            COMMENT "Deploy Ultralight runtime for ${_spline_ui_tgt}")
        add_custom_command(TARGET ${_spline_ui_tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/assets/ui/map_editor"
                "$<TARGET_FILE_DIR:${_spline_ui_tgt}>/assets/ui/map_editor"
            COMMENT "Deploy map editor web UI for ${_spline_ui_tgt}")
        add_custom_command(TARGET ${_spline_ui_tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/assets/ui/shared"
                "$<TARGET_FILE_DIR:${_spline_ui_tgt}>/assets/ui/shared"
            COMMENT "Deploy shared web UI CSS for ${_spline_ui_tgt}")
        add_custom_command(TARGET ${_spline_ui_tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/assets/ui/launcher"
                "$<TARGET_FILE_DIR:${_spline_ui_tgt}>/assets/ui/launcher"
            COMMENT "Deploy launcher web UI for ${_spline_ui_tgt}")
        add_custom_command(TARGET ${_spline_ui_tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/assets/ui/spline_creator"
                "$<TARGET_FILE_DIR:${_spline_ui_tgt}>/assets/ui/spline_creator"
            COMMENT "Deploy spline creator web UI for ${_spline_ui_tgt}")
        add_custom_command(TARGET ${_spline_ui_tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/assets/ui/map_text_editor"
                "$<TARGET_FILE_DIR:${_spline_ui_tgt}>/assets/ui/map_text_editor"
            COMMENT "Deploy .map text editor web UI for ${_spline_ui_tgt}")
        add_custom_command(TARGET ${_spline_ui_tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${OMSI_ULTRALIGHT_ROOT}/resources"
                "$<TARGET_FILE_DIR:${_spline_ui_tgt}>/assets/ui/map_editor/resources"
            COMMENT "Deploy Ultralight resources for ${_spline_ui_tgt}")
    endif()
endforeach()

add_executable(autofill_path_geometry_test src/test/autofill_path_geometry_test.cpp)
target_link_libraries(autofill_path_geometry_test PRIVATE omsi_core)
if(MSVC)
    target_compile_options(autofill_path_geometry_test PRIVATE /W4 "/permissive-")
else()
    target_compile_options(autofill_path_geometry_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(spline_attach_parse_test src/test/spline_attach_parse_test.cpp)
target_link_libraries(spline_attach_parse_test PRIVATE omsi_core)
if(MSVC)
    target_compile_options(spline_attach_parse_test PRIVATE /W4 "/permissive-")
else()
    target_compile_options(spline_attach_parse_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(tile_stream_load_test src/test/tile_stream_load_test.cpp)
target_link_libraries(tile_stream_load_test PRIVATE omsi_core)

if(MSVC)
    target_compile_options(tile_stream_load_test PRIVATE /W4 "/permissive-" /utf-8)
else()
    target_compile_options(tile_stream_load_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(omsi_o3d_microbench src/test/o3d_microbench.cpp)
target_link_libraries(omsi_o3d_microbench PRIVATE omsi_core)
if(MSVC)
    target_compile_options(omsi_o3d_microbench PRIVATE /W4 "/permissive-" /utf-8)
else()
    target_compile_options(omsi_o3d_microbench PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MSVC)
    # __try in seh_invoke.cpp cannot share a PCH TU that pulls C++ objects into that function.
    set_source_files_properties(src/app/seh_invoke.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set(_omsi_pch "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.hpp")
    foreach(_omsi_tgt IN ITEMS omsi_core omsi_f4_viewer omsi_spline_creator
            autofill_path_geometry_test spline_attach_parse_test
            tile_stream_load_test omsi_o3d_microbench)
        target_precompile_headers(${_omsi_tgt} PRIVATE "${_omsi_pch}")
    endforeach()
endif()
