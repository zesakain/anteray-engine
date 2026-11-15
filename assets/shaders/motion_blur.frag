#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrSceneTexture;
layout(set = 0, binding = 1) uniform sampler2D velocityTexture;
layout(set = 0, binding = 2) uniform sampler2D depthTexture;

layout(push_constant) uniform MotionBlurPushConstants {
    vec2 texelSize;
    float intensity;
    int sampleCount;
    float maxVelocityPixels;
    float zNear;
    float zFar;
    float pad0;
} pc;

float linearizeDepth(float d) {
    float z = clamp(d, 0.0, 1.0);
    return (pc.zNear * pc.zFar) / max(pc.zFar - z * (pc.zFar - pc.zNear), 1e-5);
}

void main() {
    vec4 centerSample = texture(hdrSceneTexture, inUV);
    if (pc.intensity <= 0.0 || pc.sampleCount <= 1) {
        outColor = centerSample;
        return;
    }

    // Velocity buffer stores UV delta (currentUV - prevUV)
    vec2 rawVelocityUv = texture(velocityTexture, inUV).rg * pc.intensity;
    vec2 velocityPixels = rawVelocityUv / max(pc.texelSize, vec2(1e-6));
    float speedPixels = length(velocityPixels);

    // Skip blur when pixel motion is sub-pixel
    if (speedPixels < 0.5) {
        outColor = centerSample;
        return;
    }

    // Clamp maximum blur length in pixels to prevent excessive smearing on fast camera cuts
    float maxPixels = max(pc.maxVelocityPixels, 1.0);
    vec2 clampedVelocityUv = rawVelocityUv * min(1.0, maxPixels / speedPixels);

    float centerDepth = linearizeDepth(texture(depthTexture, inUV).r);
    int taps = clamp(pc.sampleCount, 2, 16);

    vec3 accumColor = centerSample.rgb;
    float accumWeight = 1.0;

    for (int i = 0; i < taps; ++i) {
        float t = (float(i) + 0.5) / float(taps) - 0.5;
        vec2 sampleUv = clamp(inUV + clampedVelocityUv * t, vec2(0.001), vec2(0.999));

        vec3 sampleColor = texture(hdrSceneTexture, sampleUv).rgb;
        float sampleDepth = linearizeDepth(texture(depthTexture, sampleUv).r);
        vec2 sampleVelPixels = (texture(velocityTexture, sampleUv).rg * pc.intensity) / max(pc.texelSize, vec2(1e-6));
        float sampleSpeed = length(sampleVelPixels);

        // Bilateral depth & velocity weight (McGuire et al.):
        // Prevents stationary foreground silhouettes from smearing into moving backgrounds.
        float isForeground = step(sampleDepth, centerDepth + 0.25);
        float movingSampleWeight = clamp(sampleSpeed / max(speedPixels, 1.0), 0.15, 1.0);
        float w = mix(1.0, movingSampleWeight, isForeground);

        accumColor += sampleColor * w;
        accumWeight += w;
    }

    outColor = vec4(accumColor / max(accumWeight, 1e-4), centerSample.a);
}
