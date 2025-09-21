#version 460
layout(location=0) in vec3 vColor;layout(location=0) out vec4 o;
layout(push_constant) uniform P{vec4 tint;};
void main(){o=vec4(vColor,1.0)*tint;}

