#version 450

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in vec3 inShadowCoord;

layout(location = 0) out vec4 outColor;

// Set 0: Material Textures and Parameters
layout(set = 0, binding = 0) uniform sampler2D uBaseColorMap;
layout(set = 0, binding = 1) uniform sampler2D uMetallicRoughnessMap;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uOcclusionMap;
layout(set = 0, binding = 4) uniform sampler2D uEmissiveMap;

layout(std140, set = 0, binding = 5) uniform MaterialData {
    vec4 baseColorFactor;
    vec4 emissiveFactorAndAlphaCutoff;
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float occlusionStrength;
    uint hasBaseColorTex;
    uint hasMetallicRoughnessTex;
    uint hasNormalTex;
    uint hasOcclusionTex;
    uint hasEmissiveTex;
    uint alphaMode;
    uint doubleSided;
    float transmissionFactor;
    float ior;
    float pad0;
    float pad1;
    float pad2;
}
material;

// Set 1: Scene Lighting and Shadow Map
struct GpuDirectionalLight {
    vec4 directionAndCastShadow; // xyz: dir, w: castShadows
    vec4 colorAndIntensity;      // rgb: color, a: intensity
};

struct GpuPointLight {
    vec4 positionAndRange;  // xyz: pos, w: range
    vec4 colorAndIntensity; // rgb: color, a: intensity
};

struct GpuSpotLight {
    vec4 positionAndRange;      // xyz: pos, w: range
    vec4 directionAndIntensity; // xyz: dir, w: intensity
    vec4 colorAndCastShadow;    // rgb: color, a: castShadows
    vec4 coneAngles;            // x: cosInner, y: cosOuter, zw: pad
};

layout(std140, set = 1, binding = 0) uniform LightingData {
    mat4 cascadeLightSpaceMatrices[4];
    GpuDirectionalLight dirLight;
    GpuPointLight pointLights[16];
    GpuSpotLight spotLights[16];
    vec4 cameraPosAndAmbientIntensity;
    vec4 ambientColorAndShadowBias;
    uint numPointLights;
    uint numSpotLights;
    uint dirLightActive;
    uint shadowPackedParams;
    vec4 iblParams; // x: hasIbl, y: intensity, z: maxPrefilterMipLevel, w: unused
    vec4 giParams;  // x: giEnabled, y: giIntensity, z: ssgiRadius, w: gtaoIntensity
    vec4 probeParams; // x: probesEnabled, y: probeIntensity, zw: pad
    mat4 prevViewProj;
    mat4 invViewProj;
    vec4 probeSh[9];
    vec4 cascadeSplits; // View-space cascade far distances: x: c0, y: c1, z: c2, w: c3
    vec4 cascadeParams; // x: cascadeCount, y: cascadeBlendRange, z: contactShadows, w: softShadows
    vec4 padShadow;
}
uLighting;

layout(set = 1, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 1, binding = 2) uniform samplerCube uIrradianceMap;
layout(set = 1, binding = 3) uniform samplerCube uPrefilteredEnvMap;
layout(set = 1, binding = 4) uniform sampler2D uBrdfLut;
layout(set = 1, binding = 5) uniform sampler2D uAoMap;
layout(set = 1, binding = 6) uniform sampler2D uIndirectLightMap;

// Analytical Order-2 Spherical Harmonics Diffuse Irradiance Convolution (Ramamoorthi 2001)
vec3 evaluateProbeIrradiance(vec3 N) {
    if (uLighting.probeParams.x <= 0.5) {
        return vec3(0.0);
    }
    vec3 c0 = uLighting.probeSh[0].xyz;
    vec3 c1 = uLighting.probeSh[1].xyz;
    vec3 c2 = uLighting.probeSh[2].xyz;
    vec3 c3 = uLighting.probeSh[3].xyz;
    vec3 c4 = uLighting.probeSh[4].xyz;
    vec3 c5 = uLighting.probeSh[5].xyz;
    vec3 c6 = uLighting.probeSh[6].xyz;
    vec3 c7 = uLighting.probeSh[7].xyz;
    vec3 c8 = uLighting.probeSh[8].xyz;

    const float r1 = 0.429043;
    const float r2 = 0.511664;
    const float r3 = 0.743125;
    const float r4 = 0.886227;
    const float r5 = 0.247708;

    vec3 irr = r4 * c0 +
               (2.0 * r2) * (c3 * N.x + c1 * N.y + c2 * N.z) +
               r1 * c8 * (N.x * N.x - N.y * N.y) +
               r3 * c6 * (N.z * N.z) - r5 * c6 +
               (2.0 * r1) * (c4 * (N.x * N.y) + c7 * (N.x * N.z) + c5 * (N.y * N.z));
    return max(irr, vec3(0.0)) * uLighting.probeParams.y;
}

const float PI = 3.141592653589793;
const float INV_PI = 0.31830988618379067;

// Fast Schlick Fresnel Approximation - eliminates log2/exp2 SFU instructions
vec3 fresnelSchlick(float VdotH, vec3 F0) {
    float fc = clamp(1.0 - VdotH, 0.0, 1.0);
    float fc2 = fc * fc;
    float fc5 = fc2 * fc2 * fc;
    return F0 + (vec3(1.0) - F0) * fc5;
}

vec3 fresnelSchlickRoughness(float NdotV, vec3 F0, float roughness) {
    float fc = clamp(1.0 - NdotV, 0.0, 1.0);
    float fc2 = fc * fc;
    float fc5 = fc2 * fc2 * fc;
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * fc5;
}

// Fused Cook-Torrance BRDF (Single-Division Karis Smith-GGX + GGX NDF)
vec3 evaluateDirectLight(vec3 L, vec3 radiance, vec3 N, vec3 V, vec3 albedo, float metallic,
                         float roughness, vec3 F0, float NdotL, float transmission) {
    vec3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;

    float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;
    float gv = NdotV * (1.0 - k) + k;
    float gl = NdotL * (1.0 - k) + k;

    float specScalar = (0.5 * INV_PI * a2) / max(denom * denom * gv * gl, 0.0000001);
    vec3 F = fresnelSchlick(VdotH, F0);

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffTerm = kD * (albedo * INV_PI) * (1.0 - transmission);
    vec3 specTerm = specScalar * F;

    return (diffTerm + specTerm) * radiance * NdotL;
}

// Sample directional shadow map with rotated Vogel-disc PCF and slope-scaled depth bias
const float GOLDEN_ANGLE = 2.39996323;

float sampleCascadeShadow(int cascadeIdx, vec3 worldPos, float NdotL, float baseBias,
                           vec2 screenPos) {
    vec4 shadowPos = uLighting.cascadeLightSpaceMatrices[cascadeIdx] * vec4(worldPos, 1.0);
    vec3 shadowCoord = shadowPos.xyz;

    int cascadeCount = int(clamp(uLighting.cascadeParams.x, 1.0, 4.0));
    vec2 tileOffset = vec2(0.0);
    vec2 tileSize = vec2(1.0);
    if (cascadeCount > 1) {
        tileSize = vec2(0.5);
        tileOffset = vec2(float(cascadeIdx % 2) * 0.5, float(cascadeIdx / 2) * 0.5);
    }

    uint shadowRes = uLighting.shadowPackedParams & 0xFFFFu;
    if (shadowRes == 0u) {
        shadowRes = 2048u;
    }
    float invRes = 1.0 / float(shadowRes);

    vec2 tileMin = tileOffset + vec2(1.5 * invRes);
    vec2 tileMax = tileOffset + tileSize - vec2(1.5 * invRes);

    if (shadowCoord.x < tileMin.x || shadowCoord.x > tileMax.x || shadowCoord.y < tileMin.y ||
        shadowCoord.y > tileMax.y || shadowCoord.z > 1.0 || shadowCoord.z < 0.0) {
        return 1.0;
    }

    // Adaptive slope bias: scales with slope and cascade index to prevent grazing acne
    float tanTheta = clamp(sqrt(max(0.0, 1.0 - NdotL * NdotL)) / max(NdotL, 0.001), 0.0, 4.0);
    float cascadeBiasScale = 1.0 + float(cascadeIdx) * 0.75;
    float bias = baseBias * (0.6 + 0.4 * tanTheta) * cascadeBiasScale;
    float currentDepth = shadowCoord.z - bias;

    uint pcfQuality = (uLighting.shadowPackedParams >> 16u) & 0xFFu;
    if (pcfQuality == 0u) {
        return texture(uShadowMap, vec3(clamp(shadowCoord.xy, tileMin, tileMax), currentDepth));
    }

    // Interleaved gradient noise for screen-space sample rotation
    float noise = fract(52.9829189 * fract(dot(screenPos, vec2(0.06711056, 0.00583715))));
    float phi = noise * 6.2831853;
    float cosPhi = cos(phi);
    float sinPhi = sin(phi);
    mat2 rot = mat2(cosPhi, -sinPhi, sinPhi, cosPhi);

    if (pcfQuality == 1u) {
        // Medium: 8-tap rotated Vogel disc PCF
        float shadow = 0.0;
        const int TAPS_MED = 8;
        float filterRadius = 1.5 * invRes;
        for (int i = 0; i < TAPS_MED; ++i) {
            float r = sqrt((float(i) + 0.5) / float(TAPS_MED)) * filterRadius;
            float theta = float(i) * GOLDEN_ANGLE;
            vec2 offset = rot * vec2(cos(theta), sin(theta)) * r;
            vec2 tapCoord = clamp(shadowCoord.xy + offset, tileMin, tileMax);
            shadow += texture(uShadowMap, vec3(tapCoord, currentDepth));
        }
        return shadow * (1.0 / float(TAPS_MED));
    } else {
        // High: 16-tap rotated Vogel disc PCF for smooth cinematic penumbra
        float shadow = 0.0;
        const int TAPS_HIGH = 16;
        float filterRadius = 2.25 * invRes;
        for (int i = 0; i < TAPS_HIGH; ++i) {
            float r = sqrt((float(i) + 0.5) / float(TAPS_HIGH)) * filterRadius;
            float theta = float(i) * GOLDEN_ANGLE;
            vec2 offset = rot * vec2(cos(theta), sin(theta)) * r;
            vec2 tapCoord = clamp(shadowCoord.xy + offset, tileMin, tileMax);
            shadow += texture(uShadowMap, vec3(tapCoord, currentDepth));
        }
        return shadow * (1.0 / float(TAPS_HIGH));
    }
}

float calculateCascadedShadow(vec3 worldPos, float NdotL, float baseBias, vec2 screenPos) {
    float viewDist = length(uLighting.cameraPosAndAmbientIntensity.xyz - worldPos);
    int cascadeCount = int(clamp(uLighting.cascadeParams.x, 1.0, 4.0));
    float blendRange = clamp(uLighting.cascadeParams.y, 0.0, 0.30);

    if (viewDist >= uLighting.cascadeSplits.w) {
        return 1.0;
    }

    int cascadeIdx = 0;
    if (cascadeCount > 1 && viewDist > uLighting.cascadeSplits.x) {
        cascadeIdx = 1;
    }
    if (cascadeCount > 2 && viewDist > uLighting.cascadeSplits.y) {
        cascadeIdx = 2;
    }
    if (cascadeCount > 3 && viewDist > uLighting.cascadeSplits.z) {
        cascadeIdx = 3;
    }

    float shadow = sampleCascadeShadow(cascadeIdx, worldPos, NdotL, baseBias, screenPos);

    // Cross-cascade smooth blending
    float splitFar = uLighting.cascadeSplits[cascadeIdx];
    float blendBand = splitFar * blendRange;
    if (blendBand > 0.001 && viewDist > (splitFar - blendBand)) {
        float blendFactor = clamp((viewDist - (splitFar - blendBand)) / blendBand, 0.0, 1.0);
        if (cascadeIdx + 1 < cascadeCount) {
            float nextShadow =
                sampleCascadeShadow(cascadeIdx + 1, worldPos, NdotL, baseBias, screenPos);
            shadow = mix(shadow, nextShadow, blendFactor);
        } else {
            shadow = mix(shadow, 1.0, blendFactor);
        }
    }

    return shadow;
}

void main() {
    // 1. Base Color & Alpha
    vec4 baseColor = material.baseColorFactor * inColor;
    if (material.hasBaseColorTex != 0u) {
        baseColor *= texture(uBaseColorMap, inUV);
    }

    // Alpha Cutoff / Masking
    if (material.alphaMode == 1u) { // Mask
        if (baseColor.a < material.emissiveFactorAndAlphaCutoff.w) {
            discard;
        }
    }

    // 2. Metallic and Roughness (glTF: Green = roughness, Blue = metallic)
    float metallic = material.metallicFactor;
    float roughness = material.roughnessFactor;
    if (material.hasMetallicRoughnessTex != 0u) {
        vec4 mrSample = texture(uMetallicRoughnessMap, inUV);
        roughness *= mrSample.g;
        metallic *= mrSample.b;
    }
    metallic = clamp(metallic, 0.0, 1.0);
    roughness = clamp(roughness, 0.045, 1.0);

    // 3. Normal Mapping & TBN Basis
    vec3 N = normalize(inNormal);
    if (material.hasNormalTex != 0u) {
        vec3 T = normalize(inTangent.xyz - dot(inTangent.xyz, N) * N);
        vec3 B = cross(N, T) * inTangent.w;
        vec3 normalSample = texture(uNormalMap, inUV).xyz * 2.0 - 1.0;
        normalSample.xy *= material.normalScale;
        N = normalize(mat3(T, B, N) * normalSample);
    }
    if (material.doubleSided != 0u && !gl_FrontFacing) {
        N = -N;
    }

    // 4. View vector and Specular F0
    vec3 V = normalize(uLighting.cameraPosAndAmbientIntensity.xyz - inWorldPos);
    float dielectricF0 = 0.04;
    if (material.ior > 0.0) {
        float f0Sqrt = (material.ior - 1.0) / (material.ior + 1.0);
        dielectricF0 = clamp(f0Sqrt * f0Sqrt, 0.0, 1.0);
    }
    vec3 F0 = mix(vec3(dielectricF0), baseColor.rgb, metallic);

    float transmission = clamp(material.transmissionFactor, 0.0, 1.0);

    // 5. Direct Light Accumulation
    vec3 directLighting = vec3(0.0);

    // 5a. Directional Light
    if (uLighting.dirLightActive != 0u) {
        vec3 L = uLighting.dirLight.directionAndCastShadow.xyz;
        float NdotL = dot(N, L);
        if (NdotL > 0.0) {
            float shadow = 1.0;
            if (uLighting.dirLight.directionAndCastShadow.w > 0.5) {
                shadow = calculateCascadedShadow(inWorldPos, NdotL,
                                                 uLighting.ambientColorAndShadowBias.w,
                                                 gl_FragCoord.xy);
                // Contact Shadow grounding from bilateral filtered uAoMap (channel G)
                if (uLighting.cascadeParams.z > 0.5) {
                    vec2 screenSize = vec2(textureSize(uAoMap, 0));
                    vec2 screenUV = gl_FragCoord.xy / max(screenSize, vec2(1.0));
                    float contactShadow = texture(uAoMap, screenUV).g;
                    shadow = min(shadow, contactShadow);
                }
            }
            if (shadow > 0.001) {
                vec3 radiance = uLighting.dirLight.colorAndIntensity.rgb *
                                uLighting.dirLight.colorAndIntensity.w;
                directLighting +=
                    shadow * evaluateDirectLight(L, radiance, N, V, baseColor.rgb, metallic,
                                                 roughness, F0, NdotL, transmission);
            }
        }
    }

    // 5b. Point Lights
    if (uLighting.numPointLights > 0u) {
        for (uint i = 0u; i < uLighting.numPointLights && i < 16u; ++i) {
            vec3 lightPos = uLighting.pointLights[i].positionAndRange.xyz;
            float range = uLighting.pointLights[i].positionAndRange.w;
            vec3 toLight = lightPos - inWorldPos;
            float dist = length(toLight);
            if (dist < range && dist > 0.0001) {
                vec3 L = toLight / dist;
                float NdotL = max(dot(N, L), 0.0);
                if (NdotL > 0.0) {
                    float distSq = max(dist * dist, 0.0001);
                    float atten = 1.0 / distSq;
                    float factor = dist / range;
                    float window = clamp(1.0 - factor * factor * factor * factor, 0.0, 1.0);
                    atten *= (window * window);

                    vec3 radiance = uLighting.pointLights[i].colorAndIntensity.rgb *
                                    uLighting.pointLights[i].colorAndIntensity.w * atten;
                    directLighting +=
                        evaluateDirectLight(L, radiance, N, V, baseColor.rgb, metallic, roughness,
                                            F0, NdotL, transmission);
                }
            }
        }
    }

    // 5c. Spot Lights
    if (uLighting.numSpotLights > 0u) {
        for (uint i = 0u; i < uLighting.numSpotLights && i < 16u; ++i) {
            vec3 lightPos = uLighting.spotLights[i].positionAndRange.xyz;
            float range = uLighting.spotLights[i].positionAndRange.w;
            vec3 toLight = lightPos - inWorldPos;
            float dist = length(toLight);
            if (dist < range && dist > 0.0001) {
                vec3 L = toLight / dist;
                float NdotL = max(dot(N, L), 0.0);
                if (NdotL > 0.0) {
                    vec3 spotDir = normalize(uLighting.spotLights[i].directionAndIntensity.xyz);
                    float cosAngle = dot(-L, spotDir);
                    float cosInner = uLighting.spotLights[i].coneAngles.x;
                    float cosOuter = uLighting.spotLights[i].coneAngles.y;

                    float spotAtten =
                        clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 0.0001), 0.0, 1.0);
                    spotAtten = spotAtten * spotAtten;

                    if (spotAtten > 0.0) {
                        float distSq = max(dist * dist, 0.0001);
                        float distAtten = 1.0 / distSq;
                        float factor = dist / range;
                        float window = clamp(1.0 - factor * factor * factor * factor, 0.0, 1.0);
                        distAtten *= (window * window);

                        vec3 radiance = uLighting.spotLights[i].colorAndCastShadow.rgb *
                                        uLighting.spotLights[i].directionAndIntensity.w *
                                        distAtten * spotAtten;
                        directLighting +=
                            evaluateDirectLight(L, radiance, N, V, baseColor.rgb, metallic,
                                                roughness, F0, NdotL, transmission);
                    }
                }
            }
        }
    }

    // 6. Ambient Occlusion: combine glTF material AO with dynamic Screen-Space GTAO
    float materialOcclusion = 1.0;
    if (material.hasOcclusionTex != 0u) {
        materialOcclusion = mix(1.0, texture(uOcclusionMap, inUV).r, material.occlusionStrength);
    }
    vec2 screenSize = vec2(textureSize(uAoMap, 0));
    vec2 screenUV = gl_FragCoord.xy / max(screenSize, vec2(1.0));
    float screenAo = texture(uAoMap, screenUV).r;
    float finalOcclusion = materialOcclusion * mix(1.0, screenAo, uLighting.giParams.w);

    // 7. Ambient / Image-Based Lighting (IBL) & Screen-Space Indirect GI Term
    vec3 ambient = vec3(0.0);
    float diffTransScale = clamp(1.0 - transmission, 0.0, 1.0);

    // Sample Screen-Space Indirect Lighting (SSGI) & Probe Irradiance
    vec3 ssgiRadiance = vec3(0.0);
    if (uLighting.giParams.x > 0.5) {
        ssgiRadiance = texture(uIndirectLightMap, screenUV).rgb * uLighting.giParams.y;
    }
    vec3 probeRadiance = evaluateProbeIrradiance(N);

    if (uLighting.iblParams.x > 0.5) {
        float NdotV = max(dot(N, V), 0.0);
        vec3 F_ibl = fresnelSchlickRoughness(NdotV, F0, roughness);
        vec3 kD_ibl = (vec3(1.0) - F_ibl) * (1.0 - metallic);

        // Diffuse IBL + Diffuse Indirect (SSGI color bleed + probe bounce)
        vec3 irradiance = texture(uIrradianceMap, N).rgb;
        vec3 diffuseRadiance = irradiance + ssgiRadiance + probeRadiance;
        vec3 diffuseAmbient = kD_ibl * diffuseRadiance * baseColor.rgb * diffTransScale;

        // Specular IBL (Split-Sum Cook-Torrance with roughness-compensated Fresnel)
        vec3 R = reflect(-V, N);
        float maxMip = uLighting.iblParams.z;
        vec3 prefilteredColor = textureLod(uPrefilteredEnvMap, R, roughness * maxMip).rgb;
        vec2 envBRDF = texture(uBrdfLut, vec2(NdotV, roughness)).rg;
        vec3 F_env = F0 * envBRDF.x + max(vec3(1.0 - roughness), F0) * envBRDF.y;
        vec3 specularIBL = prefilteredColor * F_env;

        ambient = (diffuseAmbient + specularIBL) * (finalOcclusion * uLighting.iblParams.y);
    } else {
        float ambIntensity = uLighting.cameraPosAndAmbientIntensity.w;
        vec3 ambLight = (ambIntensity * finalOcclusion) * uLighting.ambientColorAndShadowBias.rgb;
        vec3 diffuseRadiance = ambLight + ssgiRadiance + probeRadiance;
        vec3 ambientDiffuse = baseColor.rgb * (1.0 - metallic) * diffuseRadiance * diffTransScale;
        vec3 ambientSpecular = mix(F0, baseColor.rgb, metallic) * (1.0 - roughness) * ambLight;
        ambient = ambientDiffuse + ambientSpecular;
    }

    // 8. Emissive Term
    vec3 emissive = vec3(0.0);
    if (material.hasEmissiveTex != 0u) {
        emissive = material.emissiveFactorAndAlphaCutoff.xyz * texture(uEmissiveMap, inUV).rgb;
    } else if (dot(material.emissiveFactorAndAlphaCutoff.xyz,
                   material.emissiveFactorAndAlphaCutoff.xyz) > 0.0) {
        emissive = material.emissiveFactorAndAlphaCutoff.xyz;
    }

    // 9. Final Radiance Output & Optical Transmission Composition
    vec3 finalColor = directLighting + ambient + emissive;
    float outAlpha = baseColor.a;

    if (transmission > 0.0) {
        float NdotV = max(dot(N, V), 0.0);
        float F_view = fresnelSchlick(NdotV, F0).r;
        // Background transmission fraction (reduced by Fresnel reflection)
        float transFraction = transmission * (1.0 - F_view);
        // Effective surface opacity for hardware blending
        outAlpha = clamp(baseColor.a * (1.0 - transFraction) + F_view, 0.04, 1.0);
        // Scale surface luminance so blending (outColor * alpha + dst * (1 - alpha)) preserves
        // specular reflection
        finalColor = finalColor / max(outAlpha, 0.001);
    }

    outColor = vec4(finalColor, outAlpha);
}
