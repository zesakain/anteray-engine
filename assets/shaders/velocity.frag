#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outVelocity;

layout(set = 0, binding = 0) uniform sampler2D uDepthMap;

layout(push_constant) uniform VelocityPushConstants {
    mat4 unjitteredInvViewProj;
    mat4 unjitteredPrevViewProj;
    vec2 resolution;
    vec2 invResolution;
    uint hasHistory;
    float pad0;
    float pad1;
    float pad2;
} pc;

void main() {
    float depth = texture(uDepthMap, inUV).r;
    if (depth >= 1.0 || pc.hasHistory == 0u) {
        outVelocity = vec4(0.0, 0.0, depth, 0.0);
        return;
    }

    vec4 ndc = vec4(inUV.x * 2.0 - 1.0, inUV.y * 2.0 - 1.0, depth, 1.0);
    vec4 worldPos = pc.unjitteredInvViewProj * ndc;
    worldPos /= worldPos.w;

    vec4 prevClip = pc.unjitteredPrevViewProj * worldPos;
    if (prevClip.w <= 1e-5) {
        outVelocity = vec4(0.0, 0.0, depth, 0.0);
        return;
    }

    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    vec2 velocity = inUV - prevUV;
    outVelocity = vec4(velocity, depth, 1.0);
}
