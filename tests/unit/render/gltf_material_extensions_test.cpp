/// @file gltf_material_extensions_test.cpp
/// @brief Comprehensive regression test suite for Milestone 11F Phase 3 glTF material extensions.
///
/// Validates:
/// - KHR_materials_clearcoat parsing and GPU representation
/// - KHR_materials_transmission parsing and GPU representation
/// - KHR_materials_volume parsing and GPU representation
/// - KHR_materials_ior parsing and GPU representation
/// - KHR_materials_specular parsing and GPU representation
/// - MaterialUniformBufferData std140 layout packing (160 bytes)
/// - Equivalence between GltfImporter and GltfSceneImporter
/// - GpuMaterial descriptor set layout creation (13 bindings) and UBO synchronization

#include "core/assert.h"
#include "framework/scene.h"
#include "render/asset_manager.h"
#include "render/gltf_importer.h"
#include "render/gltf_scene_importer.h"
#include "render/gpu_material.h"
#include "render/material.h"
#include "render/mesh_component.h"
#include "rhi/rhi_device.h"
#include "world/world.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace eng;
using namespace eng::render;

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__          \
                      << std::endl;                                                                \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

#define TEST_APPROX(a, b, eps)                                                                     \
    do {                                                                                           \
        if (std::abs((a) - (b)) > (eps)) {                                                         \
            std::cerr << "Approx assertion failed: |" #a " - " #b "| = |" << (a) << " - " << (b)   \
                      << "| > " << (eps) << " at " << __FILE__ << ":" << __LINE__ << std::endl;    \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

namespace {

const std::string kFixturesPath = "tests/fixtures/gltf/extensions_test.gltf";

// -----------------------------------------------------------------------------
// Test 1: CPU Material Model Defaults and Structures
// -----------------------------------------------------------------------------
void testMaterialDefaults() {
    std::cout << "[Test 1] CPU Material Model Defaults..." << std::endl;

    Material mat = Material::createDefault();
    TEST_APPROX(mat.clearcoat.factor, 0.0F, 1e-5F);
    TEST_APPROX(mat.clearcoat.roughnessFactor, 0.0F, 1e-5F);
    TEST_APPROX(mat.transmission.factor, 0.0F, 1e-5F);
    TEST_APPROX(mat.volume.thicknessFactor, 0.0F, 1e-5F);
    TEST_APPROX(mat.iorExt.ior, 1.5F, 1e-5F);
    TEST_APPROX(mat.specular.factor, 1.0F, 1e-5F);

    // Verify default extension values when constructed explicitly
    ClearcoatExtension cc{};
    TEST_APPROX(cc.factor, 0.0F, 1e-5F);
    TEST_APPROX(cc.roughnessFactor, 0.0F, 1e-5F);
    TEST_CHECK(!cc.texture.isPresent);
    TEST_CHECK(!cc.roughnessTexture.isPresent);
    TEST_CHECK(!cc.normalTexture.isPresent);

    TransmissionExtension tr{};
    TEST_APPROX(tr.factor, 0.0F, 1e-5F);
    TEST_CHECK(!tr.texture.isPresent);

    VolumeExtension vol{};
    TEST_APPROX(vol.thicknessFactor, 0.0F, 1e-5F);
    TEST_CHECK(vol.attenuationDistance >= 1e8F);
    TEST_APPROX(vol.attenuationColor.x, 1.0F, 1e-5F);
    TEST_APPROX(vol.attenuationColor.y, 1.0F, 1e-5F);
    TEST_APPROX(vol.attenuationColor.z, 1.0F, 1e-5F);
    TEST_CHECK(!vol.thicknessTexture.isPresent);

    IorExtension ior{};
    TEST_APPROX(ior.ior, 1.5F, 1e-5F);

    SpecularExtension spec{};
    TEST_APPROX(spec.factor, 1.0F, 1e-5F);
    TEST_APPROX(spec.colorFactor.x, 1.0F, 1e-5F);
    TEST_APPROX(spec.colorFactor.y, 1.0F, 1e-5F);
    TEST_APPROX(spec.colorFactor.z, 1.0F, 1e-5F);
    TEST_CHECK(!spec.texture.isPresent);
    TEST_CHECK(!spec.colorTexture.isPresent);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 2: MaterialUniformBufferData std140 Layout (160 Bytes)
// -----------------------------------------------------------------------------
void testUniformBufferDataLayout() {
    std::cout << "[Test 2] MaterialUniformBufferData std140 Layout..." << std::endl;

    TEST_CHECK(sizeof(MaterialUniformBufferData) == 160);
    TEST_CHECK(alignof(MaterialUniformBufferData) == 16);

    // Verify critical field offsets per std140
    TEST_CHECK(offsetof(MaterialUniformBufferData, baseColorFactor) == 0);
    TEST_CHECK(offsetof(MaterialUniformBufferData, emissiveFactorAndAlphaCutoff) == 16);
    TEST_CHECK(offsetof(MaterialUniformBufferData, metallicFactor) == 32);
    TEST_CHECK(offsetof(MaterialUniformBufferData, roughnessFactor) == 36);
    TEST_CHECK(offsetof(MaterialUniformBufferData, normalScale) == 40);
    TEST_CHECK(offsetof(MaterialUniformBufferData, occlusionStrength) == 44);

    TEST_CHECK(offsetof(MaterialUniformBufferData, transmissionFactor) == 76);
    TEST_CHECK(offsetof(MaterialUniformBufferData, ior) == 80);
    TEST_CHECK(offsetof(MaterialUniformBufferData, clearcoatFactor) == 84);
    TEST_CHECK(offsetof(MaterialUniformBufferData, clearcoatRoughness) == 88);
    TEST_CHECK(offsetof(MaterialUniformBufferData, clearcoatNormalScale) == 92);
    TEST_CHECK(offsetof(MaterialUniformBufferData, hasClearcoatTex) == 96);
    TEST_CHECK(offsetof(MaterialUniformBufferData, hasClearcoatRoughnessTex) == 100);
    TEST_CHECK(offsetof(MaterialUniformBufferData, hasClearcoatNormalTex) == 104);
    TEST_CHECK(offsetof(MaterialUniformBufferData, hasTransmissionTex) == 108);

    TEST_CHECK(offsetof(MaterialUniformBufferData, thicknessFactor) == 112);
    TEST_CHECK(offsetof(MaterialUniformBufferData, attenuationDistance) == 116);
    TEST_CHECK(offsetof(MaterialUniformBufferData, hasThicknessTex) == 120);
    TEST_CHECK(offsetof(MaterialUniformBufferData, specularFactor) == 124);

    TEST_CHECK(offsetof(MaterialUniformBufferData, attenuationColorAndHasSpecTex) == 128);
    TEST_CHECK(offsetof(MaterialUniformBufferData, specularColorFactorAndHasColorTex) == 144);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 3: GltfImporter Parses Advanced Extensions
// -----------------------------------------------------------------------------
void testGltfImporterExtensions() {
    std::cout << "[Test 3] GltfImporter Extension Parsing..." << std::endl;

    auto result = GltfImporter::importFromFile(kFixturesPath);
    TEST_CHECK(result.hasValue());

    const auto& sceneData = result.value();
    TEST_CHECK(sceneData.materials.size() == 7);

    // 0: ClearcoatMaterial
    {
        const auto& mat = sceneData.materials[0];
        TEST_CHECK(mat.name == "ClearcoatMaterial");
        TEST_APPROX(mat.clearcoat.factor, 0.85F, 1e-4F);
        TEST_APPROX(mat.clearcoat.roughnessFactor, 0.15F, 1e-4F);
        TEST_APPROX(mat.transmission.factor, 0.0F, 1e-4F);
        TEST_APPROX(mat.volume.thicknessFactor, 0.0F, 1e-4F);
        TEST_APPROX(mat.ior, 1.5F, 1e-4F);
        TEST_APPROX(mat.specular.factor, 1.0F, 1e-4F);
    }

    // 1: TransmissionMaterial
    {
        const auto& mat = sceneData.materials[1];
        TEST_CHECK(mat.name == "TransmissionMaterial");
        TEST_APPROX(mat.clearcoat.factor, 0.0F, 1e-4F);
        TEST_APPROX(mat.transmission.factor, 0.95F, 1e-4F);
        TEST_APPROX(mat.transmissionFactor, 0.95F, 1e-4F);
        TEST_APPROX(mat.volume.thicknessFactor, 0.0F, 1e-4F);
    }

    // 2: VolumeMaterial
    {
        const auto& mat = sceneData.materials[2];
        TEST_CHECK(mat.name == "VolumeMaterial");
        TEST_APPROX(mat.transmission.factor, 1.0F, 1e-4F);
        TEST_APPROX(mat.volume.thicknessFactor, 2.5F, 1e-4F);
        TEST_APPROX(mat.volume.attenuationDistance, 10.0F, 1e-4F);
        TEST_APPROX(mat.volume.attenuationColor.x, 0.2F, 1e-4F);
        TEST_APPROX(mat.volume.attenuationColor.y, 0.7F, 1e-4F);
        TEST_APPROX(mat.volume.attenuationColor.z, 0.9F, 1e-4F);
    }

    // 3: IorMaterial
    {
        const auto& mat = sceneData.materials[3];
        TEST_CHECK(mat.name == "IorMaterial");
        TEST_APPROX(mat.ior, 1.333F, 1e-4F);
        TEST_APPROX(mat.iorExt.ior, 1.333F, 1e-4F);
        TEST_APPROX(mat.clearcoat.factor, 0.0F, 1e-4F);
        TEST_APPROX(mat.transmission.factor, 0.0F, 1e-4F);
        TEST_APPROX(mat.volume.thicknessFactor, 0.0F, 1e-4F);
    }

    // 4: SpecularMaterial
    {
        const auto& mat = sceneData.materials[4];
        TEST_CHECK(mat.name == "SpecularMaterial");
        TEST_APPROX(mat.specular.factor, 0.5F, 1e-4F);
        TEST_APPROX(mat.specular.colorFactor.x, 0.8F, 1e-4F);
        TEST_APPROX(mat.specular.colorFactor.y, 0.6F, 1e-4F);
        TEST_APPROX(mat.specular.colorFactor.z, 0.4F, 1e-4F);
    }

    // 5: FullAdvancedMaterial (All extensions combined)
    {
        const auto& mat = sceneData.materials[5];
        TEST_CHECK(mat.name == "FullAdvancedMaterial");
        TEST_APPROX(mat.clearcoat.factor, 0.7F, 1e-4F);
        TEST_APPROX(mat.clearcoat.roughnessFactor, 0.3F, 1e-4F);

        TEST_APPROX(mat.transmission.factor, 0.8F, 1e-4F);
        TEST_APPROX(mat.transmissionFactor, 0.8F, 1e-4F);

        TEST_APPROX(mat.volume.thicknessFactor, 1.2F, 1e-4F);
        TEST_APPROX(mat.volume.attenuationDistance, 4.0F, 1e-4F);
        TEST_APPROX(mat.volume.attenuationColor.x, 0.9F, 1e-4F);
        TEST_APPROX(mat.volume.attenuationColor.y, 0.3F, 1e-4F);
        TEST_APPROX(mat.volume.attenuationColor.z, 0.1F, 1e-4F);

        TEST_APPROX(mat.ior, 1.77F, 1e-4F);
        TEST_APPROX(mat.iorExt.ior, 1.77F, 1e-4F);

        TEST_APPROX(mat.specular.factor, 0.85F, 1e-4F);
        TEST_APPROX(mat.specular.colorFactor.x, 0.95F, 1e-4F);
        TEST_APPROX(mat.specular.colorFactor.y, 0.9F, 1e-4F);
        TEST_APPROX(mat.specular.colorFactor.z, 0.85F, 1e-4F);
    }

    // 6: DefaultExtensionsMaterial
    {
        const auto& mat = sceneData.materials[6];
        TEST_CHECK(mat.name == "DefaultExtensionsMaterial");
        TEST_APPROX(mat.clearcoat.factor, 0.0F, 1e-4F);
        TEST_APPROX(mat.clearcoat.roughnessFactor, 0.0F, 1e-4F);

        TEST_APPROX(mat.transmission.factor, 0.0F, 1e-4F);
        TEST_APPROX(mat.volume.thicknessFactor, 0.0F, 1e-4F);

        TEST_APPROX(mat.ior, 1.5F, 1e-4F);
        TEST_APPROX(mat.iorExt.ior, 1.5F, 1e-4F);

        TEST_APPROX(mat.specular.factor, 1.0F, 1e-4F);
        TEST_APPROX(mat.specular.colorFactor.x, 1.0F, 1e-4F);
        TEST_APPROX(mat.specular.colorFactor.y, 1.0F, 1e-4F);
        TEST_APPROX(mat.specular.colorFactor.z, 1.0F, 1e-4F);
    }

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 4: GltfSceneImporter Equivalence
// -----------------------------------------------------------------------------
void testGltfSceneImporterEquivalence() {
    std::cout << "[Test 4] GltfSceneImporter Equivalence..." << std::endl;

    world::World world;
    AssetManager assetMgr;
    auto result = GltfSceneImporter::importScene(kFixturesPath, world, &assetMgr);
    TEST_CHECK(result.success);
    TEST_CHECK(!result.nodes.empty());

    // Retrieve extracted mesh from AssetManager
    std::string meshUri = kFixturesPath + "#mesh_0";
    const MeshData* meshData = assetMgr.getMeshData(meshUri);
    TEST_CHECK(meshData != nullptr);
    TEST_CHECK(meshData->materials.size() == 7);

    // Verify that the materials created in GltfSceneImporter match GltfImporter
    const auto& clearcoatMat = meshData->materials[0];
    TEST_CHECK(clearcoatMat.name == "ClearcoatMaterial");
    TEST_APPROX(clearcoatMat.clearcoat.factor, 0.85F, 1e-4F);

    const auto& volumeMat = meshData->materials[2];
    TEST_APPROX(volumeMat.volume.thicknessFactor, 2.5F, 1e-4F);
    TEST_APPROX(volumeMat.volume.attenuationDistance, 10.0F, 1e-4F);

    const auto& iorMat = meshData->materials[3];
    TEST_APPROX(iorMat.ior, 1.333F, 1e-4F);

    const auto& specMat = meshData->materials[4];
    TEST_APPROX(specMat.specular.factor, 0.5F, 1e-4F);

    std::cout << "  Passed." << std::endl;
}

// -----------------------------------------------------------------------------
// Test 5: GpuMaterial Descriptor Layout and Uniform Data Synchronization
// -----------------------------------------------------------------------------
void testGpuMaterialIntegration() {
    std::cout << "[Test 5] GpuMaterial Descriptor Layout & UBO Synchronization..." << std::endl;

    auto desc = createMaterialDescriptorSetLayoutDesc();
    TEST_CHECK(desc.bindings.size() == 13);

    // Check bindings: 12 samplers (0..4, 6..12) and 1 UBO (5)
    for (uint32_t i = 0; i < 13; ++i) {
        TEST_CHECK(desc.bindings[i].binding == i);
        if (i == 5) {
            TEST_CHECK(desc.bindings[i].descriptorType == rhi::DescriptorType::UniformBuffer);
            TEST_CHECK((static_cast<uint32_t>(desc.bindings[i].stageFlags) &
                        static_cast<uint32_t>(rhi::ShaderStageFlags::Fragment)) != 0);
        } else {
            TEST_CHECK(desc.bindings[i].descriptorType ==
                       rhi::DescriptorType::CombinedImageSampler);
            TEST_CHECK((static_cast<uint32_t>(desc.bindings[i].stageFlags) &
                        static_cast<uint32_t>(rhi::ShaderStageFlags::Fragment)) != 0);
        }
    }

    // Verify Null device creation and GpuMaterial instantiation
    auto devRes = rhi::createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto rhiDevice = std::move(devRes.value());
    TEST_CHECK(rhiDevice != nullptr);

    auto layoutRes = rhiDevice->createDescriptorSetLayout(desc);
    TEST_CHECK(layoutRes.hasValue());
    auto layout = layoutRes.value();

    AssetManager assetMgr;

    // Create a material with full extensions
    Material mat = Material::createDefault();
    mat.name = "TestExtensionGpuMaterial";
    mat.clearcoat = ClearcoatExtension{
        .factor = 0.9F,
        .roughnessFactor = 0.1F,
    };
    mat.transmission = TransmissionExtension{
        .factor = 0.85F,
    };
    mat.transmissionFactor = 0.85F;
    mat.volume = VolumeExtension{
        .thicknessFactor = 1.5F,
        .thicknessTexture = {},
        .attenuationDistance = 2.0F,
        .attenuationColor = {0.1F, 0.7F, 0.9F},
    };
    mat.ior = 1.45F;
    mat.iorExt = IorExtension{
        .ior = 1.45F,
    };
    mat.specular = SpecularExtension{
        .factor = 0.8F,
        .texture = {},
        .colorFactor = {1.0F, 0.9F, 0.8F},
        .colorTexture = {},
    };

    auto gpuMatRes = GpuMaterial::create(*rhiDevice, layout, mat, assetMgr);
    TEST_CHECK(gpuMatRes.hasValue());

    auto gpuMat = std::move(gpuMatRes.value());
    TEST_CHECK(gpuMat != nullptr);

    // Verify uniform buffer was populated with correct extension fields
    const auto& ubo = gpuMat->getUniformData();
    TEST_APPROX(ubo.clearcoatFactor, 0.9F, 1e-4F);
    TEST_APPROX(ubo.clearcoatRoughness, 0.1F, 1e-4F);
    TEST_APPROX(ubo.clearcoatNormalScale, 1.0F, 1e-4F);

    TEST_APPROX(ubo.transmissionFactor, 0.85F, 1e-4F);
    TEST_APPROX(ubo.ior, 1.45F, 1e-4F);

    TEST_APPROX(ubo.thicknessFactor, 1.5F, 1e-4F);
    TEST_APPROX(ubo.attenuationDistance, 2.0F, 1e-4F);
    TEST_APPROX(ubo.attenuationColorAndHasSpecTex.x, 0.1F, 1e-4F);
    TEST_APPROX(ubo.attenuationColorAndHasSpecTex.y, 0.7F, 1e-4F);
    TEST_APPROX(ubo.attenuationColorAndHasSpecTex.z, 0.9F, 1e-4F);

    TEST_APPROX(ubo.specularFactor, 0.8F, 1e-4F);
    TEST_APPROX(ubo.specularColorFactorAndHasColorTex.x, 1.0F, 1e-4F);
    TEST_APPROX(ubo.specularColorFactorAndHasColorTex.y, 0.9F, 1e-4F);
    TEST_APPROX(ubo.specularColorFactorAndHasColorTex.z, 0.8F, 1e-4F);

    // Verify updateTextureDescriptors works with 12 slots
    std::array<std::pair<rhi::TextureHandle, rhi::SamplerHandle>, 12> textureSlots{};
    gpuMat->updateTextureDescriptors(*rhiDevice, textureSlots);

    rhiDevice->destroyDescriptorSetLayout(layout);
    std::cout << "  Passed." << std::endl;
}

} // namespace

int main() {
    std::cout << "========================================================\n"
              << "Running glTF Material Extensions Test Suite (11F Phase 3)\n"
              << "========================================================" << std::endl;

    testMaterialDefaults();
    testUniformBufferDataLayout();
    testGltfImporterExtensions();
    testGltfSceneImporterEquivalence();
    testGpuMaterialIntegration();

    std::cout << "========================================================\n"
              << "All 5 tests in glTF Material Extensions Suite PASSED.\n"
              << "========================================================" << std::endl;
    return 0;
}
