#version 450

layout(location = 0) in vec4 inCurClip;
layout(location = 1) in vec4 inPrevClip;
layout(location = 0) out vec4 outVelocity;

layout(push_constant) uniform MeshVelocityPushConstants {
    mat4 curMvp;
    mat4 prevMvp;
} pc;

void main() {
    if (inCurClip.w <= 1e-5 || inPrevClip.w <= 1e-5) {
        outVelocity = vec4(0.0, 0.0, gl_FragCoord.z, 0.0);
        return;
    }
    vec2 curNDC = inCurClip.xy / inCurClip.w;
    vec2 prevNDC = inPrevClip.xy / inPrevClip.w;
    vec2 curUV = vec2(curNDC.x * 0.5 + 0.5, 0.5 - curNDC.y * 0.5);
    vec2 prevUV = vec2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5);
    vec2 vel = curUV - prevUV;
    outVelocity = vec4(vel, gl_FragCoord.z, 1.0);
}
