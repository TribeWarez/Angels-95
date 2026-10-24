#version 330

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
// GPU skinning inputs. raylib only enables these when its GPU-skinning build
// flag is on (a CPU-skinning build leaves them disabled and animVertices
// already holds the posed vertices); uSkinning picks the path per draw.
in vec4 vertexBoneIndices;
in vec4 vertexBoneWeights;

uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;

// 64 bones x 4 vec4 = 1024 uniform components, the GL 3.3 vertex minimum.
// Shipped rigs are tiny (weapons <= 9, player 60ish); a larger rig still works
// on the CPU-skinning path that the pinned raylib uses.
#define MAX_BONE_MATRICES 64
uniform mat4 boneMatrices[MAX_BONE_MATRICES];
uniform int uSkinning = 0;

out vec3 fragPosition;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
out vec3 fragWorldPos;    // for triplanar detail UV

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

    vec4 worldPos = matModel * skinnedPos;
    fragPosition = vec3(worldPos);
    fragWorldPos = vec3(worldPos);
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(skinnedNormal, 1.0)));

    gl_Position = mvp * skinnedPos;
}
