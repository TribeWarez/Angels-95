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
    // RGB is MULTIPLIED, alpha is multiplied: the instance colour is the lit
    // tint, the texture supplies the shape AND its own tint.
    //
    // This used to be `vec4(fragColor.rgb, fragColor.a * texel.a)` - only the
    // texture's alpha was read, so any authored `tex=` lost its colour and
    // rendered as a flat tint. The immediate fallback (DrawBillboard) multiplies
    // texture RGB, so the two paths disagreed for exactly the particles that
    // carry art: blood and gibs. The generated default sprite is white, which is
    // why the ambient emitters looked correct and hid the bug.
    finalColor = vec4(fragColor.rgb * texel.rgb, fragColor.a * texel.a);
}
