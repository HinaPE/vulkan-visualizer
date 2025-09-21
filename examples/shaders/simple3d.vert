#version 460
layout(push_constant) uniform PC { mat4 mvp; } pc;
layout(location=0) out vec3 vColor;

// 8 corners of a unit cube centered at origin, side length 1
const vec3 C[8] = vec3[](
    vec3(-0.5,-0.5,-0.5), // 0
    vec3( 0.5,-0.5,-0.5), // 1
    vec3( 0.5, 0.5,-0.5), // 2
    vec3(-0.5, 0.5,-0.5), // 3
    vec3(-0.5,-0.5, 0.5), // 4
    vec3( 0.5,-0.5, 0.5), // 5
    vec3( 0.5, 0.5, 0.5), // 6
    vec3(-0.5, 0.5, 0.5)  // 7
);

// 12 triangles (36 indices)
const uint I[36] = uint[](
    // -Z face
    0u,1u,2u,  2u,3u,0u,
    // +Z face
    4u,6u,5u,  6u,4u,7u,
    // -X face
    0u,3u,7u,  7u,4u,0u,
    // +X face
    1u,5u,6u,  6u,2u,1u,
    // -Y face
    0u,4u,5u,  5u,1u,0u,
    // +Y face
    3u,2u,6u,  6u,7u,3u
);

void main(){
    uint idx = I[gl_VertexIndex % 36u];
    vec3 pos = C[idx];
    gl_Position = pc.mvp * vec4(pos, 1.0);
    // Simple color by normal approximation from face (based on vertex index range)
    uint tri = (gl_VertexIndex / 3u);
    uint face = tri / 2u; // two triangles per face
    vec3 col = vec3(1.0);
    if (face==0u) col = vec3(0.9,0.4,0.4);
    else if(face==1u) col = vec3(0.4,0.9,0.4);
    else if(face==2u) col = vec3(0.4,0.4,0.9);
    else if(face==3u) col = vec3(0.9,0.9,0.4);
    else if(face==4u) col = vec3(0.4,0.9,0.9);
    else if(face==5u) col = vec3(0.9,0.4,0.9);
    vColor = col;
}

