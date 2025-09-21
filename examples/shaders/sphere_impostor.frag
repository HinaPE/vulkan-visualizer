#version 460
layout(location=0) in vec3 vPos; // local space cube coords in [-0.5,0.5]
layout(location=1) in vec4 vColor;
layout(location=0) out vec4 oColor;

void main(){
    float ax = abs(vPos.x);
    float ay = abs(vPos.y);
    float az = abs(vPos.z);
    float r = 0.5; // sphere radius in local space
    vec2 uv; // coordinates on the face plane
    int face = 0; // 0=X,1=Y,2=Z
    if (ax >= ay && ax >= az) { face = 0; uv = vec2(vPos.y, vPos.z); }
    else if (ay >= ax && ay >= az) { face = 1; uv = vec2(vPos.x, vPos.z); }
    else { face = 2; uv = vec2(vPos.x, vPos.y); }

    float r2 = dot(uv, uv);
    if (r2 > r*r) discard; // outside sphere projection

    // Reconstruct normal on sphere
    float zc = sqrt(max(r*r - r2, 0.0));
    vec3 n;
    if (face == 0) {
        n = normalize(vec3(sign(vPos.x)*zc, uv.x, uv.y));
    } else if (face == 1) {
        n = normalize(vec3(uv.x, sign(vPos.y)*zc, uv.y));
    } else {
        n = normalize(vec3(uv.x, uv.y, sign(vPos.z)*zc));
    }

    // Simple lighting
    vec3 L = normalize(vec3(0.4, 0.7, 0.6));
    float ndotl = max(dot(n, L), 0.0);
    vec3 col = vColor.rgb * (0.35 + 0.65*ndotl);
    oColor = vec4(col, vColor.a);
}

