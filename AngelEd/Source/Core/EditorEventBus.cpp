#include "EditorEventBus.hpp"

#include <cassert>
#include <cstdio>

namespace ed {
namespace {

// Reports a payload mismatch and keeps going. A handler that reads the wrong
// shape is a bug, but aborting the frame in an editor is worse than logging it
// and treating the payload as absent — the alternative is a hard crash with no
// indication of which event was malformed.
void mismatch(const char* fn, Ev kind) {
    std::fprintf(stderr, "[EditorEventBus] %s: event %d has the wrong payload type\n",
                 fn, static_cast<int>(kind));
}

const SpawnDesc kEmptySpawn{};
const SelRef    kEmptySel{};
const Selection kEmptySeln{};
const SurfaceEdit kEmptySurf{};
const AnimIntent  kEmptyAnim{};
const PlacementRequest kEmptyPlace{};
const TextureSelectionApply kEmptyTexSel{};
const HeightmapDesc   kEmptyHm{};
const CsgIntent   kEmptyCsg{};
const Transform kEmptyXform{};
const std::string kEmptyStr{};
const LevelStateEdit kEmptyLevel{};

} // namespace

const SpawnDesc& Event::spawn() const {
    if (const auto* v = std::get_if<SpawnDesc>(&data)) return *v;
    mismatch("spawn", kind);
    return kEmptySpawn;
}

const SelRef& Event::sel() const {
    if (const auto* v = std::get_if<SelRef>(&data)) return *v;
    mismatch("sel", kind);
    return kEmptySel;
}

const Selection& Event::selection() const {
    if (const auto* v = std::get_if<Selection>(&data)) return *v;
    mismatch("selection", kind);
    return kEmptySeln;
}

const SurfaceEdit& Event::surface() const {
    if (const auto* v = std::get_if<SurfaceEdit>(&data)) return *v;
    mismatch("surface", kind);
    return kEmptySurf;
}

const AnimIntent& Event::anim() const {
    if (const auto* v = std::get_if<AnimIntent>(&data)) return *v;
    mismatch("anim", kind);
    return kEmptyAnim;
}

const PlacementRequest& Event::placement() const {
    if (const auto* v = std::get_if<PlacementRequest>(&data)) return *v;
    mismatch("placement", kind);
    return kEmptyPlace;
}

const TextureSelectionApply& Event::textureSelectionApply() const {
    if (const auto* v = std::get_if<TextureSelectionApply>(&data)) return *v;
    mismatch("textureSelectionApply", kind);
    return kEmptyTexSel;
}

const HeightmapDesc& Event::heightmap() const {
    if (const auto* v = std::get_if<HeightmapDesc>(&data)) return *v;
    mismatch("heightmap", kind);
    return kEmptyHm;
}

const CsgIntent& Event::csg() const {
    if (const auto* v = std::get_if<CsgIntent>(&data)) return *v;
    mismatch("csg", kind);
    return kEmptyCsg;
}

const Transform& Event::xform() const {
    if (const auto* v = std::get_if<Transform>(&data)) return *v;
    mismatch("xform", kind);
    return kEmptyXform;
}

const std::string& Event::str() const {
    if (const auto* v = std::get_if<std::string>(&data)) return *v;
    mismatch("str", kind);
    return kEmptyStr;
}

const LevelStateEdit& Event::level() const {
    if (const auto* v = std::get_if<LevelStateEdit>(&data)) return *v;
    mismatch("level", kind);
    return kEmptyLevel;
}

SelRef Event::target() const {
    if (const auto* v = std::get_if<SelRef>(&data)) return *v;
    if (const auto* v = std::get_if<Selection>(&data)) return v->ref;
    return SelRef{};
}

} // namespace ed