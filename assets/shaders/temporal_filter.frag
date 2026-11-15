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

vec3 reconstructNormal(vec2 uv) {
    vec2 texel = pc.invResolution;
    float dR = texture(uDepthMap, uv + vec2(texel.x, 0.0)).r;
    float dL = texture(uDepthMap, uv - vec2(texel.x, 0.0)).r;
    float dU = texture(uDepthMap, uv + vec2(0.0, texel.y)).r;
    float dD = texture(uDepthMap, uv - vec2(0.0, texel.y)).r;
    vec3 dX = vec3(2.0 * texel.x, 0.0, dR - dL);
    vec3 dY = vec3(0.0, 2.0 * texel.y, dU - dD);
    vec3 n = cross(dX, dY);
    return (dot(n, n) > 1e-8) ? normalize(n) : vec3(0.0, 0.0, 1.0);
}

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

    // Milestone 11G: Normal-based history rejection across silhouettes
    vec3 curN = reconstructNormal(inUV);
    vec3 histN = reconstructNormal(prevUV);
    if (dot(curN, histN) < 0.707) {
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

    // Milestone 11G: 3x3 neighborhood variance clipping & box clamping for ghosting suppression
    vec3 m1 = vec3(0.0);
    vec3 m2 = vec3(0.0);
    vec3 boxMin = current.rgb;
    vec3 boxMax = current.rgb;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = inUV + vec2(float(x), float(y)) * pc.invResolution;
            vec3 val = texture(uCurrentMap, sampleUV).rgb;
            m1 += val;
            m2 += val * val;
            boxMin = min(boxMin, val);
            boxMax = max(boxMax, val);
        }
    }

    vec3 mu = m1 * (1.0 / 9.0);
    vec3 sigma = sqrt(max(m2 * (1.0 / 9.0) - mu * mu, vec3(0.0)));
    float gamma = 1.25;
    vec3 clipMin = max(boxMin, mu - gamma * sigma);
    vec3 clipMax = min(boxMax, mu + gamma * sigma);

    vec3 clampedHistory = clamp(history.rgb, clipMin, clipMax);
    float alpha = clamp(pc.filterParams.x, 0.05, 1.0);
    outColor = vec4(mix(clampedHistory, current.rgb, alpha), centerDepth);
}
