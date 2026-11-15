#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrSceneTexture;
layout(set = 0, binding = 1) uniform sampler2D depthTexture;

layout(push_constant) uniform DofPushConstants {
    vec2 texelSize;
    float focusDistance;
    float focalLengthMm;
    float aperture;
    float maxBlurRadius;
    float zNear;
    float zFar;
} pc;

// Linearize standard Vulkan [0, 1] perspective depth to view-space distance in meters
float linearizeDepth(float d) {
    float z = clamp(d, 0.0, 1.0);
    return (pc.zNear * pc.zFar) / max(pc.zFar - z * (pc.zFar - pc.zNear), 1e-5);
}

// Compute signed Circle of Confusion (CoC) in pixels:
// negative = foreground (closer than focus plane), positive = background (farther than focus plane).
float computeSignedCoc(float linearDepth) {
    // 35mm full-frame vertical sensor height = 24.0mm
    const float sensorHeightMm = 24.0;
    float f = max(pc.focalLengthMm * 0.001, 0.001);
    float focusDist = max(pc.focusDistance, f + 0.01);
    float fStop = max(pc.aperture, 0.5);

    // Thin-lens CoC fraction of sensor height
    float cocFraction = (f * f) / (fStop * (focusDist - f) * (sensorHeightMm * 0.001));
    float signedFactor = (linearDepth - focusDist) / max(linearDepth, 0.05);
    float referenceHeight = 1.0 / max(pc.texelSize.y, 1e-5);
    float cocPixels = cocFraction * signedFactor * referenceHeight;
    return clamp(cocPixels, -pc.maxBlurRadius, pc.maxBlurRadius);
}

const vec2 kVogelDisk[16] = vec2[](
    vec2( 0.176777,  0.000000),
    vec2(-0.226196,  0.194565),
    vec2( 0.030645, -0.382199),
    vec2( 0.304664,  0.343536),
    vec2(-0.506784, -0.066510),
    vec2( 0.427254, -0.371261),
    vec2(-0.072579,  0.606198),
    vec2(-0.341368, -0.558290),
    vec2( 0.686993,  0.116036),
    vec2(-0.665645,  0.325782),
    vec2( 0.254442, -0.733610),
    vec2( 0.391776,  0.706819),
    vec2(-0.818090, -0.220213),
    vec2( 0.785167, -0.398229),
    vec2(-0.258700,  0.875842),
    vec2(-0.464896, -0.824055)
);

void main() {
    vec4 centerSample = texture(hdrSceneTexture, inUV);
    float rawDepth = texture(depthTexture, inUV).r;

    // Skybox / unrendered far plane guard
    if (rawDepth >= 0.99999) {
        rawDepth = 0.9999;
    }

    float centerDepth = linearizeDepth(rawDepth);
    float centerCoc = computeSignedCoc(centerDepth);
    float absCenterCoc = abs(centerCoc);

    // Fast path for in-focus pixels (check only if no foreground bleeding nearby)
    if (absCenterCoc < 0.5) {
        outColor = centerSample;
        return;
    }

    vec3 accumColor = centerSample.rgb;
    float accumWeight = 1.0;

    for (int i = 0; i < 16; ++i) {
        vec2 sampleUv = clamp(inUV + kVogelDisk[i] * absCenterCoc * pc.texelSize, vec2(0.001), vec2(0.999));
        vec3 sampleColor = texture(hdrSceneTexture, sampleUv).rgb;
        float sampleRawDepth = min(texture(depthTexture, sampleUv).r, 0.9999);
        float sampleDepth = linearizeDepth(sampleRawDepth);
        float sampleCoc = computeSignedCoc(sampleDepth);
        float absSampleCoc = abs(sampleCoc);

        // Depth discontinuity guard: prevent sharp closer objects from bleeding onto blurry backgrounds,
        // while allowing out-of-focus foreground bokeh to bleed naturally over background.
        float depthWeight = (sampleDepth < centerDepth)
                                ? clamp(absSampleCoc / max(absCenterCoc, 0.5), 0.0, 1.0)
                                : 1.0;
        // Karis-like highlight stabilization for smooth bokeh discs
        float luma = dot(max(sampleColor, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
        float highlightWeight = 1.0 / (1.0 + 0.15 * luma);
        float w = depthWeight * highlightWeight;

        accumColor += sampleColor * w;
        accumWeight += w;
    }

    vec3 blurred = accumColor / max(accumWeight, 1e-4);
    float blend = smoothstep(0.5, 1.5, absCenterCoc);
    outColor = vec4(mix(centerSample.rgb, blurred, blend), centerSample.a);
}
