#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uSourceMap;
layout(set = 0, binding = 1) uniform sampler2D uDepthMap;

layout(push_constant) uniform BlurPushConstants {
    vec2 blurDirection;
    vec2 invResolution;
    float depthThreshold;
    float pad0;
} pc;

const float kWeights[4] = {0.382928, 0.241732, 0.060598, 0.005977};

void main() {
    float centerDepth = texture(uDepthMap, inUV).r;
    vec4 centerVal = texture(uSourceMap, inUV);

    if (centerDepth >= 1.0) {
        outColor = centerVal;
        return;
    }

    vec4 accum = centerVal * kWeights[0];
    float totalWeight = kWeights[0];
    float sigmaDepth = max(pc.depthThreshold, 0.001);

    for (int i = 1; i <= 3; ++i) {
        vec2 offset = pc.blurDirection * (float(i) * pc.invResolution);

        // Positive sample
        vec2 uvPos = inUV + offset;
        float depthPos = texture(uDepthMap, uvPos).r;
        float diffPos = abs(depthPos - centerDepth);
        float wDepthPos = exp(-(diffPos * diffPos) / (2.0 * sigmaDepth * sigmaDepth));
        float wPos = kWeights[i] * wDepthPos;
        accum += texture(uSourceMap, uvPos) * wPos;
        totalWeight += wPos;

        // Negative sample
        vec2 uvNeg = inUV - offset;
        float depthNeg = texture(uDepthMap, uvNeg).r;
        float diffNeg = abs(depthNeg - centerDepth);
        float wDepthNeg = exp(-(diffNeg * diffNeg) / (2.0 * sigmaDepth * sigmaDepth));
        float wNeg = kWeights[i] * wDepthNeg;
        accum += texture(uSourceMap, uvNeg) * wNeg;
        totalWeight += wNeg;
    }

    outColor = accum / max(totalWeight, 1e-5);
}
