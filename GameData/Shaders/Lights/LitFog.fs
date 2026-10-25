#version 330

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec3 fragNormal;
in vec3 fragWorldPos;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

// Optional detail texture (bind at application level)
//
// GATED on uHasDetail, which no CPU code uploads yet. An unbound sampler2D
// samples texture unit 0 - the DIFFUSE map - so this block multiplied every
// mesh/particle/pickup/projectile/view-model fragment's albedo by the diffuse
// texture resampled at fragWorldPos.xz * 16, i.e. squared albedo, on every lit
// draw. Leave the gate at its default 0 until an uploader binds a detail map.
uniform sampler2D DetailTexture;
uniform vec2 DetailScale = vec2(16.0);
uniform float DetailBlend = 0.3;
uniform int uHasDetail = 0;

// --- baked material maps (Material/OzMaterial.hpp) --------------------------
// Defaults keep every existing draw identical: no material = no normal/emissive
// sampling at all. The C++ uploads these per draw in oz::Mesh::BindMaterial.
uniform sampler2D normalMap;
uniform int   uHasNormalMap = 0;
uniform float uNormalStrength = 1.0;
uniform sampler2D emissiveMap;
uniform int   uHasEmissive = 0;
uniform float uEmissiveScale = 1.0;

out vec4 finalColor;

#define MAX_LIGHTS 64
#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2

#define LIGHT_EFFECT_NONE   0
#define LIGHT_EFFECT_WATERY 1
#define LIGHT_EFFECT_TORCH  2
#define LIGHT_EFFECT_FIRE   3
#define LIGHT_EFFECT_LAMP   4

struct Light {
    int enabled;
    int type;
    vec3 position;
    vec3 target;
    vec4 color;
    float intensity;
    float radius;
    float innerCone;
    float outerCone;
    // The light's own animation clock and flicker speed. Both were absent while
    // the shader declared them; `phase` was derived from the uniform array index
    // instead, so a light's flicker identity changed whenever the per-frame
    // distance sort reordered the array.
    float phase;
    float period;
    int effect;
};

uniform Light lights[MAX_LIGHTS];
uniform vec4 ambient;
uniform vec3 viewPos;
uniform float uTime;

uniform float fogStart = 10.0;
uniform float fogEnd = 100.0;
uniform float fogDensity = 1.0;
uniform vec3 fogColor = vec3(0.7, 0.7, 0.8);
uniform float fogIntensity = 1.0;

float hash11(float p) {
    return fract(sin(p * 127.1 + 311.7) * 43758.5453);
}

float lightAttenuation(vec3 lightPos, vec3 fragPos, float radius) {
    float dist = distance(lightPos, fragPos);
    if (radius > 0.0 && dist > radius) return 0.0;
    float d = dist;
    return 1.0 / (1.0 + 0.09 * d + 0.032 * d * d);
}

float spotCone(vec3 lightPos, vec3 lightTarget, vec3 fragPos, float innerCone, float outerCone) {
    vec3 spotDir = normalize(lightTarget - lightPos);
    vec3 toFrag = normalize(fragPos - lightPos);
    float cosAngle = dot(-toFrag, spotDir);
    return smoothstep(outerCone, innerCone, cosAngle);
}

// Schlick Fresnel approximation
float fresnelSchlick(float cosTheta, float F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 applyLightEffect(vec3 lightColor, float intensity, int effect,
                      float phase, float period, float ndotl) {
    vec3 col = lightColor;
    float f = intensity;
    // period <= 0 means "no animation": a static light authored with period 0
    // would otherwise divide by zero here.
    float w = (period > 0.0) ? (phase / period) : 0.0;

    if (effect == LIGHT_EFFECT_TORCH) {
        float flicker = 0.85 + 0.15 * sin(w * 17.0) * cos(w * 13.0);
        f *= flicker;
        col *= vec3(1.0, 0.85, 0.6);
    }
    else if (effect == LIGHT_EFFECT_FIRE) {
        float pulse = 0.7 + 0.3 * sin(w * 5.0) * sin(w * 7.3 + 1.2);
        float noise = hash11(w * 4.0 + floor(w * 10.0)) * 0.2;
        f *= pulse + noise;
        col *= vec3(1.4, 0.7, 0.3);
    }
    else if (effect == LIGHT_EFFECT_WATERY) {
        float shimmer = 0.9 + 0.1 * sin(w * 3.0);
        f *= shimmer;
        col *= vec3(0.8, 0.9, 1.2);
    }
    else if (effect == LIGHT_EFFECT_LAMP) {
        float pulse = 0.95 + 0.05 * sin(w * 2.0 + 1.5);
        f *= pulse;
        col *= vec3(1.1, 0.9, 0.7);
    }

    return col * f;
}

// ---------------------------------------------------------------------------
// Distance haze.
//
// `fogDensity` is an EXPONENTIAL RATE per world unit, not a multiplier on a
// linear ramp. The values these levels shipped with (0.0012..0.003 in every
// GameData/Worlds/*/skyzone_*.ozls) were only coherent as a rate, which is what
// gave the bug away:
//
//   old formula  clamp((d - start) / (end - start), 0, 1) * density
//               -> 0.2% haze at 100 units. Effectively fog OFF in all six
//                  shipped worlds, at any distance.
//
//   this formula 1 - exp(-d * density)
//               -> 12% at 62 units, 20% at 100, 48% at 300, 73% at 600.
//
// That is the whole point of atmospheric perspective, and its absence is why the
// painted backdrops read as flat cardboard: nothing separated distant scenery
// from the wall it was pasted behind, so a 62-unit-away backdrop and the 8-unit
// wall in front of it rendered at the same crispness.
//
// `fogStart` is a hard floor (nothing within it is fogged). `fogEnd` ramps the
// exponential to full strength so the far plane saturates instead of crawling
// toward 1 asymptotically. Both knobs stay live.
//
// NOTE: LitFog.fs and Surface.fs must keep IDENTICAL fog maths - a surface-flagged
// brush next to a lit wall that disagrees with it in the same frame is the exact
// failure mode of the ambient leak and the backdrop hack documented in AGENTS.md.
float fogFactorAt(float dist) {
    float d = max(dist - fogStart, 0.0);
    float expo = 1.0 - exp(-d * fogDensity);
    // The ramp is a CEILING, not a multiplier. Multiplying (expo * ramp) silently
    // halved the haze over exactly the range that matters - the mid-field - and
    // made fogEnd read as if it were an authorable strength knob.
    float ramp = clamp(d / max(fogEnd - fogStart, 0.001), 0.0, 1.0);
    return min(expo, ramp);
}

// Tangent-space normal map. raylib's glTF loader does not give the engine a
// tangent attribute, so build a TBN from screen-space derivatives of the world
// position and UV. Returns the geometric normal unchanged when no material is
// bound or the derivatives degenerate (flat/degenerate UVs), which is what keeps
// every non-material surface pixel-identical to before.
vec3 applyNormalMap(vec3 N, vec2 uv) {
    if (uHasNormalMap == 0) return N;
    vec3 dp1 = dFdx(fragWorldPos);
    vec3 dp2 = dFdy(fragWorldPos);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float m = max(dot(T, T), dot(B, B));
    if (m < 1e-10) return N;
    float inv = inversesqrt(m);
    vec3 n = texture(normalMap, uv).xyz * 2.0 - 1.0;
    n.xy *= uNormalStrength;
    return normalize(mat3(T * inv, B * inv, N) * n);
}

void main()
{
    vec4 baseColor = texture(texture0, fragTexCoord);

    // Apply detail texture using world-space UV (only when a detail map is
    // actually bound - see uHasDetail above).
    if (uHasDetail == 1 && DetailBlend > 0.0) {
        vec2 detailUV = fragWorldPos.xz * DetailScale;
        vec4 detailColor = texture(DetailTexture, detailUV);
        baseColor.rgb = mix(baseColor.rgb, baseColor.rgb * detailColor.rgb, DetailBlend);
    }

    vec3 normal = applyNormalMap(normalize(fragNormal), fragTexCoord);
    vec3 viewDir = normalize(viewPos - fragPosition);
    vec3 lightAccum = vec3(0.0);
    vec3 specAccum = vec3(0.0);
    // In-scattered light for the fog tint, accumulated in the SAME loop below.
    vec3 scatterAccum = vec3(0.0);

    for (int i = 0; i < MAX_LIGHTS; i++)
    {
        if (lights[i].enabled == 1)
        {
            vec3 lightDir = vec3(0.0);
            float attenuation = 1.0;
            float spotFactor = 1.0;

            if (lights[i].type == LIGHT_DIRECTIONAL)
                lightDir = -normalize(lights[i].target - lights[i].position);

            if (lights[i].type == LIGHT_POINT) {
                lightDir = normalize(lights[i].position - fragPosition);
                attenuation = lightAttenuation(lights[i].position, fragPosition, lights[i].radius);
            }

            if (lights[i].type == LIGHT_SPOT) {
                lightDir = normalize(lights[i].position - fragPosition);
                attenuation = lightAttenuation(lights[i].position, fragPosition, lights[i].radius);
                spotFactor = spotCone(lights[i].position, lights[i].target, fragPosition,
                                      lights[i].innerCone, lights[i].outerCone);
            }

            float NdotL = max(dot(normal, lightDir), 0.0);
            vec3 effColor = applyLightEffect(lights[i].color.rgb, lights[i].intensity,
                                              lights[i].effect, lights[i].phase,
                                              lights[i].period, NdotL);

            lightAccum += effColor * NdotL * attenuation * spotFactor;

            // In-scattered light for the fog tint. This used to be a SECOND
            // full MAX_LIGHTS loop after the gamma step, and Surface.fs had no
            // equivalent - so a per-face surface brush fogged with plain
            // fogColor while the lit wall beside it fogged with a torch tint.
            // Same formula and constants as the old block; keep Surface.fs in
            // step (tests/Surface.test.cpp enforces the shared constants).
            if (lights[i].type != LIGHT_DIRECTIONAL) {
                float ldist = distance(lights[i].position, fragPosition);
                if (!(lights[i].radius > 0.0 && ldist > lights[i].radius)) {
                    vec3 scat = applyLightEffect(lights[i].color.rgb, lights[i].intensity,
                                                 lights[i].effect, lights[i].phase,
                                                 lights[i].period, 1.0);
                    scatterAccum += scat * (1.0 / (1.0 + ldist * 0.1)) * 0.3;
                }
            }

            // Blinn-Phong specular
            if (NdotL > 0.0) {
                vec3 halfDir = normalize(lightDir + viewDir);
                float NdotH = max(dot(normal, halfDir), 0.0);
                float spec = pow(NdotH, 32.0);

                // Fresnel: stronger specular at grazing angles
                float F = fresnelSchlick(max(dot(viewDir, halfDir), 0.0), 0.04);
                specAccum += effColor * spec * F * attenuation * spotFactor;
            }
        }
    }

    vec3 litColor = baseColor.rgb * ((colDiffuse.rgb + specAccum) * lightAccum);
    litColor += baseColor.rgb * (ambient.rgb / 10.0) * colDiffuse.rgb;

    // Baked emissive gate: added BEFORE gamma so an authored glow is an additive
    // light contribution rather than a washed-out overlay.
    if (uHasEmissive == 1)
        litColor += texture(emissiveMap, fragTexCoord).rgb * uEmissiveScale;

    // Gamma
    litColor = pow(litColor, vec3(1.0 / 2.2));

    // Fog.
    float fogDist = length(viewPos - fragPosition);
    float fogFactor = fogFactorAt(fogDist) * fogIntensity;

    // Light-influenced fog: the in-scatter was accumulated in the shading loop
    // above (there used to be a second full light loop here purely for this).
    vec3 finalFogColor = fogColor + scatterAccum;
    vec3 finalLit = mix(litColor, finalFogColor, fogFactor);

    finalColor = vec4(finalLit, baseColor.a);
}
