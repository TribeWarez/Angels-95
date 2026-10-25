#version 330

// SkyPortal.fs - draws a SURF_PORTAL face by sampling the pre-rendered sky view
// in SCREEN SPACE.
//
// Why screen space and not projective/parallax UVs: this is a SKY portal. The
// sky is at infinity, so clouds must not swim when the player strafes - which is
// exactly the UE1/2 SkyZoneInfo behaviour (the sky zone camera sits at the zone
// anchor and the sky box is enormous, so translation is invisible). The face
// itself is the mask, and the depth test against world geometry provides
// occlusion, so no stencil buffer or portal recursion is involved.
//
// `uViewport` is the size of the framebuffer being drawn into, NOT the window:
// the client renders 3D into a RenderTexture (`Target`, which can be
// resolution-scaled), so GetScreenWidth() would mis-map the sample there. The
// caller passes the real bound-framebuffer size.
//
// The view already contains the world's fog, applied at the sky's own distance,
// so this pass applies no lighting and no second fog term.

in vec2 fragTexCoord;      // unused: the sample is screen-space

uniform sampler2D texture0;   // the sky view, bound by raylib from the material
uniform vec2 uViewport;

out vec4 finalColor;

void main()
{
    vec2 uv = gl_FragCoord.xy / uViewport;
    finalColor = vec4(texture(texture0, uv).rgb, 1.0);
}
