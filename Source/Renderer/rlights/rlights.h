/**********************************************************************************************
*
*   raylib.lights - Some useful functions to deal with lights data
*
*   CONFIGURATION:
*
*   #define RLIGHTS_IMPLEMENTATION
*       Generates the implementation of the library into the included file.
*       If not defined, the library is in header only mode and can be included in other headers 
*       or source files without problems. But only ONE file should hold the implementation.
*
*   LICENSE: zlib/libpng
*
*   Copyright (c) 2017-2023 Victor Fisac (@victorfisac) and Ramon Santamaria (@raysan5)
*
*   This software is provided "as-is", without any express or implied warranty. In no event
*   will the authors be held liable for any damages arising from the use of this software.
*
*   Permission is granted to anyone to use this software for any purpose, including commercial
*   applications, and to alter it and redistribute it freely, subject to the following restrictions:
*
*     1. The origin of this software must not be misrepresented; you must not claim that you
*     wrote the original software. If you use this software in a product, an acknowledgment
*     in the product documentation would be appreciated but is not required.
*
*     2. Altered source versions must be plainly marked as such, and must not be misrepresented
*     as being the original software.
*
*     3. This notice may not be removed or altered from any source distribution.
*
**********************************************************************************************/

#ifndef RLIGHTS_H
#define RLIGHTS_H

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------

// The light budget. This value is written into THREE places and must agree in
// all of them, because a mismatch is invisible at compile time and destroys lights
// at run time:
//
//   * here                                   (C++ submission)
//   * GameData/Shaders/Lights/LitFog.fs      (the world's program)
//   * GameData/Shaders/Surface.fs            (the surface program)
//
// It was 64 here while both shaders said 32. The submission loop therefore wrote
// slots 32..63, every one of which is a guaranteed-miss uniform lookup that the
// driver discards - so half of the "64 supported lights" reached the GPU as nothing,
// and WHICH half depended on the per-frame distance sort.
//
// The GLSL values cannot #include this header, so `tests/Surface.test.cpp` parses
// all three files and fails if they disagree. That test is the enforcement
// mechanism; this comment is why it exists.
//
// Changing this number means changing all three, and it changes the per-fragment
// cost of EVERY lit mesh, particle, pickup, projectile and the view-model, because
// both live shaders run an unrolled `for (i < MAX_LIGHTS)` loop per fragment.
#define MAX_LIGHTS  64

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------

// Light data

// Light type
typedef enum {
    LIGHT_DIRECTIONAL = 0,
    LIGHT_POINT,
    LIGHT_SPOT
} LightType;

// Light visual effect
typedef enum {
    LIGHT_EFFECT_NONE = 0,
    LIGHT_EFFECT_WATERY,
    LIGHT_EFFECT_TORCH,
    LIGHT_EFFECT_FIRE,
    LIGHT_EFFECT_LAMP
} LightEffect;

typedef struct {   
    int type;            // LightType
    bool enabled;
    Vector3 position;
    Vector3 target;
    Color color;
    float attenuation;
    float intensity;     // brightness multiplier. Applied ONCE, in the shader. Do
                         // NOT also pre-multiply it into `color` - that is how this
                         // engine used to render intensity squared.
    float radius;        // point/spot light range; <= 0 means unlimited
    // Spot cone half-angles as COSINES (cos(halfAngle)), NOT degrees. That is what
    // spotCone()'s smoothstep expects, what LightNode stores, and what the editor
    // round-trips (it edits degrees, converts on apply). They were documented as
    // degrees here while CreateLight seeded them as 15/45 - and nothing noticed,
    // because neither was ever uploaded: the shader degenerated to
    // smoothstep(0,0,x) == 1 and every spot light behaved as a bare point light.
    float innerCone;
    float outerCone;
    // Animation clock in SECONDS, uploaded so the shader's per-effect flicker is a
    // property of THE LIGHT rather than of its slot in the uniform array. The slot
    // changes whenever the per-frame distance sort reorders, and a slot-derived
    // flicker pops when it does.
    //
    // Seconds, not radians, and NOT pre-scaled by `period`: the shader owns the
    // conversion (`w = phase / period`). It used to be pre-divided on the CPU as
    // well, which squared the divisor - an authored period of 0.25 ran the torch
    // flicker at 1/0.0625 of the intended rate.
    float phase;
    // Flicker cycle length in seconds. <= 0 means "no animation". Authored by the
    // `.ozls` `period` stat and by OZONE `period=`.
    //
    // This field and its uniform location DID NOT EXIST until now, while BOTH live
    // shaders declared `float period;` and read it. A missing C member means a
    // missing GetShaderLocation and a missing SetShaderValue, so the uniform stayed
    // at its GLSL default of 0 - which the shaders treat as "no animation" - and
    // every torch / fire / water / lamp light rendered as a perfectly steady light
    // with a correct colour tint and zero flicker.
    //
    // `tests/Surface.test.cpp` now parses this header and both shaders and fails if
    // a field declared in the GLSL `Light` struct has no location fetched here.
    float period;
    int effect;          // LightEffect (0=none, 1=watery, 2=torch, 3=fire, 4=lamp)
    int hasFlare;        // lens flare requested (drawn CPU-side in Core.hpp)
    int hasCorona;       // corona glow requested (drawn CPU-side in Core.hpp)
    
    // Shader locations
    int enabledLoc;
    int typeLoc;
    int positionLoc;
    int targetLoc;
    int colorLoc;
    int attenuationLoc;
    int intensityLoc;
    int radiusLoc;
    int innerConeLoc;
    int outerConeLoc;
    int phaseLoc;
    int periodLoc;
    int effectLoc;
} Light;

#ifdef __cplusplus
extern "C" {            // Prevents name mangling of functions
#endif

//----------------------------------------------------------------------------------
// Module Functions Declaration
//----------------------------------------------------------------------------------
Light CreateLight(int type, Vector3 position, Vector3 target, Color color, Shader shader);   // Create a light and get shader locations
void UpdateLightValues(Shader shader, Light light);         // Send light properties to shader

#ifdef __cplusplus
}
#endif

#endif // RLIGHTS_H


/***********************************************************************************
*
*   RLIGHTS IMPLEMENTATION
*
************************************************************************************/

#if defined(RLIGHTS_IMPLEMENTATION)

#include "raylib.h"

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------
// ...

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
// ...

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
static int lightsCount = 0;    // Current amount of created lights

//----------------------------------------------------------------------------------
// Module specific Functions Declaration
//----------------------------------------------------------------------------------
// ...

//----------------------------------------------------------------------------------
// Module Functions Definition
//----------------------------------------------------------------------------------

// Create a light and get shader locations
Light CreateLight(int type, Vector3 position, Vector3 target, Color color, Shader shader)
{
    Light light = { 0 };

    if (lightsCount < MAX_LIGHTS)
    {
        light.enabled = true;
        light.type = type;
        light.position = position;
        light.target = target;
        light.color = color;
        light.intensity = 1.0f;
        light.radius = 50.0f;
        // Cosines, matching LightNode's defaults (0.95 ~ 18 deg, 0.80 ~ 37 deg)
        // and spotCone()'s smoothstep. These were 15/45 here, documented as
        // degrees - a third convention, in a third place.
        light.innerCone = 0.95f;
        light.outerCone = 0.80f;
        light.phase = 0.0f;
        light.period = 1.0f;
        light.effect = LIGHT_EFFECT_NONE;

        // NOTE: Lighting shader naming must be the provided ones
        light.enabledLoc = GetShaderLocation(shader, TextFormat("lights[%i].enabled", lightsCount));
        light.typeLoc = GetShaderLocation(shader, TextFormat("lights[%i].type", lightsCount));
        light.positionLoc = GetShaderLocation(shader, TextFormat("lights[%i].position", lightsCount));
        light.targetLoc = GetShaderLocation(shader, TextFormat("lights[%i].target", lightsCount));
        light.colorLoc = GetShaderLocation(shader, TextFormat("lights[%i].color", lightsCount));
        light.intensityLoc = GetShaderLocation(shader, TextFormat("lights[%i].intensity", lightsCount));
        light.radiusLoc = GetShaderLocation(shader, TextFormat("lights[%i].radius", lightsCount));
        light.innerConeLoc = GetShaderLocation(shader, TextFormat("lights[%i].innerCone", lightsCount));
        light.outerConeLoc = GetShaderLocation(shader, TextFormat("lights[%i].outerCone", lightsCount));
        light.phaseLoc = GetShaderLocation(shader, TextFormat("lights[%i].phase", lightsCount));
        light.periodLoc = GetShaderLocation(shader, TextFormat("lights[%i].period", lightsCount));
        light.effectLoc = GetShaderLocation(shader, TextFormat("lights[%i].effect", lightsCount));

        UpdateLightValues(shader, light);
        
        lightsCount++;
    }

    return light;
}

// Send light properties to shader
void UpdateLightValues(Shader shader, Light light)
{
    SetShaderValue(shader, light.enabledLoc, &light.enabled, SHADER_UNIFORM_INT);
    SetShaderValue(shader, light.typeLoc, &light.type, SHADER_UNIFORM_INT);

    float position[3] = { light.position.x, light.position.y, light.position.z };
    SetShaderValue(shader, light.positionLoc, position, SHADER_UNIFORM_VEC3);

    float target[3] = { light.target.x, light.target.y, light.target.z };
    SetShaderValue(shader, light.targetLoc, target, SHADER_UNIFORM_VEC3);

    float color[4] = { (float)light.color.r/255.0f, (float)light.color.g/255.0f, 
                       (float)light.color.b/255.0f, (float)light.color.a/255.0f };
    SetShaderValue(shader, light.colorLoc, color, SHADER_UNIFORM_VEC4);

    SetShaderValue(shader, light.intensityLoc, &light.intensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, light.radiusLoc, &light.radius, SHADER_UNIFORM_FLOAT);

    // These three were declared in the shader's Light struct and read by spotCone()
    // and applyLightEffect(), but nothing ever uploaded them - so they stayed 0 and
    // smoothstep(0.0, 0.0, cosAngle) collapsed to 1.0, making every spot light
    // omnidirectional. That is why the whole inner_cone/outer_cone authoring path
    // (OZONE args 11/12, kLightStats, the Properties panel's degree conversion, the
    // gizmo cone footprint) described geometry the renderer never produced.
    SetShaderValue(shader, light.innerConeLoc, &light.innerCone, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, light.outerConeLoc, &light.outerCone, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, light.phaseLoc, &light.phase, SHADER_UNIFORM_FLOAT);
    // `period` gates the entire flicker in both shaders (`w = (period > 0.0) ?
    // phase / period : 0.0`), so an un-uploaded 0.0 here means "no animation" for
    // every light in the world, not "one light looks wrong".
    SetShaderValue(shader, light.periodLoc, &light.period, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, light.effectLoc, &light.effect, SHADER_UNIFORM_INT);
}

#endif // RLIGHTS_IMPLEMENTATION
