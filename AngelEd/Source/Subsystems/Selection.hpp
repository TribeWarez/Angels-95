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

// Put `sel`'s world CENTRE at `centre`. This is the ONE definition of "move an
// entity", used both for the primary selection (which the gizmo tracks) and for
// every additional member of a multi-selection, so a Shift-selected set translates
// as one rigid group. UnrealEd's MoveActors applies the same Delta to every
// bSelected actor; before this the extra members only ever had their orange
// outline moved, never the entity.
//
// `applyTool` also writes the gizmo's SCALE and YAW into the entity. It is true
// only for the PRIMARY: the gizmo carries one scale and one yaw, so stamping them
// onto every member would flatten a mixed selection into a copy of the primary's
// rotation and size.
void MoveSelectionCentre(const EditorSelection& sel, Vector3 centre, bool applyTool);

// Clipboard. Copies EVERY entity in the selection (g_selSet, or the primary when
// the set has one entry) into a type-tagged record set, and pastes it back at a
// small offset with NEW ids, SELECTING the result - the same contract as
// UnrealEd's edactCopySelected / edactPasteSelected. Ctrl+C / Ctrl+V in the frame
// loop, and Duplicate (Ctrl+D) is copy-then-paste.
void CopySelectedEntities();
void PasteEntities();

// Apply the Texture Manager's active texture to the whole selection. Shared by the
// viewport right-click menu and the WorldGraph menu; both route into the ONE
// apply implementation in Core/EditorDispatcher.cpp.
void ApplyActiveTextureToSelection();

// Drop the whole selection (HBackdrop::Click: a plain left click on empty space).
// Clears the gizmo and any armed placement ghost with it - they are part of "the
// selection" as far as the next frame is concerned.
void ClearSelection();

// UE1's selection modifier is Ctrl (`bSelected ^= 1` in Click(HActor)); Shift does
// the same here, and Ctrl also extends while Unreal-style brush editing is OFF -
// in that mode Ctrl+click is the transform gesture, so it must not select.
bool SelectionModifierHeld();

// Drop every selected entity onto the surface below it (the heightmap where no
// solid is under it). Each member finds its OWN floor, so a crate on a ledge and a
// barrel on the ground both land correctly.
void SnapSelectionToGround();

// Push every selected entity against the nearest brush face along the view's right
// axis (snapped to the nearest cardinal, because collision is AABB-only). Capped to
// a sane reach so it cannot teleport an object across the level.
void SnapSelectionToSide();

// Centre the viewport on the current selection, preserving the current view
// direction and distance. Used by the World Hierarchy's Frame Selection item.
void FocusSelectionInViewport();

#endif // ANGEL_ED_SUBSYSTEMS_SELECTION_HPP