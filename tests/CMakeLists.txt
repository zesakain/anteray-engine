# tests/CMakeLists.txt
#
# Root test configuration for Hikari-Machine.

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE hikari::engine_base)
add_test(NAME smoke_test COMMAND smoke_test)

# Core Unit Tests
add_executable(result_test unit/core/result_test.cpp)
target_link_libraries(result_test PRIVATE hikari::engine_core)
add_test(NAME result_test COMMAND result_test)

add_executable(assert_test unit/core/assert_test.cpp)
target_link_libraries(assert_test PRIVATE hikari::engine_core)
add_test(NAME assert_test COMMAND assert_test)

add_executable(linear_allocator_test unit/core/linear_allocator_test.cpp)
target_link_libraries(linear_allocator_test PRIVATE hikari::engine_core)
add_test(NAME linear_allocator_test COMMAND linear_allocator_test)

add_executable(arena_allocator_test unit/core/arena_allocator_test.cpp)
target_link_libraries(arena_allocator_test PRIVATE hikari::engine_core)
add_test(NAME arena_allocator_test COMMAND arena_allocator_test)

add_executable(pool_allocator_test unit/core/pool_allocator_test.cpp)
target_link_libraries(pool_allocator_test PRIVATE hikari::engine_core)
add_test(NAME pool_allocator_test COMMAND pool_allocator_test)

add_executable(generational_handle_test unit/core/generational_handle_test.cpp)
target_link_libraries(generational_handle_test PRIVATE hikari::engine_core)
add_test(NAME generational_handle_test COMMAND generational_handle_test)

add_executable(time_test unit/core/time_test.cpp)
target_link_libraries(time_test PRIVATE hikari::engine_core)
add_test(NAME time_test COMMAND time_test)

add_executable(log_test unit/core/log_test.cpp)
target_link_libraries(log_test PRIVATE hikari::engine_core)
add_test(NAME log_test COMMAND log_test)

add_executable(slot_map_test unit/core/slot_map_test.cpp)
target_link_libraries(slot_map_test PRIVATE hikari::engine_core)
add_test(NAME slot_map_test COMMAND slot_map_test)

add_executable(id_test unit/core/id_test.cpp)
target_link_libraries(id_test PRIVATE hikari::engine_core)
add_test(NAME id_test COMMAND id_test)

add_executable(event_test unit/core/event_test.cpp)
target_link_libraries(event_test PRIVATE hikari::engine_core)
add_test(NAME event_test COMMAND event_test)

add_executable(json_test unit/core/json_test.cpp)
target_link_libraries(json_test PRIVATE hikari::engine_core)
add_test(NAME json_test COMMAND json_test)

add_executable(job_system_test unit/core/job_system_test.cpp)
target_link_libraries(job_system_test PRIVATE hikari::engine_core)
add_test(NAME job_system_test COMMAND job_system_test)

add_executable(math_test unit/core/math_test.cpp)
target_link_libraries(math_test PRIVATE hikari::engine_core)
add_test(NAME math_test COMMAND math_test)

# Platform Unit Tests
add_executable(platform_test unit/platform/platform_test.cpp)
target_link_libraries(platform_test PRIVATE hikari::engine_platform)
add_test(NAME platform_test COMMAND platform_test)

# RHI Unit Tests
add_executable(null_rhi_test unit/rhi/null_rhi_test.cpp)
target_link_libraries(null_rhi_test PRIVATE hikari::engine_rhi)
add_test(NAME null_rhi_test COMMAND null_rhi_test)

add_executable(vulkan_rhi_test unit/rhi/vulkan_rhi_test.cpp)
target_link_libraries(vulkan_rhi_test PRIVATE hikari::engine_rhi hikari::engine_platform)
add_test(NAME vulkan_rhi_test COMMAND vulkan_rhi_test)

# World Unit Tests
add_executable(world_test unit/world/world_test.cpp)
target_link_libraries(world_test PRIVATE hikari::engine_world)
add_test(NAME world_test COMMAND world_test)

add_executable(hierarchy_test unit/world/hierarchy_test.cpp)
target_link_libraries(hierarchy_test PRIVATE hikari::engine_world hikari::engine_physics)
add_test(NAME hierarchy_test COMMAND hierarchy_test)

add_executable(transform_propagation_test unit/world/transform_propagation_test.cpp)
target_link_libraries(transform_propagation_test PRIVATE hikari::engine_world hikari::engine_physics)
add_test(NAME transform_propagation_test COMMAND transform_propagation_test)

# Framework Unit Tests
add_executable(conductor_test unit/framework/conductor_test.cpp)
target_link_libraries(conductor_test PRIVATE hikari::engine_framework)
add_test(NAME conductor_test COMMAND conductor_test)

add_executable(shared_world_conductor_test unit/framework/shared_world_conductor_test.cpp)
target_link_libraries(shared_world_conductor_test PRIVATE hikari::engine_framework)
add_test(NAME shared_world_conductor_test COMMAND shared_world_conductor_test)

add_executable(clock_domain_test unit/framework/clock_domain_test.cpp)
target_link_libraries(clock_domain_test PRIVATE hikari::engine_framework)
add_test(NAME clock_domain_test COMMAND clock_domain_test)

add_executable(scene_test unit/framework/scene_test.cpp)
target_link_libraries(scene_test PRIVATE hikari::engine_framework)
target_compile_definitions(scene_test PRIVATE HIKARI_SAMPLE_SCENE_PATH="${CMAKE_SOURCE_DIR}/assets/scenes/sample.hikari")
add_test(NAME scene_test COMMAND scene_test)

add_executable(async_scene_loader_test unit/framework/async_scene_loader_test.cpp)
target_link_libraries(async_scene_loader_test PRIVATE hikari::engine_framework hikari::engine_render hikari::engine_rhi hikari::engine_core)
target_compile_definitions(async_scene_loader_test PRIVATE
    HIKARI_SAMPLE_SCENE_PATH="${CMAKE_SOURCE_DIR}/assets/scenes/sample.hikari"
    HIKARI_DAMAGED_HELMET_PATH="${CMAKE_SOURCE_DIR}/test-assets/DamagedHelmet.glb"
)
add_test(NAME async_scene_loader_test COMMAND async_scene_loader_test)

# Benchmark scene generator and capture tools
add_executable(generate_benchmark_scenes ${CMAKE_SOURCE_DIR}/tools/generate_benchmark_scenes.cpp)
target_link_libraries(generate_benchmark_scenes PRIVATE hikari::engine_framework hikari::engine_render)

add_executable(capture_benchmark_scenes ${CMAKE_SOURCE_DIR}/tools/capture_benchmark_scenes.cpp)
target_link_libraries(capture_benchmark_scenes PRIVATE hikari::engine_framework hikari::engine_render hikari::engine_rhi hikari::engine_world)

add_executable(runtime_session_test unit/framework/runtime_session_test.cpp)
target_link_libraries(runtime_session_test PRIVATE hikari::engine_framework)
add_test(NAME runtime_session_test COMMAND runtime_session_test)

add_executable(prefab_and_character_test unit/framework/prefab_and_character_test.cpp)
target_link_libraries(prefab_and_character_test PRIVATE hikari::engine_framework)
add_test(NAME prefab_and_character_test COMMAND prefab_and_character_test)

# Headless Integration / Stress Tests
add_executable(conductor_stress_test headless/conductor_stress_test.cpp)
target_link_libraries(conductor_stress_test PRIVATE hikari::engine_framework)
add_test(NAME conductor_stress_test COMMAND conductor_stress_test)

# Render Unit Tests
add_executable(render_graph_test unit/render/render_graph_test.cpp)
target_link_libraries(render_graph_test PRIVATE hikari::engine_render)
add_test(NAME render_graph_test COMMAND render_graph_test)

add_executable(image_decoder_test unit/render/image_decoder_test.cpp)
target_link_libraries(image_decoder_test PRIVATE hikari::engine_render)
add_test(NAME image_decoder_test COMMAND image_decoder_test)

add_executable(environment_map_test unit/render/environment_map_test.cpp)
target_link_libraries(environment_map_test PRIVATE hikari::engine_render)
add_test(NAME environment_map_test COMMAND environment_map_test)

add_executable(irradiance_probe_test unit/render/irradiance_probe_test.cpp)
target_link_libraries(irradiance_probe_test PRIVATE hikari::engine_render)
add_test(NAME irradiance_probe_test COMMAND irradiance_probe_test)

# Mesh Unit Tests
add_executable(mesh_test unit/render/mesh_test.cpp)
target_link_libraries(mesh_test PRIVATE hikari::engine_render hikari::engine_rhi)
add_test(NAME mesh_test COMMAND mesh_test)
set_tests_properties(mesh_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Asset Manager Unit Tests
add_executable(asset_manager_test unit/render/asset_manager_test.cpp)
target_link_libraries(asset_manager_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_world)
add_test(NAME asset_manager_test COMMAND asset_manager_test)
set_tests_properties(asset_manager_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# glTF 2.0 & 3D Asset Importer Unit Tests
add_executable(gltf_importer_test unit/render/gltf_importer_test.cpp)
target_link_libraries(gltf_importer_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME glTF_importer_test COMMAND gltf_importer_test)
set_tests_properties(glTF_importer_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# glTF Hierarchical Scene Import Unit Tests (Milestone 8B.6 Phase 3)
add_executable(gltf_scene_importer_test unit/render/gltf_scene_importer_test.cpp)
target_link_libraries(gltf_scene_importer_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME gltf_scene_importer_test COMMAND gltf_scene_importer_test)
set_tests_properties(gltf_scene_importer_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Scene Renderer Unit Tests
add_executable(scene_renderer_test unit/render/scene_renderer_test.cpp)
target_link_libraries(scene_renderer_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME scene_renderer_test COMMAND scene_renderer_test)
set_tests_properties(scene_renderer_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Textured Mesh Render Unit Tests (Milestone 10B)
add_executable(textured_mesh_render_test unit/render/textured_mesh_render_test.cpp)
target_link_libraries(textured_mesh_render_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME textured_mesh_render_test COMMAND textured_mesh_render_test)
set_tests_properties(textured_mesh_render_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Material Descriptor Unit Tests (Milestone 10C)
add_executable(material_descriptor_test unit/render/material_descriptor_test.cpp)
target_link_libraries(material_descriptor_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME material_descriptor_test COMMAND material_descriptor_test)
set_tests_properties(material_descriptor_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Sampler & Texture Quality Unit Tests (Milestone 11F Phase 2)
add_executable(sampler_and_texture_quality_test unit/render/sampler_and_texture_quality_test.cpp)
target_link_libraries(sampler_and_texture_quality_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME sampler_and_texture_quality_test COMMAND sampler_and_texture_quality_test)
set_tests_properties(sampler_and_texture_quality_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# glTF Material Extensions Unit Tests (Milestone 11F Phase 3)
add_executable(gltf_material_extensions_test unit/render/gltf_material_extensions_test.cpp)
target_link_libraries(gltf_material_extensions_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME gltf_material_extensions_test COMMAND gltf_material_extensions_test)
set_tests_properties(gltf_material_extensions_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Advanced PBR Pipeline Unit Tests (Milestone 11F Phase 4)
add_executable(advanced_pbr_pipeline_test unit/render/advanced_pbr_pipeline_test.cpp)
target_link_libraries(advanced_pbr_pipeline_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME advanced_pbr_pipeline_test COMMAND advanced_pbr_pipeline_test)
set_tests_properties(advanced_pbr_pipeline_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Texture Residency & Memory Management Unit Tests (Milestone 11F Phase 5)
add_executable(texture_residency_test unit/render/texture_residency_test.cpp)
target_link_libraries(texture_residency_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME texture_residency_test COMMAND texture_residency_test)
set_tests_properties(texture_residency_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Full glTF Metallic-Roughness PBR Verification Tests (Milestone 10D)
add_executable(pbr_render_test unit/render/pbr_render_test.cpp)
target_link_libraries(pbr_render_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME pbr_render_test COMMAND pbr_render_test)
set_tests_properties(pbr_render_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Lighting & Directional Shadow Mapping Verification Tests (Milestone 10E)
add_executable(lighting_render_test unit/render/lighting_render_test.cpp)
target_link_libraries(lighting_render_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME lighting_render_test COMMAND lighting_render_test)
set_tests_properties(lighting_render_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Cascaded Shadow Maps Verification Tests (Milestone 11E)
add_executable(shadow_cascade_test unit/render/shadow_cascade_test.cpp)
target_link_libraries(shadow_cascade_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_core)
add_test(NAME shadow_cascade_test COMMAND shadow_cascade_test)
set_tests_properties(shadow_cascade_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# HDR, Tone Mapping & Post-Processing Verification Tests (Milestone 10F)
add_executable(post_process_test unit/render/post_process_test.cpp)
target_link_libraries(post_process_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME post_process_test COMMAND post_process_test)
set_tests_properties(post_process_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Sponza Benchmark Certification Tests (Milestone 10G)
add_executable(sponza_benchmark_test unit/render/sponza_benchmark_test.cpp)
target_link_libraries(sponza_benchmark_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME sponza_benchmark_test COMMAND sponza_benchmark_test)
set_tests_properties(sponza_benchmark_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Graphics Settings & Scalability Verification Tests (Milestone 11B)
add_executable(graphics_settings_test unit/render/graphics_settings_test.cpp)
target_link_libraries(graphics_settings_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_core)
add_test(NAME graphics_settings_test COMMAND graphics_settings_test)
set_tests_properties(graphics_settings_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Image-Based Lighting (IBL) Verification Tests (Milestone 11C)
add_executable(ibl_render_test unit/render/ibl_render_test.cpp)
target_link_libraries(ibl_render_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME ibl_render_test COMMAND ibl_render_test)
set_tests_properties(ibl_render_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Premium Lighting & Atmospheric Conditions Verification Tests (Milestone 11C Upgrade)
add_executable(lighting_upgrade_test unit/render/lighting_upgrade_test.cpp)
target_link_libraries(lighting_upgrade_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME lighting_upgrade_test COMMAND lighting_upgrade_test)
set_tests_properties(lighting_upgrade_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Global Illumination & Indirect Lighting Verification Tests (Milestone 11D)
add_executable(gi_render_test unit/render/gi_render_test.cpp)
target_link_libraries(gi_render_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME gi_render_test COMMAND gi_render_test)
set_tests_properties(gi_render_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Reflection Quality & Temporal Stability Verification Tests (Milestone 11G)
add_executable(reflection_and_temporal_test unit/render/reflection_and_temporal_test.cpp)
target_link_libraries(reflection_and_temporal_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME reflection_and_temporal_test COMMAND reflection_and_temporal_test)
set_tests_properties(reflection_and_temporal_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Advanced Anti-Aliasing & Temporal Rendering Tests (Milestone 11H)
add_executable(taa_test unit/render/taa_test.cpp)
target_link_libraries(taa_test PRIVATE hikari::engine_render hikari::engine_rhi hikari::engine_framework hikari::engine_physics hikari::engine_world)
add_test(NAME taa_test COMMAND taa_test)
set_tests_properties(taa_test PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# Physics Unit Tests
add_executable(physics_test unit/physics/physics_test.cpp)
target_link_libraries(physics_test PRIVATE hikari::engine_physics)
add_test(NAME physics_test COMMAND physics_test)

# Animation Unit Tests
add_executable(anim_test unit/anim/anim_test.cpp)
target_link_libraries(anim_test PRIVATE hikari::engine_anim)
add_test(NAME anim_test COMMAND anim_test)

# Script Unit Tests
add_executable(script_test unit/script/script_test.cpp)
target_link_libraries(script_test PRIVATE hikari::engine_script hikari::engine_world)
add_test(NAME script_test COMMAND script_test)

# UI Unit Tests
add_executable(ui_test unit/ui/ui_test.cpp)
target_link_libraries(ui_test PRIVATE hikari::engine_ui hikari::engine_render hikari::engine_rhi)
add_test(NAME ui_test COMMAND ui_test)

# Hub Core Unit Tests
add_executable(hub_test unit/hub/hub_test.cpp)
target_link_libraries(hub_test PRIVATE hikari::hub_core)
add_test(NAME hub_test COMMAND hub_test)

# Editor Unit Tests (Guarded by EDITOR flag)
if(HIKARI_BUILD_EDITOR)
    add_executable(editor_test unit/editor/editor_test.cpp)
    target_link_libraries(editor_test PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(editor_test PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)
    add_test(NAME editor_test COMMAND editor_test)

    add_executable(content_browser_test unit/editor/content_browser_test.cpp)
    target_link_libraries(content_browser_test PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_world hikari::engine_rhi)
    add_test(NAME content_browser_test COMMAND content_browser_test)

    add_executable(script_editor_test unit/editor/script_editor_test.cpp)
    target_link_libraries(script_editor_test PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_world hikari::engine_rhi)
    add_test(NAME script_editor_test COMMAND script_editor_test)

    add_executable(editor_preferences_test unit/editor/editor_preferences_test.cpp)
    target_link_libraries(editor_preferences_test PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_world hikari::engine_rhi)
    add_test(NAME editor_preferences_test COMMAND editor_preferences_test)

    add_executable(benchmark_gi_1080p ${CMAKE_SOURCE_DIR}/tools/benchmark_gi_1080p.cpp)
    target_link_libraries(benchmark_gi_1080p PRIVATE hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi)

    add_executable(benchmark_shadow_baseline ${CMAKE_SOURCE_DIR}/tools/benchmark_shadow_baseline.cpp)
    target_link_libraries(benchmark_shadow_baseline PRIVATE hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi)

    add_executable(capture_editor_telemetry ${CMAKE_SOURCE_DIR}/tools/capture_editor_telemetry.cpp)
    target_link_libraries(capture_editor_telemetry PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(capture_editor_telemetry PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)

    add_executable(profile_gpu_serialization ${CMAKE_SOURCE_DIR}/tools/profile_gpu_serialization.cpp)
    target_link_libraries(profile_gpu_serialization PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(profile_gpu_serialization PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)

    add_executable(benchmark_recovery_r2 ${CMAKE_SOURCE_DIR}/tools/benchmark_recovery_r2.cpp)
    target_link_libraries(benchmark_recovery_r2 PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(benchmark_recovery_r2 PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)

    add_executable(benchmark_bistro_60fps ${CMAKE_SOURCE_DIR}/tools/benchmark_bistro_60fps.cpp)
    target_link_libraries(benchmark_bistro_60fps PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(benchmark_bistro_60fps PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)

    add_executable(benchmark_scalability ${CMAKE_SOURCE_DIR}/tools/benchmark_scalability.cpp)
    target_link_libraries(benchmark_scalability PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(benchmark_scalability PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)

    add_executable(benchmark_lighting_conditions ${CMAKE_SOURCE_DIR}/tools/benchmark_lighting_conditions.cpp)
    target_link_libraries(benchmark_lighting_conditions PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(benchmark_lighting_conditions PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)

    add_executable(sponza_sustained_benchmark ${CMAKE_SOURCE_DIR}/tools/sponza_sustained_benchmark.cpp)
    target_link_libraries(sponza_sustained_benchmark PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(sponza_sustained_benchmark PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)

    add_executable(benchmark_sponza_1080p ${CMAKE_SOURCE_DIR}/tools/benchmark_sponza_1080p.cpp)
    target_link_libraries(benchmark_sponza_1080p PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(benchmark_sponza_1080p PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)

    add_executable(verify_editor_settings ${CMAKE_SOURCE_DIR}/tools/verify_editor_settings.cpp)
    target_link_libraries(verify_editor_settings PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(verify_editor_settings PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)

    add_executable(graphics_settings_apply_test ${CMAKE_SOURCE_DIR}/tools/graphics_settings_apply_test.cpp)
    target_link_libraries(graphics_settings_apply_test PRIVATE hikari::editor_lib hikari::engine_framework hikari::engine_render hikari::engine_physics hikari::engine_world hikari::engine_rhi imgui_static)
    target_include_directories(graphics_settings_apply_test PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/editor/src)
    add_test(NAME graphics_settings_apply_test COMMAND graphics_settings_apply_test)
endif()








