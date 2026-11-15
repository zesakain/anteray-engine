#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTexture;

layout(push_constant) uniform BloomDownsamplePushConstants {
    vec2 texelSize;
    float threshold;
    float knee;
    int isFirstPass;
} pc;

vec3 extractThreshold(vec3 c, float thresh, float kn) {
    float brightness = max(c.r, max(c.g, c.b));
    float soft = brightness - thresh + kn;
    soft = clamp(soft, 0.0, 2.0 * kn);
    soft = (soft * soft) / (4.0 * kn + 1e-4);
    float contribution = max(soft, brightness - thresh);
    contribution /= max(brightness, 1e-4);
    return c * clamp(contribution, 0.0, 1.0);
}

float karisWeight(vec3 c) {
    float luma = dot(max(c, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722));
    return 1.0 / (1.0 + luma);
}

void main() {
    vec2 d = pc.texelSize;

    // 13-tap tent filter pattern (Jimenez 2014)
    vec3 a = texture(srcTexture, inUV + vec2(-2.0,  2.0) * d).rgb;
    vec3 b = texture(srcTexture, inUV + vec2( 0.0,  2.0) * d).rgb;
    vec3 c = texture(srcTexture, inUV + vec2( 2.0,  2.0) * d).rgb;

    vec3 d0 = texture(srcTexture, inUV + vec2(-2.0,  0.0) * d).rgb;
    vec3 e  = texture(srcTexture, inUV + vec2( 0.0,  0.0) * d).rgb;
    vec3 f  = texture(srcTexture, inUV + vec2( 2.0,  0.0) * d).rgb;

    vec3 g = texture(srcTexture, inUV + vec2(-2.0, -2.0) * d).rgb;
    vec3 h = texture(srcTexture, inUV + vec2( 0.0, -2.0) * d).rgb;
    vec3 i = texture(srcTexture, inUV + vec2( 2.0, -2.0) * d).rgb;

    vec3 j = texture(srcTexture, inUV + vec2(-1.0,  1.0) * d).rgb;
    vec3 k = texture(srcTexture, inUV + vec2( 1.0,  1.0) * d).rgb;
    vec3 l = texture(srcTexture, inUV + vec2(-1.0, -1.0) * d).rgb;
    vec3 m = texture(srcTexture, inUV + vec2( 1.0, -1.0) * d).rgb;

    vec3 color = vec3(0.0);
    if (pc.isFirstPass != 0) {
        // Karis average over the 5 overlapping 2x2 groups (Jimenez 2014) to suppress
        // subpixel specular fireflies from washing out the bloom pyramid.
        vec3 g0 = (a + b + d0 + e) * 0.25;
        vec3 g1 = (b + c + e + f) * 0.25;
        vec3 g2 = (d0 + e + g + h) * 0.25;
        vec3 g3 = (e + f + h + i) * 0.25;
        vec3 g4 = (j + k + l + m) * 0.25;

        float w0 = karisWeight(g0) * 0.125;
        float w1 = karisWeight(g1) * 0.125;
        float w2 = karisWeight(g2) * 0.125;
        float w3 = karisWeight(g3) * 0.125;
        float w4 = karisWeight(g4) * 0.5;

        float weightSum = max(w0 + w1 + w2 + w3 + w4, 1e-5);
        color = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / weightSum;
        color = max(color, vec3(0.0));
        color = extractThreshold(color, pc.threshold, pc.knee);
    } else {
        color = e * 0.125;
        color += (a + c + g + i) * 0.03125;
        color += (b + d0 + f + h) * 0.0625;
        color += (j + k + l + m) * 0.125;
        color = max(color, vec3(0.0));
    }

    outColor = vec4(color, 1.0);
}
