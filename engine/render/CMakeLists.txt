# engine/render/CMakeLists.txt
#
# High-level rendering module providing Render Graph scheduling.

add_library(engine_render STATIC
    src/asset_manager.cpp
    src/environment_map.cpp
    src/gltf_importer.cpp
    src/gltf_scene_importer.cpp
    src/gpu_environment_map.cpp
    src/gpu_material.cpp
    src/gpu_mesh.cpp
    src/gpu_texture.cpp
    src/graphics_settings.cpp
    src/graphics_settings_serializer.cpp
    src/image_decoder.cpp
    src/mesh.cpp
    src/mesh_component.cpp
    src/obj_importer.cpp
    src/render_graph.cpp
    src/sampler_cache.cpp
    src/scene_renderer.cpp
)
add_library(anteray::engine_render ALIAS engine_render)

target_include_directories(engine_render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    SYSTEM PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/cgltf>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/stb>
)

target_link_libraries(engine_render
    PUBLIC
        anteray::engine_flags
        anteray::engine_core
        anteray::engine_rhi
        anteray::engine_world
        anteray::engine_physics
        anteray::engine_anim
)
