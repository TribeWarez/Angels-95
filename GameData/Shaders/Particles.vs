#version 330

// Particles.vs - instanced billboard vertex stage for OzParticleSimulationManager.
//
// The immediate-mode path drew one DrawBillboard per particle, which is a full
// 2D texture bind + quad per particle and does not scale. This expands a single
// 4-vertex quad into N camera-facing quads with one draw call, using instanced
// vertex attributes:
//
//   attribute 0  vertexPosition  (per-vertex,  divisor 0) - the unit quad corner
//   attribute 11 instCenter      (per-instance, divisor 1)
//   attribute 12 instSize        (per-instance, divisor 1)
//   attribute 13 instColor       (per-instance, divisor 1) - already lit on CPU
//
// Camera-facing: the quad's XY is rotated into the camera's right/up plane. The
// view-projection (mvp) does the perspective divide, so we compute world space
// and let mvp project it - matching how every other vertex stage in the engine
// works, and keeping this correct under any projection raylib is configured for.
//
// COLOR IS LIT ON THE CPU (ApplyLighting) and arrives pre-multiplied per
// instance. Doing the light loop here would mean uploading the 32-light block
// again for a program that otherwise needs no uniforms, and the CPU path was
// already correct and tested; see OzParticleSimulationManager::ApplyLighting.

// The layout() qualifiers are LOAD-BEARING, not decoration. The VAO is built by
// hand in EnsureInstancedResources() with rlSetVertexAttribute() at exactly these
// locations, and GLSL assigns unspecified attribute locations in whatever order
// the linker prefers. Without the qualifiers the shader and the VAO disagree and
// every particle comes out as garbage geometry - silently, because a shader that
// links with mismatched attributes still compiles and still runs.
//
// (raylib's own default shader omits them because rlgl happens to bind the
// conventional 0..3 and the linker happens to assign the same. Neither "happens
// to" holds for a hand-built VAO.)
layout(location = 0)  in vec3 vertexPosition;   // per-vertex,  divisor 0
layout(location = 11) in vec3 instCenter;       // per-instance, divisor 1
layout(location = 12) in float instSize;        // per-instance, divisor 1
layout(location = 13) in vec4 instColor;        // per-instance, divisor 1, pre-lit

uniform mat4 mvp;
uniform vec3 camRight;
uniform vec3 camUp;

out vec2 fragTexCoord;
out vec4 fragColor;

void main()
{
    // Unit quad is XY in [-0.5, 0.5]; scale by the particle's current size.
    vec2 corner = vertexPosition.xy * instSize;

    // Camera-facing offset in world space.
    vec3 world = instCenter
               + camRight * corner.x
               + camUp    * corner.y;

    fragTexCoord = vertexPosition.xy + vec2(0.5, 0.5);
    fragColor = instColor;
    gl_Position = mvp * vec4(world, 1.0);
}
