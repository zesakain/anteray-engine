#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrSceneTexture;
layout(set = 0, binding = 1) uniform sampler2D bloomTexture;
layout(set = 0, binding = 2) uniform sampler2D colorGradingLutTexture;

layout(push_constant) uniform ToneMappingPushConstants {
    float exposure;
    int toneMapOp; // 0 = AgX, 1 = ACES, 2 = FilmicReinhard, 3 = Neutral, 4 = Bypass
    float bloomIntensity;
    int bloomEnabled;
    float whitePoint;
    float whiteBalance; // Color temperature in Kelvin (default 6500K)
    float contrast;     // Midtone contrast (default 1.0)
    float saturation;   // Saturation factor (default 1.0)
    float lutIntensity;       // 3D LUT blend weight [0, 1]
    float vignetteIntensity;  // Vignette darkening strength [0, 1]
    float vignetteSmoothness; // Vignette transition feathering
    float vignetteRoundness;  // Vignette circularity factor
    float filmGrainIntensity; // Film grain amplitude in display space
    uint frameIndex;          // Temporal frame index for film grain
    float aspectRatio;        // Viewport width / height for vignette roundness
    float pad0;
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

    // 3. Apply AgX default contrast sigmoid (outputs display-encoded ~gamma 2.2 space)
    val = clamp(agxDefaultContrastApprox(val), 0.0, 1.0);

    // 4. Calibrated AgX Punchy look (ASC-CDL):
    // Slope, power, and saturation calibrated to eliminate the gray veil, deliver
    // OLED-grade deep blacks, rich vibrant color separation, and natural highlight roll-off.
    const vec3 slope = vec3(1.15);
    const vec3 power = vec3(1.35);
    const float sat = 1.30;
    val = pow(clamp(val * slope, 0.0, 1.0), power);
    float luma = dot(val, vec3(0.2126, 0.7152, 0.0722));
    val = clamp(luma + sat * (val - luma), 0.0, 1.0);

    // 5. Decompress from 2.2 display response to linear space BEFORE applying the linear
    // AGX_OUTSET_MATRIX primary restoration (fixes washed-out midtones and chroma skew).
    val = pow(val, vec3(2.2));

    // 6. Transform back from AgX working primaries to linear sRGB display primaries
    val = clamp(AGX_OUTSET_MATRIX * val, 0.0, 1.0);

    return val;
}

// Stephen Hill ACES Fitted (sRGB -> AP1/RRT_SAT -> RRT+ODT -> ODT_SAT/sRGB)
const mat3 ACES_INPUT_MATRIX = mat3(
    vec3(0.59719, 0.07600, 0.02840),
    vec3(0.35458, 0.90834, 0.13383),
    vec3(0.04823, 0.01566, 0.83777)
);

const mat3 ACES_OUTPUT_MATRIX = mat3(
    vec3( 1.60475, -0.10208, -0.00327),
    vec3(-0.53108,  1.10813, -0.07276),
    vec3(-0.07367, -0.00605,  1.07602)
);

vec3 rrtAndOdtFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 acesFitted(vec3 color) {
    vec3 v = ACES_INPUT_MATRIX * max(color, vec3(0.0));
    v = rrtAndOdtFit(v);
    return clamp(ACES_OUTPUT_MATRIX * v, 0.0, 1.0);
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

// Samples an unwrapped 256x16 (16x16x16) 3D color grading LUT strip in sRGB display space
// using hardware bilinear interpolation within slices and linear interpolation across B slices.
vec3 sampleColorGradingLut(vec3 srgbColor) {
    const float lutSize = 16.0;
    vec3 coord = clamp(srgbColor, 0.0, 1.0) * (lutSize - 1.0);
    float slice0 = floor(coord.b);
    float slice1 = min(slice0 + 1.0, lutSize - 1.0);
    float sliceLerp = coord.b - slice0;

    float u0 = (slice0 * lutSize + coord.r + 0.5) / (lutSize * lutSize);
    float u1 = (slice1 * lutSize + coord.r + 0.5) / (lutSize * lutSize);
    float v  = (coord.g + 0.5) / lutSize;

    vec3 c0 = texture(colorGradingLutTexture, vec2(u0, v)).rgb;
    vec3 c1 = texture(colorGradingLutTexture, vec2(u1, v)).rgb;
    return mix(c0, c1, sliceLerp);
}

// Applies optical radial vignette falloff in linear space without hue distortion
vec3 applyVignette(vec3 color, vec2 uv) {
    if (pc.vignetteIntensity <= 0.0) {
        return color;
    }
    vec2 centered = (uv - 0.5) * 2.0;
    float aspect = max(pc.aspectRatio, 0.01);
    centered.x *= mix(1.0, aspect, clamp(pc.vignetteRoundness, 0.0, 1.0));
    float dist = length(centered) * 0.70710678;
    float smoothness = max(pc.vignetteSmoothness, 0.05);
    float factor = smoothstep(1.0, max(0.0, 1.0 - smoothness), dist * (0.5 + 0.5 * pc.vignetteIntensity));
    factor = mix(1.0, factor, clamp(pc.vignetteIntensity, 0.0, 1.0));
    return color * factor;
}

// Temporally-stable luminance-weighted film grain in display space
vec3 applyFilmGrain(vec3 srgbColor, vec2 fragCoord) {
    if (pc.filmGrainIntensity <= 0.0) {
        return srgbColor;
    }
    // Interleaved gradient noise with golden-ratio temporal frame offset
    float frameOffset = float(pc.frameIndex % 1024u) * 5.588238;
    vec2 p = fragCoord + vec2(frameOffset, frameOffset * 0.75487766);
    float noise = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
    float grain = (noise - 0.5) * 2.0;

    // Photographic response: stronger in midtones/shadows, attenuated in pure black and clipped highlights
    float luma = dot(srgbColor, vec3(0.2126, 0.7152, 0.0722));
    float weight = 4.0 * luma * (1.0 - luma) + 0.15;
    return clamp(srgbColor + vec3(grain * pc.filmGrainIntensity * weight), 0.0, 1.0);
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

    // Apply optical vignette in linear display-referred space (no-op when vignetteIntensity <= 0)
    if (pc.vignetteIntensity > 0.0) {
        mappedColor = applyVignette(mappedColor, inUV);
    }

    // Apply sRGB transfer curve for standard UNORM swapchain
    vec3 srgbColor = clamp(linearToSrgb(mappedColor), 0.0, 1.0);

    // Apply 3D LUT color grading in display-encoded sRGB space (no-op when lutIntensity <= 0)
    if (pc.lutIntensity > 0.0) {
        vec3 gradedSrgb = sampleColorGradingLut(srgbColor);
        srgbColor = clamp(mix(srgbColor, gradedSrgb, clamp(pc.lutIntensity, 0.0, 1.0)), 0.0, 1.0);
    }

    // Apply temporally-stable film grain in display space (no-op when filmGrainIntensity <= 0)
    if (pc.filmGrainIntensity > 0.0) {
        srgbColor = applyFilmGrain(srgbColor, gl_FragCoord.xy);
    }

    outColor = vec4(srgbColor, sceneSample.a);
}
