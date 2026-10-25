#version 330

// SkyPortal.vs - draws a SURF_PORTAL face at its own world position. The
// fragment stage does not use the interpolated UV (it samples the sky view by
// screen position), but the attribute is declared so the face mesh's layout
// matches every other surface shader and raylib can bind it without complaints.

in vec3 vertexPosition;
in vec2 vertexTexCoord;

uniform mat4 mvp;

out vec2 fragTexCoord;

void main()
{
    fragTexCoord = vertexTexCoord;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
