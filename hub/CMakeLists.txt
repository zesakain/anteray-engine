# ANTERAY Hub — Standalone Project Management Application

add_library(anteray_hub_core STATIC
    src/project_metadata.cpp
    src/project_validator.cpp
    src/recent_projects.cpp
    src/template_registry.cpp
    src/sample_catalog.cpp
    src/engine_launcher.cpp
)
add_library(anteray::hub_core ALIAS anteray_hub_core)

target_include_directories(anteray_hub_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(anteray_hub_core
    PUBLIC
        anteray::engine_core
    PRIVATE
        anteray::engine_flags
)

if(ANTERAY_BUILD_HUB OR ANTERAY_BUILD_EDITOR)
    find_package(SDL3 CONFIG REQUIRED)

    if(NOT TARGET imgui_static)
        include(FetchContent)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG        v1.91.8
            GIT_SHALLOW    TRUE
        )
        FetchContent_GetProperties(imgui)
        if(NOT imgui_POPULATED)
            FetchContent_Populate(imgui)
        endif()

        add_library(imgui_static STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/imgui_demo.cpp
        )
        target_include_directories(imgui_static PUBLIC ${imgui_SOURCE_DIR})
        target_compile_definitions(imgui_static PUBLIC ImDrawIdx=unsigned\ int)
    endif()

    add_executable(anteray_hub
        src/hub_imgui_backend.cpp
        src/hub_app.cpp
        src/main.cpp
    )

    target_link_libraries(anteray_hub
        PRIVATE
            anteray::hub_core
            anteray::engine_flags
            anteray::engine_core
            anteray::engine_platform
            anteray::engine_rhi
            anteray::engine_render
            imgui_static
            SDL3::SDL3
    )

    set_target_properties(anteray_hub PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()
