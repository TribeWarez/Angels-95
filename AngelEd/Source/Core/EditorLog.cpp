// =============================================================================
// Core/EditorLog.cpp
//
// The AngelEd.log appender and its shared state.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================


// The append handle. Moved here from EntityOps.cpp in R8-phase F: EditorLog is its
// only user, and a definition in another translation unit is exactly the kind of
// accidental coupling the Subsystems promotion exists to remove. Internal linkage is
// correct here - nothing else needs the handle.
static FILE* g_editorLog = nullptr;
void EditorLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    if (!g_editorLog) {
        g_editorLog = fopen("System/AngelEd.log", "a");
        if (!g_editorLog) g_editorLog = fopen("AngelEd.log", "a");
    }
    if (g_editorLog) {
        time_t now = time(nullptr);
        char ts[64];
        strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
        fprintf(g_editorLog, "[%s] ", ts);
        va_start(args, fmt);
        vfprintf(g_editorLog, fmt, args);
        fprintf(g_editorLog, "\n");
        fflush(g_editorLog);
        va_end(args);
    }
}

enum class PlaceMode { MODEL, PICKUP, NODE, ENV, TERRAIN };
static PlaceMode g_placeMode = PlaceMode::MODEL;

// INI config
static IniConfig g_config;
static fs::path g_documentPath;
static fs::path g_pendingOpenPath;
static bool g_pendingNew = false;
static Sound g_previewSound = {0};
static bool g_previewSoundLoaded = false;

// Orthographic / view preset state is PER PANE now: the projection lives on each
// pane's own Camera3D (see g_paneCams) and the saved perspective camera lives in
// Core/EditorShell.cpp's g_panePersp[]. The two globals that used to be here
// (g_orthoView / g_perspectiveCamState) meant pane 2's Persp chip restored pane
// 1's camera.
