// ============================================================================
// Subsystems/Selection.hpp
//
// Picking, selection and the per-entity delete/duplicate operations.
//
// A REAL translation unit as of Phase F (the .cpp is still a unity fragment for now;
// this header exists now because PropsApply.cpp needs two of its helpers and cannot
// reach into a fragment).
// ============================================================================
#ifndef ANGEL_ED_SUBSYSTEMS_SELECTION_HPP
#define ANGEL_ED_SUBSYSTEMS_SELECTION_HPP

#include "../Core/EditorState.hpp"

#include "raylib.h"
#include <vector>

struct OzoneRenderable;

// The ONE rotation-aware world box of a brush, using the same composition the
// renderer, collision and picker use (Scale then RotateY about the brush origin,
// then translate). Before this, the picker/pivot used RotatedAabbY while the
// per-frame gizmo write-back used the UNROTATED mesh box centre, so a rotated
// brush slid/jittered as the gizmo rewrote its position every frame.
BoundingBox BrushWorldBox(const OzoneRenderable& r);
Vector3      BrushCenter(const OzoneRenderable& r);

// Find a playerstart by its id. Used by the properties apply to write position and yaw.
PlayerStartNode* FindPlayerStartById(int id);

// Clamp an int into [lo, hi], logging when it had to. Shared with the properties apply
// so a row cannot write an out-of-range value by whatever route it was typed.
int ClampPropInt(int v, int lo, int hi);

// Publish `sel` as the gizmo's current transform. The frame loop syncs the gizmo
// back into the live selection every frame, so a subsystem that edits an entity
// directly (PropsApply) MUST call this afterwards or the next frame overwrites
// the edit with the pre-edit transform.
void SnapGizmoToSelection(const EditorSelection& sel);

// Centre the viewport on the current selection, preserving the current view
// direction and distance. Used by the World Hierarchy's Frame Selection item.
void FocusSelectionInViewport();

#endif // ANGEL_ED_SUBSYSTEMS_SELECTION_HPP