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

std::string buildCleanTppSceneJson(std::string_view sceneName) {
    core::JsonValue root = core::JsonValue::makeObject();
    root.set("formatVersion", 1);

    core::JsonValue meta = core::JsonValue::makeObject();
    meta.set("name", sceneName);
    meta.set("generator", "Hikari Machine Hub 0.1.0");
    meta.set("timestamp", currentUnixTimestamp());
    root.set("metadata", std::move(meta));

    auto makeVec2 = [](double x, double y) {
        core::JsonValue arr = core::JsonValue::makeArray();
        arr.pushBack(x);
        arr.pushBack(y);
        return arr;
    };
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
    auto makeTransform = [&](double px, double py, double pz, double rx, double ry, double rz,
                             double rw, double sx, double sy, double sz) {
        core::JsonValue t = core::JsonValue::makeObject();
        t.set("position", makeVec3(px, py, pz));
        t.set("rotation", makeQuat(rx, ry, rz, rw));
        t.set("scale", makeVec3(sx, sy, sz));
        return t;
    };

    core::JsonValue entities = core::JsonValue::makeArray();

    // 1. Directional Sun Light
    {
        core::JsonValue sun = core::JsonValue::makeObject();
        sun.set("id", 1);
        sun.set("name", "SunLight");
        core::JsonValue comps = core::JsonValue::makeObject();
        comps.set("Transform", makeTransform(0.0, 12.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0));
        core::JsonValue dl = core::JsonValue::makeObject();
        dl.set("direction", makeVec3(0.4, 0.8, 0.4));
        dl.set("color", makeVec3(1.0, 0.97, 0.92));
        dl.set("intensity", 3.2);
        dl.set("castShadows", true);
        dl.set("shadowBias", 0.005);
        dl.set("enabled", true);
        comps.set("DirectionalLight", std::move(dl));
        sun.set("components", std::move(comps));
        entities.pushBack(std::move(sun));
    }

    // 2. Ground Plane
    {
        core::JsonValue ground = core::JsonValue::makeObject();
        ground.set("id", 2);
        ground.set("name", "GroundPlane");
        core::JsonValue comps = core::JsonValue::makeObject();
        comps.set("Transform", makeTransform(0.0, -0.2, 0.0, 0.0, 0.0, 0.0, 1.0, 40.0, 0.4, 40.0));
        core::JsonValue rb = core::JsonValue::makeObject();
        rb.set("motionType", "Static");
        rb.set("collisionLayer", 0);
        comps.set("RigidBody", std::move(rb));
        core::JsonValue col = core::JsonValue::makeObject();
        col.set("type", "Box");
        col.set("halfExtents", makeVec3(20.0, 0.2, 20.0));
        col.set("offset", makeVec3(0.0, 0.0, 0.0));
        col.set("rotation", makeQuat(0.0, 0.0, 0.0, 1.0));
        comps.set("Collider", std::move(col));
        core::JsonValue mesh = core::JsonValue::makeObject();
        mesh.set("assetPath", "Assets/Models/cube.glb");
        mesh.set("submeshIndex", 0);
        mesh.set("visible", true);
        mesh.set("castShadows", false);
        comps.set("Mesh", std::move(mesh));
        ground.set("components", std::move(comps));
        entities.pushBack(std::move(ground));
    }

    // 3. Practice Step / Obstacle for Jump & Camera Collision Testing
    {
        core::JsonValue step = core::JsonValue::makeObject();
        step.set("id", 3);
        step.set("name", "PracticeStep");
        core::JsonValue comps = core::JsonValue::makeObject();
        comps.set("Transform", makeTransform(0.0, 0.5, 5.0, 0.0, 0.0, 0.0, 1.0, 3.0, 1.0, 3.0));
        core::JsonValue rb = core::JsonValue::makeObject();
        rb.set("motionType", "Static");
        rb.set("collisionLayer", 0);
        comps.set("RigidBody", std::move(rb));
        core::JsonValue col = core::JsonValue::makeObject();
        col.set("type", "Box");
        col.set("halfExtents", makeVec3(1.5, 0.5, 1.5));
        col.set("offset", makeVec3(0.0, 0.0, 0.0));
        col.set("rotation", makeQuat(0.0, 0.0, 0.0, 1.0));
        comps.set("Collider", std::move(col));
        core::JsonValue mesh = core::JsonValue::makeObject();
        mesh.set("assetPath", "Assets/Models/cube.glb");
        mesh.set("submeshIndex", 0);
        mesh.set("visible", true);
        mesh.set("castShadows", true);
        comps.set("Mesh", std::move(mesh));
        step.set("components", std::move(comps));
        entities.pushBack(std::move(step));
    }

    // 4. PlayerCharacter Root
    {
        core::JsonValue player = core::JsonValue::makeObject();
        player.set("id", 4);
        player.set("name", "PlayerCharacter");
        core::JsonValue comps = core::JsonValue::makeObject();
        comps.set("Transform", makeTransform(0.0, 0.05, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0));
        comps.set("CharacterTag", core::JsonValue::makeObject());
        comps.set("PlayerControlledTag", core::JsonValue::makeObject());

        core::JsonValue cfg = core::JsonValue::makeObject();
        cfg.set("capsuleRadiusMeters", 0.35);
        cfg.set("capsuleHalfHeightMeters", 0.70);
        cfg.set("eyeHeightMeters", 1.55);
        cfg.set("maxMoveSpeedMetersPerSec", 6.0);
        cfg.set("groundAccelerationMetersPerSecSq", 35.0);
        cfg.set("groundDecelerationMetersPerSecSq", 40.0);
        cfg.set("airAccelerationMetersPerSecSq", 12.0);
        cfg.set("rotationSpeedRadiansPerSec", 12.0);
        cfg.set("jumpSpeedMetersPerSec", 5.5);
        cfg.set("gravityMetersPerSecSq", 18.0);
        cfg.set("groundCheckDistanceMeters", 0.08);
        cfg.set("pushImpulseNewtonSeconds", 8.0);
        cfg.set("maxSlopeDegrees", 45.0);
        cfg.set("stepOffsetMeters", 0.35);
        cfg.set("massKilograms", 70.0);
        comps.set("CharacterConfig", std::move(cfg));

        core::JsonValue st = core::JsonValue::makeObject();
        st.set("velocityMetersPerSec", makeVec3(0.0, 0.0, 0.0));
        st.set("groundNormal", makeVec3(0.0, 1.0, 0.0));
        st.set("facingYawRadians", 0.0);
        st.set("isGrounded", false);
        comps.set("CharacterState", std::move(st));

        core::JsonValue intent = core::JsonValue::makeObject();
        intent.set("moveAxis", makeVec2(0.0, 0.0));
        intent.set("jumpRequested", false);
        intent.set("cameraYawRadians", 0.0);
        intent.set("hasCameraYaw", false);
        comps.set("CharacterMovementIntent", std::move(intent));

        core::JsonValue anim = core::JsonValue::makeObject();
        anim.set("skeletonMeshPath", "Assets/Characters/Player/Superhero_Male_FullBody.gltf");
        anim.set("animationLibraryPath", "Assets/Animations/Player/UAL1_Standard.glb");
        anim.set("idleClip", "Idle_Loop");
        anim.set("walkClip", "Walk_Loop");
        anim.set("runClip", "Jog_Fwd_Loop");
        anim.set("jumpClip", "Jump_Start");
        anim.set("inAirClip", "Jump_Loop");
        anim.set("landClip", "Jump_Land");
        anim.set("walkSpeedThreshold", 0.25);
        anim.set("runSpeedThreshold", 3.20);
        anim.set("blendDurationSeconds", 0.14);
        anim.set("landingDurationSeconds", 0.22);
        comps.set("CharacterAnimation", std::move(anim));

        core::JsonValue rb = core::JsonValue::makeObject();
        rb.set("motionType", "Kinematic");
        rb.set("collisionLayer", 2);
        comps.set("RigidBody", std::move(rb));

        core::JsonValue col = core::JsonValue::makeObject();
        col.set("type", "Capsule");
        col.set("halfExtents", makeVec3(0.35, 1.05, 0.35));
        col.set("radius", 0.35);
        col.set("halfHeight", 0.70);
        col.set("offset", makeVec3(0.0, 1.05, 0.0));
        col.set("rotation", makeQuat(0.0, 0.0, 0.0, 1.0));
        comps.set("Collider", std::move(col));

        player.set("components", std::move(comps));
        entities.pushBack(std::move(player));
    }

    // 5. PlayerVisual Child (Modular Appearance)
    {
        core::JsonValue vis = core::JsonValue::makeObject();
        vis.set("id", 5);
        vis.set("parent", 4);
        vis.set("name", "PlayerVisual");
        core::JsonValue comps = core::JsonValue::makeObject();
        comps.set("Transform", makeTransform(0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0));
        core::JsonValue mesh = core::JsonValue::makeObject();
        mesh.set("assetPath", "Assets/Characters/Player/Superhero_Male_FullBody.gltf");
        mesh.set("submeshIndex", -1);
        mesh.set("visible", true);
        mesh.set("castShadows", true);
        comps.set("Mesh", std::move(mesh));
        vis.set("components", std::move(comps));
        entities.pushBack(std::move(vis));
    }

    // 6. ThirdPersonCamera
    {
        core::JsonValue cam = core::JsonValue::makeObject();
        cam.set("id", 6);
        cam.set("name", "ThirdPersonCamera");
        core::JsonValue comps = core::JsonValue::makeObject();
        comps.set("Transform", makeTransform(0.0, 2.0, -5.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0));
        core::JsonValue cComp = core::JsonValue::makeObject();
        cComp.set("fovDegrees", 60.0);
        cComp.set("nearPlane", 0.1);
        cComp.set("farPlane", 500.0);
        cComp.set("isPrimary", true);
        comps.set("Camera", std::move(cComp));
        core::JsonValue tpc = core::JsonValue::makeObject();
        tpc.set("targetEntityId", 4);
        tpc.set("yawRadians", 0.0);
        tpc.set("pitchRadians", 0.25);
        tpc.set("minPitchRadians", -0.60);
        tpc.set("maxPitchRadians", 1.20);
        tpc.set("distanceMeters", 5.0);
        tpc.set("minDistanceMeters", 1.0);
        tpc.set("heightOffsetMeters", 1.55);
        tpc.set("collisionRadiusMeters", 0.20);
        tpc.set("followSmoothSpeed", 18.0);
        comps.set("ThirdPersonCamera", std::move(tpc));
        cam.set("components", std::move(comps));
        entities.pushBack(std::move(cam));
    }

    root.set("entities", std::move(entities));
    return root.serialize(2) + "\n";
}

std::string buildProjectTppPrefabJson() {
    core::JsonValue root = core::JsonValue::makeObject();
    root.set("formatVersion", 1);
    root.set("name", "ThirdPersonCharacter");

    auto makeVec2 = [](double x, double y) {
        core::JsonValue arr = core::JsonValue::makeArray();
        arr.pushBack(x);
        arr.pushBack(y);
        return arr;
    };
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

    core::JsonValue entities = core::JsonValue::makeArray();

    {
        core::JsonValue player = core::JsonValue::makeObject();
        player.set("id", 1);
        player.set("name", "PlayerCharacter");
        core::JsonValue comps = core::JsonValue::makeObject();

        core::JsonValue tr = core::JsonValue::makeObject();
        tr.set("position", makeVec3(0.0, 0.05, 0.0));
        tr.set("rotation", makeQuat(0.0, 0.0, 0.0, 1.0));
        tr.set("scale", makeVec3(1.0, 1.0, 1.0));
        comps.set("Transform", std::move(tr));
        comps.set("CharacterTag", core::JsonValue::makeObject());
        comps.set("PlayerControlledTag", core::JsonValue::makeObject());

        core::JsonValue cfg = core::JsonValue::makeObject();
        cfg.set("capsuleRadiusMeters", 0.35);
        cfg.set("capsuleHalfHeightMeters", 0.70);
        cfg.set("eyeHeightMeters", 1.55);
        cfg.set("maxMoveSpeedMetersPerSec", 6.0);
        cfg.set("groundAccelerationMetersPerSecSq", 35.0);
        cfg.set("groundDecelerationMetersPerSecSq", 40.0);
        cfg.set("airAccelerationMetersPerSecSq", 12.0);
        cfg.set("rotationSpeedRadiansPerSec", 12.0);
        cfg.set("jumpSpeedMetersPerSec", 5.5);
        cfg.set("gravityMetersPerSecSq", 18.0);
        cfg.set("groundCheckDistanceMeters", 0.08);
        cfg.set("pushImpulseNewtonSeconds", 8.0);
        cfg.set("maxSlopeDegrees", 45.0);
        cfg.set("stepOffsetMeters", 0.35);
        cfg.set("massKilograms", 70.0);
        comps.set("CharacterConfig", std::move(cfg));

        core::JsonValue st = core::JsonValue::makeObject();
        st.set("velocityMetersPerSec", makeVec3(0.0, 0.0, 0.0));
        st.set("groundNormal", makeVec3(0.0, 1.0, 0.0));
        st.set("facingYawRadians", 0.0);
        st.set("isGrounded", false);
        comps.set("CharacterState", std::move(st));

        core::JsonValue intent = core::JsonValue::makeObject();
        intent.set("moveAxis", makeVec2(0.0, 0.0));
        intent.set("jumpRequested", false);
        intent.set("cameraYawRadians", 0.0);
        intent.set("hasCameraYaw", false);
        comps.set("CharacterMovementIntent", std::move(intent));

        core::JsonValue anim = core::JsonValue::makeObject();
        anim.set("skeletonMeshPath", "Assets/Characters/Player/Superhero_Male_FullBody.gltf");
        anim.set("animationLibraryPath", "Assets/Animations/Player/UAL1_Standard.glb");
        anim.set("idleClip", "Idle_Loop");
        anim.set("walkClip", "Walk_Loop");
        anim.set("runClip", "Jog_Fwd_Loop");
        anim.set("jumpClip", "Jump_Start");
        anim.set("inAirClip", "Jump_Loop");
        anim.set("landClip", "Jump_Land");
        anim.set("walkSpeedThreshold", 0.25);
        anim.set("runSpeedThreshold", 3.20);
        anim.set("blendDurationSeconds", 0.14);
        anim.set("landingDurationSeconds", 0.22);
        comps.set("CharacterAnimation", std::move(anim));

        core::JsonValue rb = core::JsonValue::makeObject();
        rb.set("motionType", "Kinematic");
        rb.set("collisionLayer", 2);
        comps.set("RigidBody", std::move(rb));

        core::JsonValue col = core::JsonValue::makeObject();
        col.set("type", "Capsule");
        col.set("halfExtents", makeVec3(0.35, 1.05, 0.35));
        col.set("radius", 0.35);
        col.set("halfHeight", 0.70);
        col.set("offset", makeVec3(0.0, 1.05, 0.0));
        col.set("rotation", makeQuat(0.0, 0.0, 0.0, 1.0));
        comps.set("Collider", std::move(col));

        player.set("components", std::move(comps));
        entities.pushBack(std::move(player));
    }

    {
        core::JsonValue vis = core::JsonValue::makeObject();
        vis.set("id", 2);
        vis.set("parent", 1);
        vis.set("name", "PlayerVisual");
        core::JsonValue comps = core::JsonValue::makeObject();
        core::JsonValue tr = core::JsonValue::makeObject();
        tr.set("position", makeVec3(0.0, 0.0, 0.0));
        tr.set("rotation", makeQuat(0.0, 1.0, 0.0, 0.0));
        tr.set("scale", makeVec3(1.0, 1.0, 1.0));
        comps.set("Transform", std::move(tr));
        core::JsonValue mesh = core::JsonValue::makeObject();
        mesh.set("assetPath", "Assets/Characters/Player/Superhero_Male_FullBody.gltf");
        mesh.set("submeshIndex", -1);
        mesh.set("visible", true);
        mesh.set("castShadows", true);
        comps.set("Mesh", std::move(mesh));
        vis.set("components", std::move(comps));
        entities.pushBack(std::move(vis));
    }

    root.set("entities", std::move(entities));
    return root.serialize(2) + "\n";
}

std::string buildDefaultInputBindingsJson() {
    return R"({
  "formatVersion": 1,
  "activeContext": "ThirdPersonGameplay",
  "actions": [
    { "name": "MoveForward", "key": "W", "scale": 1.0 },
    { "name": "MoveBackward", "key": "S", "scale": -1.0 },
    { "name": "MoveRight", "key": "D", "scale": 1.0 },
    { "name": "MoveLeft", "key": "A", "scale": -1.0 },
    { "name": "Jump", "key": "Space", "scale": 1.0 },
    { "name": "LookYaw", "source": "MouseDeltaX", "sensitivity": 0.0025 },
    { "name": "LookPitch", "source": "MouseDeltaY", "sensitivity": 0.0025 }
  ]
}
)";
}

std::filesystem::path resolveRepoSourcePath(const std::filesystem::path& repoRoot,
                                            const std::filesystem::path& relPath) {
    std::error_code ec;
    if (!repoRoot.empty()) {
        const std::filesystem::path direct = repoRoot / relPath;
        if (std::filesystem::exists(direct, ec)) {
            return direct;
        }
    }
    std::filesystem::path probe = relPath;
    for (int depth = 0; depth < 5; ++depth) {
        if (std::filesystem::exists(probe, ec)) {
            return probe;
        }
        probe = std::filesystem::path("..") / probe;
    }
    return {};
}

Result<void> copyDirectoryFiles(const std::filesystem::path& srcDir,
                                const std::filesystem::path& dstDir, ProjectCreateResult& result) {
    std::error_code ec;
    if (srcDir.empty() || !std::filesystem::exists(srcDir, ec) ||
        !std::filesystem::is_directory(srcDir, ec)) {
        return Result<void>::success();
    }
    if (!std::filesystem::create_directories(dstDir, ec) && ec) {
        return Error{.code = ErrorCode::InvalidState,
                     .message = "Failed to create asset directory: " + dstDir.string()};
    }
    result.createdPaths.push_back(dstDir);
    for (const auto& entry : std::filesystem::directory_iterator(srcDir, ec)) {
        if (ec) {
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed iterating source asset directory: " + srcDir.string()};
        }
        if (entry.is_regular_file(ec)) {
            const std::filesystem::path dstFile = dstDir / entry.path().filename();
            if (!std::filesystem::copy_file(
                    entry.path(), dstFile, std::filesystem::copy_options::overwrite_existing, ec) ||
                ec) {
                return Error{.code = ErrorCode::InvalidState,
                             .message = "Failed copying asset file: " + entry.path().string()};
            }
            result.createdPaths.push_back(dstFile);
        }
    }
    return Result<void>::success();
}

std::string buildReadme(const ProjectCreateRequest& req, const ProjectTemplateDesc& tmpl) {
    std::string text = "# " + req.projectName + "\n\n";
    text += "Created with ANTERAY Hub (`" + std::string(kCurrentEngineVersion) + "`)\n";
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
        text += "ANTERAY's `Conductor` manages two standby/active time states (`StateA` and "
                "`StateB`) over a single shared Flecs `World`. Use the Editor toolbar's "
                "`Switch to State B` / `Play` controls to inspect transitions.\n";
    } else if (tmpl.id == "third_person_character") {
        text += "\n## Third-Person Character Template\n\n";
        text +=
            "Self-contained third-person character starter project:\n"
            "- `Scenes/ThirdPersonTest.hikari`: clean ground + practice step + `PlayerCharacter` "
            "with `ThirdPersonCamera`\n"
            "- `Assets/Prefabs/third_person_character.hikari_prefab`: reusable character prefab "
            "separating Character Core, Movement Intent, Camera, Animation, and Appearance\n"
            "- `Config/input_bindings.json`: editable default WASD / Space / Mouse bindings\n"
            "- `Assets/Characters/Player/` & `Assets/Animations/Player/`: copied CC0 1.0 example "
            "assets with `LICENSE_CC0.txt`\n"
            "- `Assets/Models/cube.glb`: ground and step geometry\n";
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
                "Minimal valid ANTERAY project layout with no sample assets or startup scene.",
            .defaultStartupScene = "",
            .capabilities = {"Valid .hikari-project manifest",
                             "Standard Assets/Scenes/Scripts/Config folders"},
            .requirements = {"ANTERAY ENGINE 0.1.0"},
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
            .requirements = {"ANTERAY ENGINE 0.1.0", "Vulkan or Null RHI"},
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
            .requirements = {"ANTERAY ENGINE 0.1.0",
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
            .requirements = {"ANTERAY ENGINE 0.1.0", "Jolt Physics"},
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
            .requirements = {"ANTERAY ENGINE 0.1.0", "Lua 5.4 runtime"},
        },
        ProjectTemplateDesc{
            .id = "two_state_conductor",
            .name = "Two-State Conductor Demo",
            .category = "Architecture",
            .description = "Starter project configured to explore ANTERAY's Two-State Conductor "
                           "(StateA/StateB) "
                           "over a shared ECS World in the Editor.",
            .defaultStartupScene = "Scenes/ConductorDemo.hikari",
            .capabilities = {"Shared World scene", "StateA / StateB transition inspection"},
            .requirements = {"ANTERAY ENGINE 0.1.0", "Conductor framework"},
        },
        ProjectTemplateDesc{
            .id = "third_person_character",
            .name = "Third-Person Character Template",
            .category = "Gameplay",
            .description =
                "Clean, self-contained third-person character starter with CharacterController, "
                "ThirdPersonCamera, GameplayInputSystem defaults, skeletal locomotion, and "
                "replaceable character appearance.",
            .defaultStartupScene = "Scenes/ThirdPersonTest.hikari",
            .capabilities = {"Physics-based CharacterController",
                             "ThirdPersonCamera follow & raycast",
                             "GameplayInputSystem WASD/Space/Mouse orbit",
                             "Reusable third_person_character.hikari_prefab",
                             "Self-contained CC0 character & animation assets"},
            .requirements = {"ANTERAY ENGINE 0.1.0", "Jolt Physics"},
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
        std::string sceneContent;
        if (tmpl.id == "third_person_character") {
            sceneContent = buildCleanTppSceneJson(request.projectName + " - Third-Person Starter");
        }
        if (sceneContent.empty()) {
            const bool includeDynamic = (tmpl.id == "physics_test");
            sceneContent =
                buildSceneJson(request.projectName + " - " + tmpl.name, true, includeDynamic, true);
        }
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
    } else if (tmpl.id == "third_person_character") {
        // 1. Write self-contained reusable prefab using project-relative asset paths
        const std::filesystem::path prefabsDir = projectDir / "Assets" / "Prefabs";
        if (!std::filesystem::create_directories(prefabsDir, ec) && ec) {
            rollback();
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed to create Assets/Prefabs directory"};
        }
        result.createdPaths.push_back(prefabsDir);
        const std::filesystem::path prefabOut = prefabsDir / "third_person_character.hikari_prefab";
        auto writePrefabRes = writeTextFile(prefabOut, buildProjectTppPrefabJson());
        if (!writePrefabRes.hasValue()) {
            rollback();
            return writePrefabRes.error();
        }
        result.createdPaths.push_back(prefabOut);

        // 2. Write default input configuration
        const std::filesystem::path inputCfgOut = projectDir / "Config" / "input_bindings.json";
        auto writeInputRes = writeTextFile(inputCfgOut, buildDefaultInputBindingsJson());
        if (!writeInputRes.hasValue()) {
            rollback();
            return writeInputRes.error();
        }
        result.createdPaths.push_back(inputCfgOut);

        // 3. Copy default CC0 character, animation, and ground cube assets + licenses
        const std::filesystem::path charSrc =
            resolveRepoSourcePath(request.repositoryRoot, "assets/characters/player");
        auto copyCharRes =
            copyDirectoryFiles(charSrc, projectDir / "Assets" / "Characters" / "Player", result);
        if (!copyCharRes.hasValue()) {
            rollback();
            return copyCharRes.error();
        }

        const std::filesystem::path animSrc =
            resolveRepoSourcePath(request.repositoryRoot, "assets/animations/player");
        auto copyAnimRes =
            copyDirectoryFiles(animSrc, projectDir / "Assets" / "Animations" / "Player", result);
        if (!copyAnimRes.hasValue()) {
            rollback();
            return copyAnimRes.error();
        }

        const std::filesystem::path cubeSrc =
            resolveRepoSourcePath(request.repositoryRoot, "assets/models/cube.glb");
        if (!cubeSrc.empty()) {
            const std::filesystem::path modelsDir = projectDir / "Assets" / "Models";
            if (!std::filesystem::create_directories(modelsDir, ec) && ec) {
                rollback();
                return Error{.code = ErrorCode::InvalidState,
                             .message = "Failed to create Assets/Models directory"};
            }
            result.createdPaths.push_back(modelsDir);
            const std::filesystem::path cubeDst = modelsDir / "cube.glb";
            if (!std::filesystem::copy_file(
                    cubeSrc, cubeDst, std::filesystem::copy_options::overwrite_existing, ec) ||
                ec) {
                rollback();
                return Error{.code = ErrorCode::InvalidState,
                             .message = "Failed copying cube.glb into Assets/Models"};
            }
            result.createdPaths.push_back(cubeDst);
        }
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
    } else if (tmpl.id == "third_person_character") {
        // 1. Write self-contained reusable prefab using project-relative asset paths
        const std::filesystem::path prefabsDir = projectDir / "Assets" / "Prefabs";
        if (!std::filesystem::create_directories(prefabsDir, ec) && ec) {
            rollback();
            return Error{.code = ErrorCode::InvalidState,
                         .message = "Failed to create Assets/Prefabs directory"};
        }
        result.createdPaths.push_back(prefabsDir);
        const std::filesystem::path prefabOut = prefabsDir / "third_person_character.hikari_prefab";
        auto writePrefabRes = writeTextFile(prefabOut, buildProjectTppPrefabJson());
        if (!writePrefabRes.hasValue()) {
            rollback();
            return writePrefabRes.error();
        }
        result.createdPaths.push_back(prefabOut);

        // 2. Write default input configuration
        const std::filesystem::path inputCfgOut = projectDir / "Config" / "input_bindings.json";
        auto writeInputRes = writeTextFile(inputCfgOut, buildDefaultInputBindingsJson());
        if (!writeInputRes.hasValue()) {
            rollback();
            return writeInputRes.error();
        }
        result.createdPaths.push_back(inputCfgOut);

        // 3. Copy default CC0 character, animation, and ground cube assets + licenses
        const std::filesystem::path charSrc =
            resolveRepoSourcePath(request.repositoryRoot, "assets/characters/player");
        auto copyCharRes =
            copyDirectoryFiles(charSrc, projectDir / "Assets" / "Characters" / "Player", result);
        if (!copyCharRes.hasValue()) {
            rollback();
            return copyCharRes.error();
        }

        const std::filesystem::path animSrc =
            resolveRepoSourcePath(request.repositoryRoot, "assets/animations/player");
        auto copyAnimRes =
            copyDirectoryFiles(animSrc, projectDir / "Assets" / "Animations" / "Player", result);
        if (!copyAnimRes.hasValue()) {
            rollback();
            return copyAnimRes.error();
        }

        const std::filesystem::path cubeSrc =
            resolveRepoSourcePath(request.repositoryRoot, "assets/models/cube.glb");
        if (!cubeSrc.empty()) {
            const std::filesystem::path modelsDir = projectDir / "Assets" / "Models";
            if (!std::filesystem::create_directories(modelsDir, ec) && ec) {
                rollback();
                return Error{.code = ErrorCode::InvalidState,
                             .message = "Failed to create Assets/Models directory"};
            }
            result.createdPaths.push_back(modelsDir);
            const std::filesystem::path cubeDst = modelsDir / "cube.glb";
            if (!std::filesystem::copy_file(
                    cubeSrc, cubeDst, std::filesystem::copy_options::overwrite_existing, ec) ||
                ec) {
                rollback();
                return Error{.code = ErrorCode::InvalidState,
                             .message = "Failed copying cube.glb into Assets/Models"};
            }
            result.createdPaths.push_back(cubeDst);
        }
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
