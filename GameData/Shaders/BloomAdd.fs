#version 330

// BloomAdd.fs - additive composite of the blurred bloom over the graded frame.
//
// Kept as its own pass with ONE sampler so it never needs raylib's multi-texture
// material binding: the 2D batch binds the drawn texture to texture0, and
// additive blend does the rest.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform float uStrength;

out vec4 finalColor;

void main()
{
    finalColor = vec4(texture(texture0, fragTexCoord).rgb * uStrength, 1.0);
}
