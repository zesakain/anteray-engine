/// @file gltf_importer.cpp
/// @brief Implementation of glTF 2.0 (.gltf, .glb) 3D asset importer using cgltf.

#include "render/gltf_importer.h"

#include "core/log.h"
#include "core/math.h"
#include "render/material.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "gltf_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace eng::render {

namespace {

struct CgltfGuard {
    cgltf_data* data{nullptr};
    ~CgltfGuard() noexcept {
        if (data != nullptr) {
            cgltf_free(data);
        }
    }
};

struct Mat3 {
    float m[9]{1, 0, 0, 0, 1, 0, 0, 0, 1};

    [[nodiscard]] float determinant() const noexcept {
        return (m[0] * ((m[4] * m[8]) - (m[5] * m[7]))) - (m[3] * ((m[1] * m[8]) - (m[2] * m[7]))) +
               (m[6] * ((m[1] * m[5]) - (m[2] * m[4])));
    }

    [[nodiscard]] Mat3 inverseTranspose() const noexcept {
        const float det = determinant();
        if (std::abs(det) < 1e-12F) {
            return *this;
        }
        const float invDet = 1.0F / det;
        Mat3 res;
        // In column-major order:
        res.m[0] = ((m[4] * m[8]) - (m[5] * m[7])) * invDet;
        res.m[1] = -((m[3] * m[8]) - (m[5] * m[6])) * invDet;
        res.m[2] = ((m[3] * m[7]) - (m[4] * m[6])) * invDet;

        res.m[3] = -((m[1] * m[8]) - (m[2] * m[7])) * invDet;
        res.m[4] = ((m[0] * m[8]) - (m[2] * m[6])) * invDet;
        res.m[5] = -((m[0] * m[7]) - (m[1] * m[6])) * invDet;

        res.m[6] = ((m[1] * m[5]) - (m[2] * m[4])) * invDet;
        res.m[7] = -((m[0] * m[5]) - (m[2] * m[3])) * invDet;
        res.m[8] = ((m[0] * m[4]) - (m[1] * m[3])) * invDet;
        return res;
    }

    [[nodiscard]] Vec3 transformVector(const Vec3& v) const noexcept {
        return Vec3{(m[0] * v.x) + (m[3] * v.y) + (m[6] * v.z),
                    (m[1] * v.x) + (m[4] * v.y) + (m[7] * v.z),
                    (m[2] * v.x) + (m[5] * v.y) + (m[8] * v.z)};
    }
};

[[nodiscard]] Transform extractNodeLocalTransform(const cgltf_node* node) noexcept {
    if (node == nullptr) {
        return Transform::identity();
    }
    if (node->has_matrix) {
        Mat4 mat{};
        std::memcpy(mat.m, node->matrix, sizeof(float) * 16);
        const Vec3 pos{mat.at(3, 0), mat.at(3, 1), mat.at(3, 2)};
        const Vec3 col0{mat.at(0, 0), mat.at(0, 1), mat.at(0, 2)};
        const Vec3 col1{mat.at(1, 0), mat.at(1, 1), mat.at(1, 2)};
        const Vec3 col2{mat.at(2, 0), mat.at(2, 1), mat.at(2, 2)};
        const Vec3 scale{col0.length(), col1.length(), col2.length()};
        return Transform{pos, Quat::identity(), scale};
    }

    Transform tr = Transform::identity();
    if (node->has_translation) {
        tr.position = Vec3{node->translation[0], node->translation[1], node->translation[2]};
    }
    if (node->has_rotation) {
        tr.rotation =
            Quat{node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]}
                .normalized();
    }
    if (node->has_scale) {
        tr.scale = Vec3{node->scale[0], node->scale[1], node->scale[2]};
    }
    return tr;
}

[[nodiscard]] Result<anim::Skeleton>
importSkeletonFromSkin(const cgltf_skin* skin, std::vector<uint16_t>& outSkinJointToBone) {
    if (skin == nullptr || skin->joints_count == 0) {
        return Error(ErrorCode::InvalidArgument, "glTF skin has no joints");
    }
    if (skin->joints_count > anim::kMaxSkinningBones) {
        return Error(ErrorCode::InvalidArgument,
                     "glTF skin exceeds kMaxSkinningBones (128) joint limit");
    }
    if (skin->inverse_bind_matrices != nullptr &&
        skin->inverse_bind_matrices->count < skin->joints_count) {
        return Error(ErrorCode::InvalidArgument,
                     "glTF skin inverseBindMatrices count is smaller than joints count");
    }

    const size_t jointCount = skin->joints_count;
    std::unordered_map<const cgltf_node*, size_t> nodeToSkinIdx;
    nodeToSkinIdx.reserve(jointCount);
    for (size_t i = 0; i < jointCount; ++i) {
        if (skin->joints[i] == nullptr) {
            return Error(ErrorCode::InvalidArgument, "glTF skin contains null joint node");
        }
        nodeToSkinIdx[skin->joints[i]] = i;
    }

    std::vector<size_t> topoOrder;
    topoOrder.reserve(jointCount);
    std::vector<bool> visited(jointCount, false);

    auto visitJoint = [&](auto& self, size_t skinIdx) -> void {
        if (visited[skinIdx]) {
            return;
        }
        const cgltf_node* parentNode = skin->joints[skinIdx]->parent;
        if (parentNode != nullptr) {
            auto it = nodeToSkinIdx.find(parentNode);
            if (it != nodeToSkinIdx.end()) {
                self(self, it->second);
            }
        }
        visited[skinIdx] = true;
        topoOrder.push_back(skinIdx);
    };

    for (size_t i = 0; i < jointCount; ++i) {
        visitJoint(visitJoint, i);
    }

    outSkinJointToBone.assign(jointCount, anim::kInvalidBoneIndex);
    anim::Skeleton skeleton;

    for (size_t boneIdx = 0; boneIdx < topoOrder.size(); ++boneIdx) {
        const size_t skinIdx = topoOrder[boneIdx];
        const cgltf_node* jointNode = skin->joints[skinIdx];
        outSkinJointToBone[skinIdx] = static_cast<uint16_t>(boneIdx);

        uint16_t parentBoneIdx = anim::kInvalidBoneIndex;
        if (jointNode->parent != nullptr) {
            auto it = nodeToSkinIdx.find(jointNode->parent);
            if (it != nodeToSkinIdx.end()) {
                parentBoneIdx = outSkinJointToBone[it->second];
            }
        }

        const std::string_view boneName =
            (jointNode->name != nullptr) ? std::string_view{jointNode->name} : "Bone";
        const Transform localBind = extractNodeLocalTransform(jointNode);
        auto addRes = skeleton.addBone(boneName, parentBoneIdx, localBind);
        if (!addRes.hasValue()) {
            return addRes.error();
        }
    }

    if (skin->inverse_bind_matrices != nullptr) {
        for (size_t boneIdx = 0; boneIdx < topoOrder.size(); ++boneIdx) {
            const size_t skinIdx = topoOrder[boneIdx];
            float rawIbm[16]{};
            cgltf_accessor_read_float(skin->inverse_bind_matrices, skinIdx, rawIbm, 16);
            Mat4 ibm{};
            std::memcpy(ibm.m, rawIbm, sizeof(rawIbm));
            auto setRes = skeleton.setInverseBindMatrix(static_cast<uint16_t>(boneIdx), ibm);
            if (!setRes.hasValue()) {
                return setRes.error();
            }
        }
        skeleton.markFinalized();
    } else {
        auto finRes = skeleton.finalize();
        if (!finRes.hasValue()) {
            return finRes.error();
        }
    }

    return Result<anim::Skeleton>::success(std::move(skeleton));
}

[[nodiscard]] anim::InterpolationMode mapInterpolation(cgltf_interpolation_type type) noexcept {
    return (type == cgltf_interpolation_type_step) ? anim::InterpolationMode::Step
                                                   : anim::InterpolationMode::Linear;
}

[[nodiscard]] Result<std::vector<anim::AnimationClip>>
importAnimationsFromData(const cgltf_data* data, const anim::Skeleton& skeleton) {
    std::vector<anim::AnimationClip> clips;
    clips.reserve(data->animations_count);

    for (size_t a = 0; a < data->animations_count; ++a) {
        const auto& srcAnim = data->animations[a];
        const std::string_view clipName =
            (srcAnim.name != nullptr) ? std::string_view{srcAnim.name} : "Clip";

        std::unordered_map<uint16_t, anim::BoneTrack> trackMap;
        float maxDuration = 0.0F;

        for (size_t c = 0; c < srcAnim.channels_count; ++c) {
            const auto& chan = srcAnim.channels[c];
            if (chan.target_node == nullptr || chan.sampler == nullptr ||
                chan.sampler->input == nullptr || chan.sampler->output == nullptr) {
                continue;
            }
            if (chan.target_node->name == nullptr) {
                continue;
            }
            const int32_t boneIdxSigned = skeleton.findBoneIndex(chan.target_node->name);
            if (boneIdxSigned < 0 ||
                boneIdxSigned >= static_cast<int32_t>(skeleton.getBoneCount())) {
                continue;
            }
            const uint16_t boneIdx = static_cast<uint16_t>(boneIdxSigned);

            const auto* inputAcc = chan.sampler->input;
            const auto* outputAcc = chan.sampler->output;
            const size_t keyCount = inputAcc->count;
            if (keyCount == 0) {
                continue;
            }
            const bool isCubic =
                (chan.sampler->interpolation == cgltf_interpolation_type_cubic_spline);
            const size_t expectedOut = isCubic ? (keyCount * 3) : keyCount;
            if (outputAcc->count < expectedOut) {
                return Error(
                    ErrorCode::InvalidArgument,
                    "glTF animation sampler output count smaller than input keyframe count");
            }

            auto& track = trackMap[boneIdx];
            track.boneIndex = boneIdx;
            const anim::InterpolationMode interp = mapInterpolation(chan.sampler->interpolation);

            if (chan.target_path == cgltf_animation_path_type_translation) {
                track.translationInterpolation = interp;
                track.translationKeys.reserve(keyCount);
                for (size_t k = 0; k < keyCount; ++k) {
                    float t = 0.0F;
                    cgltf_accessor_read_float(inputAcc, k, &t, 1);
                    if (!std::isfinite(t) || t < 0.0F) {
                        return Error(ErrorCode::InvalidArgument,
                                     "glTF animation keyframe timestamp is invalid");
                    }
                    float v[3]{0.0F, 0.0F, 0.0F};
                    const size_t outIdx = isCubic ? (k * 3 + 1) : k;
                    cgltf_accessor_read_float(outputAcc, outIdx, v, 3);
                    track.translationKeys.push_back(
                        anim::Keyframe<Vec3>{t, Vec3{v[0], v[1], v[2]}});
                    maxDuration = std::max(maxDuration, t);
                }
            } else if (chan.target_path == cgltf_animation_path_type_rotation) {
                track.rotationInterpolation = interp;
                track.rotationKeys.reserve(keyCount);
                for (size_t k = 0; k < keyCount; ++k) {
                    float t = 0.0F;
                    cgltf_accessor_read_float(inputAcc, k, &t, 1);
                    if (!std::isfinite(t) || t < 0.0F) {
                        return Error(ErrorCode::InvalidArgument,
                                     "glTF animation keyframe timestamp is invalid");
                    }
                    float q[4]{0.0F, 0.0F, 0.0F, 1.0F};
                    const size_t outIdx = isCubic ? (k * 3 + 1) : k;
                    cgltf_accessor_read_float(outputAcc, outIdx, q, 4);
                    track.rotationKeys.push_back(
                        anim::Keyframe<Quat>{t, Quat{q[0], q[1], q[2], q[3]}.normalized()});
                    maxDuration = std::max(maxDuration, t);
                }
            } else if (chan.target_path == cgltf_animation_path_type_scale) {
                track.scaleInterpolation = interp;
                track.scaleKeys.reserve(keyCount);
                for (size_t k = 0; k < keyCount; ++k) {
                    float t = 0.0F;
                    cgltf_accessor_read_float(inputAcc, k, &t, 1);
                    if (!std::isfinite(t) || t < 0.0F) {
                        return Error(ErrorCode::InvalidArgument,
                                     "glTF animation keyframe timestamp is invalid");
                    }
                    float s[3]{1.0F, 1.0F, 1.0F};
                    const size_t outIdx = isCubic ? (k * 3 + 1) : k;
                    cgltf_accessor_read_float(outputAcc, outIdx, s, 3);
                    track.scaleKeys.push_back(anim::Keyframe<Vec3>{t, Vec3{s[0], s[1], s[2]}});
                    maxDuration = std::max(maxDuration, t);
                }
            }
        }

        anim::AnimationClip clip(clipName, maxDuration);
        for (auto& [boneIdx, track] : trackMap) {
            clip.addTrack(std::move(track));
        }
        clips.push_back(std::move(clip));
    }

    return Result<std::vector<anim::AnimationClip>>::success(std::move(clips));
}

[[nodiscard]] Mat3 extractMat3(const Mat4& mat) noexcept {
    Mat3 res;
    // Upper-left 3x3 of column-major Mat4
    res.m[0] = mat.at(0, 0);
    res.m[1] = mat.at(1, 0);
    res.m[2] = mat.at(2, 0);

    res.m[3] = mat.at(0, 1);
    res.m[4] = mat.at(1, 1);
    res.m[5] = mat.at(2, 1);

    res.m[6] = mat.at(0, 2);
    res.m[7] = mat.at(1, 2);
    res.m[8] = mat.at(2, 2);
    return res;
}

Error mapCgltfResult(cgltf_result res) {
    switch (res) {
    case cgltf_result_file_not_found:
        return Error(ErrorCode::InvalidArgument, "glTF file or external buffer not found");
    case cgltf_result_io_error:
        return Error(ErrorCode::InvalidArgument, "I/O error reading glTF or buffer data");
    case cgltf_result_invalid_json:
        return Error(ErrorCode::InvalidArgument, "Invalid glTF JSON format");
    case cgltf_result_invalid_gltf:
        return Error(ErrorCode::InvalidArgument, "Corrupt or invalid glTF structure");
    case cgltf_result_legacy_gltf:
        return Error(ErrorCode::InvalidArgument,
                     "Unsupported glTF version (only glTF 2.0 supported)");
    case cgltf_result_data_too_short:
        return Error(ErrorCode::InvalidArgument, "Invalid GLB header or truncated data buffer");
    case cgltf_result_out_of_memory:
        return Error(ErrorCode::OutOfMemory, "Out of memory parsing glTF data");
    case cgltf_result_unknown_format:
    default:
        return Error(ErrorCode::InvalidArgument,
                     "Unrecognized or unsupported glTF/GLB file format");
    }
}

Result<MeshData> processGltfData(cgltf_data* data, std::string_view baseDir, std::string_view name,
                                 std::string_view sourcePath = "") {
    if (data == nullptr) {
        return Error(ErrorCode::InvalidArgument, "Null glTF data pointer");
    }

    MeshData meshData{std::string(name)};

    // 1. Extract Materials
    meshData.materials = detail::extractAllMaterials(data, baseDir, sourcePath);

    // 2. Discover Mesh Nodes and Transforms
    struct NodeMeshPair {
        const cgltf_mesh* mesh{nullptr};
        const cgltf_skin* skin{nullptr};
        Mat4 worldTransform{Mat4::identity()};
    };
    std::vector<NodeMeshPair> meshInstances;
    const cgltf_skin* primarySkin = (data->skins_count > 0) ? &data->skins[0] : nullptr;

    if (data->nodes_count > 0) {
        for (size_t n = 0; n < data->nodes_count; ++n) {
            const auto* node = &data->nodes[n];
            if (node->mesh != nullptr) {
                float rawMat[16];
                cgltf_node_transform_world(node, rawMat);
                Mat4 worldMat{};
                std::memcpy(worldMat.m, rawMat, sizeof(rawMat));
                if (node->skin != nullptr && primarySkin == nullptr) {
                    primarySkin = node->skin;
                }
                meshInstances.push_back(NodeMeshPair{
                    .mesh = node->mesh,
                    .skin = node->skin,
                    .worldTransform = (node->skin != nullptr) ? Mat4::identity() : worldMat});
            }
        }
    }

    // Fallback: If no node references any mesh, use all meshes with identity transform
    if (meshInstances.empty()) {
        for (size_t m = 0; m < data->meshes_count; ++m) {
            meshInstances.push_back(NodeMeshPair{
                .mesh = &data->meshes[m], .skin = nullptr, .worldTransform = Mat4::identity()});
        }
    }

    if (meshInstances.empty()) {
        return Error(ErrorCode::InvalidArgument, "glTF asset contains no meshes or geometry");
    }

    std::vector<uint16_t> skinJointToBone;
    if (primarySkin != nullptr) {
        auto skelRes = importSkeletonFromSkin(primarySkin, skinJointToBone);
        if (!skelRes.hasValue()) {
            return skelRes.error();
        }
        meshData.skeleton = std::move(skelRes.value());
    }

    // 3. Process Primitives and Geometry
    bool anyMissingNormals = false;

    for (const auto& inst : meshInstances) {
        const auto* mesh = inst.mesh;
        const Mat4& worldMat = inst.worldTransform;
        const Mat3 m33 = extractMat3(worldMat);
        const float det = m33.determinant();
        const Mat3 normMat = m33.inverseTranspose();
        const bool isMirrored = (det < 0.0F);

        for (size_t pIdx = 0; pIdx < mesh->primitives_count; ++pIdx) {
            const auto& prim = mesh->primitives[pIdx];

            if (prim.type != cgltf_primitive_type_triangles) {
                return Error(ErrorCode::InvalidArgument,
                             "Unsupported primitive mode: only triangles (mode 4) are supported");
            }

            const cgltf_attribute* posAttr = nullptr;
            const cgltf_attribute* normAttr = nullptr;
            const cgltf_attribute* uvAttr = nullptr;
            const cgltf_attribute* colAttr = nullptr;
            const cgltf_attribute* tanAttr = nullptr;
            const cgltf_attribute* jointsAttr = nullptr;
            const cgltf_attribute* weightsAttr = nullptr;

            for (size_t a = 0; a < prim.attributes_count; ++a) {
                const auto& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) {
                    posAttr = &attr;
                } else if (attr.type == cgltf_attribute_type_normal) {
                    normAttr = &attr;
                } else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) {
                    uvAttr = &attr;
                } else if (attr.type == cgltf_attribute_type_color && attr.index == 0) {
                    colAttr = &attr;
                } else if (attr.type == cgltf_attribute_type_tangent && attr.index == 0) {
                    tanAttr = &attr;
                } else if (attr.type == cgltf_attribute_type_joints && attr.index == 0) {
                    jointsAttr = &attr;
                } else if (attr.type == cgltf_attribute_type_weights && attr.index == 0) {
                    weightsAttr = &attr;
                }
            }

            if (posAttr == nullptr || posAttr->data == nullptr) {
                return Error(ErrorCode::InvalidArgument,
                             "glTF primitive missing required POSITION attribute");
            }

            const size_t vertCount = posAttr->data->count;
            if (vertCount == 0) {
                continue;
            }

            const uint32_t vertexBase = static_cast<uint32_t>(meshData.vertices.size());
            meshData.vertices.reserve(meshData.vertices.size() + vertCount);

            if (normAttr == nullptr) {
                anyMissingNormals = true;
            }

            for (size_t v = 0; v < vertCount; ++v) {
                float p[3] = {0.0F, 0.0F, 0.0F};
                cgltf_accessor_read_float(posAttr->data, v, p, 3);
                const Vec3 localPos{p[0], p[1], p[2]};
                const Vec3 worldPos = worldMat.transformPoint(localPos);

                Vec3 worldNorm{0.0F, 1.0F, 0.0F};
                if (normAttr != nullptr && normAttr->data != nullptr) {
                    float n[3] = {0.0F, 1.0F, 0.0F};
                    cgltf_accessor_read_float(normAttr->data, v, n, 3);
                    const Vec3 localNorm{n[0], n[1], n[2]};
                    const Vec3 transformedNorm = normMat.transformVector(localNorm);
                    const float len = transformedNorm.length();
                    worldNorm = (len > 1e-6F) ? (transformedNorm / len) : Vec3::unitY();
                }

                Vec2 uv{0.0F, 0.0F};
                if (uvAttr != nullptr && uvAttr->data != nullptr) {
                    float t[2] = {0.0F, 0.0F};
                    cgltf_accessor_read_float(uvAttr->data, v, t, 2);
                    uv = Vec2{t[0], t[1]};
                }

                Vec4 col{1.0F, 1.0F, 1.0F, 1.0F};
                if (colAttr != nullptr && colAttr->data != nullptr) {
                    float c[4] = {1.0F, 1.0F, 1.0F, 1.0F};
                    cgltf_accessor_read_float(colAttr->data, v, c, 4);
                    col = Vec4{c[0], c[1], c[2],
                               (colAttr->data->type == cgltf_type_vec3 ? 1.0F : c[3])};
                }

                Vec4 tangent{1.0F, 0.0F, 0.0F, 1.0F};
                if (tanAttr != nullptr && tanAttr->data != nullptr) {
                    float t[4] = {1.0F, 0.0F, 0.0F, 1.0F};
                    cgltf_accessor_read_float(tanAttr->data, v, t, 4);
                    const Vec3 localTan{t[0], t[1], t[2]};
                    const Vec3 transformedTan = worldMat.transformVector(localTan);
                    const float len = transformedTan.length();
                    const Vec3 worldTan = (len > 1e-6F) ? (transformedTan / len) : Vec3::unitX();
                    tangent = Vec4{worldTan.x, worldTan.y, worldTan.z, t[3]};
                }

                Vertex vert{worldPos, worldNorm, uv, col, tangent};
                if (jointsAttr != nullptr && jointsAttr->data != nullptr &&
                    weightsAttr != nullptr && weightsAttr->data != nullptr) {
                    cgltf_uint j[4]{0, 0, 0, 0};
                    float w[4]{0.0F, 0.0F, 0.0F, 0.0F};
                    cgltf_accessor_read_uint(jointsAttr->data, v, j, 4);
                    cgltf_accessor_read_float(weightsAttr->data, v, w, 4);

                    float wSum = 0.0F;
                    for (int k = 0; k < 4; ++k) {
                        uint32_t mappedJoint = 0;
                        if (!skinJointToBone.empty()) {
                            if (j[k] >= skinJointToBone.size()) {
                                return Error(ErrorCode::InvalidArgument,
                                             "glTF JOINTS_0 index out of skin joints range");
                            }
                            mappedJoint = skinJointToBone[j[k]];
                        } else {
                            mappedJoint = static_cast<uint32_t>(j[k]);
                        }
                        vert.joints[k] = mappedJoint;
                        w[k] = std::max(0.0F, w[k]);
                        wSum += w[k];
                    }
                    if (wSum > 1e-5F) {
                        const float invW = 1.0F / wSum;
                        vert.weights = Vec4{w[0] * invW, w[1] * invW, w[2] * invW, w[3] * invW};
                    }
                }
                meshData.vertices.push_back(vert);
            }

            Submesh submesh{};
            submesh.name = (prim.material != nullptr && prim.material->name != nullptr)
                               ? prim.material->name
                               : ((mesh->name != nullptr) ? mesh->name : "Submesh");
            submesh.firstIndex = static_cast<uint32_t>(meshData.indices.size());
            // Absolute vertex indices (vertexBase + localIndex) are baked into meshData.indices.
            submesh.vertexOffset = 0;

            if (prim.material != nullptr) {
                const auto diff = prim.material - data->materials;
                if (diff >= 0 && diff < static_cast<ptrdiff_t>(data->materials_count)) {
                    submesh.materialIndex = static_cast<uint32_t>(diff);
                }
            }

            if (prim.indices != nullptr) {
                const size_t idxCount = prim.indices->count;
                if ((idxCount % 3) != 0) {
                    return Error(ErrorCode::InvalidArgument,
                                 "glTF primitive index count is not a multiple of 3");
                }

                meshData.indices.reserve(meshData.indices.size() + idxCount);

                for (size_t t = 0; t < idxCount / 3; ++t) {
                    const uint32_t i0 =
                        vertexBase +
                        static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, (t * 3) + 0));
                    const uint32_t i1 =
                        vertexBase +
                        static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, (t * 3) + 1));
                    const uint32_t i2 =
                        vertexBase +
                        static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, (t * 3) + 2));

                    if (i0 >= meshData.vertices.size() || i1 >= meshData.vertices.size() ||
                        i2 >= meshData.vertices.size()) {
                        return Error(ErrorCode::OutOfBounds,
                                     "glTF index references non-existent vertex");
                    }

                    if (isMirrored) {
                        meshData.indices.push_back(i0);
                        meshData.indices.push_back(i2);
                        meshData.indices.push_back(i1);
                    } else {
                        meshData.indices.push_back(i0);
                        meshData.indices.push_back(i1);
                        meshData.indices.push_back(i2);
                    }
                }
            } else {
                // Non-indexed primitive
                if ((vertCount % 3) != 0) {
                    return Error(ErrorCode::InvalidArgument,
                                 "Non-indexed glTF primitive vertex count is not a multiple of 3");
                }

                meshData.indices.reserve(meshData.indices.size() + vertCount);

                for (size_t t = 0; t < vertCount / 3; ++t) {
                    const uint32_t i0 = vertexBase + static_cast<uint32_t>((t * 3) + 0);
                    const uint32_t i1 = vertexBase + static_cast<uint32_t>((t * 3) + 1);
                    const uint32_t i2 = vertexBase + static_cast<uint32_t>((t * 3) + 2);

                    if (isMirrored) {
                        meshData.indices.push_back(i0);
                        meshData.indices.push_back(i2);
                        meshData.indices.push_back(i1);
                    } else {
                        meshData.indices.push_back(i0);
                        meshData.indices.push_back(i1);
                        meshData.indices.push_back(i2);
                    }
                }
            }

            submesh.indexCount =
                static_cast<uint32_t>(meshData.indices.size() - submesh.firstIndex);

            if (tanAttr == nullptr && submesh.indexCount >= 3) {
                meshData.generateTangentsForRange(submesh.firstIndex, submesh.indexCount,
                                                  vertexBase, static_cast<uint32_t>(vertCount));
            }

            meshData.submeshes.push_back(std::move(submesh));
        }
    }

    if (anyMissingNormals) {
        meshData.generateNormals();
    }

    meshData.recalculateBounds();

    auto valRes = meshData.validate();
    if (!valRes.hasValue()) {
        return valRes.error();
    }

    if (meshData.skeleton.has_value() && data->animations_count > 0) {
        auto animRes = importAnimationsFromData(data, *meshData.skeleton);
        if (!animRes.hasValue()) {
            return animRes.error();
        }
        meshData.animations = std::move(animRes.value());
    }

    return Result<MeshData>::success(std::move(meshData));
}

} // namespace

Result<MeshData> GltfImporter::importFromFile(std::string_view filePath) {
    if (filePath.empty()) {
        return Error(ErrorCode::InvalidArgument, "Empty glTF file path");
    }

    cgltf_options options{};
    cgltf_data* parsedData = nullptr;
    const std::string pathStr(filePath);

    const cgltf_result parseRes = cgltf_parse_file(&options, pathStr.c_str(), &parsedData);
    if (parseRes != cgltf_result_success) {
        return mapCgltfResult(parseRes);
    }

    CgltfGuard guard{parsedData};

    const cgltf_result loadRes = cgltf_load_buffers(&options, parsedData, pathStr.c_str());
    if (loadRes != cgltf_result_success) {
        return mapCgltfResult(loadRes);
    }

    const cgltf_result valRes = cgltf_validate(parsedData);
    if (valRes != cgltf_result_success) {
        return Error(ErrorCode::InvalidArgument,
                     "glTF validation failed: invalid accessor ranges or corrupt mesh data");
    }

    const std::filesystem::path p(filePath);
    const std::string baseDir = p.parent_path().string();
    const std::string stem = p.stem().string();

    return processGltfData(parsedData, baseDir, stem, filePath);
}

Result<MeshData> GltfImporter::importFromMemory(const void* data, size_t size,
                                                std::string_view baseDir, std::string_view name) {
    if (data == nullptr || size == 0) {
        return Error(ErrorCode::InvalidArgument, "Empty or null glTF memory buffer");
    }

    cgltf_options options{};
    cgltf_data* parsedData = nullptr;

    const cgltf_result parseRes = cgltf_parse(&options, data, size, &parsedData);
    if (parseRes != cgltf_result_success) {
        return mapCgltfResult(parseRes);
    }

    CgltfGuard guard{parsedData};

    const std::string baseDirStr(baseDir);
    const char* pathForBuffers = baseDir.empty() ? nullptr : baseDirStr.c_str();
    const cgltf_result loadRes = cgltf_load_buffers(&options, parsedData, pathForBuffers);
    if (loadRes != cgltf_result_success) {
        return mapCgltfResult(loadRes);
    }

    const cgltf_result valRes = cgltf_validate(parsedData);
    if (valRes != cgltf_result_success) {
        return Error(ErrorCode::InvalidArgument,
                     "glTF validation failed: invalid accessor ranges or corrupt mesh data");
    }

    return processGltfData(parsedData, baseDir, name);
}

Result<std::vector<anim::AnimationClip>>
GltfImporter::importAnimationsFromFile(std::string_view filePath,
                                       const anim::Skeleton& targetSkeleton) {
    if (filePath.empty()) {
        return Error(ErrorCode::InvalidArgument, "Empty glTF animation file path");
    }

    cgltf_options options{};
    cgltf_data* parsedData = nullptr;
    const std::string pathStr(filePath);

    const cgltf_result parseRes = cgltf_parse_file(&options, pathStr.c_str(), &parsedData);
    if (parseRes != cgltf_result_success) {
        return mapCgltfResult(parseRes);
    }

    CgltfGuard guard{parsedData};

    const cgltf_result loadRes = cgltf_load_buffers(&options, parsedData, pathStr.c_str());
    if (loadRes != cgltf_result_success) {
        return mapCgltfResult(loadRes);
    }

    const cgltf_result valRes = cgltf_validate(parsedData);
    if (valRes != cgltf_result_success) {
        return Error(ErrorCode::InvalidArgument,
                     "glTF animation validation failed: invalid accessor ranges");
    }

    return importAnimationsFromData(parsedData, targetSkeleton);
}

} // namespace eng::render
