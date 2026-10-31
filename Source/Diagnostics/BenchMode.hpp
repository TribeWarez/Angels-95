#pragma once
// ---------------------------------------------------------------------------
// BenchMode - deterministic, headless-ish frame benchmarking for the client.
//
// `--bench` reuses the screenshot camera vocabulary (ShotCam / ParseShotCam /
// ApplyShotCamera) and the same "skip the menu, hold the camera still" route,
// but it does not capture an image. Instead it lets FrameProfiler record a
// fixed number of frames, then writes the per-frame CSV and a summary. The
// point is comparability: same world, same camera, same resolution, same
// systems enabled -> the numbers from two builds can be diffed.
//
// Flags (parsed in Source/Main.cpp):
//   --bench [frames]          default 600
//   --bench-frames <n>
//   --bench-warmup <n>        default 60; history is reset at this boundary
//   --bench-csv <path>        default System/perf_frames.csv
//   --bench-cam "x,y,z,yaw[,pitch]"   OZONE Z-up, same as --shot-cam
//   --bench-phase all|render  render freezes simulation ticks
//   --bench-res <WxH>
//   --bench-disable a,b,c     ablation switches (see PerfToggles.hpp)
// ---------------------------------------------------------------------------

#include "../Screenshot.hpp"   // ShotCam
#include <string>

namespace oz::diag {

struct BenchSession {
    bool active = false;
    bool done = false;
    bool freezeSim = false;   // --bench-phase render
    bool hasCam = false;

    int frames = 600;         // measured frames
    int warmup = 60;          // discarded frames (shader compile, first uploads)
    int width = 0;            // 0 = keep configured window size
    int height = 0;
    int views = -1;           // editor only: 0..3 split layout, -1 = leave as loaded

    std::string csvPath = "System/perf_frames.csv";
    ShotCam cam{};
};

inline BenchSession& Bench() {
    static BenchSession session;
    return session;
}

} // namespace oz::diag
