// Shaders for Coral Bay (GLSL 330 on desktop OpenGL 3.3, GLSL ES 300 on WebGL 2)
//
//  LIGHT   sunset sun + sky light, flat shading, soft shadows, distance fog.
//          Emissive surfaces (neon) skip lighting and mark themselves in the
//          alpha channel so the glow pass can find them.
//  DEPTH   writes depth packed into RGBA8 for the shadow map.
//  EXTRACT pulls the neon mask out of the scene, BLUR spreads it,
//  COMPOSITE adds the glow back with a soft tone map and vignette.
#pragma once

#if defined(PLATFORM_WEB)
#define GLSL_HEADER "#version 300 es\nprecision highp float;\n"
#else
#define GLSL_HEADER "#version 330\n"
#endif

static const char* LIGHT_VS = GLSL_HEADER R"(in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
out vec3 fragPos;
out vec2 fragTexCoord;
out vec4 fragColor;
void main() {
    fragPos = vec3(matModel * vec4(vertexPosition, 1.0));
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

static const char* LIGHT_FS = GLSL_HEADER R"(in vec3 fragPos;
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 viewPos;
uniform vec3 sunDir;
uniform vec3 sunColor;
uniform vec3 skyColor;
uniform vec3 groundColor;
uniform vec3 fogColor;
uniform float fogDensity;
uniform float emissive;
uniform sampler2D shadowMap;
uniform mat4 lightVP;
uniform int shadowsOn;
out vec4 finalColor;

float unpackDepth(vec4 c) { return dot(c, vec4(1.0, 1.0/255.0, 1.0/65025.0, 1.0/16581375.0)); }

void main() {
    vec4 base = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    if (base.a < 0.1) discard;
    vec3 toCam = viewPos - fragPos;
    vec3 n = normalize(cross(dFdx(fragPos), dFdy(fragPos)));
    if (dot(n, toCam) < 0.0) n = -n;
    vec3 col;
    if (emissive > 0.5) {
        col = min(base.rgb * 1.3, vec3(1.0));
    } else {
        float diff = max(dot(n, sunDir), 0.0);
        float lit = 1.0;
        if (shadowsOn == 1 && diff > 0.0) {
            vec4 ls = lightVP * vec4(fragPos, 1.0);
            vec3 p = ls.xyz / ls.w * 0.5 + 0.5;
            if (p.x > 0.0 && p.x < 1.0 && p.y > 0.0 && p.y < 1.0 && p.z < 1.0) {
                float bias = max(0.0006 * (1.0 - diff), 0.00025);
                vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
                float s = 0.0;
                for (int x = -1; x <= 1; x++)
                    for (int y = -1; y <= 1; y++) {
                        float d = unpackDepth(texture(shadowMap, p.xy + vec2(x, y) * texel));
                        s += (p.z - bias > d) ? 1.0 : 0.0;
                    }
                lit = 1.0 - (s / 9.0) * 0.8;
            }
        }
        vec3 hemi = mix(groundColor, skyColor, n.y * 0.5 + 0.5);
        col = base.rgb * (hemi + sunColor * diff * lit);
    }
    float dist = length(toCam);
    float fog = 1.0 - exp(-pow(dist * fogDensity, 2.0));
    col = mix(col, fogColor, clamp(fog, 0.0, 1.0));
    finalColor = vec4(col, emissive > 0.5 ? 0.5 : 1.0);
}
)";

static const char* DEPTH_FS = GLSL_HEADER R"(in vec3 fragPos;
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
void main() {
    vec4 enc = vec4(1.0, 255.0, 65025.0, 16581375.0) * gl_FragCoord.z;
    enc = fract(enc);
    enc -= enc.yzww * vec4(1.0/255.0, 1.0/255.0, 1.0/255.0, 0.0);
    finalColor = enc;
}
)";

static const char* EXTRACT_FS = GLSL_HEADER R"(in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
out vec4 finalColor;
void main() {
    vec4 c = texture(texture0, fragTexCoord);
    float m = clamp((1.0 - c.a) * 2.0, 0.0, 1.0);
    finalColor = vec4(c.rgb * m, 1.0);
}
)";

static const char* BLUR_FS = GLSL_HEADER R"(in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 dir;
out vec4 finalColor;
void main() {
    float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 c = texture(texture0, fragTexCoord).rgb * w[0];
    for (int i = 1; i < 5; i++) {
        vec2 o = dir * float(i) * 1.8;
        c += texture(texture0, fragTexCoord + o).rgb * w[i];
        c += texture(texture0, fragTexCoord - o).rgb * w[i];
    }
    finalColor = vec4(c, 1.0);
}
)";

static const char* COMPOSITE_FS = GLSL_HEADER R"(in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform sampler2D glowTex;
uniform float glowAmt;
out vec4 finalColor;
void main() {
    vec3 c = texture(texture0, fragTexCoord).rgb;
    c += texture(glowTex, fragTexCoord).rgb * glowAmt;
    c = c / (1.0 + c * 0.12);
    vec2 uv = fragTexCoord - 0.5;
    c *= 1.0 - dot(uv, uv) * 0.55;
    finalColor = vec4(c, 1.0);
}
)";
