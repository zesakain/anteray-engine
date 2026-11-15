#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrSceneTexture;
layout(set = 0, binding = 1) uniform sampler2D bloomTexture;

layout(push_constant) uniform ToneMappingPushConstants {
    float exposure;
    int toneMapOp; // 0 = AgX, 1 = ACES, 2 = FilmicReinhard, 3 = Neutral, 4 = Bypass
    float bloomIntensity;
    int bloomEnabled;
    float whitePoint;
    float whiteBalance; // Color temperature in Kelvin (default 6500K)
    float contrast;     // Midtone contrast (default 1.0)
    float saturation;   // Saturation factor (default 1.0)
} pc;

vec3 colorTemperatureToRgb(float kelvin) {
    if (abs(kelvin - 6500.0) < 1.0) {
        return vec3(1.0);
    }
    float temp = clamp(kelvin, 1000.0, 12000.0) / 100.0;
    float r, g, b;

    if (temp <= 66.0) {
        r = 1.0;
        g = clamp(0.39008157876901960784 * log(temp) - 0.63184144378862745098, 0.0, 1.0);
    } else {
        float t = temp - 60.0;
        r = clamp(1.29293618606274509804 * pow(t, -0.1332047592), 0.0, 1.0);
        g = clamp(1.12989086089529411765 * pow(t, -0.0755148492), 0.0, 1.0);
    }

    if (temp >= 66.0) {
        b = 1.0;
    } else if (temp <= 19.0) {
        b = 0.0;
    } else {
        float t = temp - 10.0;
        b = clamp(0.54320678911019607843 * log(t) - 1.19625408914, 0.0, 1.0);
    }

    // Normalize against D65 standard reference (6500K)
    vec3 d65 = vec3(1.0, 0.9931, 0.9852);
    return vec3(r, g, b) / d65;
}

vec3 applyColorGrading(vec3 c, float contrast, float saturation) {
    c = max(vec3(0.0), c);
    if (abs(contrast - 1.0) > 0.001) {
        c = pow(c / 0.18, vec3(contrast)) * 0.18;
    }
    if (abs(saturation - 1.0) > 0.001) {
        float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = mix(vec3(luma), c, max(0.0, saturation));
    }
    return c;
}

// Troy Sobotka AgX tone mapping formulation
// Working color primaries inset matrix (converting linear sRGB to AgX log working space)
const mat3 AGX_INSET_MATRIX = mat3(
    vec3(0.842479062253094, 0.0423282422610123, 0.0423756549057051),
    vec3(0.0784335999999992, 0.878468646464646, 0.0784335999999992),
    vec3(0.0792237451977152, 0.0792237451977152, 0.879237451977152)
);

// AgX working space to sRGB display primaries outset matrix
const mat3 AGX_OUTSET_MATRIX = mat3(
    vec3(1.19687900512017, -0.0528968517574562, -0.0529716355178468),
    vec3(-0.0980208811401368, 1.15190312990417, -0.0980434501171801),
    vec3(-0.0990297440797205, -0.0990297440797205, 1.15107360136118)
);

// Dynamic range: min EV = -12.47393, max EV = 4.026069 (approx. 16.5 stops)
const float AGX_MIN_EV = -12.47393;
const float AGX_MAX_EV = 4.026069;

// AgX default contrast sigmoid approximation (6th order polynomial fit)
vec3 agxDefaultContrastApprox(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return + 15.5 * (x4 * x2)
           - 40.14 * (x4 * x)
           + 31.96 * x4
           - 6.868 * (x2 * x)
           + 0.4298 * x2
           + 0.1191 * x
           - 0.00232;
}

vec3 agxToneMapping(vec3 color) {
    // 1. Transform linear sRGB to AgX working primaries
    vec3 val = AGX_INSET_MATRIX * max(color, vec3(0.0));

    // Clamp floor to avoid log2(0) or negative values
    val = max(val, vec3(1e-10));

    // 2. Log2 encoding mapped to [0, 1] range across 16.5 stops
    val = clamp((log2(val) - AGX_MIN_EV) / (AGX_MAX_EV - AGX_MIN_EV), 0.0, 1.0);

    // 3. Apply AgX default contrast sigmoid
    val = clamp(agxDefaultContrastApprox(val), 0.0, 1.0);

    // 4. Transform back from AgX working primaries to target display primaries
    val = clamp(AGX_OUTSET_MATRIX * val, 0.0, 1.0);

    // 5. Decompress from 2.2 display response to linear HDR space.
    // This ensures color remains in linear photometric space prior to the pipeline's
    // final linearToSrgb() swapchain EOTF, strictly preventing double gamma encoding.
    val = pow(val, vec3(2.2));

    return val;
}

vec3 acesFitted(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 filmicReinhard(vec3 color, float white) {
    float w = max(white, 0.01);
    return clamp(color * (1.0 + color / (w * w)) / (1.0 + color), 0.0, 1.0);
}

// Khronos PBR Neutral Tone Mapper
vec3 neutralToneMapping(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return clamp(color, 0.0, 1.0);

    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return clamp(mix(color, vec3(newPeak), g), 0.0, 1.0);
}

vec3 linearBypass(vec3 color) {
    return clamp(color, 0.0, 1.0);
}

float linearToSrgb(float x) {
    return x <= 0.0031308 ? 12.92 * x : 1.055 * pow(max(x, 0.0), 1.0 / 2.4) - 0.055;
}

vec3 linearToSrgb(vec3 c) {
    return vec3(linearToSrgb(c.r), linearToSrgb(c.g), linearToSrgb(c.b));
}

void main() {
    vec4 sceneSample = texture(hdrSceneTexture, inUV);
    vec3 color = sceneSample.rgb;

    if (pc.bloomEnabled != 0 && pc.bloomIntensity > 0.0) {
        vec3 bloom = texture(bloomTexture, inUV).rgb;
        color += bloom * pc.bloomIntensity;
    }

    // Apply photometric linear exposure scale and chromatic white balance
    color *= max(pc.exposure, 0.0);
    color *= colorTemperatureToRgb(pc.whiteBalance);

    // Apply color grading (contrast & saturation in linear HDR space)
    color = applyColorGrading(color, pc.contrast, pc.saturation);

    // Apply tone mapping operator
    vec3 mappedColor = color;
    if (pc.toneMapOp == 0) {
        mappedColor = agxToneMapping(color);
    } else if (pc.toneMapOp == 1) {
        mappedColor = acesFitted(color);
    } else if (pc.toneMapOp == 2) {
        mappedColor = filmicReinhard(color, pc.whitePoint);
    } else if (pc.toneMapOp == 3) {
        mappedColor = neutralToneMapping(color);
    } else {
        mappedColor = linearBypass(color);
    }

    // Apply sRGB transfer curve for standard UNORM swapchain
    vec3 srgbColor = clamp(linearToSrgb(mappedColor), 0.0, 1.0);

    outColor = vec4(srgbColor, sceneSample.a);
}
