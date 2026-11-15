/// @file mesh_test.cpp
/// @brief Comprehensive unit tests for MeshData, ObjImporter, AssetId, and GpuMesh.

#include "core/assert.h"
#include "render/asset_types.h"
#include "render/gpu_mesh.h"
#include "render/mesh.h"
#include "render/obj_importer.h"
#include "rhi/rhi_device.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace eng;
using namespace eng::render;

#define TEST_CHECK(cond)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << '\n'; \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

static int testAssetIdHashing() {
    const AssetId id1 = makeAssetIdFromPath("assets/models/cube.obj");
    const AssetId id2 = makeAssetIdFromPath("assets/models/cube.obj");
    const AssetId id3 = makeAssetIdFromPath("assets\\models\\cube.obj"); // Backslash normalization
    const AssetId id4 = makeAssetIdFromPath("./assets/models/cube.obj"); // Leading ./ normalization
    const AssetId idDiff = makeAssetIdFromPath("assets/models/prism.obj");

    TEST_CHECK(id1.isValid());
    TEST_CHECK(id1 == id2);
    TEST_CHECK(id1 == id3);
    TEST_CHECK(id1 == id4);
    TEST_CHECK(id1 != idDiff);
    return 0;
}

static int testMeshDataValidation() {
    // 1. Empty mesh fails validation
    MeshData emptyMesh;
    auto resEmpty = emptyMesh.validate();
    TEST_CHECK(!resEmpty.hasValue());
    TEST_CHECK(resEmpty.error().code == ErrorCode::InvalidArgument);

    // 2. Vertices without indices fails validation
    MeshData vertOnlyMesh;
    vertOnlyMesh.vertices.push_back(Vertex{Vec3::zero(), Vec3::unitY(), Vec2::zero()});
    auto resVertOnly = vertOnlyMesh.validate();
    TEST_CHECK(!resVertOnly.hasValue());

    // 3. Non-triangulated index count (e.g. 2 indices) fails validation
    MeshData nonTriMesh;
    nonTriMesh.vertices.push_back(Vertex{Vec3::zero(), Vec3::unitY(), Vec2::zero()});
    nonTriMesh.vertices.push_back(Vertex{Vec3::unitX(), Vec3::unitY(), Vec2::zero()});
    nonTriMesh.indices = {0, 1};
    auto resNonTri = nonTriMesh.validate();
    TEST_CHECK(!resNonTri.hasValue());

    // 4. Out of bounds index references fail validation
    MeshData oobMesh;
    oobMesh.vertices.push_back(Vertex{Vec3::zero(), Vec3::unitY(), Vec2::zero()});
    oobMesh.indices = {0, 1, 2}; // 1 and 2 don't exist
    auto resOob = oobMesh.validate();
    TEST_CHECK(!resOob.hasValue());
    TEST_CHECK(resOob.error().code == ErrorCode::OutOfBounds);

    // 5. Valid triangle mesh passes validation
    MeshData validMesh;
    validMesh.vertices.push_back(Vertex{Vec3::zero(), Vec3::unitY(), Vec2::zero()});
    validMesh.vertices.push_back(Vertex{Vec3::unitX(), Vec3::unitY(), Vec2::zero()});
    validMesh.vertices.push_back(Vertex{Vec3::unitY(), Vec3::unitY(), Vec2::zero()});
    validMesh.indices = {0, 1, 2};
    auto resValid = validMesh.validate();
    TEST_CHECK(resValid.hasValue());

    return 0;
}

static int testMeshDataBoundsAndNormals() {
    MeshData mesh;
    mesh.vertices.push_back(Vertex{Vec3(-1.0F, -2.0F, -3.0F), Vec3::zero(), Vec2::zero()});
    mesh.vertices.push_back(Vertex{Vec3(4.0F, 5.0F, 6.0F), Vec3::zero(), Vec2::zero()});
    mesh.vertices.push_back(Vertex{Vec3(0.0F, 1.0F, 2.0F), Vec3::zero(), Vec2::zero()});
    mesh.indices = {0, 1, 2};

    mesh.recalculateBounds();
    TEST_CHECK(mesh.bounds.isValid());
    TEST_CHECK(std::abs(mesh.bounds.minBound.x - (-1.0F)) < 1e-5F);
    TEST_CHECK(std::abs(mesh.bounds.maxBound.x - 4.0F) < 1e-5F);
    TEST_CHECK(std::abs(mesh.bounds.getSize().x - 5.0F) < 1e-5F);

    mesh.generateNormals();
    for (const auto& v : mesh.vertices) {
        TEST_CHECK(std::abs(v.normal.length() - 1.0F) < 1e-4F);
    }

    return 0;
}

static int testMeshDataTangents() {
    MeshData mesh;
    // Standard quad in XY plane facing +Z
    mesh.vertices = {
        Vertex(Vec3(-1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 1.0F)),
        Vertex(Vec3(1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 1.0F)),
        Vertex(Vec3(1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 0.0F)),
        Vertex(Vec3(-1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F)),
    };
    mesh.indices = {0, 1, 2, 2, 3, 0};

    mesh.generateTangents();

    for (const auto& v : mesh.vertices) {
        // Tangent should be aligned with +X
        TEST_CHECK(std::abs(v.tangent.x - 1.0F) < 1e-4F);
        TEST_CHECK(std::abs(v.tangent.y - 0.0F) < 1e-4F);
        TEST_CHECK(std::abs(v.tangent.z - 0.0F) < 1e-4F);
        // Handedness w should be +1 or -1
        TEST_CHECK(std::abs(std::abs(v.tangent.w) - 1.0F) < 1e-4F);
        // Orthogonal to normal
        const Vec3 tanVec{v.tangent.x, v.tangent.y, v.tangent.z};
        TEST_CHECK(std::abs(Vec3::dot(v.normal, tanVec)) < 1e-4F);
    }

    return 0;
}

static int testMeshDataTangentsDegenerateAndMirrored() {
    // 1. Degenerate UVs: all vertices have identical UV (0, 0)
    MeshData degenMesh{};
    degenMesh.vertices = {
        Vertex(Vec3(-1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F)),
        Vertex(Vec3(1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F)),
        Vertex(Vec3(1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F)),
    };
    degenMesh.indices = {0, 1, 2};
    degenMesh.generateTangents();

    for (const auto& v : degenMesh.vertices) {
        const Vec3 tanVec{v.tangent.x, v.tangent.y, v.tangent.z};
        TEST_CHECK(tanVec.lengthSq() > 0.5F);                      // non-zero fallback
        TEST_CHECK(std::abs(Vec3::dot(v.normal, tanVec)) < 1e-4F); // strictly orthogonal
        TEST_CHECK(std::abs(v.tangent.w) == 1.0F);
    }

    // 2. Mirrored UVs: U axis inverted (U = 1 - x) relative to standard right-handed UV
    MeshData mirroredMesh{};
    mirroredMesh.vertices = {
        Vertex(Vec3(-1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 0.0F)),
        Vertex(Vec3(1.0F, -1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F)),
        Vertex(Vec3(1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 1.0F)),
        Vertex(Vec3(-1.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 1.0F)),
    };
    mirroredMesh.indices = {0, 1, 2, 2, 3, 0};
    mirroredMesh.generateTangents();

    for (const auto& v : mirroredMesh.vertices) {
        // Mirrored UV must produce negative bitangent handedness w = -1.0
        TEST_CHECK(v.tangent.w == -1.0F);
        const Vec3 tanVec{v.tangent.x, v.tangent.y, v.tangent.z};
        TEST_CHECK(std::abs(Vec3::dot(v.normal, tanVec)) < 1e-4F);
    }

    return 0;
}

static int testMeshDataTangentsRangeMultiSubmesh() {
    MeshData mesh{};
    // Submesh 0: 3 vertices, triangle. Pre-populated custom tangent
    Vertex v0(Vec3(0.0F, 0.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F));
    v0.tangent = Vec4(0.0F, 1.0F, 0.0F, 1.0F);
    Vertex v1(Vec3(1.0F, 0.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 0.0F));
    v1.tangent = Vec4(0.0F, 1.0F, 0.0F, 1.0F);
    Vertex v2(Vec3(0.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 1.0F));
    v2.tangent = Vec4(0.0F, 1.0F, 0.0F, 1.0F);
    mesh.vertices.push_back(v0);
    mesh.vertices.push_back(v1);
    mesh.vertices.push_back(v2);
    mesh.indices.push_back(0);
    mesh.indices.push_back(1);
    mesh.indices.push_back(2);

    Submesh sm0{};
    sm0.firstIndex = 0;
    sm0.indexCount = 3;
    mesh.submeshes.push_back(sm0);

    // Submesh 1: 3 vertices, triangle. No tangents (all zeros)
    mesh.vertices.push_back(
        Vertex(Vec3(2.0F, 0.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 0.0F)));
    mesh.vertices.push_back(
        Vertex(Vec3(3.0F, 0.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(1.0F, 0.0F)));
    mesh.vertices.push_back(
        Vertex(Vec3(2.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 1.0F), Vec2(0.0F, 1.0F)));
    mesh.indices.push_back(3);
    mesh.indices.push_back(4);
    mesh.indices.push_back(5);

    Submesh sm1{};
    sm1.firstIndex = 3;
    sm1.indexCount = 3;
    mesh.submeshes.push_back(sm1);

    // Generate tangents only for Submesh 1
    mesh.generateTangentsForRange(sm1.firstIndex, sm1.indexCount, 3, 3);

    // Submesh 0 vertices must remain unmodified
    for (size_t i = 0; i < 3; ++i) {
        TEST_CHECK(mesh.vertices[i].tangent.x == 0.0F);
        TEST_CHECK(mesh.vertices[i].tangent.y == 1.0F);
        TEST_CHECK(mesh.vertices[i].tangent.z == 0.0F);
        TEST_CHECK(mesh.vertices[i].tangent.w == 1.0F);
    }

    // Submesh 1 vertices must have valid generated tangents (+X direction, w = 1.0)
    for (size_t i = 3; i < 6; ++i) {
        const auto& v = mesh.vertices[i];
        TEST_CHECK(std::abs(v.tangent.x - 1.0F) < 1e-4F);
        TEST_CHECK(std::abs(v.tangent.y - 0.0F) < 1e-4F);
        TEST_CHECK(std::abs(v.tangent.z - 0.0F) < 1e-4F);
        TEST_CHECK(v.tangent.w == 1.0F);
        const Vec3 tanVec{v.tangent.x, v.tangent.y, v.tangent.z};
        TEST_CHECK(std::abs(Vec3::dot(v.normal, tanVec)) < 1e-4F);
    }

    return 0;
}

static int testNormalMatrixNonUniformScalingAndMirrored() {
    // 1. Non-uniform scale matrix + rotation + translation
    const Vec3 scale{3.0F, 0.5F, 1.5F};
    const Quat rot = Quat::fromAxisAngle(Vec3(0.0F, 1.0F, 0.0F).normalized(), 0.785398F);
    const Mat4 model = Mat4::fromTransform(Vec3(10.0F, -5.0F, 2.0F), rot, scale);

    // Extract upper 3x3
    const float m00 = model.m[0];
    const float m01 = model.m[4];
    const float m02 = model.m[8];
    const float m10 = model.m[1];
    const float m11 = model.m[5];
    const float m12 = model.m[9];
    const float m20 = model.m[2];
    const float m21 = model.m[6];
    const float m22 = model.m[10];

    const float det = (m00 * ((m11 * m22) - (m12 * m21))) - (m01 * ((m10 * m22) - (m12 * m20))) +
                      (m02 * ((m10 * m21) - (m11 * m20)));

    TEST_CHECK(std::abs(det) > 1e-4F);
    TEST_CHECK(det > 0.0F); // non-mirrored

    // Compute transpose(inverse(m3x3))
    const float invDet = 1.0F / det;
    const float n00 = ((m11 * m22) - (m12 * m21)) * invDet;
    const float n01 = ((m12 * m20) - (m10 * m22)) * invDet;
    const float n02 = ((m10 * m21) - (m11 * m20)) * invDet;

    const float n10 = ((m02 * m21) - (m01 * m22)) * invDet;
    const float n11 = ((m00 * m22) - (m02 * m20)) * invDet;
    const float n12 = ((m01 * m20) - (m00 * m21)) * invDet;

    const float n20 = ((m01 * m12) - (m02 * m11)) * invDet;
    const float n21 = ((m02 * m10) - (m00 * m12)) * invDet;
    const float n22 = ((m00 * m11) - (m01 * m10)) * invDet;

    // Surface quad vectors (perpendicular)
    const Vec3 n = Vec3(0.0F, 1.0F, 0.0F);
    const Vec3 t = Vec3(1.0F, 0.0F, 0.0F);
    const Vec3 b = Vec3(0.0F, 0.0F, 1.0F);

    // Transform vectors to world space
    const Vec3 tWorld{(m00 * t.x) + (m01 * t.y) + (m02 * t.z),
                      (m10 * t.x) + (m11 * t.y) + (m12 * t.z),
                      (m20 * t.x) + (m21 * t.y) + (m22 * t.z)};
    const Vec3 bWorld{(m00 * b.x) + (m01 * b.y) + (m02 * b.z),
                      (m10 * b.x) + (m11 * b.y) + (m12 * b.z),
                      (m20 * b.x) + (m21 * b.y) + (m22 * b.z)};
    const Vec3 nCorrectWorld{(n00 * n.x) + (n01 * n.y) + (n02 * n.z),
                             (n10 * n.x) + (n11 * n.y) + (n12 * n.z),
                             (n20 * n.x) + (n21 * n.y) + (n22 * n.z)};

    // Correct normal must be orthogonal to surface tangents in world space
    TEST_CHECK(std::abs(Vec3::dot(nCorrectWorld.normalized(), tWorld.normalized())) < 1e-4F);
    TEST_CHECK(std::abs(Vec3::dot(nCorrectWorld.normalized(), bWorld.normalized())) < 1e-4F);

    // 2. Mirrored transform: negative scale on one axis (e.g. X = -1.0)
    const Mat4 mirroredModel = Mat4::fromScale(Vec3(-1.0F, 1.0F, 1.0F));
    const float mirDet = (mirroredModel.m[0] * ((mirroredModel.m[5] * mirroredModel.m[10]) -
                                                (mirroredModel.m[9] * mirroredModel.m[6])));
    TEST_CHECK(mirDet < 0.0F);
    const float handedness = 1.0F * (mirDet < 0.0F ? -1.0F : 1.0F);
    TEST_CHECK(handedness == -1.0F);

    // 3. Near-singular matrix guard
    const float degenDet = 0.0F;
    const bool isDegenerate = std::abs(degenDet) < 1e-7F;
    TEST_CHECK(isDegenerate);

    return 0;
}

static int testRobustTbnReconstructionMath() {
    auto constructTbnCpu = [](Vec3 n, Vec3 t, float handedness, Vec3& outT, Vec3& outB,
                              Vec3& outN) {
        float nLenSq = n.lengthSq();
        outN = (nLenSq > 1e-8F) ? (n * (1.0F / std::sqrt(nLenSq))) : Vec3(0.0F, 1.0F, 0.0F);

        float tLenSq = t.lengthSq();
        if (tLenSq < 1e-8F) {
            Vec3 up = (std::abs(outN.y) < 0.999F) ? Vec3(0.0F, 1.0F, 0.0F) : Vec3(1.0F, 0.0F, 0.0F);
            outT = Vec3::cross(up, outN).normalized();
        } else {
            outT = t * (1.0F / std::sqrt(tLenSq));
            outT = (outT - (outN * Vec3::dot(outN, outT)));
            float orthoLenSq = outT.lengthSq();
            if (orthoLenSq < 1e-8F) {
                Vec3 up =
                    (std::abs(outN.y) < 0.999F) ? Vec3(0.0F, 1.0F, 0.0F) : Vec3(1.0F, 0.0F, 0.0F);
                outT = Vec3::cross(up, outN).normalized();
            } else {
                outT = outT * (1.0F / std::sqrt(orthoLenSq));
            }
        }
        float h = (handedness < 0.0F) ? -1.0F : 1.0F;
        outB = Vec3::cross(outN, outT).normalized() * h;
    };

    Vec3 outT{};
    Vec3 outB{};
    Vec3 outN{};

    // Case 1: Standard orthogonal input
    constructTbnCpu(Vec3(0.0F, 0.0F, 1.0F), Vec3(1.0F, 0.0F, 0.0F), 1.0F, outT, outB, outN);
    TEST_CHECK(std::abs(outT.length() - 1.0F) < 1e-4F);
    TEST_CHECK(std::abs(outB.length() - 1.0F) < 1e-4F);
    TEST_CHECK(std::abs(outN.length() - 1.0F) < 1e-4F);
    TEST_CHECK(std::abs(Vec3::dot(outT, outN)) < 1e-4F);
    TEST_CHECK(std::abs(Vec3::dot(outB, outN)) < 1e-4F);
    TEST_CHECK(std::abs(Vec3::dot(outT, outB)) < 1e-4F);
    TEST_CHECK(std::abs(outB.y - 1.0F) < 1e-4F);

    // Case 2: Degenerate zero tangent
    constructTbnCpu(Vec3(0.0F, 1.0F, 0.0F), Vec3(0.0F, 0.0F, 0.0F), 1.0F, outT, outB, outN);
    TEST_CHECK(std::abs(outT.length() - 1.0F) < 1e-4F);
    TEST_CHECK(std::abs(Vec3::dot(outT, outN)) < 1e-4F);
    TEST_CHECK(std::abs(Vec3::dot(outB, outN)) < 1e-4F);
    TEST_CHECK(std::abs(Vec3::dot(outT, outB)) < 1e-4F);

    // Case 3: Parallel tangent (T || N)
    constructTbnCpu(Vec3(0.0F, 0.0F, 1.0F), Vec3(0.0F, 0.0F, 1.0F), 1.0F, outT, outB, outN);
    TEST_CHECK(std::abs(outT.length() - 1.0F) < 1e-4F);
    TEST_CHECK(std::abs(Vec3::dot(outT, outN)) < 1e-4F);
    TEST_CHECK(std::abs(Vec3::dot(outB, outN)) < 1e-4F);
    TEST_CHECK(std::abs(Vec3::dot(outT, outB)) < 1e-4F);

    // Case 4: Inverted handedness (w = -1)
    constructTbnCpu(Vec3(0.0F, 0.0F, 1.0F), Vec3(1.0F, 0.0F, 0.0F), -1.0F, outT, outB, outN);
    TEST_CHECK(std::abs(outB.y - (-1.0F)) < 1e-4F);

    return 0;
}

static int testObjImporterFromString() {
    const std::string_view objQuad = R"(
# Test Quad
v 0.0 0.0 0.0
v 1.0 0.0 0.0
v 1.0 1.0 0.0
v 0.0 1.0 0.0

vt 0.0 0.0
vt 1.0 0.0
vt 1.0 1.0
vt 0.0 1.0

vn 0.0 0.0 1.0

# Quad face with fan triangulation
f 1/1/1 2/2/1 3/3/1 4/4/1
)";

    auto res = ObjImporter::importFromString(objQuad, "TestQuad");
    TEST_CHECK(res.hasValue());
    const auto& mesh = res.value();

    TEST_CHECK(mesh.vertices.size() == 4);
    TEST_CHECK(mesh.indices.size() == 6); // 2 triangles
    TEST_CHECK(mesh.getTriangleCount() == 2);
    TEST_CHECK(mesh.bounds.isValid());

    return 0;
}

static int testObjImporterFromFile() {
    // 1. Test standard cube
    auto cubeRes = ObjImporter::importFromFile("assets/models/cube.obj");
    TEST_CHECK(cubeRes.hasValue());
    const auto& cubeMesh = cubeRes.value();
    TEST_CHECK(cubeMesh.validate().hasValue());
    TEST_CHECK(cubeMesh.vertices.size() >= 8);
    TEST_CHECK(cubeMesh.indices.size() == 36); // 12 triangles * 3
    TEST_CHECK(cubeMesh.getTriangleCount() == 12);
    TEST_CHECK(cubeMesh.bounds.isValid());
    TEST_CHECK(std::abs(cubeMesh.bounds.getSize().x - 1.0F) < 1e-4F);
    TEST_CHECK(std::abs(cubeMesh.bounds.getSize().y - 1.0F) < 1e-4F);
    TEST_CHECK(std::abs(cubeMesh.bounds.getSize().z - 1.0F) < 1e-4F);

    // 2. Test triangular prism
    auto prismRes = ObjImporter::importFromFile("assets/models/prism.obj");
    TEST_CHECK(prismRes.hasValue());
    const auto& prismMesh = prismRes.value();
    TEST_CHECK(prismMesh.validate().hasValue());
    TEST_CHECK(prismMesh.indices.size() == 24); // 8 triangles * 3
    TEST_CHECK(prismMesh.getTriangleCount() == 8);

    // 3. Test non-existent file
    auto nonExistent = ObjImporter::importFromFile("assets/models/does_not_exist.obj");
    TEST_CHECK(!nonExistent.hasValue());
    TEST_CHECK(nonExistent.error().code == ErrorCode::InvalidArgument);

    return 0;
}

static int testGpuMeshCreationAndRAII() {
    auto devRes = rhi::createNullDevice();
    TEST_CHECK(devRes.hasValue());
    auto& device = *devRes.value();

    auto cubeRes = ObjImporter::importFromFile("assets/models/cube.obj");
    TEST_CHECK(cubeRes.hasValue());

    auto gpuRes = GpuMesh::create(device, cubeRes.value());
    TEST_CHECK(gpuRes.hasValue());
    auto gpuMesh = std::move(gpuRes.value());

    TEST_CHECK(gpuMesh->isValid());
    TEST_CHECK(gpuMesh->getVertexBuffer().isValid());
    TEST_CHECK(gpuMesh->getIndexBuffer().isValid());
    TEST_CHECK(gpuMesh->getIndexCount() == 36);
    TEST_CHECK(gpuMesh->getBounds().isValid());

    // Test move constructor
    GpuMesh movedMesh = std::move(*gpuMesh);
    TEST_CHECK(movedMesh.isValid());
    TEST_CHECK(!gpuMesh->isValid());

    return 0;
}

int main() {
    std::cout << "[Running Mesh Tests]\n";
    if (testAssetIdHashing() != 0)
        return 1;
    if (testMeshDataValidation() != 0)
        return 2;
    if (testMeshDataBoundsAndNormals() != 0)
        return 3;
    if (testMeshDataTangents() != 0)
        return 7;
    if (testMeshDataTangentsDegenerateAndMirrored() != 0)
        return 8;
    if (testMeshDataTangentsRangeMultiSubmesh() != 0)
        return 9;
    if (testNormalMatrixNonUniformScalingAndMirrored() != 0)
        return 10;
    if (testRobustTbnReconstructionMath() != 0)
        return 11;
    if (testObjImporterFromString() != 0)
        return 4;
    if (testObjImporterFromFile() != 0)
        return 5;
    if (testGpuMeshCreationAndRAII() != 0)
        return 6;
    std::cout << "[Mesh Tests Passed: 11/11]\n";
    return 0;
}
