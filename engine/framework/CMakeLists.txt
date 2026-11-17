# engine/framework/CMakeLists.txt
#
# Framework module providing Mode interfaces and Two-State Conductor execution runtime.

add_library(engine_framework STATIC
    src/async_scene_loader.cpp
    src/character_animation.cpp
    src/character_components.cpp
    src/character_controller.cpp
    src/conductor.cpp
    src/framework_script_bindings.cpp
    src/gameplay_input.cpp
    src/prefab.cpp
    src/runtime_session.cpp
    src/scene.cpp
    src/scene_serializer.cpp
    src/third_person_camera.cpp
    src/third_person_character_template.cpp
)
add_library(anteray::engine_framework ALIAS engine_framework)

target_include_directories(engine_framework
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
)

target_link_libraries(engine_framework
    PUBLIC
        anteray::engine_flags
        anteray::engine_core
        anteray::engine_platform
        anteray::engine_world
        anteray::engine_physics
        anteray::engine_render
        anteray::engine_anim
        anteray::engine_script
)
