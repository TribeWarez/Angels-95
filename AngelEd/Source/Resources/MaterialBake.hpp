#pragma once
// ---------------------------------------------------------------------------
// Resources/MaterialBake — evaluate a .ozmat graph and write the baked PNGs.
//
// Editor-only (it uses raylib to decode and encode images); the runtime only
// samples the result. Lives in Resources/ because the material panel and the
// Tools menu both call it, and because it is a REAL translation unit shared by
// two panels (the same rule as AssetScope/PackageIO).
// ---------------------------------------------------------------------------

#include <string>

// Bake one material file in place: writes <dir>/<stem>/albedo.png + normal.png
// + emissive.png, records their paths and the graph hash in the .ozmat, and
// rewrites it canonically. Returns false when the graph fails to parse or a
// gate fails to evaluate; `log` then names the reason.
bool BakeMaterialFile(const std::string& ozmatPath, std::string* log);

// Bake every loose GameData/Materials/*.ozmat. Returns the number baked.
int BakeAllMaterials(std::string* log);

// Diagnostic text for an editor log line.
const char* MaterialBakeDir();
