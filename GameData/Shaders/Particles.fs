#version 330

// Particles.fs - samples the particle atlas and multiplies the per-instance
// (already CPU-lit) colour. Alpha blend is set by the C++ side (BLEND_ALPHA),
// matching the immediate-mode path so an emitter looks identical either way.

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;

out vec4 finalColor;

void main()
{
    vec4 texel = texture(texture0, fragTexCoord);
    // Alpha comes from the texture, colour from the lit instance colour.
    finalColor = vec4(fragColor.rgb, fragColor.a * texel.a);
}
