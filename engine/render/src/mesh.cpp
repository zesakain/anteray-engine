/// @file mesh.cpp
/// @brief CPU mesh data validation, bounding box calculation, and normal generation.

#include "render/mesh.h"

#include <cmath>

namespace eng::render {

Result<void> MeshData::validate() const noexcept {
    if (vertices.empty()) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "Mesh vertex buffer is empty"};
    }

    if (indices.empty()) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "Mesh index buffer is empty"};
    }

    if ((indices.size() % 3) != 0) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "Mesh index count is not a multiple of 3 (not triangulated)"};
    }

    const auto vertexCount = static_cast<uint32_t>(vertices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= vertexCount) {
            return Error{.code = ErrorCode::OutOfBounds,
                         .message = "Mesh index references non-existent vertex"};
        }
    }

    const auto indexCount = static_cast<uint32_t>(indices.size());
    for (const auto& submesh : submeshes) {
        if (submesh.firstIndex + submesh.indexCount > indexCount) {
            return Error{.code = ErrorCode::OutOfBounds,
                         .message = "Submesh range exceeds mesh index buffer bounds"};
        }
    }

    return Result<void>::success();
}

void MeshData::recalculateBounds() noexcept {
    bounds = AABB{};
    for (const auto& v : vertices) {
        bounds.expand(v.position);
    }

    for (auto& submesh : submeshes) {
        submesh.bounds = AABB{};
        const uint32_t endIdx = submesh.firstIndex + submesh.indexCount;
        for (uint32_t i = submesh.firstIndex; i < endIdx && i < indices.size(); ++i) {
            const uint32_t vIdx = indices[i];
            if (vIdx < vertices.size()) {
                submesh.bounds.expand(vertices[vIdx].position);
            }
        }
    }
}

void MeshData::generateNormals() noexcept {
    if (vertices.empty() || indices.size() < 3) {
        return;
    }

    // Reset normals to zero
    for (auto& v : vertices) {
        v.normal = Vec3::zero();
    }

    // Accumulate face normals
    const size_t triCount = indices.size() / 3;
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t i0 = indices[(t * 3) + 0];
        const uint32_t i1 = indices[(t * 3) + 1];
        const uint32_t i2 = indices[(t * 3) + 2];

        if (i0 < vertices.size() && i1 < vertices.size() && i2 < vertices.size()) {
            const Vec3& p0 = vertices[i0].position;
            const Vec3& p1 = vertices[i1].position;
            const Vec3& p2 = vertices[i2].position;

            const Vec3 edge1 = p1 - p0;
            const Vec3 edge2 = p2 - p0;
            const Vec3 faceNormal = Vec3::cross(edge1, edge2);

            vertices[i0].normal += faceNormal;
            vertices[i1].normal += faceNormal;
            vertices[i2].normal += faceNormal;
        }
    }

    // Normalize
    for (auto& v : vertices) {
        const float lenSq = v.normal.lengthSq();
        if (lenSq > 1e-12F) {
            v.normal = v.normal.normalized();
        } else {
            v.normal = Vec3::unitY();
        }
    }
}

void MeshData::generateTangents() noexcept {
    if (vertices.empty() || indices.size() < 3) {
        return;
    }
    generateTangentsForRange(0, static_cast<uint32_t>(indices.size()), 0,
                             static_cast<uint32_t>(vertices.size()));
}

void MeshData::generateTangentsForRange(uint32_t firstIndex, uint32_t indexCount,
                                        uint32_t vertexBase, uint32_t vertexCount) noexcept {
    if (indexCount < 3 || vertexCount == 0 || firstIndex + indexCount > indices.size() ||
        vertexBase + vertexCount > vertices.size()) {
        return;
    }

    std::vector<Vec3> tan1(vertexCount, Vec3::zero());
    std::vector<Vec3> tan2(vertexCount, Vec3::zero());

    const size_t triCount = indexCount / 3;
    for (size_t t = 0; t < triCount; ++t) {
        const uint32_t i0 = indices[firstIndex + (t * 3) + 0];
        const uint32_t i1 = indices[firstIndex + (t * 3) + 1];
        const uint32_t i2 = indices[firstIndex + (t * 3) + 2];

        if (i0 >= vertexBase && i0 < vertexBase + vertexCount && i1 >= vertexBase &&
            i1 < vertexBase + vertexCount && i2 >= vertexBase && i2 < vertexBase + vertexCount) {
            const Vertex& v0 = vertices[i0];
            const Vertex& v1 = vertices[i1];
            const Vertex& v2 = vertices[i2];

            const Vec3 e1 = v1.position - v0.position;
            const Vec3 e2 = v2.position - v0.position;

            const float deltaU1 = v1.uv.x - v0.uv.x;
            const float deltaV1 = v1.uv.y - v0.uv.y;
            const float deltaU2 = v2.uv.x - v0.uv.x;
            const float deltaV2 = v2.uv.y - v0.uv.y;

            const float det = (deltaU1 * deltaV2) - (deltaU2 * deltaV1);
            if (std::abs(det) > 1e-7F) {
                const float invDet = 1.0F / det;
                if (std::isfinite(invDet)) {
                    const Vec3 sdir = ((e1 * deltaV2) - (e2 * deltaV1)) * invDet;
                    const Vec3 tdir = ((e2 * deltaU1) - (e1 * deltaU2)) * invDet;

                    if (std::isfinite(sdir.x) && std::isfinite(sdir.y) && std::isfinite(sdir.z) &&
                        std::isfinite(tdir.x) && std::isfinite(tdir.y) && std::isfinite(tdir.z)) {
                        const uint32_t l0 = i0 - vertexBase;
                        const uint32_t l1 = i1 - vertexBase;
                        const uint32_t l2 = i2 - vertexBase;

                        tan1[l0] += sdir;
                        tan1[l1] += sdir;
                        tan1[l2] += sdir;

                        tan2[l0] += tdir;
                        tan2[l1] += tdir;
                        tan2[l2] += tdir;
                    }
                }
            }
        }
    }

    for (size_t localIdx = 0; localIdx < vertexCount; ++localIdx) {
        const size_t vIdx = vertexBase + localIdx;
        Vec3 n = vertices[vIdx].normal;
        if (n.lengthSq() > 1e-10F) {
            n = n.normalized();
        } else {
            n = Vec3::unitY();
            vertices[vIdx].normal = n;
        }

        const Vec3& t = tan1[localIdx];
        if (t.lengthSq() > 1e-10F) {
            // Gram-Schmidt orthogonalization: t' = normalize(t - n * dot(n, t))
            Vec3 orthoT = (t - (n * Vec3::dot(n, t))).normalized();
            if (orthoT.lengthSq() <= 1e-10F) {
                Vec3 c1 = Vec3::cross(n, Vec3(0.0F, 0.0F, 1.0F));
                Vec3 c2 = Vec3::cross(n, Vec3(0.0F, 1.0F, 0.0F));
                orthoT = (c1.lengthSq() > c2.lengthSq()) ? c1.normalized() : c2.normalized();
                if (orthoT.lengthSq() <= 1e-10F) {
                    orthoT = Vec3::unitX();
                }
            }
            // Calculate handedness (w): sign of dot(cross(n, t), tan2)
            const float dotVal = Vec3::dot(Vec3::cross(n, t), tan2[localIdx]);
            const float w = (dotVal < 0.0F) ? -1.0F : 1.0F;
            vertices[vIdx].tangent = Vec4(orthoT.x, orthoT.y, orthoT.z, w);
        } else {
            // Fallback tangent perpendicular to normal
            Vec3 c1 = Vec3::cross(n, Vec3(0.0F, 0.0F, 1.0F));
            Vec3 c2 = Vec3::cross(n, Vec3(0.0F, 1.0F, 0.0F));
            Vec3 tDef = (c1.lengthSq() > c2.lengthSq()) ? c1.normalized() : c2.normalized();
            if (tDef.lengthSq() <= 1e-10F) {
                tDef = Vec3::unitX();
            }
            vertices[vIdx].tangent = Vec4(tDef.x, tDef.y, tDef.z, 1.0F);
        }
    }
}

void MeshData::applyCpuSkinning(std::span<const Mat4> skinningMatrices,
                                std::vector<Vertex>& outVertices) const {
    outVertices.resize(vertices.size());
    if (skinningMatrices.empty()) {
        outVertices = vertices;
        return;
    }

    const size_t boneCount = skinningMatrices.size();
    for (size_t i = 0; i < vertices.size(); ++i) {
        const Vertex& src = vertices[i];
        Vertex& dst = outVertices[i];
        dst = src;

        const float wSum = src.weights.x + src.weights.y + src.weights.z + src.weights.w;
        if (wSum <= 1e-4F) {
            continue;
        }

        const float invSum = 1.0F / wSum;
        const float w[4] = {src.weights.x * invSum, src.weights.y * invSum, src.weights.z * invSum,
                            src.weights.w * invSum};

        Mat4 skin{};
        for (float& v : skin.m) {
            v = 0.0F;
        }
        for (int k = 0; k < 4; ++k) {
            if (w[k] > 0.0F) {
                const uint32_t j = (src.joints[k] < boneCount) ? src.joints[k] : 0U;
                const Mat4& bm = skinningMatrices[j];
                for (size_t idx = 0; idx < 16; ++idx) {
                    skin.m[idx] += bm.m[idx] * w[k];
                }
            }
        }

        dst.position = skin.transformPoint(src.position);
        const Vec3 n = skin.transformVector(src.normal);
        dst.normal = (n.lengthSq() > 1e-8F) ? n.normalized() : Vec3::unitY();
        const Vec3 t = skin.transformVector(Vec3{src.tangent.x, src.tangent.y, src.tangent.z});
        const Vec3 tn = (t.lengthSq() > 1e-8F) ? t.normalized() : Vec3::unitX();
        dst.tangent = Vec4{tn.x, tn.y, tn.z, src.tangent.w};
    }
}

} // namespace eng::render
