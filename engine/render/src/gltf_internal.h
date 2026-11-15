/// @file gltf_internal.h
/// @brief Internal shared parsing helpers for glTF materials, textures, and samplers.
///
/// Private to engine/render; not exposed to public consumers.

#pragma once

#include "core/error.h"
#include "core/log.h"
#include "core/result.h"
#include "render/image.h"
#include "render/material.h"

#ifndef CGLTF_H_INCLUDED__
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include "cgltf/cgltf.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace eng::render::detail {

[[nodiscard]] inline TextureSamplerDesc parseSampler(const cgltf_texture_view& view) noexcept {
    TextureSamplerDesc desc{};
    if (view.texture != nullptr && view.texture->sampler != nullptr) {
        const auto* s = view.texture->sampler;

        if (s->mag_filter == 9728) { // GL_NEAREST
            desc.magFilter = TextureSamplerDesc::Filter::Nearest;
        } else {
            desc.magFilter = TextureSamplerDesc::Filter::Linear;
        }

        switch (s->min_filter) {
        case 9728: // GL_NEAREST
            desc.minFilter = TextureSamplerDesc::Filter::Nearest;
            desc.mipmapMode = TextureSamplerDesc::Filter::Nearest;
            break;
        case 9729: // GL_LINEAR
            desc.minFilter = TextureSamplerDesc::Filter::Linear;
            desc.mipmapMode = TextureSamplerDesc::Filter::Nearest;
            break;
        case 9984: // GL_NEAREST_MIPMAP_NEAREST
            desc.minFilter = TextureSamplerDesc::Filter::Nearest;
            desc.mipmapMode = TextureSamplerDesc::Filter::Nearest;
            break;
        case 9985: // GL_LINEAR_MIPMAP_NEAREST
            desc.minFilter = TextureSamplerDesc::Filter::Linear;
            desc.mipmapMode = TextureSamplerDesc::Filter::Nearest;
            break;
        case 9986: // GL_NEAREST_MIPMAP_LINEAR
            desc.minFilter = TextureSamplerDesc::Filter::Nearest;
            desc.mipmapMode = TextureSamplerDesc::Filter::Linear;
            break;
        case 9987: // GL_LINEAR_MIPMAP_LINEAR
        default:
            desc.minFilter = TextureSamplerDesc::Filter::Linear;
            desc.mipmapMode = TextureSamplerDesc::Filter::Linear;
            break;
        }

        auto convertWrap = [](cgltf_int wrap) noexcept {
            if (wrap == 33071) { // GL_CLAMP_TO_EDGE
                return TextureSamplerDesc::WrapMode::ClampToEdge;
            }
            if (wrap == 33648) { // GL_MIRRORED_REPEAT
                return TextureSamplerDesc::WrapMode::MirroredRepeat;
            }
            return TextureSamplerDesc::WrapMode::Repeat;
        };
        desc.wrapS = convertWrap(s->wrap_s);
        desc.wrapT = convertWrap(s->wrap_t);
    }
    return desc;
}

[[nodiscard]] inline TextureInfo
parseTextureInfo(const cgltf_data* data, const cgltf_texture_view& view, std::string_view baseDir,
                 TextureColorSpace colorSpace, std::string_view sourcePath = "") noexcept {
    TextureInfo info{};
    if (view.texture == nullptr || view.texture->image == nullptr) {
        return info;
    }

    info.isPresent = true;
    info.colorSpace = colorSpace;
    info.texCoordIndex = view.texcoord;
    if (info.texCoordIndex > 0) {
        ENG_LOG_FORMAT(
            ::eng::LogLevel::Warning, "glTF",
            "Texture coordinate set %d requested, but vertex format currently supports TEXCOORD_0; "
            "sampling will use TEXCOORD_0",
            info.texCoordIndex);
    }
    info.scale = (view.scale != 0.0F) ? view.scale : 1.0F;
    info.strength = (view.scale != 0.0F) ? view.scale : 1.0F;
    info.sampler = parseSampler(view);

    const std::string colorSuffix = (colorSpace == TextureColorSpace::sRGB) ? "#srgb" : "#linear";

    if (view.texture->image->uri != nullptr) {
        std::string rawUri = view.texture->image->uri;
        if (!baseDir.empty() && rawUri.rfind("data:", 0) != 0) {
            std::filesystem::path fullP = std::filesystem::path(baseDir) / rawUri;
            info.uri = fullP.lexically_normal().string();
        } else {
            info.uri = std::move(rawUri);
        }
        std::replace(info.uri.begin(), info.uri.end(), '\\', '/');
        info.textureAsset = makeAssetIdFromPath(info.uri + colorSuffix);
    } else if (view.texture->image->buffer_view != nullptr) {
        size_t imgIdx = 0;
        if (data != nullptr && data->images != nullptr) {
            imgIdx = cgltf_image_index(data, view.texture->image);
        }
        if (!sourcePath.empty()) {
            info.uri = std::string(sourcePath) + "#image_" + std::to_string(imgIdx);
        } else {
            info.uri = "embedded://image_" + std::to_string(imgIdx);
        }
        info.textureAsset = makeAssetIdFromPath(info.uri + colorSuffix);
    }

    return info;
}

[[nodiscard]] inline Material parseMaterial(const cgltf_data* data, const cgltf_material& cgMat,
                                            std::string_view baseDir, size_t index,
                                            std::string_view sourcePath = "") noexcept {
    Material mat{};
    mat.name = (cgMat.name != nullptr && cgMat.name[0] != '\0')
                   ? std::string(cgMat.name)
                   : ("Material_" + std::to_string(index));
    mat.doubleSided = (cgMat.double_sided != 0);

    if (cgMat.has_pbr_metallic_roughness) {
        const auto& pbr = cgMat.pbr_metallic_roughness;
        mat.baseColor = Vec4{pbr.base_color_factor[0], pbr.base_color_factor[1],
                             pbr.base_color_factor[2], pbr.base_color_factor[3]};
        mat.roughness = pbr.roughness_factor;
        mat.metallic = pbr.metallic_factor;

        mat.baseColorTexture = parseTextureInfo(data, pbr.base_color_texture, baseDir,
                                                TextureColorSpace::sRGB, sourcePath);
        mat.textureAsset = mat.baseColorTexture.textureAsset;
        mat.metallicRoughnessTexture = parseTextureInfo(
            data, pbr.metallic_roughness_texture, baseDir, TextureColorSpace::Linear, sourcePath);
    } else if (cgMat.has_pbr_specular_glossiness) {
        const auto& sg = cgMat.pbr_specular_glossiness;
        mat.baseColor = Vec4{sg.diffuse_factor[0], sg.diffuse_factor[1], sg.diffuse_factor[2],
                             sg.diffuse_factor[3]};

        mat.baseColorTexture = parseTextureInfo(data, sg.diffuse_texture, baseDir,
                                                TextureColorSpace::sRGB, sourcePath);
        mat.textureAsset = mat.baseColorTexture.textureAsset;

        // If diffuse texture is present and diffuse_factor is all zeroes (e.g. windows/frames
        // exported with zero diffuse factor), restore full diffuse multiplier so the texture
        // is not multiplied by zero.
        if (mat.baseColorTexture.isPresent &&
            (mat.baseColor.x + mat.baseColor.y + mat.baseColor.z) < 0.01F) {
            mat.baseColor.x = 1.0F;
            mat.baseColor.y = 1.0F;
            mat.baseColor.z = 1.0F;
        }

        mat.metallicRoughnessTexture = parseTextureInfo(
            data, sg.specular_glossiness_texture, baseDir, TextureColorSpace::Linear, sourcePath);

        // Standard analytical conversion from KHR_materials_pbrSpecularGlossiness
        // to PBR Metallic-Roughness (per Khronos specification):
        const float maxSpec =
            std::max(sg.specular_factor[0], std::max(sg.specular_factor[1], sg.specular_factor[2]));

        if (mat.metallicRoughnessTexture.isPresent) {
            // Specular-glossiness texture is converted to glTF metallic-roughness layout
            // (roughness in Green, metallic in Blue). Identity multipliers allow texture to govern.
            mat.roughness = (sg.glossiness_factor > 0.0F) ? sg.glossiness_factor : 1.0F;
            mat.metallic = 1.0F;
        } else {
            // Uniform analytical conversion without textures:
            // Standard physical dielectric reflectance F0 is approx. 0.04 (4% at normal incidence).
            if (maxSpec <= 0.05F) {
                mat.metallic = 0.0F;
            } else {
                mat.metallic = std::clamp((maxSpec - 0.04F) / 0.96F, 0.0F, 1.0F);
            }
            mat.roughness = std::clamp(1.0F - sg.glossiness_factor, 0.045F, 1.0F);

            // Metals reflect incoming light through specular color rather than diffuse
            if (mat.metallic > 0.0F && !mat.baseColorTexture.isPresent) {
                mat.baseColor.x =
                    std::lerp(sg.diffuse_factor[0], sg.specular_factor[0], mat.metallic);
                mat.baseColor.y =
                    std::lerp(sg.diffuse_factor[1], sg.specular_factor[1], mat.metallic);
                mat.baseColor.z =
                    std::lerp(sg.diffuse_factor[2], sg.specular_factor[2], mat.metallic);
            }
        }
    } else {
        mat.baseColor = Vec4{1.0F, 1.0F, 1.0F, 1.0F};
        mat.roughness = 1.0F;
        mat.metallic = 0.0F;
    }

    mat.normalTexture = parseTextureInfo(data, cgMat.normal_texture, baseDir,
                                         TextureColorSpace::Linear, sourcePath);
    if (mat.normalTexture.isPresent && cgMat.normal_texture.scale != 0.0F) {
        mat.normalTexture.scale = cgMat.normal_texture.scale;
    }

    mat.occlusionTexture = parseTextureInfo(data, cgMat.occlusion_texture, baseDir,
                                            TextureColorSpace::Linear, sourcePath);
    if (mat.occlusionTexture.isPresent && cgMat.occlusion_texture.scale != 0.0F) {
        mat.occlusionTexture.strength = cgMat.occlusion_texture.scale;
    }

    mat.emissiveTexture = parseTextureInfo(data, cgMat.emissive_texture, baseDir,
                                           TextureColorSpace::sRGB, sourcePath);
    mat.emissiveFactor =
        Vec3{cgMat.emissive_factor[0], cgMat.emissive_factor[1], cgMat.emissive_factor[2]};

    if (cgMat.alpha_mode == cgltf_alpha_mode_mask) {
        mat.alphaMode = AlphaMode::Mask;
        mat.alphaCutoff = cgMat.alpha_cutoff;
    } else if (cgMat.alpha_mode == cgltf_alpha_mode_blend) {
        mat.alphaMode = AlphaMode::Blend;
    } else {
        mat.alphaMode = AlphaMode::Opaque;
    }

    // 1. KHR_materials_clearcoat
    if (cgMat.has_clearcoat) {
        mat.clearcoat.factor = cgMat.clearcoat.clearcoat_factor;
        mat.clearcoat.roughnessFactor = cgMat.clearcoat.clearcoat_roughness_factor;
        mat.clearcoat.texture = parseTextureInfo(data, cgMat.clearcoat.clearcoat_texture, baseDir,
                                                 TextureColorSpace::Linear, sourcePath);
        mat.clearcoat.roughnessTexture =
            parseTextureInfo(data, cgMat.clearcoat.clearcoat_roughness_texture, baseDir,
                             TextureColorSpace::Linear, sourcePath);
        mat.clearcoat.normalTexture =
            parseTextureInfo(data, cgMat.clearcoat.clearcoat_normal_texture, baseDir,
                             TextureColorSpace::Linear, sourcePath);
        if (mat.clearcoat.normalTexture.isPresent &&
            cgMat.clearcoat.clearcoat_normal_texture.scale != 0.0F) {
            mat.clearcoat.normalTexture.scale = cgMat.clearcoat.clearcoat_normal_texture.scale;
        }
    }

    // 2. KHR_materials_transmission
    if (cgMat.has_transmission) {
        mat.transmission.factor = cgMat.transmission.transmission_factor;
        mat.transmissionFactor = mat.transmission.factor;
        mat.transmission.texture = parseTextureInfo(data, cgMat.transmission.transmission_texture,
                                                    baseDir, TextureColorSpace::Linear, sourcePath);
    }

    // 3. KHR_materials_volume
    if (cgMat.has_volume) {
        mat.volume.thicknessFactor = cgMat.volume.thickness_factor;
        mat.volume.thicknessTexture = parseTextureInfo(
            data, cgMat.volume.thickness_texture, baseDir, TextureColorSpace::Linear, sourcePath);
        mat.volume.attenuationDistance =
            (cgMat.volume.attenuation_distance > 0.0F) ? cgMat.volume.attenuation_distance : 1e9F;
        mat.volume.attenuationColor =
            Vec3{cgMat.volume.attenuation_color[0], cgMat.volume.attenuation_color[1],
                 cgMat.volume.attenuation_color[2]};
    }

    // 4. KHR_materials_ior
    if (cgMat.has_ior) {
        mat.iorExt.ior = cgMat.ior.ior;
        mat.ior = mat.iorExt.ior;
    }

    // 5. KHR_materials_specular
    if (cgMat.has_specular) {
        mat.specular.factor = cgMat.specular.specular_factor;
        mat.specular.texture = parseTextureInfo(data, cgMat.specular.specular_texture, baseDir,
                                                TextureColorSpace::Linear, sourcePath);
        mat.specular.colorFactor =
            Vec3{cgMat.specular.specular_color_factor[0], cgMat.specular.specular_color_factor[1],
                 cgMat.specular.specular_color_factor[2]};
        mat.specular.colorTexture = parseTextureInfo(data, cgMat.specular.specular_color_texture,
                                                     baseDir, TextureColorSpace::sRGB, sourcePath);
    }

    // 6. KHR_materials_anisotropy
    if (cgMat.has_anisotropy) {
        mat.anisotropy.strength = cgMat.anisotropy.anisotropy_strength;
        mat.anisotropy.rotation = cgMat.anisotropy.anisotropy_rotation;
        mat.anisotropy.texture = parseTextureInfo(data, cgMat.anisotropy.anisotropy_texture,
                                                  baseDir, TextureColorSpace::Linear, sourcePath);
    }

    // 7. KHR_materials_sheen
    if (cgMat.has_sheen) {
        mat.sheen.colorFactor =
            Vec3{cgMat.sheen.sheen_color_factor[0], cgMat.sheen.sheen_color_factor[1],
                 cgMat.sheen.sheen_color_factor[2]};
        mat.sheen.roughnessFactor = cgMat.sheen.sheen_roughness_factor;
        mat.sheen.colorTexture = parseTextureInfo(data, cgMat.sheen.sheen_color_texture, baseDir,
                                                  TextureColorSpace::sRGB, sourcePath);
        mat.sheen.roughnessTexture =
            parseTextureInfo(data, cgMat.sheen.sheen_roughness_texture, baseDir,
                             TextureColorSpace::Linear, sourcePath);
    }

    return mat;
}

[[nodiscard]] inline std::vector<Material>
extractAllMaterials(const cgltf_data* data, std::string_view baseDir,
                    std::string_view sourcePath = "") noexcept {
    std::vector<Material> materials;
    if (data == nullptr || data->materials_count == 0) {
        materials.push_back(Material::createDefault());
        return materials;
    }

    materials.reserve(data->materials_count);
    for (size_t i = 0; i < data->materials_count; ++i) {
        materials.push_back(parseMaterial(data, data->materials[i], baseDir, i, sourcePath));
    }
    return materials;
}

[[nodiscard]] inline Result<ImageData>
extractEmbeddedImage(const cgltf_data* data, size_t imageIndex, bool srgb = true) noexcept {
    if (data == nullptr) {
        return Error(ErrorCode::InvalidArgument, "Null glTF data pointer");
    }
    if (imageIndex >= data->images_count) {
        return Error(ErrorCode::InvalidArgument,
                     "Image index out of bounds: " + std::to_string(imageIndex));
    }
    const auto& image = data->images[imageIndex];
    if (image.buffer_view == nullptr) {
        return Error(ErrorCode::InvalidArgument, "Image is not backed by a buffer_view");
    }

    const auto* bv = image.buffer_view;
    const uint8_t* basePtr = static_cast<const uint8_t*>(
        bv->data != nullptr ? bv->data : (bv->buffer != nullptr ? bv->buffer->data : nullptr));
    if (basePtr == nullptr) {
        return Error(ErrorCode::InvalidArgument, "glTF buffer_view has no loaded memory");
    }

    std::span<const uint8_t> bufferBytes(basePtr + bv->offset, bv->size);
    return ImageDecoder::decodeFromMemory(bufferBytes, 4, srgb);
}

[[nodiscard]] inline Result<ImageData> extractEmbeddedImageFromFile(std::string_view filePath,
                                                                    size_t imageIndex,
                                                                    bool srgb = true) noexcept {
    cgltf_options options{};
    cgltf_data* parsedData = nullptr;
    const std::string pathStr(filePath);

    const cgltf_result parseRes = cgltf_parse_file(&options, pathStr.c_str(), &parsedData);
    if (parseRes != cgltf_result_success) {
        return Error(ErrorCode::InvalidArgument, "Failed to parse glTF for embedded image");
    }

    const cgltf_result loadRes = cgltf_load_buffers(&options, parsedData, pathStr.c_str());
    if (loadRes != cgltf_result_success) {
        cgltf_free(parsedData);
        return Error(ErrorCode::InvalidArgument, "Failed to load glTF buffers for embedded image");
    }

    auto result = extractEmbeddedImage(parsedData, imageIndex, srgb);
    cgltf_free(parsedData);
    return result;
}

} // namespace eng::render::detail
