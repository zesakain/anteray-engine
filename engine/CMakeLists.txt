# engine/CMakeLists.txt
#
# Engine root CMake configuration.
# Subsystems link against `anteray::engine_flags` which enforces:
# - C++20 standard
# - Strict warnings (-Wall -Wextra -Wconversion -Wshadow -Wpedantic)
# - No exceptions (-fno-exceptions, _HAS_EXCEPTIONS=0)
# - No RTTI (-fno-rtti, /GR-)

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(rhi)
add_subdirectory(world)
add_subdirectory(framework)
add_subdirectory(render)
add_subdirectory(physics)
add_subdirectory(anim)
add_subdirectory(script)
add_subdirectory(ui)

add_library(engine_base INTERFACE)
add_library(anteray::engine_base ALIAS engine_base)

target_link_libraries(engine_base
    INTERFACE
        anteray::engine_flags
        anteray::engine_core
        anteray::engine_platform
        anteray::engine_rhi
        anteray::engine_world
        anteray::engine_framework
        anteray::engine_render
        anteray::engine_physics
        anteray::engine_anim
        anteray::engine_script
        anteray::engine_ui
)
