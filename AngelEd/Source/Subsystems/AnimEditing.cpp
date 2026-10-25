// =============================================================================
// Subsystems/AnimEditing.cpp
//
// Vertex-keyframe editing: picking, snapshots, undo/redo, and the animation event handlers.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================

// ---------------------------------------------------------------------------
// Animation tool helpers
// ---------------------------------------------------------------------------
static void EnsureMeshNodeLoaded(MeshObjectNode* n) {
    if (!n || n->mesh) return;
    if (!n->animFile.empty())
        n->mesh = oz::MeshCache::Instance().GetAnimated(n->meshPath, n->texturePath, n->animFile, n->baseDir, true);
    else if (n->skeletal)
        n->mesh = oz::MeshCache::Instance().GetSkeletalChecked(n->meshPath, n->texturePath, n->baseDir, true);
    else
        n->mesh = oz::MeshCache::Instance().GetStatic(n->meshPath, n->texturePath, n->baseDir, true);
}

static oz::AnimatedMesh* AnimTarget() {
    MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
    if (!n || !n->mesh) return nullptr;
    return dynamic_cast<oz::AnimatedMesh*>(n->mesh.get());
}

// The panel's read-only GLB preview target (see UI/Panels/AnimPanel.cpp). A
// second 4-line accessor rather than a shared header: AnimPanel is compiled into
// UiShell.cpp and this into Main.cpp, and both fragments already carry their own
// AnimTarget accessor for the same reason.
static oz::SkeletalMesh* AnimTargetSkeletal() {
    MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
    if (!n || !n->mesh) return nullptr;
    return dynamic_cast<oz::SkeletalMesh*>(n->mesh.get());
}

static float AnimDuration() {
    if (oz::AnimatedMesh* am = AnimTarget()) {
        const ozanim::Clip* c = am->GetAnimation().FindClip(g_editorPanels.animClipName);
        return c ? c->Duration() : 0.0f;
    }
    // Skeletal: the GLB clip's length in seconds (raylib's 60 fps resample).
    if (oz::SkeletalMesh* sm = AnimTargetSkeletal()) {
        const int idx = sm->FindClip(g_editorPanels.animClipName);
        return idx >= 0 ? sm->ClipSeconds(idx) : 0.0f;
    }
    return 0.0f;
}

// World position of a mesh vertex (local = base + offset, then node TRS).
static Vector3 MeshVertexWorld(const MeshObjectNode& n, const oz::AnimatedMesh& am,
                               const std::vector<float>& offs, int vi) {
    const std::vector<float>& base = am.BasePositions();
    size_t i = (size_t)vi * 3;
    Vector3 local = {
        base[i + 0] + (i + 0 < offs.size() ? offs[i + 0] : 0.0f),
        base[i + 1] + (i + 1 < offs.size() ? offs[i + 1] : 0.0f),
        base[i + 2] + (i + 2 < offs.size() ? offs[i + 2] : 0.0f)
    };
    Vector3 s = Vector3Scale(local, n.scale);
    Vector3 r = Vector3RotateByAxisAngle(s, {0.0f, 1.0f, 0.0f}, n.yaw * DEG2RAD);
    return Vector3Add(n.position, r);
}

// Nearest vertex to the mouse within maxPx screen pixels (-1 if none).
static int PickVertex(const MeshObjectNode& n, const oz::AnimatedMesh& am,
                      const std::vector<float>& offs, Camera3D& cam, Vector2 mouse, float maxPx) {
    int best = -1; float bestD = maxPx;
    int vc = am.TotalVertexCount();
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    for (int vi = 0; vi < vc; vi++) {
        Vector2 sp = GetWorldToScreen(MeshVertexWorld(n, am, offs, vi), cam);
        if (sp.x < -50 || sp.y < -50 || sp.x > sw + 50 || sp.y > sh + 50) continue;
        float d = sqrtf((sp.x - mouse.x) * (sp.x - mouse.x) + (sp.y - mouse.y) * (sp.y - mouse.y));
        if (d < bestD) { bestD = d; best = vi; }
    }
    return best;
}

// Full undo/redo for the animation tool: each snapshot captures the live vertex
// edit pose (if editing) AND the whole clip set, so vertex moves, key add/delete
// and clip create/delete are all covered.
struct AnimSnapshot {
    std::vector<float> pose;
    bool hasPose = false;
    ozanim::Animation clips;
};
static std::vector<AnimSnapshot> g_animUndo;
static std::vector<AnimSnapshot> g_animRedo;

static AnimSnapshot AnimCapture() {
    AnimSnapshot s;
    MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
    if (n && n->editPose) { s.pose = *n->editPose; s.hasPose = true; }
    if (oz::AnimatedMesh* am = AnimTarget()) s.clips = am->GetAnimation();
    return s;
}

static void AnimSnapshotPush() {
    g_animRedo.clear();
    g_animUndo.push_back(AnimCapture());
    if (g_animUndo.size() > 32) g_animUndo.erase(g_animUndo.begin());
}

static void AnimRestore(const AnimSnapshot& s) {
    if (oz::AnimatedMesh* am = AnimTarget()) am->SetAnimation(s.clips);
    MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(g_editorPanels.animTargetMesh);
    if (n && n->editPose && s.hasPose) *n->editPose = s.pose;
}

static void AnimUndo() {
    if (g_animUndo.empty()) return;
    g_animRedo.push_back(AnimCapture());
    AnimSnapshot s = g_animUndo.back();
    g_animUndo.pop_back();
    AnimRestore(s);
}

static void AnimRedo() {
    if (g_animRedo.empty()) return;
    g_animUndo.push_back(AnimCapture());
    AnimSnapshot s = g_animRedo.back();
    g_animRedo.pop_back();
    AnimRestore(s);
}

// ---------------------------------------------------------------------------
// ApplyAnimIntents — R2 batch B4
//
// Every animation command the panel posts, dispatched from one place. Each intent
// carries the clip name, playhead and fps/loop captured when the button was
// pressed; previously the handlers read `animClipName` and `animTime` from live
// panel state at drain time, so a clip-list selection that moved between the click
// and the frame deleted or keyed a DIFFERENT clip. That is the same staleness
// class as the WorldGraph index bug, and easier to hit here because a list
// selection changes on every click.
//
// `AnimTarget()`, `AnimSnapshotPush()` and the undo stack still resolve the
// CURRENT target internally, so the mesh id carried in an intent cannot yet be
// threaded through them. Rather than leave that silent, a mismatch is detected and
// reported: the command is skipped and logged instead of landing on whatever mesh
// happens to be selected. Subsystems/AnimEditing in R4 threads the id properly.
// ---------------------------------------------------------------------------
static void ApplyAnimIntents(const std::vector<ed::Event>& events) {
    for (const ed::Event& ev : events) {
        const ed::AnimIntent& ai = ev.anim();

        // Scrub is special: it addresses the target directly and is a continuous
        // signal, so it neither needs nor wants the target-mismatch guard below.
        if (ev.kind == ed::Ev::AnimScrub) {
            MeshObjectNode* tn = PawnSystem::Instance().GetMeshObject(ai.meshId);
            if (!tn) continue;
            // A skeletal GLB has no .ozanim file but IS scrubbable (read-only).
            if (tn->animFile.empty() && !AnimTargetSkeletal()) continue;
            const float dur = AnimDuration();
            // ai.time is the slider position 0..1, captured at post time.
            g_editorPanels.animTime = (dur > 0.0f) ? ai.time * dur : 0.0f;
            tn->animTime = g_editorPanels.animTime;
            g_editorPanels.animPlaying = false;
            continue;
        }

        if (ai.meshId >= 0 && ai.meshId != g_editorPanels.animTargetMesh) {
            EditorLog("Anim: command skipped, target moved (event=%d was for %d, now %d)",
                      (int)ev.kind, ai.meshId, g_editorPanels.animTargetMesh);
            continue;
        }
        oz::AnimatedMesh* am = AnimTarget();

        switch (ev.kind) {
            case ed::Ev::AnimNewClip: {
                if (!am) break;
                static int s_clipN = 1;
                AnimSnapshotPush();
                ozanim::Clip c;
                c.name = "Clip" + std::to_string(s_clipN++);
                c.fps  = ai.fps > 0.0f ? ai.fps : 30.0f;
                c.loop = ai.loop;
                am->MutableAnimation().clips.push_back(c);
                g_editorPanels.animClipName = c.name;
                g_editorPanels.animTime = 0.0f;
                g_editorPanels.actionAnimSave = true;
                break;
            }
            case ed::Ev::AnimDeleteClip: {
                if (!am) break;
                auto& clips = am->MutableAnimation().clips;
                // The clip NAME from the event, not the live panel field.
                int idx = am->FindClip(ai.clipName);
                if (idx >= 0 && idx < (int)clips.size()) {
                    AnimSnapshotPush();
                    clips.erase(clips.begin() + idx);
                    g_editorPanels.animClipName = clips.empty() ? "" : clips.front().name;
                    g_editorPanels.animTime = 0.0f;
                    g_editorPanels.actionAnimSave = true;
                }
                break;
            }
            case ed::Ev::AnimApplyClipMeta: {
                if (!am) break;
                if (ozanim::Clip* c = am->MutableAnimation().FindClip(ai.clipName)) {
                    if (ai.fps > 0.0f) c->fps = ai.fps;
                    c->loop = ai.loop;
                    g_editorPanels.actionAnimSave = true;
                }
                break;
            }
            case ed::Ev::AnimAddKey: {
                if (!am) break;
                MeshObjectNode* kn = PawnSystem::Instance().GetMeshObject(ai.meshId);
                ozanim::Clip* c = am->MutableAnimation().FindClip(ai.clipName);
                if (!c) break;
                AnimSnapshotPush();
                const float t = ai.time;
                ozanim::Keyframe* kf = nullptr;
                for (auto& k : c->keys) if (fabsf(k.time - t) < 1e-3f) kf = &k;
                if (!kf) {
                    ozanim::Keyframe k; k.time = t;
                    c->keys.push_back(k);
                    std::sort(c->keys.begin(), c->keys.end(),
                              [](const ozanim::Keyframe& a, const ozanim::Keyframe& b) { return a.time < b.time; });
                    for (auto& k : c->keys) if (fabsf(k.time - t) < 1e-3f) kf = &k;
                }
                if (kf) {
                    const int vc = am->TotalVertexCount();
                    // Source offsets: the live edit pose when editing, else the sample.
                    std::vector<float> sampled;
                    std::vector<float>* offs = nullptr;
                    if (kn && kn->editPose) offs = kn->editPose.get();
                    else { c->SampleOffsets(t, vc, sampled); offs = &sampled; }

                    std::vector<int> verts;
                    if (g_editorPanels.animEditVerts && !g_editorPanels.animSelVerts.empty())
                        verts = g_editorPanels.animSelVerts;
                    else for (int i = 0; i < vc; i++) verts.push_back(i);

                    for (int vi : verts) {
                        if (vi < 0 || vi >= vc) continue;
                        const float dx = (*offs)[vi * 3 + 0];
                        const float dy = (*offs)[vi * 3 + 1];
                        const float dz = (*offs)[vi * 3 + 2];
                        kf->offsets.erase(std::remove_if(kf->offsets.begin(), kf->offsets.end(),
                            [vi](const ozanim::VertexOffset& o) { return o.index == vi; }),
                            kf->offsets.end());
                        if (fabsf(dx) > 1e-5f || fabsf(dy) > 1e-5f || fabsf(dz) > 1e-5f)
                            kf->offsets.push_back({vi, dx, dy, dz});
                    }
                    std::sort(kf->offsets.begin(), kf->offsets.end(),
                              [](const ozanim::VertexOffset& a, const ozanim::VertexOffset& b) { return a.index < b.index; });
                }
                g_editorPanels.actionAnimSave = true;
                break;
            }
            case ed::Ev::AnimDeleteKey: {
                if (!am) break;
                ozanim::Clip* c = am->MutableAnimation().FindClip(ai.clipName);
                if (!c) break;
                AnimSnapshotPush();
                int best = -1; float bd = 1e9f;
                for (int i = 0; i < (int)c->keys.size(); i++) {
                    const float d = fabsf(c->keys[i].time - ai.time);
                    if (d < bd) { bd = d; best = i; }
                }
                if (best >= 0 && bd < 0.05f) c->keys.erase(c->keys.begin() + best);
                g_editorPanels.actionAnimSave = true;
                break;
            }
            case ed::Ev::AnimToggleEdit: {
                MeshObjectNode* n = PawnSystem::Instance().GetMeshObject(ai.meshId);
                if (!n || !am) break;
                if (!g_editorPanels.animEditVerts) {
                    std::vector<float> offs((size_t)am->TotalVertexCount() * 3, 0.0f);
                    if (const ozanim::Clip* c = am->GetAnimation().FindClip(ai.clipName))
                        c->SampleOffsets(ai.time, am->TotalVertexCount(), offs);
                    n->editPose = std::make_shared<std::vector<float>>(std::move(offs));
                    n->animPaused = true;
                    g_editorPanels.animEditVerts = true;
                    g_editorPanels.animSelVerts.clear();
                    g_animUndo.clear();
                    g_animRedo.clear();
                    g_editorPanels.animPrevValid = false;
                } else {
                    n->editPose.reset();
                    g_editorPanels.animEditVerts = false;
                    g_editorPanels.animSelVerts.clear();
                }
                g_editorPanels.actionAnimRefresh = true;
                break;
            }
            case ed::Ev::AnimSelectAll: {
                if (!g_editorPanels.animEditVerts || !am) break;
                g_editorPanels.animSelVerts.clear();
                for (int i = 0; i < am->TotalVertexCount(); i++)
                    g_editorPanels.animSelVerts.push_back(i);
                break;
            }
            case ed::Ev::AnimClearSelection:
                g_editorPanels.animSelVerts.clear();
                break;
            case ed::Ev::AnimUndo:
                AnimUndo();
                g_editorPanels.actionAnimRefresh = true;
                break;
            case ed::Ev::AnimRedo:
                AnimRedo();
                g_editorPanels.actionAnimRefresh = true;
                break;
            default:
                break;
        }
    }
}
// ---------------------------------------------------------------------------
// Editor History â€” full-document undo/redo via OZONE text snapshots.
// A snapshot captures geometry, entities, level metadata and the heightmap
// (everything ExportToOzone writes). Camera and selection are left untouched.
// ---------------------------------------------------------------------------
