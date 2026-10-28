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

    // Height-weighted sway: 0 at the mesh base (so trunks and roots stay
    // planted) and 1 at the top, growing quadratically with height.
    float h = clamp((pos.y - windBaseY) / max(windHeight, 0.001), 0.0, 1.0);
    float sway = h * h;

    // Two motion layers with SPATIAL phase, so neighbouring vertices move at
    // different times and a foliage card bends instead of translating as a rigid
    // sheet: a slow main sway plus a smaller, faster gust. Previously the phase
    // was a single time term, which moved every vertex of a card in lockstep.
    float phase1 = windTime * windParams.w + pos.x * 0.7 + pos.z * 0.5;
    float phase2 = windTime * windParams.w * 1.7 + pos.x * 1.3 - pos.z * 0.9;
    // Clamped at 0: 0.65 - 0.35 - 0.35 could go slightly negative, which flipped
    // the sway direction for a frame - a visible pop at the gust's turning point.
    float gust = clamp(0.65 + 0.35 * sin(phase1) + 0.35 * sin(phase2), 0.0, 1.35);

    // windParams.z is a DISPLACEMENT IN WORLD UNITS, not a breeze rating (see the
    // WindZone section of AGENTS.md): author 0.2-0.5. A mistyped 5-7 shears a
    // hedge flat onto the ground, which reads as a broken asset.
    vec2 off = windParams.xy * (windParams.z * gust * sway);

    // Last-resort guard so a typo cannot fling geometry across the map. Clamp the
    // LENGTH, not each axis: a per-axis clamp bends the displacement toward the
    // diagonal at the limit.
    float offLen = length(off);
    const float kMaxDisplacement = 0.8;
    if (offLen > kMaxDisplacement) off *= kMaxDisplacement / offLen;

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
