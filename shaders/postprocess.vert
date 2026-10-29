#version 450
#extension GL_GOOGLE_include_directive : require
#include "platform.glsl"

VARYING(0) out vec2 uv;

void main() {
    uv = vec2((VERTEX_INDEX << 1) & 2, VERTEX_INDEX & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
