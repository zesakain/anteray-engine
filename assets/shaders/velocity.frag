#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outVelocity;

layout(set = 0, binding = 0) uniform sampler2D uDepthMap;

layout(push_constant) uniform VelocityPushConstants {
    mat4 reprojectMatrix;
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

    // In Vulkan Y-up NDC:
    // NDC.x in [-1, 1], NDC.y in [-1, 1] (+1 is top, -1 is bottom)
    // UV.x in [0, 1], UV.y in [0, 1] (0 is top, 1 is bottom)
    vec4 ndc = vec4(inUV.x * 2.0 - 1.0, 1.0 - inUV.y * 2.0, depth, 1.0);
    vec4 prevClip = pc.reprojectMatrix * ndc;
    if (prevClip.w <= 1e-5) {
        outVelocity = vec4(0.0, 0.0, depth, 0.0);
        return;
    }

    vec2 prevNDC = prevClip.xy / prevClip.w;
    vec2 prevUV = vec2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5);
    vec2 velocity = inUV - prevUV;
    outVelocity = vec4(velocity, depth, 1.0);
}
