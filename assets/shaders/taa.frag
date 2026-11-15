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

// 5-tap Catmull-Rom filtering with bilinear texture fetches to eliminate temporal blur
vec3 sampleHistoryCatmullRom(sampler2D tex, vec2 uv, vec2 invRes) {
    vec2 samplePos = uv / invRes;
    vec2 tc = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - tc;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;

    vec2 w0 = f2 - 0.5 * (f3 + f);
    vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    vec2 w3 = 0.5 * (f3 - f2);
    vec2 w2 = 1.0 - w0 - w1 - w3;

    vec2 w12 = w1 + w2;
    vec2 tc12 = tc + (w2 / max(w12, vec2(1e-5)));

    vec2 tc0 = tc - 1.0;
    vec2 tc3 = tc + 2.0;

    vec3 c12 = texture(tex, vec2(tc12.x, tc12.y) * invRes).rgb;
    vec3 c0  = texture(tex, vec2(tc12.x, tc0.y) * invRes).rgb;
    vec3 c1  = texture(tex, vec2(tc0.x, tc12.y) * invRes).rgb;
    vec3 c2  = texture(tex, vec2(tc3.x, tc12.y) * invRes).rgb;
    vec3 c3  = texture(tex, vec2(tc12.x, tc3.y) * invRes).rgb;

    float weight12 = w12.x * w12.y;
    float weight0 = w12.x * w0.y;
    float weight1 = w0.x * w12.y;
    float weight2 = w3.x * w12.y;
    float weight3 = w12.x * w3.y;

    vec3 color = c12 * weight12 + c0 * weight0 + c1 * weight1 + c2 * weight2 + c3 * weight3;
    float totalWeight = weight12 + weight0 + weight1 + weight2 + weight3;
    return max(color / max(totalWeight, 1e-5), vec3(0.0));
}

void main() {
    vec3 currentRgb = texture(uCurrentColorMap, inUV).rgb;
    float centerDepth = texture(uDepthMap, inUV).r;

    // If history is globally invalid, pass current frame directly
    if (pc.isHistoryValid == 0u) {
        outColor = vec4(currentRgb, 1.0);
        return;
    }

    // 1. Determine velocity using 3x3 closest depth dilation (prevents edge silhouette ghosting)
    vec2 velocity = vec2(0.0);
    float confidence = 1.0;
    if (pc.useMotionVectors != 0u) {
        float closestDepth = centerDepth;
        vec2 bestUV = inUV;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                vec2 sampleUV = inUV + vec2(float(dx), float(dy)) * pc.invResolution;
                float d = texture(uDepthMap, sampleUV).r;
                // Vulkan depth range [0, 1]: smaller depth is closer to camera
                if (d < closestDepth) {
                    closestDepth = d;
                    bestUV = sampleUV;
                }
            }
        }
        vec4 velSample = texture(uVelocityMap, bestUV);
        velocity = velSample.xy;
        confidence = velSample.w;
    }

    vec2 prevUV = inUV - velocity;

    // 2. Screen boundary validation
    if (prevUV.x < 0.0 || prevUV.x > 1.0 || prevUV.y < 0.0 || prevUV.y > 1.0 || confidence < 0.05) {
        outColor = vec4(currentRgb, 1.0);
        return;
    }

    // 3. 3x3 Neighborhood in YCoCg space for variance clipping
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);
    vec3 boxMin = vec3(1e6);
    vec3 boxMax = vec3(-1e6);

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = inUV + vec2(float(x), float(y)) * pc.invResolution;
            vec3 sampleRgb = texture(uCurrentColorMap, sampleUV).rgb;
            vec3 sampleYCoCg = rgbToYCoCg(sampleRgb);

            m1 += sampleYCoCg;
            m2 += sampleYCoCg * sampleYCoCg;

            boxMin = min(boxMin, sampleYCoCg);
            boxMax = max(boxMax, sampleYCoCg);
        }
    }

    vec3 mu = m1 * (1.0 / 9.0);
    vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, vec3(0.0)));
    float gamma = max(pc.taaParams.y, 0.75);

    vec3 clipMin = max(boxMin, mu - gamma * sigma);
    vec3 clipMax = min(boxMax, mu + gamma * sigma);

    // 4. Sample history with 5-tap Catmull-Rom filter, clip in YCoCg, and convert back to RGB
    vec3 historyRgb = sampleHistoryCatmullRom(uHistoryColorMap, prevUV, pc.invResolution);
    vec3 historyYCoCg = rgbToYCoCg(historyRgb);
    vec3 clampedHistoryYCoCg = clamp(historyYCoCg, clipMin, clipMax);
    vec3 clampedHistoryRgb = max(yCoCgToRgb(clampedHistoryYCoCg), vec3(0.0));

    // 5. Exponential History Accumulation
    float historyWeight = clamp(pc.taaParams.x, 0.80, 0.98);
    // Slight adaptive weight reduction on high pixel velocity to resolve rapid disocclusions faster
    float velPixels = length(velocity * pc.resolution);
    float dynamicWeight = mix(historyWeight, 0.82, clamp(velPixels * 0.05, 0.0, 0.5));
    float alpha = 1.0 - (dynamicWeight * confidence);
    alpha = clamp(alpha, 0.02, 1.0);

    vec3 resolved = mix(clampedHistoryRgb, currentRgb, alpha);
    outColor = vec4(resolved, 1.0);
}
