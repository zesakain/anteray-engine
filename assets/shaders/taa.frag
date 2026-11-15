#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uCurrentColorMap;
layout(set = 0, binding = 1) uniform sampler2D uHistoryColorMap;
layout(set = 0, binding = 2) uniform sampler2D uVelocityMap;
layout(set = 0, binding = 3) uniform sampler2D uDepthMap;

layout(push_constant) uniform TaaPushConstants {
    vec4 taaParams; // x: historyWeight (0.90), y: varianceGamma (1.25), z: pad, w: pad
    vec2 resolution;
    vec2 invResolution;
    uint isHistoryValid;
    uint useMotionVectors;
    float pad0, pad1;
} pc;

vec3 rgbToYCoCg(vec3 c) {
    return vec3(
        0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
        0.5 * c.r - 0.5 * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b
    );
}

vec3 yCoCgToRgb(vec3 c) {
    return vec3(
        c.x + c.y - c.z,
        c.x + c.z,
        c.x - c.y - c.z
    );
}

void main() {
    vec3 currentRgb = texture(uCurrentColorMap, inUV).rgb;
    float centerDepth = texture(uDepthMap, inUV).r;

    // If history is globally invalid, pass current frame directly
    if (pc.isHistoryValid == 0u) {
        outColor = vec4(currentRgb, 1.0);
        return;
    }

    // 1. Determine velocity and reprojected UV
    vec2 velocity = vec2(0.0);
    float confidence = 1.0;
    if (pc.useMotionVectors != 0u) {
        vec4 velSample = texture(uVelocityMap, inUV);
        velocity = velSample.xy;
        confidence = velSample.w;
    }

    vec2 prevUV = inUV - velocity;

    // 2. History validation & rejection
    // Rejection test 1: Screen boundary
    if (prevUV.x < 0.0 || prevUV.x > 1.0 || prevUV.y < 0.0 || prevUV.y > 1.0 || confidence < 0.5) {
        outColor = vec4(currentRgb, 1.0);
        return;
    }

    // Rejection test 2: Depth disocclusion check
    float historyDepth = texture(uVelocityMap, prevUV).z;
    if (abs(historyDepth - centerDepth) > 0.1 * (1.0 + centerDepth * 2.0)) {
        confidence *= 0.5;
    }

    // 3. 3x3 Neighborhood in YCoCg space with Karis tone weighting
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);
    float totalWeight = 0.0;
    vec3 boxMin = vec3(1e6);
    vec3 boxMax = vec3(-1e6);

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = inUV + vec2(float(x), float(y)) * pc.invResolution;
            vec3 sampleRgb = texture(uCurrentColorMap, sampleUV).rgb;
            vec3 sampleYCoCg = rgbToYCoCg(sampleRgb);

            // Karis tone weighting to dampen high-contrast fireflies
            float w = 1.0 / (1.0 + max(sampleYCoCg.x, 0.0));
            m1 += sampleYCoCg * w;
            m2 += sampleYCoCg * sampleYCoCg * w;
            totalWeight += w;

            boxMin = min(boxMin, sampleYCoCg);
            boxMax = max(boxMax, sampleYCoCg);
        }
    }

    vec3 mu = m1 / max(totalWeight, 1e-5);
    vec3 sigma = sqrt(max(m2 / max(totalWeight, 1e-5) - mu * mu, vec3(0.0)));
    float gamma = max(pc.taaParams.y, 0.5);

    vec3 clipMin = max(boxMin, mu - gamma * sigma);
    vec3 clipMax = min(boxMax, mu + gamma * sigma);

    // 4. Sample history, clip in YCoCg, and convert back to RGB
    vec3 historyRgb = texture(uHistoryColorMap, prevUV).rgb;
    vec3 historyYCoCg = rgbToYCoCg(historyRgb);
    vec3 clampedHistoryYCoCg = clamp(historyYCoCg, clipMin, clipMax);
    vec3 clampedHistoryRgb = yCoCgToRgb(clampedHistoryYCoCg);

    // Clamp to avoid negative values
    clampedHistoryRgb = max(clampedHistoryRgb, vec3(0.0));

    // 5. Exponential History Accumulation
    float baseAlpha = clamp(1.0 - pc.taaParams.x, 0.02, 0.50);
    float alpha = mix(1.0, baseAlpha, confidence);

    vec3 resolved = mix(clampedHistoryRgb, currentRgb, alpha);
    outColor = vec4(resolved, 1.0);
}
