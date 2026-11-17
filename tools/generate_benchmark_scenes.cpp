/// @file generate_benchmark_scenes.cpp
/// @brief Generator tool synthesizing persistent .hikari benchmark scene files.
///
/// Purpose:
/// Imports Khronos glTF benchmark assets (Sponza, FlightHelmet, DamagedHelmet),
/// attaches directional sunlight with shadow mapping, primary framed camera entities,
/// ACES tone mapping and bloom post-processing settings, and writes self-contained
/// portable .hikari scene files into assets/benchmark-scenes/.

#include "core/log.h"
#include "framework/scene_serializer.h"
#include "render/asset_manager.h"
#include "render/camera_component.h"
#include "render/gltf_scene_importer.h"
#include "render/irradiance_probe.h"
#include "render/light_components.h"
#include "render/post_process_types.h"
#include "world/hierarchy_systems.h"
#include "world/world.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

using namespace eng;

static bool generateSponza(const std::filesystem::path& outDir) {
    const std::string gltfPath = "test-assets/benchmark/sponza/Sponza.gltf";
    if (!std::filesystem::exists(gltfPath)) {
        std::cerr << "Sponza glTF source not found: " << gltfPath << '\n';
        return false;
    }

    world::World world;
    render::AssetManager assetMgr;
    auto importRes = render::GltfSceneImporter::importScene(gltfPath, world, &assetMgr);
    if (!importRes.success) {
        std::cerr << "Failed to import Sponza: " << importRes.errorMessage << '\n';
        return false;
    }

    // Directional Sunlight with shadow mapping
    auto sun = world.createEntity("Sunlight");
    render::DirectionalLightComponent dl{};
    dl.direction = Vec3(0.4F, 0.85F, 0.3F).normalized();
    dl.color = Vec3(1.0F, 0.96F, 0.90F);
    dl.intensity = 3.5F;
    dl.castShadows = true;
    dl.shadowBias = 0.0025F;
    dl.enabled = true;
    world.setComponent(sun, dl);

    // Primary Overview Camera looking down the atrium colonnade
    auto cam = world.createEntity("DefaultBenchmarkCamera");
    world.setComponent(cam, world::TransformComponent{Transform(Vec3(0.0F, 2.0F, 0.0F))});
    render::CameraComponent cc{};
    cc.fovDegrees = 65.0F;
    cc.nearPlane = 0.1F;
    cc.farPlane = 2000.0F;
    cc.isPrimary = true;
    world.setComponent(cam, cc);

    // Localized Scene Irradiance Probes for Milestone 11D GI
    auto probeAtrium = world.createEntity("IrradianceProbe_AtriumCenter");
    render::IrradianceProbeComponent ipAtrium{};
    ipAtrium.position = Vec3(0.0F, 2.0F, 0.0F);
    ipAtrium.radius = 25.0F;
    ipAtrium.intensity = 1.0F;
    ipAtrium.enabled = true;
    // Ambient diffuse bounce: warm sandstone floor + open blue skylight
    ipAtrium.sh = render::SphericalHarmonicsL2::fromDirectional(Vec3(0.0F, -1.0F, 0.0F),
                                                                Vec3(0.25F, 0.22F, 0.18F));
    ipAtrium.sh += render::SphericalHarmonicsL2::fromDirectional(Vec3(0.0F, 1.0F, 0.0F),
                                                                 Vec3(0.10F, 0.14F, 0.22F));
    world.setComponent(probeAtrium, ipAtrium);

    world::updateHierarchyTransforms(world);

    // Post-processing: AgX tone mapping + exposure 1.0 + bloom
    render::PostProcessSettings pp{};
    pp.enabled = true;
    pp.exposure = 1.0F;
    pp.toneMapOp = render::ToneMapOperator::AgX;
    pp.bloomEnabled = true;
    pp.bloomThreshold = 1.0F;
    pp.bloomIntensity = 0.04F;
    pp.bloomKnee = 0.5F;
    pp.reinhardWhitePoint = 4.0F;

    framework::SceneMetadata meta{
        .name = "Sponza Benchmark",
        .generator = "ANTERAY ENGINE 10G Benchmark Certification",
        .timestamp = 20261112,
    };

    const Vec3 camPos(0.0F, 2.0F, 0.0F);
    const Vec3 camTarget(0.0F, 2.0F, -6.0F);

    const std::filesystem::path dest = outDir / "Sponza_Benchmark.hikari";
    auto serRes =
        framework::SceneSerializer::serializeToFile(world, dest, meta, {}, pp, camPos, camTarget);
    if (!serRes.hasValue()) {
        std::cerr << "Failed to serialize Sponza benchmark: " << serRes.error().message << '\n';
        return false;
    }

    std::cout << "[SUCCESS] Wrote " << dest.string() << " (" << importRes.nodeCount + 2
              << " entities)\n";
    return true;
}

static bool generateFlightHelmet(const std::filesystem::path& outDir) {
    const std::string gltfPath = "test-assets/benchmark/flight_helmet/FlightHelmet.gltf";
    if (!std::filesystem::exists(gltfPath)) {
        std::cerr << "FlightHelmet glTF source not found: " << gltfPath << '\n';
        return false;
    }

    world::World world;
    render::AssetManager assetMgr;
    auto importRes = render::GltfSceneImporter::importScene(gltfPath, world, &assetMgr);
    if (!importRes.success) {
        std::cerr << "Failed to import FlightHelmet: " << importRes.errorMessage << '\n';
        return false;
    }

    // Directional Key Light
    auto sun = world.createEntity("KeyLight");
    render::DirectionalLightComponent dl{};
    dl.direction = Vec3(0.3F, 0.8F, 0.5F).normalized();
    dl.color = Vec3(1.0F, 1.0F, 1.0F);
    dl.intensity = 3.0F;
    dl.castShadows = true;
    dl.shadowBias = 0.0015F;
    dl.enabled = true;
    world.setComponent(sun, dl);

    // Primary Framed Camera
    auto cam = world.createEntity("DefaultBenchmarkCamera");
    world.setComponent(cam, world::TransformComponent{Transform(Vec3(0.0F, 0.35F, 0.85F))});
    render::CameraComponent cc{};
    cc.fovDegrees = 60.0F;
    cc.nearPlane = 0.05F;
    cc.farPlane = 500.0F;
    cc.isPrimary = true;
    world.setComponent(cam, cc);

    world::updateHierarchyTransforms(world);

    // Post-processing
    render::PostProcessSettings pp{};
    pp.enabled = true;
    pp.exposure = 1.0F;
    pp.toneMapOp = render::ToneMapOperator::AgX;
    pp.bloomEnabled = true;
    pp.bloomThreshold = 1.0F;
    pp.bloomIntensity = 0.04F;
    pp.bloomKnee = 0.5F;

    framework::SceneMetadata meta{
        .name = "Flight Helmet Benchmark",
        .generator = "ANTERAY ENGINE 10G Benchmark Certification",
        .timestamp = 20261112,
    };

    const Vec3 camPos(0.0F, 0.35F, 0.85F);
    const Vec3 camTarget(0.0F, 0.2F, 0.0F);

    const std::filesystem::path dest = outDir / "FlightHelmet_Benchmark.hikari";
    auto serRes =
        framework::SceneSerializer::serializeToFile(world, dest, meta, {}, pp, camPos, camTarget);
    if (!serRes.hasValue()) {
        std::cerr << "Failed to serialize FlightHelmet benchmark: " << serRes.error().message
                  << '\n';
        return false;
    }

    std::cout << "[SUCCESS] Wrote " << dest.string() << " (" << importRes.nodeCount + 2
              << " entities)\n";
    return true;
}

static bool generateDamagedHelmet(const std::filesystem::path& outDir) {
    const std::string glbPath = "test-assets/DamagedHelmet.glb";
    if (!std::filesystem::exists(glbPath)) {
        std::cerr << "DamagedHelmet GLB source not found: " << glbPath << '\n';
        return false;
    }

    world::World world;
    render::AssetManager assetMgr;
    auto importRes = render::GltfSceneImporter::importScene(glbPath, world, &assetMgr);
    if (!importRes.success) {
        std::cerr << "Failed to import DamagedHelmet: " << importRes.errorMessage << '\n';
        return false;
    }

    // Directional Sunlight
    auto sun = world.createEntity("Sunlight");
    render::DirectionalLightComponent dl{};
    dl.direction = Vec3(0.4F, 0.7F, 0.5F).normalized();
    dl.color = Vec3(1.0F, 0.98F, 0.95F);
    dl.intensity = 3.5F;
    dl.castShadows = true;
    dl.shadowBias = 0.002F;
    dl.enabled = true;
    world.setComponent(sun, dl);

    // Primary Framed Camera
    auto cam = world.createEntity("DefaultBenchmarkCamera");
    world.setComponent(cam, world::TransformComponent{Transform(Vec3(0.0F, 0.0F, 2.5F))});
    render::CameraComponent cc{};
    cc.fovDegrees = 60.0F;
    cc.nearPlane = 0.05F;
    cc.farPlane = 500.0F;
    cc.isPrimary = true;
    world.setComponent(cam, cc);

    world::updateHierarchyTransforms(world);

    // Post-processing
    render::PostProcessSettings pp{};
    pp.enabled = true;
    pp.exposure = 1.0F;
    pp.toneMapOp = render::ToneMapOperator::AgX;
    pp.bloomEnabled = true;
    pp.bloomThreshold = 1.0F;
    pp.bloomIntensity = 0.04F;
    pp.bloomKnee = 0.5F;

    framework::SceneMetadata meta{
        .name = "Damaged Helmet Benchmark",
        .generator = "ANTERAY ENGINE 10G Benchmark Certification",
        .timestamp = 20261112,
    };

    const Vec3 camPos(0.0F, 0.0F, 2.5F);
    const Vec3 camTarget(0.0F, 0.0F, 0.0F);

    const std::filesystem::path dest = outDir / "DamagedHelmet_Benchmark.hikari";
    auto serRes =
        framework::SceneSerializer::serializeToFile(world, dest, meta, {}, pp, camPos, camTarget);
    if (!serRes.hasValue()) {
        std::cerr << "Failed to serialize DamagedHelmet benchmark: " << serRes.error().message
                  << '\n';
        return false;
    }

    std::cout << "[SUCCESS] Wrote " << dest.string() << " (" << importRes.nodeCount + 2
              << " entities)\n";
    return true;
}

static bool generateBistro(const std::filesystem::path& outDir) {
    const std::string gltfPath = "test-assets/benchmark/bistro/bistro.gltf";
    if (!std::filesystem::exists(gltfPath)) {
        std::cerr << "Bistro glTF source not found: " << gltfPath << '\n';
        return false;
    }

    world::World world;
    render::AssetManager assetMgr;
    std::cout << "Importing Amazon Lumberyard Bistro glTF hierarchy...\n";
    auto importRes = render::GltfSceneImporter::importScene(gltfPath, world, &assetMgr);
    if (!importRes.success) {
        std::cerr << "Failed to import Bistro: " << importRes.errorMessage << '\n';
        return false;
    }

    // Directional Sunlight with calibrated afternoon lighting and shadow mapping
    // Sunlight shines from upper-right-rear (towards +X, -Y, +Z) matching Bistro_Exterior_1.png
    auto sun = world.createEntity("Sunlight");
    render::DirectionalLightComponent dl{};
    dl.direction = Vec3(-0.45F, 0.78F, -0.42F).normalized();
    dl.color = Vec3(1.0F, 0.97F, 0.92F);
    dl.intensity = 3.6F;
    dl.castShadows = true;
    dl.shadowBias = 0.0012F;
    dl.enabled = true;
    world.setComponent(sun, dl);

    // Canonical Bistro Hero Corner Camera looking at Le Petit Coin storefront and terrace
    auto cam = world.createEntity("Bistro_Hero_Corner");
    world.setComponent(cam, world::TransformComponent{Transform(Vec3(-22.8F, 2.1F, 12.4F))});
    render::CameraComponent cc{};
    cc.fovDegrees = 52.0F;
    cc.nearPlane = 0.1F;
    cc.farPlane = 2000.0F;
    cc.isPrimary = true;
    world.setComponent(cam, cc);

    // Localized Scene Irradiance Probes for Milestone 11D GI
    auto probeStreet = world.createEntity("IrradianceProbe_Street");
    render::IrradianceProbeComponent ipStreet{};
    ipStreet.position = Vec3(-20.0F, 2.0F, 12.0F);
    ipStreet.radius = 35.0F;
    ipStreet.intensity = 1.0F;
    ipStreet.enabled = true;
    ipStreet.sh = render::SphericalHarmonicsL2::fromDirectional(Vec3(0.0F, -1.0F, 0.0F),
                                                                Vec3(0.18F, 0.16F, 0.14F));
    ipStreet.sh += render::SphericalHarmonicsL2::fromDirectional(Vec3(0.0F, 1.0F, 0.0F),
                                                                 Vec3(0.12F, 0.16F, 0.22F));
    world.setComponent(probeStreet, ipStreet);

    auto probeCafe = world.createEntity("IrradianceProbe_CafeTerrace");
    render::IrradianceProbeComponent ipCafe{};
    ipCafe.position = Vec3(-16.0F, 2.2F, 14.0F);
    ipCafe.radius = 15.0F;
    ipCafe.intensity = 1.2F;
    ipCafe.enabled = true;
    ipCafe.sh = render::SphericalHarmonicsL2::fromDirectional(Vec3(0.0F, 1.0F, 0.0F),
                                                              Vec3(0.24F, 0.14F, 0.10F));
    ipCafe.sh += render::SphericalHarmonicsL2::fromDirectional(Vec3(0.0F, -1.0F, 0.0F),
                                                               Vec3(0.15F, 0.14F, 0.12F));
    world.setComponent(probeCafe, ipCafe);

    auto probeCourtyard = world.createEntity("IrradianceProbe_Courtyard");
    render::IrradianceProbeComponent ipCourt{};
    ipCourt.position = Vec3(4.0F, 2.5F, 0.0F);
    ipCourt.radius = 28.0F;
    ipCourt.intensity = 1.0F;
    ipCourt.enabled = true;
    ipCourt.sh = render::SphericalHarmonicsL2::fromDirectional(Vec3(0.0F, -1.0F, 0.0F),
                                                               Vec3(0.16F, 0.16F, 0.15F));
    ipCourt.sh += render::SphericalHarmonicsL2::fromDirectional(Vec3(0.0F, 1.0F, 0.0F),
                                                                Vec3(0.10F, 0.14F, 0.20F));
    world.setComponent(probeCourtyard, ipCourt);

    world::updateHierarchyTransforms(world);

    // Post-processing: AgX tone mapping + exposure 1.0 + bloom
    render::PostProcessSettings pp{};
    pp.enabled = true;
    pp.exposure = 1.0F;
    pp.toneMapOp = render::ToneMapOperator::AgX;
    pp.bloomEnabled = true;
    pp.bloomThreshold = 1.6F;
    pp.bloomIntensity = 0.035F;
    pp.bloomKnee = 0.5F;
    pp.reinhardWhitePoint = 4.0F;

    framework::SceneMetadata meta{
        .name = "Amazon Lumberyard Bistro Benchmark",
        .generator = "ANTERAY ENGINE Benchmark Certification",
        .timestamp = 20261114,
    };

    const Vec3 camPos(-22.8F, 2.1F, 12.4F);
    const Vec3 camTarget(-14.2F, 2.2F, 14.5F);

    const std::filesystem::path dest = outDir / "Bistro_Benchmark.hikari";
    std::cout << "Serializing Bistro benchmark scene to " << dest.string() << "...\n";
    auto serRes =
        framework::SceneSerializer::serializeToFile(world, dest, meta, {}, pp, camPos, camTarget);
    if (!serRes.hasValue()) {
        std::cerr << "Failed to serialize Bistro benchmark: " << serRes.error().message << '\n';
        return false;
    }

    std::cout << "[SUCCESS] Wrote " << dest.string() << " (" << importRes.nodeCount + 2
              << " entities)\n";
    return true;
}

static bool generateMaterialLabGltf(const std::filesystem::path& outDir) {
    constexpr uint32_t nLat = 32;
    constexpr uint32_t nLon = 32;
    constexpr float radius = 0.45F;
    constexpr float kPi = 3.14159265358979323846F;

    std::vector<float> spherePos;
    std::vector<float> sphereNorm;
    std::vector<float> sphereUv;
    std::vector<float> sphereTan;
    spherePos.reserve((nLat + 1) * (nLon + 1) * 3);
    sphereNorm.reserve((nLat + 1) * (nLon + 1) * 3);
    sphereUv.reserve((nLat + 1) * (nLon + 1) * 2);
    sphereTan.reserve((nLat + 1) * (nLon + 1) * 4);

    for (uint32_t i = 0; i <= nLat; ++i) {
        float theta = kPi * static_cast<float>(i) / static_cast<float>(nLat);
        float sinTheta = std::sin(theta);
        float cosTheta = std::cos(theta);

        for (uint32_t j = 0; j <= nLon; ++j) {
            float phi = 2.0F * kPi * static_cast<float>(j) / static_cast<float>(nLon);
            float sinPhi = std::sin(phi);
            float cosPhi = std::cos(phi);

            float nx = sinTheta * cosPhi;
            float ny = cosTheta;
            float nz = sinTheta * sinPhi;

            spherePos.push_back(radius * nx);
            spherePos.push_back(radius * ny);
            spherePos.push_back(radius * nz);

            sphereNorm.push_back(nx);
            sphereNorm.push_back(ny);
            sphereNorm.push_back(nz);

            sphereUv.push_back(static_cast<float>(j) / static_cast<float>(nLon));
            sphereUv.push_back(static_cast<float>(i) / static_cast<float>(nLat));

            if (sinTheta > 1e-4F) {
                sphereTan.push_back(-sinPhi);
                sphereTan.push_back(0.0F);
                sphereTan.push_back(cosPhi);
                sphereTan.push_back(1.0F);
            } else {
                sphereTan.push_back(1.0F);
                sphereTan.push_back(0.0F);
                sphereTan.push_back(0.0F);
                sphereTan.push_back(1.0F);
            }
        }
    }

    std::vector<uint32_t> sphereIndices;
    sphereIndices.reserve(nLat * nLon * 6);
    for (uint32_t i = 0; i < nLat; ++i) {
        for (uint32_t j = 0; j < nLon; ++j) {
            uint32_t v00 = i * (nLon + 1) + j;
            uint32_t v01 = v00 + 1;
            uint32_t v10 = (i + 1) * (nLon + 1) + j;
            uint32_t v11 = v10 + 1;

            sphereIndices.push_back(v00);
            sphereIndices.push_back(v10);
            sphereIndices.push_back(v01);

            sphereIndices.push_back(v01);
            sphereIndices.push_back(v10);
            sphereIndices.push_back(v11);
        }
    }

    std::vector<float> planePos = {-6.0F, 0.0F, -6.0F, -6.0F, 0.0F, 6.0F,
                                   6.0F,  0.0F, 6.0F,  6.0F,  0.0F, -6.0F};
    std::vector<float> planeNorm = {0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                                    0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    std::vector<float> planeUv = {0.0F, 0.0F, 0.0F, 6.0F, 6.0F, 6.0F, 6.0F, 0.0F};
    std::vector<float> planeTan = {1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F, 1.0F,
                                   1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F, 1.0F};
    std::vector<uint32_t> planeIndices = {0, 1, 2, 0, 2, 3};

    const std::filesystem::path binPath = outDir / "material_lab.bin";
    std::ofstream binFile(binPath, std::ios::binary);
    if (!binFile.is_open()) {
        std::cerr << "Failed to write " << binPath.string() << '\n';
        return false;
    }

    auto writeVec = [&binFile](const auto& v) {
        binFile.write(reinterpret_cast<const char*>(v.data()),
                      static_cast<std::streamsize>(
                          v.size() * sizeof(typename std::decay_t<decltype(v)>::value_type)));
    };

    uint32_t offSpherePos = 0;
    uint32_t lenSpherePos = static_cast<uint32_t>(spherePos.size() * sizeof(float));
    writeVec(spherePos);

    uint32_t offSphereNorm = offSpherePos + lenSpherePos;
    uint32_t lenSphereNorm = static_cast<uint32_t>(sphereNorm.size() * sizeof(float));
    writeVec(sphereNorm);

    uint32_t offSphereUv = offSphereNorm + lenSphereNorm;
    uint32_t lenSphereUv = static_cast<uint32_t>(sphereUv.size() * sizeof(float));
    writeVec(sphereUv);

    uint32_t offSphereTan = offSphereUv + lenSphereUv;
    uint32_t lenSphereTan = static_cast<uint32_t>(sphereTan.size() * sizeof(float));
    writeVec(sphereTan);

    uint32_t offSphereIdx = offSphereTan + lenSphereTan;
    uint32_t lenSphereIdx = static_cast<uint32_t>(sphereIndices.size() * sizeof(uint32_t));
    writeVec(sphereIndices);

    uint32_t offPlanePos = offSphereIdx + lenSphereIdx;
    uint32_t lenPlanePos = static_cast<uint32_t>(planePos.size() * sizeof(float));
    writeVec(planePos);

    uint32_t offPlaneNorm = offPlanePos + lenPlanePos;
    uint32_t lenPlaneNorm = static_cast<uint32_t>(planeNorm.size() * sizeof(float));
    writeVec(planeNorm);

    uint32_t offPlaneUv = offPlaneNorm + lenPlaneNorm;
    uint32_t lenPlaneUv = static_cast<uint32_t>(planeUv.size() * sizeof(float));
    writeVec(planeUv);

    uint32_t offPlaneTan = offPlaneUv + lenPlaneUv;
    uint32_t lenPlaneTan = static_cast<uint32_t>(planeTan.size() * sizeof(float));
    writeVec(planeTan);

    uint32_t offPlaneIdx = offPlaneTan + lenPlaneTan;
    uint32_t lenPlaneIdx = static_cast<uint32_t>(planeIndices.size() * sizeof(uint32_t));
    writeVec(planeIndices);

    uint32_t totalBinBytes = offPlaneIdx + lenPlaneIdx;
    binFile.close();

    const std::filesystem::path gltfPath = outDir / "material_lab.gltf";
    std::ofstream gltfFile(gltfPath);
    if (!gltfFile.is_open()) {
        std::cerr << "Failed to write " << gltfPath.string() << '\n';
        return false;
    }

    std::ostringstream ss;
    ss << "{\n"
       << "  \"asset\": { \"version\": \"2.0\", \"generator\": \"ANTERAY MaterialLab Generator\" "
          "},\n"
       << "  \"extensionsUsed\": [\n"
       << "    \"KHR_materials_clearcoat\",\n"
       << "    \"KHR_materials_sheen\",\n"
       << "    \"KHR_materials_anisotropy\",\n"
       << "    \"KHR_materials_transmission\",\n"
       << "    \"KHR_materials_volume\",\n"
       << "    \"KHR_materials_ior\",\n"
       << "    \"KHR_materials_specular\"\n"
       << "  ],\n"
       << "  \"buffers\": [\n"
       << "    { \"byteLength\": " << totalBinBytes << ", \"uri\": \"material_lab.bin\" }\n"
       << "  ],\n"
       << "  \"bufferViews\": [\n"
       << "    { \"buffer\": 0, \"byteOffset\": " << offSpherePos
       << ", \"byteLength\": " << lenSpherePos << ", \"target\": 34962 },\n"
       << "    { \"buffer\": 0, \"byteOffset\": " << offSphereNorm
       << ", \"byteLength\": " << lenSphereNorm << ", \"target\": 34962 },\n"
       << "    { \"buffer\": 0, \"byteOffset\": " << offSphereUv
       << ", \"byteLength\": " << lenSphereUv << ", \"target\": 34962 },\n"
       << "    { \"buffer\": 0, \"byteOffset\": " << offSphereTan
       << ", \"byteLength\": " << lenSphereTan << ", \"target\": 34962 },\n"
       << "    { \"buffer\": 0, \"byteOffset\": " << offSphereIdx
       << ", \"byteLength\": " << lenSphereIdx << ", \"target\": 34963 },\n"
       << "    { \"buffer\": 0, \"byteOffset\": " << offPlanePos
       << ", \"byteLength\": " << lenPlanePos << ", \"target\": 34962 },\n"
       << "    { \"buffer\": 0, \"byteOffset\": " << offPlaneNorm
       << ", \"byteLength\": " << lenPlaneNorm << ", \"target\": 34962 },\n"
       << "    { \"buffer\": 0, \"byteOffset\": " << offPlaneUv
       << ", \"byteLength\": " << lenPlaneUv << ", \"target\": 34962 },\n"
       << "    { \"buffer\": 0, \"byteOffset\": " << offPlaneTan
       << ", \"byteLength\": " << lenPlaneTan << ", \"target\": 34962 },\n"
       << "    { \"buffer\": 0, \"byteOffset\": " << offPlaneIdx
       << ", \"byteLength\": " << lenPlaneIdx << ", \"target\": 34963 }\n"
       << "  ],\n"
       << "  \"accessors\": [\n"
       << "    { \"bufferView\": 0, \"componentType\": 5126, \"count\": " << (spherePos.size() / 3)
       << ", \"type\": \"VEC3\", \"min\": [-0.45, -0.45, -0.45], \"max\": [0.45, 0.45, 0.45] },\n"
       << "    { \"bufferView\": 1, \"componentType\": 5126, \"count\": " << (sphereNorm.size() / 3)
       << ", \"type\": \"VEC3\" },\n"
       << "    { \"bufferView\": 2, \"componentType\": 5126, \"count\": " << (sphereUv.size() / 2)
       << ", \"type\": \"VEC2\" },\n"
       << "    { \"bufferView\": 3, \"componentType\": 5126, \"count\": " << (sphereTan.size() / 4)
       << ", \"type\": \"VEC4\" },\n"
       << "    { \"bufferView\": 4, \"componentType\": 5125, \"count\": " << sphereIndices.size()
       << ", \"type\": \"SCALAR\" },\n"
       << "    { \"bufferView\": 5, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\", "
          "\"min\": [-6.0, 0.0, -6.0], \"max\": [6.0, 0.0, 6.0] },\n"
       << "    { \"bufferView\": 6, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC3\" },\n"
       << "    { \"bufferView\": 7, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC2\" },\n"
       << "    { \"bufferView\": 8, \"componentType\": 5126, \"count\": 4, \"type\": \"VEC4\" },\n"
       << "    { \"bufferView\": 9, \"componentType\": 5125, \"count\": 6, \"type\": \"SCALAR\" }\n"
       << "  ],\n"
       << "  \"materials\": [\n"
       // Row 0: Dielectrics & IOR
       << "    { \"name\": \"Dielectric_Rough_0_05\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.9, 0.9, 0.9, 1.0], \"metallicFactor\": 0.0, "
          "\"roughnessFactor\": 0.05 } },\n"
       << "    { \"name\": \"Dielectric_Rough_0_25\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.9, 0.9, 0.9, 1.0], \"metallicFactor\": 0.0, "
          "\"roughnessFactor\": 0.25 } },\n"
       << "    { \"name\": \"Dielectric_Rough_0_50\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.9, 0.9, 0.9, 1.0], \"metallicFactor\": 0.0, "
          "\"roughnessFactor\": 0.50 } },\n"
       << "    { \"name\": \"Dielectric_Rough_0_75\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.9, 0.9, 0.9, 1.0], \"metallicFactor\": 0.0, "
          "\"roughnessFactor\": 0.75 } },\n"
       << "    { \"name\": \"Dielectric_Rough_0_95\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.9, 0.9, 0.9, 1.0], \"metallicFactor\": 0.0, "
          "\"roughnessFactor\": 0.95 } },\n"
       << "    { \"name\": \"High_IOR_Diamond\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.95, 0.95, 0.95, 1.0], \"metallicFactor\": 0.0, \"roughnessFactor\": 0.08 }, "
          "\"extensions\": { \"KHR_materials_ior\": { \"ior\": 2.417 }, "
          "\"KHR_materials_specular\": { \"specularFactor\": 1.0 } } },\n"
       // Row 1: Metals & Specular
       << "    { \"name\": \"Polished_Gold\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[1.0, 0.766, 0.336, 1.0], \"metallicFactor\": 1.0, \"roughnessFactor\": 0.08 } },\n"
       << "    { \"name\": \"Polished_Copper\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.955, 0.637, 0.538, 1.0], \"metallicFactor\": 1.0, \"roughnessFactor\": 0.15 } },\n"
       << "    { \"name\": \"Polished_Chrome\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.913, 0.921, 0.925, 1.0], \"metallicFactor\": 1.0, \"roughnessFactor\": 0.05 } },\n"
       << "    { \"name\": \"Satin_Silver\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.972, 0.960, 0.915, 1.0], \"metallicFactor\": 1.0, \"roughnessFactor\": 0.35 } },\n"
       << "    { \"name\": \"Rough_Cast_Iron\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.4, 0.4, 0.4, 1.0], \"metallicFactor\": 1.0, \"roughnessFactor\": 0.75 } },\n"
       << "    { \"name\": \"Specular_Tinted_Dielectric\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.2, 0.2, 0.2, 1.0], \"metallicFactor\": 0.0, "
          "\"roughnessFactor\": 0.2 }, \"extensions\": { \"KHR_materials_specular\": { "
          "\"specularFactor\": 1.0, \"specularColorFactor\": [0.8, 0.2, 0.9] } } },\n"
       // Row 2: Clearcoat & Sheen
       << "    { \"name\": \"Red_Car_Paint\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.8, 0.05, 0.05, 1.0], \"metallicFactor\": 0.0, \"roughnessFactor\": 0.6 }, "
          "\"extensions\": { \"KHR_materials_clearcoat\": { \"clearcoatFactor\": 1.0, "
          "\"clearcoatRoughnessFactor\": 0.03 } } },\n"
       << "    { \"name\": \"Polyurethane_Wood\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.55, 0.35, 0.18, 1.0], \"metallicFactor\": 0.0, \"roughnessFactor\": 0.7 }, "
          "\"extensions\": { \"KHR_materials_clearcoat\": { \"clearcoatFactor\": 0.8, "
          "\"clearcoatRoughnessFactor\": 0.15 } } },\n"
       << "    { \"name\": \"Clearcoat_Carbon\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.05, 0.05, 0.05, 1.0], \"metallicFactor\": 0.0, \"roughnessFactor\": 0.4 }, "
          "\"extensions\": { \"KHR_materials_clearcoat\": { \"clearcoatFactor\": 1.0, "
          "\"clearcoatRoughnessFactor\": 0.05 } } },\n"
       << "    { \"name\": \"Blue_Velvet_Sheen\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.02, 0.05, 0.35, 1.0], \"metallicFactor\": 0.0, \"roughnessFactor\": 0.7 }, "
          "\"extensions\": { \"KHR_materials_sheen\": { \"sheenColorFactor\": [0.4, 0.6, 1.0], "
          "\"sheenRoughnessFactor\": 0.3 } } },\n"
       << "    { \"name\": \"Crimson_Silk_Sheen\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.5, 0.02, 0.05, 1.0], \"metallicFactor\": 0.0, "
          "\"roughnessFactor\": 0.5 }, \"extensions\": { \"KHR_materials_sheen\": { "
          "\"sheenColorFactor\": [1.0, 0.5, 0.6], \"sheenRoughnessFactor\": 0.5 } } },\n"
       << "    { \"name\": \"Peach_Fuzz_Sheen\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.9, 0.6, 0.5, 1.0], \"metallicFactor\": 0.0, \"roughnessFactor\": 0.8 }, "
          "\"extensions\": { \"KHR_materials_sheen\": { \"sheenColorFactor\": [1.0, 0.85, 0.75], "
          "\"sheenRoughnessFactor\": 0.7 } } },\n"
       // Row 3: Anisotropy, Transmission & Multi-Feature Combined
       << "    { \"name\": \"Radial_Brushed_Aluminum\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.9, 0.9, 0.9, 1.0], \"metallicFactor\": 1.0, "
          "\"roughnessFactor\": 0.25 }, \"extensions\": { \"KHR_materials_anisotropy\": { "
          "\"anisotropyStrength\": 0.85, \"anisotropyRotation\": 0.0 } } },\n"
       << "    { \"name\": \"Cross_Brushed_Steel\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.85, 0.85, 0.88, 1.0], \"metallicFactor\": 1.0, "
          "\"roughnessFactor\": 0.35 }, \"extensions\": { \"KHR_materials_anisotropy\": { "
          "\"anisotropyStrength\": 0.75, \"anisotropyRotation\": 1.5708 } } },\n"
       << "    { \"name\": \"Clear_Optical_Glass\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [1.0, 1.0, 1.0, 1.0], \"metallicFactor\": 0.0, "
          "\"roughnessFactor\": 0.02 }, \"extensions\": { \"KHR_materials_transmission\": { "
          "\"transmissionFactor\": 1.0 }, \"KHR_materials_ior\": { \"ior\": 1.52 }, "
          "\"KHR_materials_volume\": { \"thicknessFactor\": 0.9 } } },\n"
       << "    { \"name\": \"Tinted_Emerald_Glass\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.2, 0.9, 0.5, 1.0], \"metallicFactor\": 0.0, "
          "\"roughnessFactor\": 0.05 }, \"extensions\": { \"KHR_materials_transmission\": { "
          "\"transmissionFactor\": 0.9 }, \"KHR_materials_ior\": { \"ior\": 1.55 }, "
          "\"KHR_materials_volume\": { \"thicknessFactor\": 0.9, \"attenuationColor\": [0.2, 0.8, "
          "0.4], \"attenuationDistance\": 1.5 } } },\n"
       << "    { \"name\": \"Coated_Brushed_Metal\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.8, 0.8, 0.8, 1.0], \"metallicFactor\": 1.0, "
          "\"roughnessFactor\": 0.3 }, \"extensions\": { \"KHR_materials_anisotropy\": { "
          "\"anisotropyStrength\": 0.8, \"anisotropyRotation\": 0.0 }, "
          "\"KHR_materials_clearcoat\": { \"clearcoatFactor\": 1.0, \"clearcoatRoughnessFactor\": "
          "0.05 } } },\n"
       << "    { \"name\": \"Coated_Sheen_Fabric\", \"pbrMetallicRoughness\": { "
          "\"baseColorFactor\": [0.1, 0.2, 0.1, 1.0], \"metallicFactor\": 0.0, "
          "\"roughnessFactor\": 0.6 }, \"extensions\": { \"KHR_materials_sheen\": { "
          "\"sheenColorFactor\": [0.5, 0.8, 0.5], \"sheenRoughnessFactor\": 0.4 }, "
          "\"KHR_materials_clearcoat\": { \"clearcoatFactor\": 0.6, \"clearcoatRoughnessFactor\": "
          "0.1 } } },\n"
       // Material 24: Ground floor
       << "    { \"name\": \"Neutral_Floor\", \"pbrMetallicRoughness\": { \"baseColorFactor\": "
          "[0.35, 0.35, 0.35, 1.0], \"metallicFactor\": 0.0, \"roughnessFactor\": 0.6 } }\n"
       << "  ],\n"
       << "  \"meshes\": [\n";

    for (int i = 0; i < 24; ++i) {
        ss << "    { \"name\": \"SphereMesh_" << i
           << "\", \"primitives\": [ { \"attributes\": { \"POSITION\": 0, \"NORMAL\": 1, "
              "\"TEXCOORD_0\": 2, \"TANGENT\": 3 }, \"indices\": 4, \"material\": "
           << i << " } ] },\n";
    }
    ss << "    { \"name\": \"GroundPlaneMesh\", \"primitives\": [ { \"attributes\": { "
          "\"POSITION\": 5, \"NORMAL\": 6, \"TEXCOORD_0\": 7, \"TANGENT\": 8 }, \"indices\": 9, "
          "\"material\": 24 } ] }\n"
       << "  ],\n"
       << "  \"nodes\": [\n";

    constexpr float xCoords[6] = {-3.5F, -2.1F, -0.7F, 0.7F, 2.1F, 3.5F};
    constexpr float zCoords[4] = {-2.1F, -0.7F, 0.7F, 2.1F};

    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 6; ++col) {
            int idx = row * 6 + col;
            ss << "    { \"name\": \"Sphere_" << row << "_" << col << "\", \"mesh\": " << idx
               << ", \"translation\": [ " << xCoords[col] << ", 0.45, " << zCoords[row]
               << " ] },\n";
        }
    }
    ss << "    { \"name\": \"GroundPlane\", \"mesh\": 24, \"translation\": [ 0.0, 0.0, 0.0 ] }\n"
       << "  ],\n"
       << "  \"scene\": 0,\n"
       << "  \"scenes\": [\n"
       << "    { \"name\": \"MaterialLab\", \"nodes\": [";
    for (int i = 0; i < 25; ++i) {
        ss << i << (i < 24 ? ", " : "");
    }
    ss << "] }\n"
       << "  ]\n"
       << "}\n";

    gltfFile << ss.str();
    gltfFile.close();
    return true;
}

static bool generateMaterialLab(const std::filesystem::path& outDir) {
    if (!generateMaterialLabGltf(outDir)) {
        std::cerr << "Failed to synthesize MaterialLab glTF source\n";
        return false;
    }

    const std::string gltfPath = (outDir / "material_lab.gltf").string();
    world::World world;
    render::AssetManager assetMgr;
    std::cout << "Importing MaterialLab glTF hierarchy...\n";
    auto importRes = render::GltfSceneImporter::importScene(gltfPath, world, &assetMgr);
    if (!importRes.success) {
        std::cerr << "Failed to import MaterialLab: " << importRes.errorMessage << '\n';
        return false;
    }

    // Directional Sunlight with calibrated afternoon lighting and shadow mapping
    auto sun = world.createEntity("Sunlight");
    render::DirectionalLightComponent dl{};
    dl.direction = Vec3(0.4F, 0.8F, 0.35F).normalized();
    dl.color = Vec3(1.0F, 0.98F, 0.94F);
    dl.intensity = 3.5F;
    dl.castShadows = true;
    dl.shadowBias = 0.0015F;
    dl.enabled = true;
    world.setComponent(sun, dl);

    // Primary Overview Camera framing the 4x6 grid of material spheres
    auto cam = world.createEntity("DefaultBenchmarkCamera");
    world.setComponent(cam, world::TransformComponent{Transform(Vec3(0.0F, 3.8F, 7.2F))});
    render::CameraComponent cc{};
    cc.fovDegrees = 50.0F;
    cc.nearPlane = 0.1F;
    cc.farPlane = 500.0F;
    cc.isPrimary = true;
    world.setComponent(cam, cc);

    // Neutral Irradiance Probe for ambient diffuse illumination
    auto probe = world.createEntity("IrradianceProbe_MaterialLab");
    render::IrradianceProbeComponent ip{};
    ip.position = Vec3(0.0F, 1.5F, 0.0F);
    ip.radius = 20.0F;
    ip.intensity = 1.0F;
    ip.enabled = true;
    ip.sh = render::SphericalHarmonicsL2::fromDirectional(Vec3(0.0F, -1.0F, 0.0F),
                                                          Vec3(0.18F, 0.18F, 0.18F));
    ip.sh += render::SphericalHarmonicsL2::fromDirectional(Vec3(0.0F, 1.0F, 0.0F),
                                                           Vec3(0.12F, 0.15F, 0.20F));
    world.setComponent(probe, ip);

    world::updateHierarchyTransforms(world);

    // Post-processing: AgX tone mapping + exposure 1.0 + bloom
    render::PostProcessSettings pp{};
    pp.enabled = true;
    pp.exposure = 1.0F;
    pp.toneMapOp = render::ToneMapOperator::AgX;
    pp.bloomEnabled = true;
    pp.bloomThreshold = 1.2F;
    pp.bloomIntensity = 0.03F;
    pp.bloomKnee = 0.5F;
    pp.reinhardWhitePoint = 4.0F;

    framework::SceneMetadata meta{
        .name = "MaterialLab Benchmark",
        .generator = "ANTERAY ENGINE Milestone 11F Phase 6 Visual Certification",
        .timestamp = 20261115,
    };

    const Vec3 camPos(0.0F, 3.8F, 7.2F);
    const Vec3 camTarget(0.0F, 0.45F, 0.0F);

    const std::filesystem::path dest = outDir / "MaterialLab_Benchmark.hikari";
    std::cout << "Serializing MaterialLab benchmark scene to " << dest.string() << "...\n";
    auto serRes =
        framework::SceneSerializer::serializeToFile(world, dest, meta, {}, pp, camPos, camTarget);
    if (!serRes.hasValue()) {
        std::cerr << "Failed to serialize MaterialLab benchmark: " << serRes.error().message
                  << '\n';
        return false;
    }

    std::cout << "[SUCCESS] Wrote " << dest.string() << " (" << importRes.nodeCount + 2
              << " entities)\n";
    return true;
}

int main(int argc, char** argv) {
    std::filesystem::path outDir = "assets/benchmark-scenes";
    if (argc > 1) {
        outDir = argv[1];
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    std::cout << "Generating persistent benchmark scenes into: " << outDir.string() << '\n';

    bool ok = true;
    ok &= generateSponza(outDir);
    ok &= generateFlightHelmet(outDir);
    ok &= generateDamagedHelmet(outDir);
    ok &= generateBistro(outDir);
    ok &= generateMaterialLab(outDir);

    if (!ok) {
        std::cerr << "One or more benchmark scenes failed to generate.\n";
        return 1;
    }

    std::cout << "All benchmark scenes successfully generated.\n";
    return 0;
}
