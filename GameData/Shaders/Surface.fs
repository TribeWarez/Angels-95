#version 330

// Surface.fs â€” LitFog.fs plus the UT99-style per-face surface properties.
//
// Why a separate shader instead of adding uniforms to LitFog.fs:
//   * Lighting.vs/LitFog.fs is used by every mesh, particle, pickup, projectile
//     and the view-model. Touching its output changes the whole game.
//   * The surface properties are UNIFORMS, i.e. per draw call, so a brush with
//     per-face differences needs its own draw anyway. A dedicated program means
//     only surface-flagged brushes pay for it.
//
// uSurface is a bitfield; see World/SurfaceFlags.hpp for the bit allocation. The
// flag VALUES are duplicated here as #defines - they must stay in step with the
// header, and tests/Surface.test.cpp asserts the two legacy bits (3 and 4) that
// shipped worlds depend on.

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec3 fragNormal;
in vec3 fragWorldPos;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

out vec4 finalColor;

#define MAX_LIGHTS 32
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

// --- per-surface uniforms ---------------------------------------------------
uniform int   uSurfaceFlags = 0;
uniform vec2  uPan          = vec2(0.0);   // texture units / second
uniform float uAlpha        = 1.0;
uniform float uAlphaCutoff  = 0.0;
uniform vec3  uGlow         = vec3(0.0);
uniform float uGlowScale    = 1.0;
// Gain applied by the full-bright branch (UNLIT / FAKE_LIT / FAKEBACKDROP).
// 1.0 means "the painted value exactly". The backdrop pass pushes 0.55, which is
// the gain the old DrawZoneGeometry uniform hack produced via ambient/10 - see
// OzoneLoader::DrawZoneGeometry. A per-pass scalar rather than a per-face one, so
// it is uploaded once per pass instead of once per draw.
uniform float uFullBright   = 1.0;

// Set by DrawGlowGeometry around its additive re-draw of a glowing face. When
// it is 1 the fragment stage emits ONLY the emissive term (see the SF_GLOW
// block below), because the halo is meant to bloom the glow, not to add the
// whole face a second time - that second add took a mid-grey texture to 1.2,
// clipped, and made every textured glow surface read as a white card.
uniform int   uHaloPass     = 0;

// --- baked material maps (Material/OzMaterial.hpp) --------------------------
// Same four uniforms as LitFog.fs; the two programs are hand-maintained copies
// and MUST stay in step or a surface-flagged brush disagrees with the room
// around it (see the ambient-leak note in AGENTS.md).
uniform sampler2D normalMap;
uniform int   uHasNormalMap = 0;
uniform float uNormalStrength = 1.0;
uniform sampler2D emissiveMap;
uniform int   uHasEmissive = 0;
uniform float uEmissiveScale = 1.0;

uniform float fogStart = 10.0;
uniform float fogEnd = 100.0;
uniform float fogDensity = 1.0;
uniform vec3 fogColor = vec3(0.7, 0.7, 0.8);
uniform float fogIntensity = 1.0;

// Must match oz::surface::SurfaceFlag in Source/World/SurfaceFlags.hpp
#define SF_INVISIBLE   (1 << 5)
#define SF_MASKED      (1 << 6)
#define SF_TRANSLUCENT (1 << 7)
#define SF_ALPHABLEND  (1 << 8)
#define SF_MODULATED   (1 << 9)
#define SF_TWO_SIDED   (1 << 10)
#define SF_UNLIT       (1 << 11)
#define SF_FAKE_LIT    (1 << 12)
#define SF_GLOW        (1 << 14)
#define SF_PAN_U       (1 << 18)
#define SF_PAN_V       (1 << 19)
#define SF_NO_FOG      (1 << 26)
#define SF_NO_SMOOTH   (1 << 25)
#define SF_FAKEBACKDROP (1 << 3)

bool HasFlag(int bit) { return (uSurfaceFlags & bit) != 0; }

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

// `phase` is the LIGHT's own clock (LightNode::phase, advanced once per frame by
// LitLightning_Animate), NOT a function of the uniform array index. It used to be
// `float(i) * 2.399` - the slot index - so a light's flicker identity changed
// whenever the per-frame distance sort reordered the array, and the flicker popped.
// Carrying the clock on the light makes the animation stable under reordering.
//
// `period` scales the authored flicker speed. It is uploaded so the `.ozls`
// `period` stat and LightNode::period actually reach the shader instead of being
// CPU-only, and so the frequencies below are multipliers on one clock rather than
// six unrelated literals.
vec3 applyLightEffect(vec3 lightColor, float intensity, int effect,
                      float phase, float period, float ndotl) {
    vec3 col = lightColor;
    float f = intensity;
    // period <= 0 means "no animation": a static light authored with period 0 would
    // otherwise divide by zero here.
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

// See LitFog.fs's applyNormalMap: derivative TBN, identical body on purpose.
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
    // U/V pan. The speed lives in the flag, not in the value, so a zero speed is
    // a hard no-op rather than a per-frame multiply.
    vec2 uv = fragTexCoord;
    if (HasFlag(SF_PAN_U) || HasFlag(SF_PAN_V)) {
        if (HasFlag(SF_PAN_U)) uv.x += uPan.x * uTime;
        if (HasFlag(SF_PAN_V)) uv.y += uPan.y * uTime;
        // Wrap instead of letting the offset grow without bound. uTime is seconds
        // since startup, so at 0.5 units/s a long session reaches ~1800 texels of
        // offset and texture() sampling precision visibly degrades long before
        // that. fract() keeps the sampled argument small. Guarded by the pan flags
        // so an un-panned surface takes the identical path it always did.
        uv = fract(uv);
    }

    vec4 baseColor = texture(texture0, uv);

    // Alpha test (masked). Cheaper than blending and needs no depth sorting,
    // which is why it is a separate flag from translucent.
    if (HasFlag(SF_MASKED) && uAlphaCutoff > 0.0 && baseColor.a < uAlphaCutoff) discard;

    float alpha = baseColor.a * uAlpha;

    vec3 normal = applyNormalMap(normalize(fragNormal), uv);
    if (HasFlag(SF_NO_SMOOTH)) normal = normal;   // reserved: flat shading hook

    vec3 viewDir = normalize(viewPos - fragPosition);
    vec3 lightAccum = vec3(0.0);
    vec3 specAccum = vec3(0.0);
    // In-scattered light for the fog tint - identical accumulation to LitFog.fs,
    // folded into the shading loop instead of a second MAX_LIGHTS pass. Before
    // this, this shader fogged with plain fogColor while LitFog.fs added a
    // torch tint, so a per-face brush disagreed with the wall beside it.
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

            // Fog in-scatter, same body/constants as LitFog.fs. Keep the two in
            // step: tests/Surface.test.cpp pins the shared fog maths.
            if (lights[i].type != LIGHT_DIRECTIONAL) {
                float ldist = distance(lights[i].position, fragPosition);
                if (!(lights[i].radius > 0.0 && ldist > lights[i].radius)) {
                    vec3 scat = applyLightEffect(lights[i].color.rgb, lights[i].intensity,
                                                 lights[i].effect, lights[i].phase,
                                                 lights[i].period, 1.0);
                    scatterAccum += scat * (1.0 / (1.0 + ldist * 0.1)) * 0.3;
                }
            }

            if (NdotL > 0.0) {
                vec3 halfDir = normalize(lightDir + viewDir);
                float NdotH = max(dot(normal, halfDir), 0.0);
                specAccum += effColor * pow(NdotH, 32.0) * attenuation * spotFactor;
            }
        }
    }

    vec4 amb = ambient;
    vec3 lit;

    // Two flat branches, then the real lit one.
    //
    //   UNLIT / FAKEBACKDROP
    //       Flat: the painted value, scaled by the PASS gain (uFullBright). Both
    //       share this because a painted backdrop's job is to be seen at its
    //       authored value - the only thing that distinguishes a backdrop is WHICH
    //       pass draws it, not how it is shaded.
    //
    //   FAKE_LIT
    //       Lit geometry with NO light contribution: takes the world's ambient
    //       term only, so it sits in the room's exposure but no
    //       directional/point/spot light touches it. This is what a signage panel
    //       wants - present in the room, not blown out by the sun.
    //
    // FAKE_LIT must NOT carry uFullBright. The ambient term is a floor
    // (0.1 / 10 == 0.01 by default), not an exposure reference, so multiplying it
    // by the backdrop gain turned a 0.55 backdrop into 0.0055 - a 100x darkening
    // of every shipped painted backdrop. Caught by rendering, not by reading.
    //
    // FAKE_LIT used to share UNLIT's branch, which made it a dead checkbox.
    if (HasFlag(SF_UNLIT) || HasFlag(SF_FAKEBACKDROP)) {
        lit = baseColor.rgb * colDiffuse.rgb * uFullBright;
    } else if (HasFlag(SF_FAKE_LIT)) {
        lit = baseColor.rgb * (amb.rgb / 10.0) * colDiffuse.rgb;
    } else {
        lit = baseColor.rgb * ((colDiffuse.rgb + specAccum) * lightAccum);
        lit += baseColor.rgb * (amb.rgb / 10.0) * colDiffuse.rgb;
    }

    // Modulated: multiply against what is already in the framebuffer (set up by
    // the renderer's BLEND_MULTIPLY state).
    if (HasFlag(SF_MODULATED)) lit *= colDiffuse.rgb;

    // Self-illumination. Added AFTER lighting so a glowing surface stays bright
    // in an unlit room, which is the whole point of the flag.
    if (HasFlag(SF_GLOW)) lit += uGlow * uGlowScale;

    // Baked emissive gate, same placement/order as LitFog.fs.
    if (uHasEmissive == 1)
        lit += texture(emissiveMap, uv).rgb * uEmissiveScale;

    // Halo pass: emit the emissive term only. Everything after this point (gamma,
    // fog, alpha) still applies, so a distant glow still recedes. Without this
    // the halo added the whole lit face a second time and every textured glow
    // surface clipped to white.
    if (uHaloPass == 1)
        lit = uGlow * uGlowScale;

    lit = pow(max(lit, vec3(0.0)), vec3(1.0 / 2.2));

    // Fog, unless the surface opts out. SF_ALPHABLEND opts out too - it is the
    // documented "blend but do not fog" flag (SurfaceFlags.hpp), which TRANSLUCENT
    // deliberately is NOT: a sheet of glass in a foggy corridor should still
    // recede, whereas an AlphaBlend surface is used for UI-ish decals and signage
    // that must stay legible at any distance. Before this distinction the two
    // flags were pixel-identical and the header comment was simply false.
    if (!HasFlag(SF_NO_FOG) && !HasFlag(SF_ALPHABLEND)) {
        float fogDist = length(viewPos - fragPosition);
        float fogFactor = fogFactorAt(fogDist) * fogIntensity;
        lit = mix(lit, fogColor + scatterAccum, fogFactor);
    }

    if (HasFlag(SF_ALPHABLEND) || HasFlag(SF_TRANSLUCENT))
        finalColor = vec4(lit, alpha);
    else
        finalColor = vec4(lit, 1.0);
}
