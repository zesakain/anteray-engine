#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDepthMap;

layout(push_constant) uniform GtaoPushConstants {
    vec4 lightDirAndContact; // xyz: lightDir, w: maxContactDist (16 B)
    vec4 cameraPosAndRadius; // xyz: camPos, w: radius         (16 B)
    vec4 gtaoParams;         // x: intensity, y: bias, z: power, w: contactShadowEnabled (16 B)
    vec4 screenAndProj;      // xy: resolution, z: P00, w: P11 (16 B)
    mat4 invViewProj;        // (64 B)
} pc;

const float PI = 3.141592653589793;

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

// Ray marches along sunlight direction in screen/world space to detect fine occluders
float calculateContactShadow(vec3 P, vec3 N, vec3 lightDir, float maxDist, float noise) {
    if (dot(N, lightDir) <= 0.0) {
        return 1.0;
    }
    const int kSteps = 10;
    float stepSize = maxDist / float(kSteps);
    vec3 rayPos = P + N * 0.015;
    vec3 rayStep = lightDir * stepSize;
    rayPos += rayStep * (noise * 0.5);

    vec3 camPos = pc.cameraPosAndRadius.xyz;
    vec3 camRight = normalize(pc.invViewProj[0].xyz);
    vec3 camUp = normalize(pc.invViewProj[1].xyz);
    vec3 camForward = -normalize(pc.invViewProj[2].xyz);
    float p00 = pc.screenAndProj.z;
    float p11 = pc.screenAndProj.w;

    float shadow = 1.0;
    for (int i = 0; i < kSteps; ++i) {
        rayPos += rayStep;
        vec3 toRay = rayPos - camPos;
        float vZ = dot(toRay, camForward);
        if (vZ <= 0.01) {
            break;
        }
        float vX = dot(toRay, camRight);
        float vY = dot(toRay, camUp);
        float ndcX = (vX / vZ) * p00;
        float ndcY = (vY / vZ) * p11;
        vec2 sampleUV = vec2(ndcX * 0.5 + 0.5, ndcY * -0.5 + 0.5);
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
            break;
        }
        vec3 hitWorld = getWorldPos(sampleUV);
        float hitZ = dot(hitWorld - camPos, camForward);
        float depthDiff = vZ - hitZ;
        if (depthDiff > 0.01 && depthDiff < 0.35) {
            float fade = 1.0 - (float(i) / float(kSteps));
            shadow = clamp(1.0 - fade * 0.85, 0.0, 1.0);
            break;
        }
    }
    return shadow;
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
    float screenRadius = clamp((radiusWorld * pc.screenAndProj.y * 0.4) / max(camDist, 0.1), 2.0,
                               pc.screenAndProj.y * 0.5);
    vec2 invResolution = 1.0 / pc.screenAndProj.xy;

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
            vec2 sampleUV = inUV + dir * (stepScale * screenRadius) * invResolution;
            if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
                continue;
            }
            vec3 S = getWorldPos(sampleUV);
            vec3 horizonVec = S - P;
            float dist = length(horizonVec);
            if (dist > 0.005 && dist < radiusWorld * 1.5) {
                float elevation = dot(horizonVec / dist, N);
                h1 = max(h1, elevation);
            }
        }

        // Search negative direction
        for (int s = 1; s <= kNumSteps; ++s) {
            float stepScale = (float(s) + noise) / float(kNumSteps + 1);
            vec2 sampleUV = inUV - dir * (stepScale * screenRadius) * invResolution;
            if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
                continue;
            }
            vec3 S = getWorldPos(sampleUV);
            vec3 horizonVec = S - P;
            float dist = length(horizonVec);
            if (dist > 0.005 && dist < radiusWorld * 1.5) {
                float elevation = dot(horizonVec / dist, N);
                h2 = max(h2, elevation);
            }
        }

        // Analytical horizon occlusion without transcendental asin/sin calls
        h1 = clamp(h1 - bias, 0.0, 1.0);
        h2 = clamp(h2 - bias, 0.0, 1.0);
        float dirOcclusion = 0.5 * (h1 + h2);
        totalOcclusion += clamp(dirOcclusion, 0.0, 1.0);
    }

    float occlusion = totalOcclusion / float(kNumDirections);
    occlusion = clamp(pow(occlusion, max(power, 0.1)) * intensity, 0.0, 1.0);
    float ao = 1.0 - occlusion;

    float contactShadow = 1.0;
    if (pc.gtaoParams.w > 0.5) {
        vec3 lightDir = normalize(pc.lightDirAndContact.xyz);
        contactShadow = calculateContactShadow(P, N, lightDir, pc.lightDirAndContact.w, noise);
    }

    outColor = vec4(ao, contactShadow, 0.0, 1.0);
}
