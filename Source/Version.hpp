#pragma once
// ---------------------------------------------------------------------------
// Version.hpp — the ONE build-identity definition.
//
// The release tag is injected at compile time by the build:
//   make BUILD_VERSION=b97 ...
//   CI: env BUILD_VERSION=${{ github.ref_name }}
// The fallback is "dev" so a local build is honestly labelled and can never be
// mistaken for a release.
//
// `gamever` on the wire and in /status is this build id (e.g. "b97"), NOT the
// long-lived marketing version. The wire COMPATIBILITY gate is
// net::PROTOCOL_VERSION in Source/Network/Network.hpp and is reported
// separately as `proto` in the heartbeat / server list / /status. Keeping the
// two apart is deliberate: a build id changes every release, a protocol id
// changes only when a peer would misparse the other.
//
// Header-only on purpose (like CullState.hpp): no .cpp means no Makefile,
// AngelEd/Makefile or CI object-list edit.
// ---------------------------------------------------------------------------

#ifndef OZ_BUILD_VERSION
#define OZ_BUILD_VERSION "dev"
#endif

// The human-facing game version. Only used where a release-series number is
// wanted (currently nowhere in the runtime; kept as the documented default).
#ifndef OZ_GAME_VERSION
#define OZ_GAME_VERSION "0.2.1"
#endif

namespace oz {

inline constexpr const char* kBuildVersion = OZ_BUILD_VERSION;
inline constexpr const char* kGameVersion  = OZ_GAME_VERSION;

} // namespace oz
