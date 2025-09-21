#version 460
layout(push_constant) uniform PC { mat4 mvp; vec4 color; } pc;
layout(location=0) out vec4 vColor;

const vec3 C[8] = vec3[](
    vec3(-0.5,-0.5,-0.5),
    vec3( 0.5,-0.5,-0.5),
    vec3( 0.5, 0.5,-0.5),
    vec3(-0.5, 0.5,-0.5),
    vec3(-0.5,-0.5, 0.5),
    vec3( 0.5,-0.5, 0.5),
    vec3( 0.5, 0.5, 0.5),
    vec3(-0.5, 0.5, 0.5)
);

const uint I[36] = uint[](
    0u,1u,2u,  2u,3u,0u,
    4u,6u,5u,  6u,4u,7u,
    0u,3u,7u,  7u,4u,0u,
    1u,5u,6u,  6u,2u,1u,
    0u,4u,5u,  5u,1u,0u,
    3u,2u,6u,  6u,7u,3u
);

void main(){
    uint idx = I[gl_VertexIndex % 36u];
    vec3 pos = C[idx];
    gl_Position = pc.mvp * vec4(pos, 1.0);
    vColor = pc.color;
}

