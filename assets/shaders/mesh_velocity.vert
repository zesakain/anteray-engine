#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 0) out vec4 outCurClip;
layout(location = 1) out vec4 outPrevClip;

layout(push_constant) uniform MeshVelocityPushConstants {
    mat4 curMvp;     // unjitteredViewProj * curModel
    mat4 prevMvp;    // unjitteredPrevViewProj * prevModel
    uint isValid;
    float pad0, pad1, pad2;
} pc;

void main() {
    vec4 curClip = pc.curMvp * vec4(inPosition, 1.0);
    vec4 prevClip = pc.prevMvp * vec4(inPosition, 1.0);
    gl_Position = curClip;
    outCurClip = curClip;
    outPrevClip = prevClip;
}
