#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4 inWeights;
layout(location = 0) out vec4 outCurClip;
layout(location = 1) out vec4 outPrevClip;

layout(push_constant) uniform MeshVelocityPushConstants {
    mat4 curMvp;     // unjitteredViewProj * curModel
    mat4 prevMvp;    // unjitteredPrevViewProj * prevModel
} pc;

layout(std140, set = 0, binding = 0) uniform SkinningData {
    uint boneCount;
    uint pad0;
    uint pad1;
    uint pad2;
    mat4 bones[128];
} uSkin;

void main() {
    vec4 pos = vec4(inPosition, 1.0);
    if (uSkin.boneCount > 0u) {
        float wSum = inWeights.x + inWeights.y + inWeights.z + inWeights.w;
        if (wSum >= 1e-4) {
            vec4 w = inWeights / wSum;
            uint maxIdx = uSkin.boneCount - 1u;
            mat4 skinMat = w.x * uSkin.bones[min(inJoints.x, maxIdx)] +
                           w.y * uSkin.bones[min(inJoints.y, maxIdx)] +
                           w.z * uSkin.bones[min(inJoints.z, maxIdx)] +
                           w.w * uSkin.bones[min(inJoints.w, maxIdx)];
            pos = skinMat * pos;
        }
    }
    vec4 curClip = pc.curMvp * pos;
    vec4 prevClip = pc.prevMvp * pos;
    gl_Position = curClip;
    outCurClip = curClip;
    outPrevClip = prevClip;
}
