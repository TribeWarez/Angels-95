#pragma once
// ---------------------------------------------------------------------------
// Core/AssetPath.hpp - turn a machine-anchored asset path into a portable one.
//
// WHY THIS EXISTS
//   Filesystem paths enter the editor RESOLVED (absolute) from every picker: the
//   Model Browser, the Texture Manager, a drag-and-drop. They were written into
//   World.ozone VERBATIM for `Mesh.Static` / `Mesh.Skeletal` and their `tex=`,
//   which is how a committed world ended up holding
//   `G:\projects\...\GameData\Environment\Props\Bush_Dry_B.glb`.
//
//   That path RESOLVES on the machine that authored it, so `make test` was green
//   locally while the Linux CI failed `worldcheck --asset-refs` - the one place
//   the platform difference is visible. The loader accepts `GameData/`-rooted
//   paths as-is, so that is the form to WRITE.
//
// Header-only and dependency-free (no raylib, no Win32) so
// tests/AssetPath.test.cpp can pin both halves: the rewrite, and the
// "is this anchored to one machine?" predicate worldcheck now rejects.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <string>

namespace ozasset {

// Repo-relative is the most portable form: the loader resolves `GameData/...`
// directly, and the world-relative subdirectories are what the editor has always
// written (`oztex/...`, `<WorldDir>/Models/...`).
inline std::string ToPortablePath(const std::string& p) {
    std::string s = p;
    for (auto& c : s) if (c == '\\') c = '/';

    size_t gd = s.find("GameData/");
    if (gd != std::string::npos) return s.substr(gd);

    size_t oz = s.find("oztex/");
    if (oz != std::string::npos) return s.substr(oz);

    size_t wt = s.find("Worlds/");
    if (wt != std::string::npos) {
        size_t w1 = s.find('/', wt + 7);   // end of the world folder
        if (w1 != std::string::npos && w1 + 1 < s.size()) return s.substr(w1 + 1);
    }
    return s;
}

// True when the path names one machine's filesystem rather than the repo:
// `C:\...`, `C:/...`, `/home/...`, `\\server\share`, or a leading separator.
//
// The editor must never WRITE one (see ToPortablePath), and worldcheck rejects
// them explicitly - without that check a Windows checkout accepts the path and
// only the Linux job notices.
inline bool IsMachineAnchored(const std::string& p) {
    if (p.size() >= 2) {
        const char d = p[0];
        const bool letter = (d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z');
        if (letter && p[1] == ':') return true;
    }
    if (!p.empty() && (p[0] == '/' || p[0] == '\\')) return true;
    return false;
}

} // namespace ozasset
