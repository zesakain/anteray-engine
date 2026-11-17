#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4 inWeights;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
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
    gl_Position = pc.mvp * pos;
}

