#pragma once
#include "raylib.h"
#include <raymath.h>
#include <string>
#include <vector>
#include <filesystem>
#include <system_error>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Screenshot mode — deterministic, HUD-free world capture for promo shots.
//
// Enabled with --shot. Everything that would make a run non-reproducible or
// screenshot-ugly is suppressed: the splash screen, the title menu, audio
// device creation, the HUD/hotbar/crosshair/viewmodel, the raygui panel, the
// FPS counter, head bob and the debug light gizmos.
//
//   Angels95.exe --world Dust_Ravine --shot out/dust.png --shot-delay 90
//   Angels95.exe --world Dust_Ravine --shot out/dust.png \
//                 --shot-cam "-70,0,-70,90,0" --shot-cam "0,10,45,0,-10"
//
// --shot-cam coordinates are OZONE Z-up (x=east, y=north, z=up) and
// yaw/pitch are degrees, matching how .ozone files are authored. Repeating
// --shot-cam captures one PNG per camera: out/dust_1.png, out/dust_2.png ...
// With no --shot-cam the world spawn (playerstart #1) is used.
// ---------------------------------------------------------------------------

struct ShotCam {
    float x = 0, y = 0, z = 0;   // OZONE Z-up world position
    float yaw = 0;              // degrees, 0 = -Z (south), 90 = +X (east)
    float pitch = 0;            // degrees, positive = look up
};

// Developer menu capture (--menu-shot). Separate from ScreenshotSession because
// it runs the REAL title menu, not the world renderer. Page ids:
// 0 Home, 1 Play, 2 Multiplayer, 3 Character, 4 Settings. -1 = off.
inline int g_menuShotPage = -1;
inline std::string g_menuShotOut;
inline int g_menuShotFrames = 10;

struct ScreenshotSession {
    bool active = false;
    std::string outPath;               // base .png path
    std::vector<ShotCam> cams;         // one capture per entry
    int delayFrames = 90;              // frames to settle before capturing
    int resWidth = 0, resHeight = 0;   // 0 = keep the configured window size
    bool hideHud = true;               // drop hotbar/HUD/crosshair/viewmodel

    // --- runtime ---
    int shotIndex = 0;                 // which camera is being served
    int settleFrames = 0;              // frames rendered since the last capture
    bool camApplied = false;           // first camera pushed to the camera yet
    double startTime = -1.0;           // GetTime() when the first camera was armed

    // Hard wall-clock ceiling so an unattended run can never hang forever
    // (e.g. a world whose load blocks). GetTime() is used rather than a frame
    // counter so it still trips when the render loop is not being reached.
    static constexpr double kMaxSeconds = 60.0;

    void Reset() {
        outPath.clear();
        cams.clear();
        shotIndex = 0;
        settleFrames = 0;
        camApplied = false;
        startTime = -1.0;
        delayFrames = 90;
        resWidth = resHeight = 0;
        hideHud = true;
    }

    bool IsDone() const { return active && shotIndex >= (int)cams.size(); }
};

inline ScreenshotSession g_shot;

// Parse "x,y,z,yaw[,pitch]". Returns false on malformed input so the caller can
// warn instead of silently capturing the spawn view.
inline bool ParseShotCam(const char* value, ShotCam& out) {
    if (!value || !*value) return false;
    float v[5] = {0, 0, 0, 0, 0};
    int n = 0;
    const char* p = value;
    while (n < 5) {
        char* end = nullptr;
        v[n] = strtof(p, &end);
        if (end == p) return false;
        n++;
        p = end;
        while (*p == ' ') p++;
        if (*p == ',') { p++; continue; }
        break;
    }
    if (n < 4) return false;
    out.x = v[0]; out.y = v[1]; out.z = v[2];
    out.yaw = v[3]; out.pitch = v[4];
    return true;
}

// OZONE Z-up -> engine Y-up, same swap the world loader uses.
inline Vector3 ShotCamToEngine(const ShotCam& c) {
    return Vector3{ c.x, c.z, c.y };
}

// Point the camera at the current shot camera. Yaw is measured from -Z in the
// OZONE (x,y) plane; pitch is applied in engine space.
inline void ApplyShotCamera(Camera3D& cam, const ShotCam& c) {
    const float yr = c.yaw * DEG2RAD;
    const float pr = c.pitch * DEG2RAD;
    const Vector3 pos = ShotCamToEngine(c);
    // Horizontal heading in engine space: OZONE -Z maps to engine -Z, OZONE +X
    // maps to engine +X, so the heading is the same (sin yaw, 0, -cos yaw).
    const Vector3 heading = Vector3{ cosf(pr) * sinf(yr), sinf(pr), -cosf(pr) * cosf(yr) };
    cam.position = pos;
    cam.target = Vector3Add(pos, Vector3Scale(heading, 10.0f));
    cam.up = Vector3{ 0, 1, 0 };
}

// "out/dust.png" with 2 cameras -> "out/dust_2.png" (1 camera keeps the name).
inline std::string ShotOutputPath(const std::string& base, int index, int total) {
    if (total <= 1) return base;
    const size_t dot = base.find_last_of('.');
    const size_t slash = base.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return base + "_" + std::to_string(index + 1);
    return base.substr(0, dot) + "_" + std::to_string(index + 1) + base.substr(dot);
}

// ExportImage does not create directories, so an unattended run writing to a
// fresh path would otherwise fail at the last step.
inline bool EnsureShotOutputDir(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos)
        return true;   // writing into the cwd
    const std::string dir = path.substr(0, slash);
    if (dir.empty())
        return true;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        TraceLog(LOG_WARNING, "Shot: cannot create output dir '%s': %s",
                 dir.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

// Called once per frame after EndDrawing(). Captures and, for multi-camera
// runs, immediately arms the next camera. Returns true when every camera has
// been captured and the caller should quit.
inline bool TickScreenshot() {
    if (!g_shot.active) return false;

    // Watchdog: bail out rather than spin forever.
    if (g_shot.startTime >= 0.0 &&
        GetTime() - g_shot.startTime > ScreenshotSession::kMaxSeconds) {
        TraceLog(LOG_WARNING, "Shot: time budget (%.0fs) exhausted, aborting at shot %d/%d",
                 GetTime() - g_shot.startTime, g_shot.shotIndex + 1,
                 (int)g_shot.cams.size());
        g_shot.shotIndex = (int)g_shot.cams.size();
        return true;
    }

    if (g_shot.settleFrames < g_shot.delayFrames) {
        g_shot.settleFrames++;
        return false;
    }

    const std::string path = ShotOutputPath(g_shot.outPath, g_shot.shotIndex,
                                            (int)g_shot.cams.size());
    Image img = LoadImageFromScreen();
    if (img.data) {
        const int rc = ExportImage(img, path.c_str());
        if (rc)
            TraceLog(LOG_INFO, "Shot saved: %s (%dx%d)", path.c_str(), img.width, img.height);
        else
            TraceLog(LOG_WARNING, "Shot save FAILED: %s", path.c_str());
        UnloadImage(img);
    } else {
        TraceLog(LOG_WARNING, "Shot capture returned no image data (shot %d/%d)",
                 g_shot.shotIndex + 1, (int)g_shot.cams.size());
    }

    g_shot.shotIndex++;
    g_shot.settleFrames = 0;
    g_shot.camApplied = false;   // main loop re-arms the next camera
    return g_shot.shotIndex >= (int)g_shot.cams.size();
}
