#include "../Core/EditorShell.hpp"
#include "../Core/EditorState.hpp"
#include "History.hpp"
#include "LevelState.hpp"
#include "OzoneExport.hpp"

// Subsystems/History.cpp
//
// Document undo/redo via ExportToOzone snapshots, plus ApplyMapProperties.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================


// The document undo stacks. These were declared in AnimEditing.cpp until Phase F and
// used here - it worked only because the unity include order happened to put
// AnimEditing before History. The animation tool has its own separate undo stack
// (AnimSnapshotPush / AnimUndo / AnimRedo); these are the DOCUMENT stack.
static std::vector<std::string> g_histUndo;
static std::vector<std::string> g_histRedo;
static const size_t kHistMax = 64;
// A snapshot is the whole world's OZONE text, so 64 entries of a large level is
// gigabytes. Cap each stack by total bytes as well as by count, evicting the
// oldest first; an undo step older than ~64 MB of history is not one anybody
// scrolls back to.
static const size_t kHistMaxBytes = 64ull * 1024ull * 1024ull;

static void TrimHistory(std::vector<std::string>& stack) {
    size_t total = 0;
    for (const auto& s : stack) total += s.size();
    while (stack.size() > 1 && total > kHistMaxBytes) {
        total -= stack.front().size();
        stack.erase(stack.begin());
    }
    while (stack.size() > kHistMax) stack.erase(stack.begin());
}

std::string HistoryCapture() {
    std::ostringstream oss;
    ExportToOzone(oss);
    return oss.str();
}

void HistoryClear() {
    g_histUndo.clear();
    g_histRedo.clear();
}

// Call BEFORE a mutation: snapshots current state and invalidates redo.
//
// HistoryPush runs BEFORE the edit, so two consecutive pushes with no edit in
// between (a click that pushes, then another that pushes) capture the SAME
// document. Collapsing them keeps one undo step and one world-sized string
// instead of two, and — importantly — a no-op push must not clear the redo
// stack, or a stray click after an undo would throw the redo away.
void HistoryPush() {
    g_documentDirty = true;
    std::string snap = HistoryCapture();
    if (!g_histUndo.empty() && g_histUndo.back() == snap) return; // nothing changed
    g_histRedo.clear();
    g_histUndo.push_back(std::move(snap));
    TrimHistory(g_histUndo);
}


void HistoryRestore(const std::string& text) {
    ClearScene();
    OzoneLoader::Instance().LoadString(
        text.c_str(), OTEditor.Path[0] ? OTEditor.Path : nullptr);
    InjectOzoneEntities(OzoneLoader::Instance().GetEntities(),
                        PawnSystem::Instance());
    OzoneLoader::Instance().RebuildCollisionVolumes();
    // The snapshot carries the `levelinfo` / `particles` lines (ExportToOzone wrote
    // them), so the editor's metadata has to be re-read from it too. Skipping this
    // left g_levelMeta at whatever it was, and since a snapshot taken after a
    // metadata-less load is itself metadata-less, an undo followed by a save wrote
    // the level with no levelinfo at all - the same data loss as the startup path.
    LoadLevelMetadataFromPrims(OzoneParser::parse_string(text));
    g_sel = { SelType::NONE, -1, "", {0,0,0} };
    g_hoverSel = { SelType::NONE, -1, "", {0,0,0} };
    g_selSet.clear();
    AngelTechEditor.DrawModel = false;
    // Undo replaces the whole scene, so every WorldGraph row is invalid. Setting the
    // flag is not a mutation and keeps the layer direction right: Core consumes it.
    MarkWorldGraphDirty();
}

void HistoryUndo() {
    if (g_histUndo.empty()) return;
    g_histRedo.push_back(HistoryCapture());
    TrimHistory(g_histRedo);
    std::string snap = g_histUndo.back();
    g_histUndo.pop_back();
    HistoryRestore(snap);
    EditorLog("Undo (%zu undo / %zu redo)", g_histUndo.size(), g_histRedo.size());
}

void HistoryRedo() {
    if (g_histRedo.empty()) return;
    g_histUndo.push_back(HistoryCapture());
    TrimHistory(g_histUndo);
    std::string snap = g_histRedo.back();
    g_histRedo.pop_back();
    HistoryRestore(snap);
    EditorLog("Redo (%zu undo / %zu redo)", g_histUndo.size(), g_histRedo.size());
}
