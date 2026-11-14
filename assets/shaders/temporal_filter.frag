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
        outColor = vec4(current.rgb, centerDepth);
        return;
    }

    vec4 ndc = vec4(inUV * 2.0 - 1.0, centerDepth, 1.0);
    vec4 prevClip = pc.reprojectMat * ndc;
    if (prevClip.w <= 0.001) {
        outColor = vec4(current.rgb, centerDepth);
        return;
    }

    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;

    // Disocclusion / off-screen test
    if (prevUV.x < 0.005 || prevUV.x > 0.995 || prevUV.y < 0.005 || prevUV.y > 0.995) {
        outColor = vec4(current.rgb, centerDepth);
        return;
    }

    // Temporal depth rejection (reject history when reprojected surface depth differs from stored history)
    vec4 history = texture(uHistoryMap, prevUV);
    float reprojectedZ = prevClip.z / prevClip.w;
    float depthThreshold = max(pc.filterParams.y, 0.01) * (1.0 + centerDepth * 2.0);
    if (abs(history.a - reprojectedZ) > depthThreshold) {
        outColor = vec4(current.rgb, centerDepth);
        return;
    }

    // 3x3 neighborhood color clamping to prevent ghosting and trailing artifacts under motion
    vec3 boxMin = current.rgb;
    vec3 boxMax = current.rgb;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = inUV + vec2(float(x), float(y)) * pc.invResolution;
            vec3 val = texture(uCurrentMap, sampleUV).rgb;
            boxMin = min(boxMin, val);
            boxMax = max(boxMax, val);
        }
    }

    vec3 clampedHistory = clamp(history.rgb, boxMin, boxMax);
    float alpha = clamp(pc.filterParams.x, 0.05, 1.0);
    outColor = vec4(mix(clampedHistory, current.rgb, alpha), centerDepth);
}
