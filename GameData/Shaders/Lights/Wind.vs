#version 330

// Wind-aware variant of Lighting.vs. Identical varyings/outputs so it pairs
// with LitFog.fs; adds a height-weighted sway driven by the WindZone uniforms.
// Non-foliage meshes use Lighting.vs unchanged. Skinning is duplicated here so
// an animated foliage mesh cannot silently fall back to bind pose.

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in vec4 vertexBoneIndices;
in vec4 vertexBoneWeights;

uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;

// See Lighting.vs: 64 bones x 4 vec4 = the GL 3.3 vertex uniform minimum.
#define MAX_BONE_MATRICES 64
uniform mat4 boneMatrices[MAX_BONE_MATRICES];
uniform int uSkinning = 0;

// Wind (set per foliage draw; strength 0 => no displacement)
uniform vec4 windParams;   // xy = world XZ direction, z = strength, w = frequency
uniform float windTime;
uniform float windBaseY;    // model-space Y where sway begins (bounds min)
uniform float windHeight;   // model-space height of the sway range

out vec3 fragPosition;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
out vec3 fragWorldPos;

void main()
{
    vec4 skinnedPos = vec4(vertexPosition, 1.0);
    vec3 skinnedNormal = vertexNormal;
    if (uSkinning == 1)
    {
        mat4 b0 = boneMatrices[int(vertexBoneIndices.x)];
        mat4 b1 = boneMatrices[int(vertexBoneIndices.y)];
        mat4 b2 = boneMatrices[int(vertexBoneIndices.z)];
        mat4 b3 = boneMatrices[int(vertexBoneIndices.w)];
        skinnedPos = vertexBoneWeights.x * (b0 * skinnedPos) +
                     vertexBoneWeights.y * (b1 * skinnedPos) +
                     vertexBoneWeights.z * (b2 * skinnedPos) +
                     vertexBoneWeights.w * (b3 * skinnedPos);
        skinnedNormal = normalize(
            vertexBoneWeights.x * (mat3(b0) * skinnedNormal) +
            vertexBoneWeights.y * (mat3(b1) * skinnedNormal) +
            vertexBoneWeights.z * (mat3(b2) * skinnedNormal) +
            vertexBoneWeights.w * (mat3(b3) * skinnedNormal));
    }

    vec3 pos = skinnedPos.xyz;

    // Sway scales with height above the mesh base so trunks/roots stay planted.
    float h = clamp((pos.y - windBaseY) / max(windHeight, 0.001), 0.0, 1.0);
    float sway = h * h;
    float phase = windTime * windParams.w + pos.x * 0.15 + pos.z * 0.15;
    float gust = 0.7 + 0.3 * sin(phase);
    vec2 off = windParams.xy * (windParams.z * gust * sway);
    pos.x += off.x;
    pos.z += off.y;

    vec4 worldPos = matModel * vec4(pos, 1.0);
    fragPosition = vec3(worldPos);
    fragWorldPos = vec3(worldPos);
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(skinnedNormal, 1.0)));

    gl_Position = mvp * vec4(pos, 1.0);
}
