#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDepthMap;
layout(set = 0, binding = 1) uniform sampler2D uSceneRadianceMap;

layout(push_constant) uniform SsgiPushConstants {
    mat4 reprojectMat;
    vec4 projParams; // x: P00, y: P11, z: P22, w: P23
    vec4 ssgiParams; // x: intensity, y: raySteps, z: thickness, w: maxRadius
    vec2 resolution;
    vec2 invResolution;
    uint frameIndex;
    float pad0;
} pc;

const float PI = 3.141592653589793;

vec3 getViewPos(vec2 uv) {
    float depth = texture(uDepthMap, uv).r;
    float denom = max(-depth - pc.projParams.z, 1e-6);
    float ze = -pc.projParams.w / denom;
    return vec3((uv.x * 2.0 - 1.0) * (ze / max(pc.projParams.x, 1e-6)),
                (1.0 - uv.y * 2.0) * (ze / max(pc.projParams.y, 1e-6)),
                -ze);
}

vec2 projectViewPos(vec3 S) {
    float ze = -S.z;
    if (ze <= 0.001) {
        return vec2(-1.0);
    }
    float clipX = S.x * pc.projParams.x;
    float clipY = S.y * pc.projParams.y;
    vec2 ndc = vec2(clipX, clipY) / ze;
    return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float spatialHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    float centerDepth = texture(uDepthMap, inUV).r;
    if (centerDepth >= 1.0) {
        outColor = vec4(0.0);
        return;
    }

    vec3 P = getViewPos(inUV);

    // View-space surface normal from screen derivatives
    vec3 dPdx = dFdx(P);
    vec3 dFdyP = dFdy(P);
    vec3 N = normalize(cross(dPdx, dFdyP));
    if (dot(N, -P) < 0.0) {
        N = -N;
    }

    // Construct local tangent frame around N
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);

    float intensity = pc.ssgiParams.x;
    int raySteps = int(clamp(pc.ssgiParams.y, 4.0, 16.0));
    float thickness = pc.ssgiParams.z;
    float maxRadius = pc.ssgiParams.w;

    float noise = spatialHash(gl_FragCoord.xy + float(pc.frameIndex % 16u) * 1.6180339887);

    const int kNumRays = 8;
    vec3 accumRadiance = vec3(0.0);
    float totalWeight = 0.0;
    float hitCount = 0.0;

    for (int r = 0; r < kNumRays; ++r) {
        float phi = float(r) * (2.0 * PI / float(kNumRays)) + noise * 2.0 * PI;
        float cosTheta = sqrt((float(r) + 0.5 + noise) / float(kNumRays));
        float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));

        vec3 dirTangent = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
        vec3 rayDir = normalize(T * dirTangent.x + B * dirTangent.y + N * dirTangent.z);

        float NdotL = max(dot(N, rayDir), 0.0);
        if (NdotL <= 0.001) {
            continue;
        }

        vec3 rayOrigin = P + N * 0.04;
        bool hit = false;
        vec3 hitRadiance = vec3(0.0);

        for (int s = 1; s <= raySteps; ++s) {
            float tNorm = (float(s) + noise) / float(raySteps + 1);
            float t = maxRadius * (tNorm * tNorm);
            vec3 S = rayOrigin + rayDir * t;

            vec2 sampleUV = projectViewPos(S);
            if (sampleUV.x < 0.02 || sampleUV.x > 0.98 || sampleUV.y < 0.02 || sampleUV.y > 0.98) {
                break;
            }

            vec3 samplePos = getViewPos(sampleUV);
            float rayCamDist = length(S);
            float surfaceCamDist = length(samplePos);
            float depthDiff = rayCamDist - surfaceCamDist;

            float dynamicThickness = thickness * (1.0 + t * 0.15);
            if (depthDiff > 0.02 && depthDiff < dynamicThickness) {
                vec2 edgeDist = min(sampleUV, 1.0 - sampleUV) * 10.0;
                float edgeFade = clamp(min(edgeDist.x, edgeDist.y), 0.0, 1.0);

                float distFalloff = clamp(1.0 - (t / maxRadius), 0.0, 1.0);
                distFalloff *= distFalloff;

                // Reproject hit UV to previous frame screen space to eliminate motion drift
                float sampleDepth = texture(uDepthMap, sampleUV).r;
                vec4 hitNdc = vec4(sampleUV * 2.0 - 1.0, sampleDepth, 1.0);
                vec4 prevClip = pc.reprojectMat * hitNdc;
                vec2 radUV = sampleUV;
                if (prevClip.w > 0.001) {
                    vec2 pUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
                    if (pUV.x >= 0.01 && pUV.x <= 0.99 && pUV.y >= 0.01 && pUV.y <= 0.99) {
                        radUV = pUV;
                    }
                }

                // Sample radiance from previous frame's lit scene without recursive runaway
                vec3 sampledColor = texture(uSceneRadianceMap, radUV).rgb;
                sampledColor = min(sampledColor, vec3(10.0));

                hitRadiance = sampledColor * (distFalloff * edgeFade);
                hit = true;
                hitCount += 1.0;
                break;
            }
        }

        if (hit) {
            accumRadiance += hitRadiance * NdotL;
        }
        totalWeight += NdotL;
    }

    vec3 indirect = (totalWeight > 1e-4) ? (accumRadiance / totalWeight) * intensity : vec3(0.0);
    outColor = vec4(indirect, clamp(hitCount / float(kNumRays), 0.0, 1.0));
}
