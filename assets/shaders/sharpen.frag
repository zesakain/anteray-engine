#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uInputMap;

layout(push_constant) uniform SharpenPushConstants {
    vec2 invResolution;
    float intensity; // 0.0 = neutral/off, up to 1.0
    float pad0;
} pc;

void main() {
    vec3 c = texture(uInputMap, inUV).rgb;
    if (pc.intensity <= 0.001) {
        outColor = vec4(c, 1.0);
        return;
    }

    vec3 n = texture(uInputMap, inUV + vec2(0.0, -pc.invResolution.y)).rgb;
    vec3 s = texture(uInputMap, inUV + vec2(0.0, pc.invResolution.y)).rgb;
    vec3 w = texture(uInputMap, inUV + vec2(-pc.invResolution.x, 0.0)).rgb;
    vec3 e = texture(uInputMap, inUV + vec2(pc.invResolution.x, 0.0)).rgb;

    vec3 minColor = min(c, min(min(n, s), min(w, e)));
    vec3 maxColor = max(c, max(max(n, s), max(w, e)));

    // Contrast-adaptive cross filter
    vec3 delta = (4.0 * c) - (n + s + w + e);
    vec3 sharpened = c + delta * (pc.intensity * 0.25);

    // Limit overshoot to prevent edge ringing
    sharpened = clamp(sharpened, minColor, maxColor);
    outColor = vec4(sharpened, 1.0);
}
