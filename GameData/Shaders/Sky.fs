#version 330

// Sky.fs - horizon fog for the skybox cube.
//
// WHY THIS EXISTS
//
// World geometry fogs, the skybox did not. The two programs disagree where the
// world's most distant geometry ends and the sky begins, so there was a hard
// horizontal line across every outdoor view: crisp, fully-hazed geometry below
// it and an untouched sky above it. With a painted backdrop behind that line the
// result read as cardboard scenery pasted onto the sky, which is the specific
// look the FAKEBACKDROP flag exists to avoid.
//
// THE FIX
//
// Two terms, multiplied:
//
//   1. HOW MUCH fog there is at this fragment's distance. This is fogFactorAt(),
//      copied VERBATIM from LitFog.fs / Surface.fs. Reusing the real function is
//      what makes the seam close by construction: at the horizon the sky and the
//      geometry behind it evaluate the same formula at the same distance, so they
//      agree on the colour no matter what fogStart/fogEnd/fogDensity a level
//      authors. A flat "blend fully at the horizon" would only agree for levels
//      whose fog is already near-opaque at 700 units - and would over-haze a clear
//      day's sky, replacing one mismatch with a different one.
//
//   2. WHERE on the sky we are. The sky cube's side faces are vertical planes
//      through the camera's own height, so the true horizon is always at
//      dir.y == 0 and the quantity that distinguishes "overhead sky" from "haze
//      at the skyline" is the view ray's elevation. Above kSkyHorizonBlend the
//      artwork is untouched; below it the fog term wins.
//
// The product is what closes the seam: term 1 alone would fog the whole sky
// uniformly (it is a distance term, and the cube is at a fixed distance), and term
// 2 alone would be wrong at any fog density other than "maximal".
//
// With fogDensity 0, term 1 is 0, so the sky renders EXACTLY as it did before
// this file existed. That is the fallback path, not a special case.
//
// GAMMA
//
// Deliberately NOT gamma-corrected, which is pre-existing behaviour and is
// preserved: the surrounding world geometry IS gamma-corrected (LitFog.fs applies
// pow(1/2.2)) and the fog colour is mixed in after gamma on both sides. Changing
// the sky's transfer function here would MOVE the horizon rather than remove it,
// because the entire point is to meet that post-gamma fog colour.
//
// Uniform names match LitFog.fs / Surface.fs so OzoneLoader can publish the same
// world state to all three programs. Keep fogFactorAt() in sync with both copies.

in vec2 fragTexCoord;
in vec3 fragWorldPos;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 uCamPos;

uniform vec3  fogColor;
uniform float fogStart;
uniform float fogEnd;
uniform float fogDensity;
uniform float fogIntensity;
// Sine of the elevation at which the sky is clear again; ~0.10 is about 6 degrees,
// which is roughly how high a haze layer actually sits.
uniform float uSkyHorizonBlend;

out vec4 finalColor;

// Verbatim from LitFog.fs / Surface.fs. Do not diverge - see the header.
float fogFactorAt(float dist) {
    float d = max(dist - fogStart, 0.0);
    float expo = 1.0 - exp(-d * fogDensity);
    float ramp = clamp(d / max(fogEnd - fogStart, 0.001), 0.0, 1.0);
    return min(expo, ramp);
}

void main()
{
    vec4 sky = texture(texture0, fragTexCoord) * colDiffuse;

    vec3 toFrag = fragWorldPos - uCamPos;
    float dist = length(toFrag);

    // Term 1: the world's own fog, at this fragment, on the same curve the
    // geometry around it uses. This is the half that guarantees the seam closes.
    float fog = clamp(fogFactorAt(dist) * fogIntensity, 0.0, 1.0);
    // The sky is drawn FIRST with the depth mask off, so it is the background:
    // nothing is ever blended through it and it must be opaque. The shipped sky
    // panoramas are RGBA with alpha 0 across their LOWER HALF (the artist's way
    // of saying "the sky is the top part"), so forwarding sky.a made the whole
    // box transparent below the horizon and the clear colour - black - showed in
    // every gap between the world's silhouette and the horizon. That read as
    // "the world is visible through the walls" and "the sky has holes in it".
    // Keep the RGB, force the alpha.
    if (fog <= 0.0) {
        finalColor = vec4(sky.rgb, 1.0);
        return;
    }

    // Term 2: elevation falloff. 1.0 straight up, 0.0 at the horizon, negative
    // below it (where the fog is total, since you are looking through the most
    // atmosphere). clamp() floors it at 0 rather than letting blend exceed 1.
    float up = normalize(toFrag).y;
    float clear = clamp(up / max(uSkyHorizonBlend, 0.001), 0.0, 1.0);

    finalColor = vec4(mix(sky.rgb, fogColor, fog * (1.0 - clear)), 1.0);
}