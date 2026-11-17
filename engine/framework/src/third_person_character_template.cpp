/// @file third_person_character_template.cpp
/// @brief Implementation of the reusable Third-Person Character Template helper.

#include "framework/third_person_character_template.h"

#include "framework/third_person_camera.h"
#include "render/camera_component.h"
#include "render/mesh_component.h"
#include "world/components.h"

#include <system_error>

namespace eng::framework {

namespace {

std::filesystem::path resolvePrefabPath(const std::filesystem::path& inputPath) {
    std::error_code ec;
    if (std::filesystem::exists(inputPath, ec)) {
        return inputPath;
    }
    std::filesystem::path probe = inputPath;
    for (int d = 0; d < 4; ++d) {
        probe = std::filesystem::path("..") / probe;
        if (std::filesystem::exists(probe, ec)) {
            return probe;
        }
    }
    return inputPath;
}

} // namespace

Result<ThirdPersonTemplateResult>
ThirdPersonCharacterTemplate::instantiate(world::World& world,
                                          const PrefabInstantiateOptions& options,
                                          const std::filesystem::path& prefabPath) {
    const std::filesystem::path resolved = resolvePrefabPath(prefabPath);
    auto descRes = PrefabLoader::loadFromFile(resolved);
    if (!descRes.hasValue()) {
        return descRes.error();
    }

    auto instRes = PrefabLoader::instantiate(world, descRes.value(), options);
    if (!instRes.hasValue()) {
        return instRes.error();
    }

    ThirdPersonTemplateResult result{};
    result.characterRoot = instRes.value();

    world.forEachChild(result.characterRoot, [&](world::EntityHandle child) {
        if (!result.visualMesh.isValid() && world.hasComponent<render::MeshComponent>(child)) {
            result.visualMesh = child;
        }
    });

    auto& ecs = world.getRawWorld();
    ecs.each<const ThirdPersonCameraComponent>(
        [&](flecs::entity e, const ThirdPersonCameraComponent& tpc) {
            if (!result.cameraEntity.isValid() && tpc.targetEntityId == result.characterRoot.id) {
                result.cameraEntity = world::EntityHandle{e.id()};
            }
        });

    if (!result.cameraEntity.isValid()) {
        world::EntityHandle camEnt = world.createEntity("ThirdPersonCamera");
        world.setComponent(camEnt, world::TransformComponent{Transform{
                                       Vec3{0.0F, 2.0F, -5.0F}, Quat::identity(), Vec3::one()}});
        world.setComponent(camEnt, render::CameraComponent{
                                       .fovDegrees = 60.0F,
                                       .nearPlane = 0.1F,
                                       .farPlane = 500.0F,
                                       .isPrimary = true,
                                   });
        world.setComponent(camEnt, ThirdPersonCameraComponent{
                                       .targetEntityId = result.characterRoot.id,
                                   });
        if (options.parent.isValid() && world.isAlive(options.parent)) {
            (void)world.setParent(camEnt, options.parent, false);
        }
        result.cameraEntity = camEnt;
    }

    return result;
}

} // namespace eng::framework
