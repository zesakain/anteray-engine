/// @file template_registry.cpp
/// @brief Project template registry and safe project instantiation implementation.

#include "hub/template_registry.h"

#include "core/json.h"

#include <chrono>
#include <fstream>
#include <system_error>

namespace eng::hub {

namespace {

int64_t currentUnixTimestamp() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

Result<void> writeTextFile(const std::filesystem::path& filePath, std::string_view content) {
    std::ofstream out(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return Error{.code = ErrorCode::InvalidState, .message = "Failed to create project file"};
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    if (!out.good()) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Failed while writing project file"};
    }
    return Result<void>::success();
}

std::string buildSceneJson(std::string_view sceneName, bool includeGround, bool includeDynamicBox,
                           bool includeCamera) {
    core::JsonValue root = core::JsonValue::makeObject();
    root.set("formatVersion", 1);

    core::JsonValue meta = core::JsonValue::makeObject();
    meta.set("name", sceneName);
    meta.set("generator", "Hikari Machine Hub 0.1.0");
    meta.set("timestamp", currentUnixTimestamp());
    root.set("metadata", std::move(meta));

    core::JsonValue entities = core::JsonValue::makeArray();
    int64_t nextId = 1;

    auto makeVec3 = [](double x, double y, double z) {
        core::JsonValue arr = core::JsonValue::makeArray();
        arr.pushBack(x);
        arr.pushBack(y);
        arr.pushBack(z);
        return arr;
    };
    auto makeQuat = [](double x, double y, double z, double w) {
        core::JsonValue arr = core::JsonValue::makeArray();
        arr.pushBack(x);
        arr.pushBack(y);
        arr.pushBack(z);
        arr.pushBack(w);
        return arr;
    };
    auto makeTransform = [&](double px, double py, double pz, double sx, double sy, double sz) {
        core::JsonValue t = core::JsonValue::makeObject();
        t.set("position", makeVec3(px, py, pz));
        t.set("rotation", makeQuat(0.0, 0.0, 0.0, 1.0));
        t.set("scale", makeVec3(sx, sy, sz));
        return t;
    };

    if (includeCamera) {
        core::JsonValue cam = core::JsonValue::makeObject();
        cam.set("id", nextId++);
        cam.set("name", "MainCameraRig");
        core::JsonValue comps = core::JsonValue::makeObject();
        comps.set("Transform", makeTransform(0.0, 2.0, -6.0, 1.0, 1.0, 1.0));
        cam.set("components", std::move(comps));
        entities.pushBack(std::move(cam));
    }

    if (includeGround) {
        core::JsonValue ground = core::JsonValue::makeObject();
        ground.set("id", nextId++);
        ground.set("name", "GroundPlane");
        core::JsonValue comps = core::JsonValue::makeObject();
        comps.set("Transform", makeTransform(0.0, 0.0, 0.0, 20.0, 0.2, 20.0));

        core::JsonValue rb = core::JsonValue::makeObject();
        rb.set("motionType", "Static");
        rb.set("collisionLayer", 0);
        comps.set("RigidBody", std::move(rb));

        core::JsonValue col = core::JsonValue::makeObject();
        col.set("type", "Box");
        col.set("halfExtents", makeVec3(10.0, 0.1, 10.0));
        col.set("radius", 0.5);
        col.set("halfHeight", 0.5);
        col.set("offset", makeVec3(0.0, 0.0, 0.0));
        col.set("rotation", makeQuat(0.0, 0.0, 0.0, 1.0));
        comps.set("Collider", std::move(col));

        ground.set("components", std::move(comps));
        entities.pushBack(std::move(ground));
    }

    if (includeDynamicBox) {
        core::JsonValue box = core::JsonValue::makeObject();
        box.set("id", nextId++);
        box.set("name", "DynamicTestBox");
        core::JsonValue comps = core::JsonValue::makeObject();
        comps.set("Transform", makeTransform(0.0, 4.0, 0.0, 1.0, 1.0, 1.0));

        core::JsonValue rb = core::JsonValue::makeObject();
        rb.set("motionType", "Dynamic");
        rb.set("collisionLayer", 1);
        comps.set("RigidBody", std::move(rb));

        core::JsonValue col = core::JsonValue::makeObject();
        col.set("type", "Box");
        col.set("halfExtents", makeVec3(0.5, 0.5, 0.5));
        col.set("radius", 0.5);
        col.set("halfHeight", 0.5);
        col.set("offset", makeVec3(0.0, 0.0, 0.0));
        col.set("rotation", makeQuat(0.0, 0.0, 0.0, 1.0));
        comps.set("Collider", std::move(col));

        box.set("components", std::move(comps));
        entities.pushBack(std::move(box));
    }

    root.set("entities", std::move(entities));
    return root.serialize(2) + "\n";
}

std::string buildReadme(const ProjectCreateRequest& req, const ProjectTemplateDesc& tmpl) {
    std::string text = "# " + req.projectName + "\n\n";
    text += "Created with Hikari Machine Hub (`" + std::string(kCurrentEngineVersion) + "`)\n";
    text += "Template: **" + tmpl.name + "** (`" + tmpl.id + "`)\n\n";
    text += "## Description\n\n" + tmpl.description + "\n";

    if (tmpl.id == "rendering_benchmark") {
        text += "\n## Benchmark Assets\n\n";
        text += "Large benchmark scenes (Sponza, Bistro, DamagedHelmet, MaterialLab) are stored in "
                "the shared repository `assets/benchmark-scenes/` and `test-assets/benchmark/` "
                "directories and are not duplicated into individual projects.\n";
    } else if (tmpl.id == "scripting_playground") {
        text += "\n## Lua Script Bindings\n\n";
        text += "`Scripts/playground.lua` uses the verified engine Lua 5.4 lifecycle callbacks "
                "(`on_init()`, `on_update(dt)`) and `Vec3` / `print` bindings.\n";
    } else if (tmpl.id == "two_state_conductor") {
        text += "\n## Two-State Conductor Architecture\n\n";
        text += "Hikari's `Conductor` manages two standby/active time states (`StateA` and "
                "`StateB`) over a single shared Flecs `World`. Use the Editor toolbar's "
                "`Switch to State B` / `Play` controls to inspect transitions.\n";
    }
    return text;
}

} // namespace

Result<void> validateTemplateDesc(const ProjectTemplateDesc& desc) {
    if (desc.id.empty()) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "Template id cannot be empty"};
    }
    if (desc.name.empty()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Template name cannot be empty"};
    }
    if (!desc.defaultStartupScene.empty() && !isSafeProjectRelativePath(desc.defaultStartupScene)) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message =
                         "Template defaultStartupScene must be a safe project-relative path"};
    }
    return Result<void>::success();
}

const std::vector<ProjectTemplateDesc>& TemplateRegistry::builtinTemplates() {
    static const std::vector<ProjectTemplateDesc> kTemplates{
        ProjectTemplateDesc{
            .id = "empty",
            .name = "Empty Project",
            .category = "Core",
            .description =
                "Minimal valid Hikari project layout with no sample assets or startup scene.",
            .defaultStartupScene = "",
            .capabilities = {"Valid .hikari-project manifest",
                             "Standard Assets/Scenes/Scripts/Config folders"},
            .requirements = {"Hikari Engine 0.1.0"},
        },
        ProjectTemplateDesc{
            .id = "basic_3d",
            .name = "Basic 3D Scene",
            .category = "Scene",
            .description =
                "Minimal 3D scene with a camera rig, static ground plane, and box collider ready "
                "for the Editor.",
            .defaultStartupScene = "Scenes/Main.hikari",
            .capabilities = {"Transform hierarchy", "Static GroundPlane with Box Collider"},
            .requirements = {"Hikari Engine 0.1.0", "Vulkan or Null RHI"},
        },
        ProjectTemplateDesc{
            .id = "rendering_benchmark",
            .name = "Rendering Benchmark",
            .category = "Rendering",
            .description =
                "Entry point for rendering experiments with benchmark references to shared "
                "repository scenes without copying large assets.",
            .defaultStartupScene = "Scenes/BenchmarkEntry.hikari",
            .capabilities = {"Lightweight entry scene", "Shared benchmark catalog references"},
            .requirements = {"Hikari Engine 0.1.0",
                             "Optional repository test-assets/benchmark/ for full scenes"},
        },
        ProjectTemplateDesc{
            .id = "physics_test",
            .name = "Physics Test",
            .category = "Simulation",
            .description =
                "Minimal Jolt Physics sandbox containing a static ground plane and dynamic rigid "
                "body cube.",
            .defaultStartupScene = "Scenes/PhysicsSandbox.hikari",
            .capabilities = {"Static & Dynamic RigidBody", "Box Collider simulation"},
            .requirements = {"Hikari Engine 0.1.0", "Jolt Physics"},
        },
        ProjectTemplateDesc{
            .id = "scripting_playground",
            .name = "Scripting Playground",
            .category = "Scripting",
            .description =
                "Minimal Lua 5.4 scripting setup using verified on_init/on_update callbacks and "
                "Vec3 math bindings.",
            .defaultStartupScene = "Scenes/ScriptingPlayground.hikari",
            .capabilities = {"Lua 5.4 ScriptComponent", "Vec3 math & print() bindings"},
            .requirements = {"Hikari Engine 0.1.0", "Lua 5.4 runtime"},
        },
        ProjectTemplateDesc{
            .id = "two_state_conductor",
            .name = "Two-State Conductor Demo",
            .category = "Architecture",
            .description = "Starter project configured to explore Hikari's Two-State Conductor "
                           "(StateA/StateB) "
                           "over a shared ECS World in the Editor.",
            .defaultStartupScene = "Scenes/ConductorDemo.hikari",
            .capabilities = {"Shared World scene", "StateA / StateB transition inspection"},
            .requirements = {"Hikari Engine 0.1.0", "Conductor framework"},
        },
    };
    return kTemplates;
}

const ProjectTemplateDesc* TemplateRegistry::findById(std::string_view id) {
    for (const auto& tmpl : builtinTemplates()) {
        if (tmpl.id == id) {
            return &tmpl;
        }
    }
    return nullptr;
}

Result<ProjectCreateResult> TemplateRegistry::createProject(const ProjectCreateRequest& request,
                                                            const ProjectTemplateDesc& tmpl) {
    auto tmplCheck = validateTemplateDesc(tmpl);
    if (!tmplCheck.hasValue()) {
        return tmplCheck.error();
    }

    auto nameCheck = validateProjectName(request.projectName);
    if (!nameCheck.hasValue()) {
        return nameCheck.error();
    }

    if (request.parentDirectory.empty()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Parent directory cannot be empty"};
    }

    std::error_code ec;
    if (!std::filesystem::exists(request.parentDirectory, ec) ||
        !std::filesystem::is_directory(request.parentDirectory, ec)) {
        return Error{.code = ErrorCode::InvalidState,
                     .message =
                         "Parent directory does not exist: " + request.parentDirectory.string()};
    }

    const std::filesystem::path projectDir = request.parentDirectory / request.projectName;
    if (std::filesystem::exists(projectDir, ec)) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Target project directory already exists"};
    }
    if (ec) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Failed to inspect target directory"};
    }

    // Create root directory; on any subsequent failure, roll back the entire created directory.
    if (!std::filesystem::create_directory(projectDir, ec) || ec) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Failed to create project directory"};
    }

    auto rollback = [&]() {
        std::error_code cleanupEc;
        std::filesystem::remove_all(projectDir, cleanupEc);
    };

    ProjectCreateResult result{};
    result.projectDirectory = projectDir;
    result.createdPaths.push_back(projectDir);

    const std::vector<std::string> dirs{"Assets", "Scenes", "Scripts", "Config"};
    for (const auto& d : dirs) {
        const std::filesystem::path sub = projectDir / d;
        if (!std::filesystem::create_directory(sub, ec) || ec) {
            rollback();
            return Error{.code = ErrorCode::InvalidState,
                         .message =
                             "Failed to create project subdirectory '" + d + "': " + ec.message()};
        }
        result.createdPaths.push_back(sub);
    }

    // Write README.md
    {
        const std::filesystem::path readmePath = projectDir / "README.md";
        auto writeRes = writeTextFile(readmePath, buildReadme(request, tmpl));
        if (!writeRes.hasValue()) {
            rollback();
            return writeRes.error();
        }
        result.createdPaths.push_back(readmePath);
    }

    // Write template-specific scene and config/script assets
    if (!tmpl.defaultStartupScene.empty()) {
        const std::filesystem::path scenePath = projectDir / tmpl.defaultStartupScene;
        const bool includeDynamic = (tmpl.id == "physics_test");
        const std::string sceneContent =
            buildSceneJson(request.projectName + " - " + tmpl.name, true, includeDynamic, true);
        auto writeRes = writeTextFile(scenePath, sceneContent);
        if (!writeRes.hasValue()) {
            rollback();
            return writeRes.error();
        }
        result.createdPaths.push_back(scenePath);
    }

    if (tmpl.id == "scripting_playground") {
        const std::filesystem::path scriptPath = projectDir / "Scripts" / "playground.lua";
        const std::string luaContent =
            "-- playground.lua\n"
            "-- Verified Hikari Lua 5.4 ScriptComponent callbacks and bindings\n\n"
            "local elapsed = 0.0\n\n"
            "function on_init()\n"
            "    local origin = Vec3(0.0, 1.0, 0.0)\n"
            "    print(\"Scripting Playground initialized at\", origin)\n"
            "end\n\n"
            "function on_update(dt)\n"
            "    elapsed = elapsed + dt\n"
            "end\n";
        auto writeRes = writeTextFile(scriptPath, luaContent);
        if (!writeRes.hasValue()) {
            rollback();
            return writeRes.error();
        }
        result.createdPaths.push_back(scriptPath);
    } else if (tmpl.id == "rendering_benchmark") {
        core::JsonValue benchCfg = core::JsonValue::makeObject();
        benchCfg.set("note",
                     "Shared benchmark scenes live in the repository assets/benchmark-scenes/ "
                     "directory and are opened via the Hub Samples view without copying assets.");
        core::JsonValue refs = core::JsonValue::makeArray();
        refs.pushBack("assets/benchmark-scenes/Sponza_Benchmark.hikari");
        refs.pushBack("assets/benchmark-scenes/DamagedHelmet_Benchmark.hikari");
        refs.pushBack("assets/benchmark-scenes/MaterialLab_Benchmark.hikari");
        refs.pushBack("assets/benchmark-scenes/Bistro_Benchmark.hikari");
        benchCfg.set("sharedBenchmarkScenes", std::move(refs));

        const std::filesystem::path cfgPath = projectDir / "Config" / "benchmarks.json";
        auto writeRes = writeTextFile(cfgPath, benchCfg.serialize(2) + "\n");
        if (!writeRes.hasValue()) {
            rollback();
            return writeRes.error();
        }
        result.createdPaths.push_back(cfgPath);
    } else if (tmpl.id == "two_state_conductor") {
        core::JsonValue condCfg = core::JsonValue::makeObject();
        condCfg.set("architecture", "Two-State Conductor (StateA / StateB over shared World)");
        condCfg.set("notes",
                    "Use Conductor::requestTransition() in C++ or the Editor Conductor controls "
                    "to observe StateA/StateB standby preparation and switching.");
        const std::filesystem::path cfgPath = projectDir / "Config" / "conductor_demo.json";
        auto writeRes = writeTextFile(cfgPath, condCfg.serialize(2) + "\n");
        if (!writeRes.hasValue()) {
            rollback();
            return writeRes.error();
        }
        result.createdPaths.push_back(cfgPath);
    }

    // Write <ProjectName>.hikari-project manifest last
    const int64_t nowTs = currentUnixTimestamp();
    ProjectMetadata metadata{};
    metadata.formatVersion = kCurrentProjectFormatVersion;
    metadata.projectName = request.projectName;
    metadata.engineVersion = std::string(kCurrentEngineVersion);
    metadata.startupScene = tmpl.defaultStartupScene;
    metadata.assetDirectories = dirs;
    metadata.templateOrigin = tmpl.id;
    metadata.createdTimestamp = nowTs;
    metadata.lastModifiedTimestamp = nowTs;

    const std::filesystem::path manifestPath =
        projectDir / (request.projectName + std::string(kProjectFileExtension));
    auto saveRes = saveProjectMetadataFile(manifestPath, metadata);
    if (!saveRes.hasValue()) {
        rollback();
        return saveRes.error();
    }

    result.manifestPath = manifestPath;
    result.metadata = std::move(metadata);
    result.createdPaths.push_back(manifestPath);
    return result;
}

Result<ProjectCreateResult>
TemplateRegistry::createProjectFromBuiltin(const ProjectCreateRequest& request) {
    const ProjectTemplateDesc* tmpl = findById(request.templateId);
    if (tmpl == nullptr) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "Unknown project template id"};
    }
    return createProject(request, *tmpl);
}

} // namespace eng::hub
