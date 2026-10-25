#version 330

// BloomBlur.fs - separable 9-tap Gaussian, run twice (H then V).
//
// `uDirection` is in UV units, so the same shader works at any resolution and the
// caller owns the radius. The weights are the standard 5-sample Gaussian
// approximation, which is indistinguishable from a full 9-tap at half res.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec2 uDirection;

out vec4 finalColor;

void main()
{
    vec3 sum = texture(texture0, fragTexCoord).rgb * 0.2270270270;
    sum += texture(texture0, fragTexCoord + uDirection * 1.3846153846).rgb * 0.3162162162;
    sum += texture(texture0, fragTexCoord - uDirection * 1.3846153846).rgb * 0.3162162162;
    sum += texture(texture0, fragTexCoord + uDirection * 3.2307692308).rgb * 0.0702702703;
    sum += texture(texture0, fragTexCoord - uDirection * 3.2307692308).rgb * 0.0702702703;
    finalColor = vec4(sum, 1.0);
}
