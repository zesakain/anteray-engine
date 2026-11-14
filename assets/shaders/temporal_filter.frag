#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uCurrentMap;
layout(set = 0, binding = 1) uniform sampler2D uHistoryMap;
layout(set = 0, binding = 2) uniform sampler2D uDepthMap;

layout(push_constant) uniform TemporalPushConstants {
    mat4 reprojectMat; // prevViewProj * invViewProj
    vec4 filterParams; // x: alpha (0.1F), y: depthThreshold (0.05F), zw: pad
    vec2 resolution;
    vec2 invResolution;
    uint isHistoryValid;
    float pad0;
} pc;

void main() {
    float centerDepth = texture(uDepthMap, inUV).r;
    vec4 current = texture(uCurrentMap, inUV);

    if (centerDepth >= 1.0 || pc.isHistoryValid == 0u) {
        outColor = current;
        return;
    }

    vec4 ndc = vec4(inUV * 2.0 - 1.0, centerDepth, 1.0);
    vec4 prevClip = pc.reprojectMat * ndc;
    if (prevClip.w <= 0.001) {
        outColor = current;
        return;
    }

    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;

    // Disocclusion / off-screen test
    if (prevUV.x < 0.005 || prevUV.x > 0.995 || prevUV.y < 0.005 || prevUV.y > 0.995) {
        outColor = current;
        return;
    }

    // 3x3 neighborhood color clamping to prevent ghosting and trailing artifacts
    vec4 boxMin = current;
    vec4 boxMax = current;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = inUV + vec2(float(x), float(y)) * pc.invResolution;
            vec4 val = texture(uCurrentMap, sampleUV);
            boxMin = min(boxMin, val);
            boxMax = max(boxMax, val);
        }
    }

    vec4 history = texture(uHistoryMap, prevUV);
    vec4 clampedHistory = clamp(history, boxMin, boxMax);

    float alpha = clamp(pc.filterParams.x, 0.05, 1.0);
    outColor = mix(clampedHistory, current, alpha);
}
