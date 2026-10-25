#version 330

// Grade.fs - the display-space grade applied when resolving the HDR scene.
//
// The world shaders already gamma-correct their own output, so this works in
// display space: exposure, contrast, saturation, tint, vignette. It deliberately
// does NOT tonemap - the scene is not rendered in linear HDR yet; the float
// target exists so bloom has headroom and so a tonemap can be added later
// without re-plumbing the pipeline.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform float uExposure;
uniform float uContrast;
uniform float uSaturation;
uniform float uVignette;
uniform vec3  uTint;

out vec4 finalColor;

void main()
{
    vec3 c = texture(texture0, fragTexCoord).rgb * uExposure;

    // Contrast around mid-grey, not around black (which would only brighten).
    c = (c - 0.5) * uContrast + 0.5;

    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, uSaturation);

    c *= uTint;

    // Radial vignette from the UV centre; 2x keeps the falloff gentle at the edge.
    vec2 d = fragTexCoord - vec2(0.5);
    c *= 1.0 - uVignette * dot(d, d) * 2.0;

    finalColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
