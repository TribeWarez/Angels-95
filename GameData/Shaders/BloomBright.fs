#version 330

// BloomBright.fs - threshold pass for the bloom chain.
//
// Soft-knee threshold: a hard cutoff makes bloom pop on as a surface crosses the
// threshold; the knee fades it in. `uKnee` is in the same 0..1-ish scene units as
// the colour, so it is not a physical exposure value.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform float uThreshold;
uniform float uKnee;

out vec4 finalColor;

void main()
{
    vec3 c = texture(texture0, fragTexCoord).rgb;
    float brightness = max(c.r, max(c.g, c.b));

    float knee = max(uKnee, 1e-4);
    float soft = clamp(brightness - uThreshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);

    float contribution = max(soft, brightness - uThreshold) / max(brightness, 1e-4);
    finalColor = vec4(c * contribution, 1.0);
}
