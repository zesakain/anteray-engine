#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4 inWeights;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

layout(std140, set = 1, binding = 0) uniform LightingData {
    mat4 cascadeLightSpaceMatrices[4];
} uLighting;

layout(std140, set = 2, binding = 0) uniform SkinningData {
    uint boneCount;
    uint pad0;
    uint pad1;
    uint pad2;
    mat4 bones[128];
} uSkin;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec4 outTangent;
layout(location = 5) out vec3 outShadowCoord;

mat4 computeSkinMatrix() {
    if (uSkin.boneCount == 0u) {
        return mat4(1.0);
    }
    float wSum = inWeights.x + inWeights.y + inWeights.z + inWeights.w;
    if (wSum < 1e-4) {
        return mat4(1.0);
    }
    vec4 w = inWeights / wSum;
    uint maxIdx = uSkin.boneCount - 1u;
    return w.x * uSkin.bones[min(inJoints.x, maxIdx)] +
           w.y * uSkin.bones[min(inJoints.y, maxIdx)] +
           w.z * uSkin.bones[min(inJoints.z, maxIdx)] +
           w.w * uSkin.bones[min(inJoints.w, maxIdx)];
}

void main() {
    mat4 skinMat = computeSkinMatrix();
    vec4 skinnedPos = skinMat * vec4(inPosition, 1.0);
    mat3 skinRot = mat3(skinMat);
    vec3 skinnedNormal = skinRot * inNormal;
    vec3 skinnedTangent = skinRot * inTangent.xyz;

    gl_Position = pc.mvp * skinnedPos;
    vec4 worldPos = pc.model * skinnedPos;

    mat3 m = mat3(pc.model);
    float det = determinant(m);
    mat3 normalMatrix = (abs(det) > 1e-7) ? transpose(inverse(m)) : m;

    outWorldPos = worldPos.xyz;
    vec3 worldNorm = normalMatrix * skinnedNormal;
    float normLen = length(worldNorm);
    outNormal = (normLen > 1e-6) ? (worldNorm / normLen) : vec3(0.0, 1.0, 0.0);
    outUV = inUV;
    outColor = inColor;

    // Transform surface tangent vectors via model linear transform
    vec3 worldTan = m * skinnedTangent;
    float tanLen = length(worldTan);
    outTangent = vec4(tanLen > 1e-6 ? (worldTan / tanLen) : vec3(1.0, 0.0, 0.0),
                      inTangent.w * (det < 0.0 ? -1.0 : 1.0));

    // Compute light-space coordinates for primary directional cascade
    vec4 lpos = uLighting.cascadeLightSpaceMatrices[0] * worldPos;
    outShadowCoord = lpos.xyz;
}
