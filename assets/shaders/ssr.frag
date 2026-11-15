#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDepthMap;
layout(set = 0, binding = 1) uniform sampler2D uSceneRadianceMap;

layout(push_constant) uniform SsrPushConstants {
    mat4 reprojectMat;       // prevViewProj * invViewProj (64 B)
    vec4 projParams;         // x: P00, y: P11, z: P22, w: P23 (16 B)
    vec4 ssrParams;          // x: maxDistance, y: stepCount, z: thickness, w: roughnessCutoff (16 B)
    vec4 cameraPosAndIntensity; // xyz: camPos, w: ssrIntensity (16 B)
    vec2 resolution;         // (8 B)
    vec2 invResolution;      // (8 B)
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

    // Surface normal in view space reconstructed from screen derivatives
    vec3 dPdx = dFdx(P);
    vec3 dPdy = dFdy(P);
    vec3 N = normalize(cross(dPdx, dPdy));
    if (dot(N, -P) < 0.0) {
        N = -N;
    }

    vec3 V = normalize(-P);
    vec3 R = normalize(reflect(-V, N));

    // Reject rays facing back towards camera origin
    if (dot(N, R) <= 0.001) {
        outColor = vec4(0.0);
        return;
    }

    float maxDist = pc.ssrParams.x;
    int stepCount = int(clamp(pc.ssrParams.y, 8.0, 32.0));
    float baseThickness = pc.ssrParams.z;

    float noise = spatialHash(gl_FragCoord.xy);

    // Initial ray bias along surface normal to eliminate self-intersection acne
    vec3 rayOrigin = P + N * 0.035;
    vec3 hitRadiance = vec3(0.0);
    float confidence = 0.0;

    float tPrev = 0.0;
    vec2 prevUV = inUV;

    for (int s = 1; s <= stepCount; ++s) {
        float tNorm = (float(s) + noise * 0.5) / float(stepCount);
        // Exponential step distribution: finer precision near origin, larger steps far away
        float t = maxDist * (tNorm * tNorm);

        vec3 rayPos = rayOrigin + R * t;
        vec2 sampleUV = projectViewPos(rayPos);

        // Screen boundary exit test
        if (sampleUV.x < 0.001 || sampleUV.x > 0.999 || sampleUV.y < 0.001 || sampleUV.y > 0.999) {
            break;
        }

        vec3 surfacePos = getViewPos(sampleUV);
        float depthDelta = -rayPos.z - (-surfacePos.z);
        float adaptiveThickness = baseThickness * (1.0 + t * 0.15);

        // Intersection test: ray is behind surface within thickness threshold
        if (depthDelta > 0.005 && depthDelta < adaptiveThickness) {
            // Binary search refinement for sub-step hit precision
            float tLow = tPrev;
            float tHigh = t;
            vec2 refinedUV = sampleUV;

            for (int b = 0; b < 2; ++b) {
                float tMid = 0.5 * (tLow + tHigh);
                vec3 midPos = rayOrigin + R * tMid;
                vec2 midUV = projectViewPos(midPos);
                if (midUV.x < 0.0 || midUV.x > 1.0 || midUV.y < 0.0 || midUV.y > 1.0) {
                    break;
                }
                vec3 midSurf = getViewPos(midUV);
                float midDelta = -midPos.z - (-midSurf.z);
                if (midDelta > 0.002 && midDelta < adaptiveThickness) {
                    tHigh = tMid;
                    refinedUV = midUV;
                } else {
                    tLow = tMid;
                }
            }

            // Screen-edge falloff to eliminate harsh cutoff at viewport boundaries
            vec2 edgeDist = min(refinedUV, vec2(1.0) - refinedUV);
            float edgeFade = clamp(min(edgeDist.x, edgeDist.y) / 0.08, 0.0, 1.0);

            // Distance falloff: smooth quadratic attenuation toward max traversal distance
            float distNorm = clamp(tHigh / maxDist, 0.0, 1.0);
            float distFade = 1.0 - distNorm * distNorm;

            // Surface normal facing check: reject back-facing hits
            vec3 hitSurfNorm = normalize(cross(dFdx(getViewPos(refinedUV)), dFdy(getViewPos(refinedUV))));
            float hitFacing = clamp(dot(-R, hitSurfNorm), 0.0, 1.0);

            confidence = edgeFade * distFade * hitFacing * pc.cameraPosAndIntensity.w;
            hitRadiance = textureLod(uSceneRadianceMap, refinedUV, 0.0).rgb;
            break;
        }

        tPrev = t;
        prevUV = sampleUV;
    }

    outColor = vec4(hitRadiance, clamp(confidence, 0.0, 1.0));
}
