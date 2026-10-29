#version 330

// ---------------------------------------------------------------------------
// CRTWarp.fs - ZONE_CRT post-process pass (Phase 7.3).
//
// Drawn by oz::PostFX::Resolve as a normal raylib 2D pass over the graded frame
// (BeginShaderMode + DrawTexturePro), never as a hand-driven GL draw: the 2D
// batch binds the program, VAO, sampler and MVP correctly, and raylib 6.0's
// batch flush leaves glUseProgram(0) behind - see the manual-draw traps in
// AGENTS.md. One sampler, one uniform set, no texture-unit juggling.
//
// The whole effect is driven by uWarpIntensity, which PostFX lerps from the
// player's ZONE_CRT membership. At 0.0 the pass is SKIPPED by the caller, so
// this shader never needs a "do nothing" early-out.
// ---------------------------------------------------------------------------

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

uniform float uWarpIntensity;   // 0..1
uniform float uTime;            // seconds, for the roll/jitter

out vec4 finalColor;

// Barrel distortion about the screen centre.
//
// The UV is DIVIDED by (1 + k*r^2) rather than multiplied: a multiply bows the
// edges outward past [0,1] and leaves a black frame border, while a divide keeps
// the centre fixed and compresses the rim, which is what a CRT actually does
// (the raster is larger than the visible glass). `aspect` predivides x so a 21:9
// frame does not curl harder at the corners than a 4:3 one.
vec2 Barreled(vec2 uv, float k, float aspect) {
    vec2 c = uv - 0.5;
    c.x *= aspect;
    float r2 = dot(c, c);
    c /= (1.0 + k * r2);
    c.x /= aspect;
    return c + 0.5;
}

void main() {
    vec2 texSize = vec2(textureSize(texture0, 0));
    float aspect = texSize.x / max(texSize.y, 1.0);

    // Horizontal roll: a slow sine plus a faster wobble, both scaled by
    // intensity so the frame settles cleanly as the player leaves the zone.
    vec2 uv = fragTexCoord;

    // Rolling-wave jitter, evaluated per scanline so it reads as horizontal tear
    // rather than a global shake.
    float wob = sin(uv.y * 24.0 + uTime * 2.2) * 0.0012
              + sin(uv.y * 160.0 - uTime * 7.0) * 0.0004;
    uv.x += wob * uWarpIntensity;

    float k = 0.24 * uWarpIntensity;
    vec2 warped = Barreled(uv, k, aspect);

    // Chromatic aberration: R and B sample slightly further out along the radial
    // direction than G. Scaled by r^2 so the centre stays sharp and only the rim
    // fringes - the same term the barrel distortion uses.
    vec2 dir = warped - 0.5;
    float r2 = dot(dir, dir);
    float ca = 0.0022 * uWarpIntensity * (0.25 + r2);
    vec3 col;
    col.r = texture(texture0, warped + dir * ca).r;
    col.g = texture(texture0, warped).g;
    col.b = texture(texture0, warped - dir * ca).b;

    // Aperture grille. Deliberately ~3 pixels per band instead of 1: a 1-pixel
    // alternating pattern beats against the display's own pixel grid (moire) and
    // the band width is derived from the bound texture, never hardcoded.
    float band = 3.0;
    float grille = 0.5 + 0.5 * cos(warped.y * texSize.y * 6.2831853 / band);
    col *= 1.0 - 0.10 * uWarpIntensity * grille;

    // Phosphor glow near the centre plus a soft vignette at the rim.
    // `1.0 - smoothstep(lo, hi, r2)` rather than smoothstep(hi, lo, r2): GLSL
    // leaves the result UNDEFINED when edge0 >= edge1, so the reversed-edge form
    // is driver-dependent even though most implementations "just work".
    float vign = 1.0 - smoothstep(0.15, 0.85, r2);
    col *= mix(1.0, vign, 0.55 * uWarpIntensity);
    col *= 1.0 + 0.06 * uWarpIntensity * (1.0 - r2);

    finalColor = vec4(col, 1.0) * colDiffuse * fragColor;
}
