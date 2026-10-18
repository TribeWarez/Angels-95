// =============================================================================
// Subsystems/OzoneExport.cpp
//
// ExportToOzone, SaveWorldDocument, the File menu actions, texture application.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================

// NOT static: Subsystems/History.cpp is a real translation unit and calls this from
// HistoryCapture(). A `static` function in a unity fragment has internal linkage and
// cannot satisfy an external reference, so this one function carries the linkage.
// The rest of this file stays a fragment - it calls UI file dialogs and uses Core
// statics, so promoting it means cutting those seams first. See
// Wiki/Editor-Architecture-Refactor.md.
void ExportToOzone(std::ostream& output) {
    // Header
    output << "# OZONE world exported from AngelEd\n";
    output << "# Format: ozone v1.0\n\n";

    // Geometry: export the AUTHORED renderables (original primitives + CSG ops +
    // material kwargs). Exporting post-CSG collision volumes here would re-apply
    // add/sub on already-carved hulls (double subtraction â†’ empty world) and
    // lose textures/csg ops.
    // (OZONE is Z-up: file y/z swapped vs engine coords)
    auto& loader = OzoneLoader::Instance();
    // Skyboxes are render-only rooms, exported before the solid brushes so the
    // shell that surrounds them reads naturally in the file.
    for (int i = 0; i < loader.Count(); i++) {
        OzoneRenderable* r = loader.Get(i);
        if (!r || !r->loaded) continue;
        if (r->typeId != (int)OzonePrimitiveType::SKYBOX) continue;
        // The mesh is unit-sized around its own origin, so recover the authored
        // extent from the bounds rather than the (always 1.0) renderable scale.
        BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
        const float size = (mb.max.x - mb.min.x) * (r->scale > 0.0001f ? r->scale : 1.0f);
        // OZONE is Z-up, engine is Y-up (the same swap the zup lambda below
        // applies to every other entity; spelled out here because this loop runs
        // before that lambda is declared).
        const Vector3 p = { r->position.x, r->position.z, r->position.y };
        output << "skybox " << MakeWorldRelativePath(r->texPath)
               << " " << p.x << " " << p.y << " " << p.z
               << " " << (size > 0.01f ? size : 512.0f);
        if (r->surface.def.panU != 0.0f) output << " panU=" << r->surface.def.panU;
        if (r->surface.def.panV != 0.0f) output << " panV=" << r->surface.def.panV;
        output << "\n";
    }
    for (int i = 0; i < loader.Count(); i++) {
        OzoneRenderable* r = loader.Get(i);
        if (!r || !r->loaded) continue;
        const char* prim = OzonePrimName(r->typeId);
        if (!prim) continue; // entity types / heightmap handled below

        BoundingBox mb = GetMeshBoundingBox(r->model.meshes[0]);
        float sc = (r->scale > 0.0001f) ? r->scale : 1.0f; // mesh bounds are unscaled
        float w = (mb.max.x - mb.min.x) * sc;
        float h = (mb.max.y - mb.min.y) * sc;
        float d = (mb.max.z - mb.min.z) * sc;
        if (w < 0.01f) w = 1.0f;
        if (h < 0.01f) h = 1.0f;
        if (d < 0.01f) d = 1.0f;

        // Engine Y-up -> OZONE Z-up. Cylinder/pyramid meshes span [-h, 0] so the
        // authored `cz` IS the top and needs no compensation. There used to be a
        // `center.y += h * 0.5` here justified by "the loader re-centers them on
        // import" - it does not, so every save/open moved those brushes half a
        // height upward and the offset compounded on each cycle.
        Vector3 c = {r->position.x, r->position.z, r->position.y};
        float rotDeg = r->rotation * RAD2DEG;

        output << CsgPrefixName(r->csgOp) << " " << prim
               << " " << c.x << " " << c.y << " " << c.z;

        if (r->typeId == (int)OzonePrimitiveType::BOX) {
            // box x y z w h d rot [texSlot]
            output << " " << w << " " << h << " " << d << " " << rotDeg;
            if (r->texSlot > 0) output << " " << r->texSlot;
        } else if (r->typeId == (int)OzonePrimitiveType::CYLINDER) {
            // cyl x y z rTop rBot h slices rot [texSlot]
            //
            // The radii and slice count come from the renderable, not from the
            // mesh bounding box: the bbox only knows max(rTop, rBot), so writing
            // it back as "rad rad" silently flattened every taper the author made.
            const float rTop = (r->cylRTop > 0.0f) ? r->cylRTop : ((w > d) ? w : d) * 0.5f;
            const float rBot = (r->cylRBot > 0.0f) ? r->cylRBot : rTop;
            const int slices = (r->cylSlices > 0) ? r->cylSlices : 16;
            output << " " << rTop << " " << rBot << " " << h << " " << slices
                   << " " << rotDeg;
            if (r->texSlot > 0) output << " " << r->texSlot;
        } else if (r->typeId == (int)OzonePrimitiveType::SPHERE) {
            // sph x y z r segments
            float rad = ((w > h) ? ((w > d) ? w : d) : ((h > d) ? h : d)) * 0.5f;
            output << " " << rad << " 16";
        } else if (r->typeId == (int)OzonePrimitiveType::PYRAMID) {
            // pyr x y z w d h [texSlot]
            output << " " << w << " " << d << " " << h;
            if (r->texSlot > 0) output << " " << r->texSlot;
        } else { // PLANE - normal + distance (see below)
            // normal + distance, converted back to OZONE's Z-up. The loader
            // swaps the authored normal into engine Y-up, and it cannot be
            // re-derived from the mesh bounding box, so OzoneRenderable carries it.
            float pnx = r->planeNX, pny = r->planeNZ, pnz = r->planeNY;
            float plen = sqrtf(pnx*pnx + pny*pny + pnz*pnz);
            if (plen < 1e-6f) { pnx = 0.0f; pny = 0.0f; pnz = 1.0f; plen = 1.0f; }
            output << " " << (pnx/plen) << " " << (pny/plen) << " " << (pnz/plen)
                   << " " << r->planeDist;
        }

        // Material kwargs (consumed by the OZONE brush parser).
        //
        // texPath / texScale* / texOffset* are now VIEWS onto
        // OzoneRenderable::surface.def (see OzoneParser's DeriveLegacySurface-
        // Fields), so writing them from the legacy fields cannot disagree with
        // the surface block below.
        if (!r->texPath.empty()) {
            std::string tp = MakeWorldRelativePath(r->texPath);
            bool quote = tp.find(' ') != std::string::npos;
            output << " texPath=" << (quote ? "\"" + tp + "\"" : tp);
        }
        if (r->texScaleU != 1.0f || r->texScaleV != 1.0f)
            output << " texScaleU=" << r->texScaleU << " texScaleV=" << r->texScaleV;
        if (r->texOffsetU != 0.0f || r->texOffsetV != 0.0f)
            output << " texOffsetU=" << r->texOffsetU << " texOffsetV=" << r->texOffsetV;

        // Per-face surface properties, as `face<name>_<field>=` kwargs. Only
        // fields the author actually set are emitted, so a face override stays
        // a small readable diff and an untouched face inherits the brush
        // default on reload. The face NAMES are engine Y-up (see
        // World/SurfaceFlags.hpp) - OZONE is Z-up, but surface faces are a
        // renderer concept, not world coordinates, so they are NOT swapped.
        {
            const oz::surface::BrushSurface& bs = r->surface;
            const oz::surface::SurfaceProps& d = bs.def;
            // Brush-wide surface fields the legacy kwargs above do not cover.
            //
            // `flags=` is read from the OWNER (surface.def.flags) and never
            // compared against OzoneRenderable::surfaceFlags. That legacy field is
            // a derived mirror of this very value, so the old guard
            // (`d.flags != r->surfaceFlags`) was false for every brush that came
            // through the loader — the flags were never written, and re-exporting
            // a level silently deleted every painted backdrop and every AutoConvex
            // collision proxy in it. See oz::surface::NeedsFlagsKwarg.
            if (oz::surface::NeedsFlagsKwarg(d.flags))
                output << " flags=" << d.flags;
            if (d.texSlot > 0) output << " surfTexSlot=" << d.texSlot;
            if (!d.texPath.empty() && r->texPath.empty())
                output << " surfTex=" << MakeWorldRelativePath(d.texPath);
            if (d.panU != 0.0f) output << " panU=" << d.panU;
            if (d.panV != 0.0f) output << " panV=" << d.panV;
            if (d.alpha != 1.0f) output << " surfAlpha=" << d.alpha;
            if (d.alphaCutoff != 0.0f) output << " surfCutoff=" << d.alphaCutoff;
            if (d.glowR != 0.0f || d.glowG != 0.0f || d.glowB != 0.0f)
                output << " surfGlow=(" << d.glowR << "," << d.glowG << "," << d.glowB << ")";
            if (d.glowScale != 1.0f) output << " surfGlowScale=" << d.glowScale;

            for (int f = 0; f < oz::surface::FACE_COUNT; f++) {
                if (!bs.IsFaceOverridden((oz::surface::SurfaceFace)f)) continue;
                const std::string pfx =
                    std::string("face") + oz::surface::FaceName((oz::surface::SurfaceFace)f) + "_";
                // Resolve against the brush default so only the DIFFERENCE is
                // written; the parser re-seeds a face from the default, so this
                // round-trips exactly.
                const oz::surface::SurfaceProps& p = bs.Resolve((oz::surface::SurfaceFace)f);
                if (p.flags != d.flags)     output << " " << pfx << "flags=" << p.flags;
                if (p.texSlot != d.texSlot)  output << " " << pfx << "texSlot=" << p.texSlot;
                if (p.texPath != d.texPath)
                    output << " " << pfx << "tex=" << MakeWorldRelativePath(p.texPath);
                if (p.uvScaleU != d.uvScaleU)  output << " " << pfx << "uvScaleU=" << p.uvScaleU;
                if (p.uvScaleV != d.uvScaleV)  output << " " << pfx << "uvScaleV=" << p.uvScaleV;
                if (p.uvOffsetU != d.uvOffsetU) output << " " << pfx << "uvOffsetU=" << p.uvOffsetU;
                if (p.uvOffsetV != d.uvOffsetV) output << " " << pfx << "uvOffsetV=" << p.uvOffsetV;
                if (p.panU != d.panU)   output << " " << pfx << "panU=" << p.panU;
                if (p.panV != d.panV)   output << " " << pfx << "panV=" << p.panV;
                if (p.alpha != d.alpha) output << " " << pfx << "alpha=" << p.alpha;
                if (p.alphaCutoff != d.alphaCutoff)
                    output << " " << pfx << "cutoff=" << p.alphaCutoff;
                if (p.glowR != d.glowR || p.glowG != d.glowG || p.glowB != d.glowB)
                    output << " " << pfx << "glow=(" << p.glowR << "," << p.glowG << "," << p.glowB << ")";
                if (p.glowScale != d.glowScale)
                    output << " " << pfx << "glowScale=" << p.glowScale;
            }
        }
        output << "\n";
    }

    // Heightmap â€” prefer the OZONE loader's terrain (authored in this document),
    // fall back to the legacy WDL heightmap.
    if (loader.HasHeightmap() && !loader.GetHeightmapImagePath().empty()) {
        Vector3 hp = loader.GetHeightmapPosition();
        Vector3 hs = loader.GetHeightmapSize();
        std::string img = loader.GetHeightmapImagePath();
        std::string tex = loader.GetHeightmapTexturePath();
        output << "heightmap " << img << " " << (tex.empty() ? img : tex)
               << " " << hp.x << " " << hp.z << " " << hp.y   // Z-up
               << " " << loader.GetHeightmapScale()
               << " " << hs.x << " " << hs.y << " " << hs.z << "\n";
    } else if (WDLModels.HeightMapReady) {
        Vector3 hp = {WDLModels.HeightMapPosition.x,
                      WDLModels.HeightMapPosition.z,
                      WDLModels.HeightMapPosition.y};
        output << "heightmap Models/HeightMap.png Models/HeightMapTexture.png "
               << hp.x << " " << hp.y << " " << hp.z
               << " " << WDLModels.HeightMapScale
               << " " << WDLModels.HeightMapSize.x << " " << WDLModels.HeightMapSize.y << " " << WDLModels.HeightMapSize.z << "\n";
    }

    output << "\n# Entities\n";

    // OZONE files are Z-up ("x=east, y=north, z=up"); loaders convert to
    // engine Y-up by swapping y/z. Swap here so round-trips are lossless.
    auto& pawns = PawnSystem::Instance();
    auto zup = [](const Vector3& v) { return Vector3{v.x, v.z, v.y}; };

    // Player starts
    for (auto& start : pawns.GetPlayerStarts()) {
        Vector3 p = zup(start.position);
        output << "playerstart " << p.x << " " << p.y << " " << p.z << " " << start.yaw << "\n";
    }

    // Pickups
    for (auto& pickup : pawns.GetPickups()) {
        Vector3 p = zup(pickup.position);
        output << "pickup " << pickup.typeName << " " << p.x << " " << p.y << " " << p.z;
        if (pickup.respawnTime > 0.01f) output << " " << pickup.respawnTime;
        output << "\n";
    }

    // NPCs
    for (auto& pawn : pawns.GetPawns()) {
        if (!pawn.active || pawn.defName.empty()) continue;
        Vector3 p = zup(pawn.position);
        output << "npc " << pawn.defName << " " << p.x << " " << p.y << " " << p.z << "\n";
    }

    // Lights
    //
    // Optional attributes (effect / flare / corona / name) are written as named
    // kwargs, never as trailing positional floats. The positional tail is
    // position-dependent: a light with no effect but a flare emitted
    // `... 16 1 0`, which the loader read back as effect=1 (WATERY) + flare=0.
    // The editor is also the only place these were editable, so the round trip
    // has to be lossless or every save silently retuned the level's lighting.
    for (auto& light : pawns.GetLights()) {
        if (!light.active) continue;
        Vector3 p = zup(light.position);
        Vector3 t = zup(light.target);
        // Color components are unsigned char â€” stream them as integers, never as
        // raw bytes (a raw byte >= 0x80 corrupts the UTF-8 text file and breaks
        // the client's numeric light parser).
        int r = (int)light.color.r, g = (int)light.color.g, b = (int)light.color.b;
        if (light.type == LitLightType::DIRECTIONAL) {
            // directional x y z r g b intensity  (x y z is the SOURCE; the
            // loader aims it at the world origin)
            output << "light directional " << p.x << " " << p.y << " " << p.z
                   << " " << r << " " << g << " " << b << " " << light.intensity;
        } else if (light.type == LitLightType::SPOT) {
            // spot x y z tx ty tz r g b intensity radius innerCone outerCone
            output << "light spot " << p.x << " " << p.y << " " << p.z
                   << " " << t.x << " " << t.y << " " << t.z
                   << " " << r << " " << g << " " << b
                   << " " << light.intensity << " " << light.radius
                   << " " << light.innerCone << " " << light.outerCone;
        } else {
            // point x y z r g b intensity radius
            output << "light point " << p.x << " " << p.y << " " << p.z
                   << " " << r << " " << g << " " << b
                   << " " << light.intensity << " " << light.radius;
        }
        if (light.effect != LitLightEffect::NONE) output << " effect=" << (int)light.effect;
        if (light.flare)  output << " flare=1";
        if (light.corona) output << " corona=1";
        if (!light.name.empty()) {
            std::string ln = light.name;
            bool quote = ln.find(' ') != std::string::npos;
            output << " name=" << (quote ? "\"" + ln + "\"" : ln);
        }
        output << "\n";
    }

    // Zone volumes
    for (auto& zone : ZoneManager::Instance().GetZones()) {
        const char* zt = WDLZoneTypeName(zone.zoneType);
        Vector3 mn = zup(zone.bounds.min);
        Vector3 mx = zup(zone.bounds.max);
        output << "zone " << zt
               << " " << std::min(mn.x, mx.x) << " " << std::min(mn.y, mx.y) << " " << std::min(mn.z, mx.z)
               << " " << std::max(mn.x, mx.x) << " " << std::max(mn.y, mx.y) << " " << std::max(mn.z, mx.z)
               << " " << zone.intensity;
        // Export env overrides if any are set
        auto& eo = zone.envOverrides;
        if (eo.applyFog || eo.applyAmbient || eo.reverbMix > 0.0f) {
            output << " " << eo.fogR << " " << eo.fogG << " " << eo.fogB
                   << " " << eo.fogDensity << " " << eo.fogStart << " " << eo.fogEnd
                   << " " << eo.ambR << " " << eo.ambG << " " << eo.ambB
                   << " " << eo.ambIntensity
                   << " " << eo.reverbMix << " " << eo.reverbDecay;
        }
        // Script-hook name (name= kwarg; consumed by the loader, not an arg index)
        if (!zone.name.empty())
            output << " name=" << zone.name;
        // Per-zone physics overrides (named kwargs; absent = engine defaults)
        auto& ph = zone.physics;
        output << " gravity="   << ph.gravity
               << " jump="      << ph.jumpSpeed
               << " terminal="  << ph.terminalVelocity
               << " water_gravity=" << ph.waterGravity
               << " water_drag="    << ph.waterDrag
               << " swim_up="   << ph.swimUpSpeed
               << " ladder_speed="  << ph.ladderSpeed
               << " fly_mult="  << ph.flySpeedMult;
        output << "\n";
    }

    // Portals (level connections)
    for (auto& portal : ZoneManager::Instance().GetPortals()) {
        Vector3 mn = zup(portal.bounds.min);
        Vector3 mx = zup(portal.bounds.max);
        Vector3 sp = zup(portal.targetSpawn);
        output << "portal " << portal.targetWorld
               << " " << std::min(mn.x, mx.x) << " " << std::min(mn.y, mx.y) << " " << std::min(mn.z, mx.z)
               << " " << std::max(mn.x, mx.x) << " " << std::max(mn.y, mx.y) << " " << std::max(mn.z, mx.z)
               << " " << sp.x << " " << sp.y << " " << sp.z
               << (portal.bidirectional ? " bidir" : "") << "\n";
    }

    // Gameplay trigger volumes. The editor has no trigger UI, so these are
    // re-emitted from the entity set the loader parsed (fresh per load: both
    // LoadFile and LoadString call Unload() -> OzoneEntitySet::Clear() first).
    // This leg is REQUIRED, not cosmetic: ExportToOzone is also the History
    // snapshot format, so without it every undo restore - and every save -
    // silently deleted each `trigger` line in the world (the same
    // authored-vocabulary-loss class as ExportToOzone dropping `flags=`).
    for (const auto& triggerVolume : loader.GetEntities().triggers) {
        Vector3 tn = zup(triggerVolume.bounds.min);
        Vector3 tm = zup(triggerVolume.bounds.max);
        output << "trigger " << triggerVolume.defName
               << " " << std::min(tn.x, tm.x) << " " << std::min(tn.y, tm.y) << " " << std::min(tn.z, tm.z)
               << " " << std::max(tn.x, tm.x) << " " << std::max(tn.y, tm.y) << " " << std::max(tn.z, tm.z)
               << (triggerVolume.once ? " once" : "");
        if (triggerVolume.team != 0)
            output << " team=" << triggerVolume.team;
        output << "\n";
    }

    // Emitters
    for (auto& emitter : pawns.GetEmitters()) {
        const char* et = (emitter.type == EmitterType::SOUND) ? "sound" : "music";
        Vector3 p = zup(emitter.position);
        output << "emitter " << et << " " << p.x << " " << p.y << " " << p.z << "\n";
    }

    // GameEngine.Mesh.Static / GameEngine.Mesh.Skeletal placed objects
    for (auto& m : pawns.GetMeshObjects()) {
        if (m.meshPath.empty()) continue;
        Vector3 p = zup(m.position);
        output << (m.skeletal ? "Mesh.Skeletal " : "Mesh.Static ")
               << m.meshPath << " " << p.x << " " << p.y << " " << p.z << " " << m.yaw;
        if (m.scale != 1.0f) output << " scale=" << m.scale;
        if (!m.texturePath.empty()) output << " tex=" << m.texturePath;
        if (m.skeletal && !m.animClip.empty()) output << " anim=" << m.animClip;
        if (m.skeletal && !m.animFile.empty()) output << " animfile=" << m.animFile;
        if (m.animSpeed != 1.0f) output << " speed=" << m.animSpeed;
        if (m.windAffected) output << " wind=1";
        output << "\n";
    }

    // GameEngine.ParticleEmitter nodes
    for (auto& e : pawns.GetParticleEmitters()) {
        if (!e.active) continue;
        Vector3 p = zup(e.position);
        Vector3 d = zup(e.direction);
        output << "ParticleEmitter " << (e.type.empty() ? "fire" : e.type)
               << " " << p.x << " " << p.y << " " << p.z
               << " " << e.rate << " " << e.lifetime << " " << e.speed << " " << e.spread
               << " " << e.sizeStart << " " << e.sizeEnd
               << " " << (int)e.colorStart.r << " " << (int)e.colorStart.g << " " << (int)e.colorStart.b
               << " " << (int)e.colorEnd.r << " " << (int)e.colorEnd.g << " " << (int)e.colorEnd.b
               << " " << e.gravity << " " << e.radius
               << " " << d.x << " " << d.y << " " << d.z
               << " " << e.yaw;
        if (!e.texturePath.empty()) output << " tex=" << e.texturePath;
        output << "\n";
    }

    // GameEngine.PathNode waypoints
    for (auto& pn : pawns.GetPathNodes()) {
        Vector3 p = zup(pn.position);
        output << "PathNode " << (pn.name.empty() ? "path" : pn.name)
               << " " << p.x << " " << p.y << " " << p.z;
        if (pn.radius != 1.0f) output << " radius=" << pn.radius;
        if (!pn.next.empty()) {
            output << " next=";
            for (size_t i = 0; i < pn.next.size(); i++) {
                if (i) output << ",";
                output << pn.next[i];
            }
        }
        if (pn.loop) output << " loop";
        output << "\n";
    }

    // WindZone regions
    for (auto& wz : pawns.GetWindZones()) {
        Vector3 mn = zup(wz.bounds.min);
        Vector3 mx = zup(wz.bounds.max);
        Vector3 d = zup(wz.direction);
        output << "WindZone "
               << std::min(mn.x, mx.x) << " " << std::min(mn.y, mx.y) << " " << std::min(mn.z, mx.z)
               << " " << std::max(mn.x, mx.x) << " " << std::max(mn.y, mx.y) << " " << std::max(mn.z, mx.z)
               << " " << d.x << " " << d.y << " " << d.z
               << " " << wz.strength << " " << wz.frequency << "\n";
    }

    // Level metadata
    {
        LevelMetadata meta = GetLevelMetadata();
        // `levelinfo` is omitted entirely when every field is default, to keep
        // shipped files clean. The test must therefore cover EVERY field the line
        // can carry — it previously missed timeLimitMinutes and the side skybox,
        // so changing only a time limit or only the cap texture made the whole
        // line vanish on save.
        bool metaNonDefault = meta.gameType != GameType::SINGLEPLAYER ||
                              meta.maxPlayers != 8 || meta.respawnTime != 5.0f ||
                              meta.timeLimitEnabled ||
                              meta.timeLimitMinutes != 10.0f ||
                              meta.scoreLimit != 50 ||
                              meta.friendlyFire ||
                              !meta.skyboxTexturePath.empty() ||
                              !meta.skyboxSidePath.empty();
        if (metaNonDefault) {
            output << "levelinfo " << (int)meta.gameType << " " << meta.maxPlayers << " "
                   << meta.respawnTime << " " << (meta.timeLimitEnabled ? 1 : 0) << " "
                   << meta.timeLimitMinutes << " " << meta.scoreLimit << " "
                   << (meta.friendlyFire ? 1 : 0) << " " << meta.skyboxTexturePath;
            // Second path token: side/cap skybox. Omitted when empty so a level
            // with only the main path keeps its previous token layout — the parser
            // classifies tail tokens positionally after `gametype=`.
            if (!meta.skyboxSidePath.empty())
                output << " " << meta.skyboxSidePath;
            // Append gametype=<key> when the mode is not the default, so the
            // file carries the human-readable name alongside the numeric id.
            // The parser recognises it in the tail; older tools ignore it.
            const char* gtKey = oz::gametype::GameTypeKey(meta.gameType);
            if (gtKey && std::string(gtKey) != "singleplayer")
                output << " gametype=" << gtKey;
            output << "\n";
        }
        // Gate the particles line on ANY field differing from default, not on the
        // type alone. Type-gating meant a wind value authored in the file was
        // silently discarded whenever the type happened to be NONE — the data was
        // in the model, just not in the output.
        const bool particlesNonDefault =
            meta.particleType != ParticleType::NONE ||
            meta.particleDensity != 50.0f ||
            meta.particleSpeed != 1.0f ||
            meta.particleColorR != 200 || meta.particleColorG != 200 ||
            meta.particleColorB != 200 ||
            meta.particleWindX != 0.0f || meta.particleWindZ != 0.0f;
        if (particlesNonDefault) {
            output << "particles " << (int)meta.particleType << " " << meta.particleDensity << " "
                   << meta.particleSpeed << " " << meta.particleColorR << " " << meta.particleColorG << " "
                   << meta.particleColorB << " " << meta.particleWindX << " " << meta.particleWindZ << "\n";
        }
    }

    output << "\n# End of OZONE export\n";
}

static bool SaveWorldDocument(const fs::path& path) {
    // Never overwrite shipped OZWN packages with plain text
    std::string pnorm = path.string();
    for (auto& c : pnorm) if (c == '\\') c = '/';
    if (pnorm.find("System/Data/Zones") != std::string::npos) {
        MessageBoxA(nullptr,
            "This document is a packaged world (System/Data/Zones).\n"
            "Use Save As into GameData/Worlds/<name>/World.ozone to edit it.",
            "Save World", MB_OK | MB_ICONWARNING);
        return false;
    }

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".ozone") {
        std::ofstream output(path);
        if (!output.is_open()) return false;
        ExportToOzone(output);
        g_documentPath = path;
        SetWorldDirectory(path.parent_path());
        return true;
    }

    return false;
}

static void FileNew() {
    g_pendingNew = true;
}

static void FileOpen() {
    std::string path;
    if (ChooseOpenWorldFile(path)) g_pendingOpenPath = fs::path(path);
}

static void FileSaveAs() {
    std::string path;
    if (ChooseSaveWorldFile(path)) SaveWorldDocument(fs::path(path));
}

static bool ApplyTextureToModel(int target, const char* path) {
    LoadedModel* lm = WDLModels.GetModelByWDLId(target);
    if (!lm || lm->model.materialCount == 0) return false;
    Texture2D texture = LoadTextureWithFallback(path);
    if (texture.id == 0) return false;
    if (lm->texture.id > 0) UnloadTexture(lm->texture);
    lm->texture = texture;
    SetMaterialTexture(&lm->model.materials[0], MATERIAL_MAP_DIFFUSE, texture);
    return true;
}
