#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDepthMap;

layout(push_constant) uniform GtaoPushConstants {
    mat4 invViewProj;
    vec4 cameraPosAndRadius; // xyz: camPos, w: radius
    vec4 gtaoParams;          // x: intensity, y: bias, z: power, w: unused
    vec2 resolution;
    vec2 invResolution;
} pc;

const float PI = 3.141592653589793;
const float HALF_PI = 1.5707963267948966;

// Reconstructs world-space position from UV and depth buffer
vec3 getWorldPos(vec2 uv) {
    float depth = texture(uDepthMap, uv).r;
    vec4 ndc = vec4(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
    vec4 worldH = pc.invViewProj * ndc;
    return worldH.xyz / max(abs(worldH.w), 1e-6);
}

float spatialHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    float centerDepth = texture(uDepthMap, inUV).r;
    if (centerDepth >= 1.0) {
        outColor = vec4(1.0);
        return;
    }

    vec3 P = getWorldPos(inUV);

    // World-space surface normal from screen derivatives
    vec3 dPdx = dFdx(P);
    vec3 dFdyP = dFdy(P);
    vec3 N = normalize(cross(dPdx, dFdyP));

    vec3 camPos = pc.cameraPosAndRadius.xyz;
    if (dot(N, camPos - P) < 0.0) {
        N = -N;
    }

    float radiusWorld = pc.cameraPosAndRadius.w;
    float intensity = pc.gtaoParams.x;
    float bias = pc.gtaoParams.y;
    float power = pc.gtaoParams.z;

    float noise = spatialHash(gl_FragCoord.xy);
    float angleOffset = noise * PI;

    // Screen-space search radius scaled inversely with camera distance and viewport resolution
    float camDist = length(P - camPos);
    float screenRadius = clamp((radiusWorld * pc.resolution.y * 0.4) / max(camDist, 0.1), 2.0, pc.resolution.y * 0.5);

    const int kNumDirections = 4;
    const int kNumSteps = 4;
    float totalOcclusion = 0.0;

    for (int d = 0; d < kNumDirections; ++d) {
        float angle = angleOffset + float(d) * (PI / float(kNumDirections));
        vec2 dir = vec2(cos(angle), sin(angle));

        float h1 = 0.0;
        float h2 = 0.0;

        // Search positive direction
        for (int s = 1; s <= kNumSteps; ++s) {
            float stepScale = (float(s) + noise) / float(kNumSteps + 1);
            vec2 sampleUV = inUV + dir * (stepScale * screenRadius) * pc.invResolution;
            if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
                continue;
            }
            vec3 S = getWorldPos(sampleUV);
            vec3 horizonVec = S - P;
            float dist = length(horizonVec);
            if (dist > 0.005 && dist < radiusWorld * 1.5) {
                float elevation = asin(clamp(dot(horizonVec / dist, N), -1.0, 1.0));
                h1 = max(h1, elevation);
            }
        }

        // Search negative direction
        for (int s = 1; s <= kNumSteps; ++s) {
            float stepScale = (float(s) + noise) / float(kNumSteps + 1);
            vec2 sampleUV = inUV - dir * (stepScale * screenRadius) * pc.invResolution;
            if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
                continue;
            }
            vec3 S = getWorldPos(sampleUV);
            vec3 horizonVec = S - P;
            float dist = length(horizonVec);
            if (dist > 0.005 && dist < radiusWorld * 1.5) {
                float elevation = asin(clamp(dot(horizonVec / dist, N), -1.0, 1.0));
                h2 = max(h2, elevation);
            }
        }

        // Analytical horizon occlusion
        h1 = clamp(h1 - bias, 0.0, HALF_PI);
        h2 = clamp(h2 - bias, 0.0, HALF_PI);
        float dirOcclusion = 0.5 * (sin(h1) + sin(h2));
        totalOcclusion += clamp(dirOcclusion, 0.0, 1.0);
    }

    float occlusion = totalOcclusion / float(kNumDirections);
    occlusion = clamp(pow(occlusion, max(power, 0.1)) * intensity, 0.0, 1.0);
    float ao = 1.0 - occlusion;

    outColor = vec4(ao, ao, ao, 1.0);
}
