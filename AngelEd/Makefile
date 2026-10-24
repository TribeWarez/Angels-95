# Detect OS
UNAME_S := $(shell uname -s)

# --- Build mode: release (default) or debug ---
#   release: -O3 -g       shipping build (editor keeps symbols)
#   debug:   -O0 -g       fast compiles for iteration
# Usage:  make -C AngelEd MODE=debug AngelEd
MODE ?= release
ifeq ($(MODE),debug)
  OPTFLAGS := -O0 -g
else ifeq ($(MODE),release)
  OPTFLAGS := -O3 -g
else
  $(error MODE must be 'release' or 'debug' (got '$(MODE)'))
endif

# Optional compiler cache (ccache) and faster linker (LLD), auto-detected via
# `command -v`. Only probed when a POSIX shell is present (UNAME_S non-empty).
# Disable with CCACHE= / LDEXTRA=, or force explicitly.
ifneq ($(UNAME_S),)
  CCACHE ?= $(shell command -v ccache 2>/dev/null)
  LLD_BIN := $(shell command -v ld.lld 2>/dev/null || command -v lld 2>/dev/null)
endif

ifneq ($(CCACHE),)
  CCACHE_PREFIX := $(CCACHE)
else
  CCACHE_PREFIX :=
endif

ifneq ($(LLD_BIN),)
  LDEXTRA ?= -fuse-ld=lld
else
  LDEXTRA ?=
endif

ifeq ($(UNAME_S),Linux)
  $(error AngelEd requires Win32 API (native panels). Build on Windows with w64devkit or MSYS2.)
  CFLAGS   = $(OPTFLAGS) --std=c++20 -fPIC -Wno-narrowing -MMD -MP
  LDFLAGS  = -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
  WIN32_OBJS :=
else
  # Windows (MSYS2/MINGW64)
  # -MMD -MP emits ../build-ed/<obj>.d listing every header that went into it, so
  # a header edit recompiles its consumers. The hand-maintained prerequisites used
  # to be incomplete (OzOzoneLoader.o and SurfaceOps.o both consume
  # Source/World/SurfaceFlags.hpp and neither listed it), which silently produced a
  # stale object and an editor that disagreed with the client. See the root Makefile.
  CFLAGS   = $(OPTFLAGS) --std=c++20 -Wno-narrowing -DWIN32_LEAN_AND_MEAN -I../Source -MMD -MP
  LDFLAGS  = -lraylib -lopengl32 -lgdi32 -lcomctl32 -lwinmm -lcomdlg32 -lm
  # The UI layer is ONE object. Source/UI/UiShell.cpp is a unity TU that includes
# every Source/UI/Panels/*.cpp fragment, so adding a panel means adding a fragment
# - NOT a Makefile rule and NOT a ci.yml line. That is the whole point: CI builds
# the editor by hand, so per-file entries here would have to be duplicated there
# and the two lists would drift silently. See Source/UI/UiShell.cpp.
WIN32_OBJS := UiShell.o
endif

COMP := $(CCACHE_PREFIX) g++
BUILD_DIR := ../build-ed

# Generated header dependencies. Same reason as the root Makefile, and the editor
# had the identical gap: OzOzoneLoader.o and SurfaceOps.o both consume
# Source/World/SurfaceFlags.hpp and neither listed it, so a flag-bit change left
# a stale object and the editor disagreed with the client about what a surface
# flag means. See the root Makefile for the full account.
#
# This is MAKE's -include, not the compiler's: a .d file is makefile syntax.
# make's -include also silently ignores a missing file, which is what you want on
# a clean tree.
#
# .DEFAULT_GOAL is REQUIRED and must come first. A -included .d file begins with
# a target line, and if that file is read before the Makefile names a default,
# make adopts the .d file's first target as the default goal. Symptom: bare
# `make` builds ONE object and exits, printing only that object's name, while
# `make AngelEd` works fine - which looks like a flaky build rather than a
# default-goal hijack.
.DEFAULT_GOAL := all
-include $(wildcard $(BUILD_DIR)/*.d)

.PHONY: all clean

all: AngelEd

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 1. Compile Main Editor Source
$(BUILD_DIR)/Main.o: Source/Main.cpp Source/*.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Main.cpp -o $@

# The editor event bus (AngelEd/Source/Core/). Only linked here because R2; it has
# no other consumer in the editor yet. Kept as its own object rather than folded
# into Main.o so the Makefile and ci.yml lists match one-for-one.
$(BUILD_DIR)/EditorEventBus.o: Source/Core/EditorEventBus.cpp Source/Core/EditorEventBus.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Core/EditorEventBus.cpp -o $@

# 2. Compile Win32 Dialog support (Windows only)
$(BUILD_DIR)/UiShell.o: Source/UI/UiShell.cpp Source/UI/UiCommon.hpp Source/UI/UiPanels.hpp $(wildcard Source/UI/Panels/*.cpp) | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/UI/UiShell.cpp -o $@

# 3. Compile Custom Engine Behavior
$(BUILD_DIR)/Custom.so:
	@if [ -f Source/OTCustom.cpp ]; then \
		$(COMP) $(CFLAGS) -c Source/OTCustom.cpp -o $(BUILD_DIR)/OTCustom.o; \
	elif [ -f Source/Custom.cpp ]; then \
		$(COMP) $(CFLAGS) -c Source/Custom.cpp -o $(BUILD_DIR)/OTCustom.o; \
	else \
		echo "int main_custom() { return 0; }" | $(COMP) $(CFLAGS) -x c++ -c - -o $(BUILD_DIR)/OTCustom.o; \
	fi
	$(COMP) $(CFLAGS) $(LDEXTRA) -shared $(BUILD_DIR)/OTCustom.o -o $@
	rm -f $(BUILD_DIR)/OTCustom.o

# 4a. Compile LightningScript system
$(BUILD_DIR)/LightningScriptContext.o: ../Source/Script/LightningScriptContext.cpp ../Source/Script/LightningScriptContext.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Script/LightningScriptContext.cpp -o $@

$(BUILD_DIR)/LightningScriptParser.o: ../Source/Script/LightningScriptParser.cpp ../Source/Script/LightningScriptParser.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Script/LightningScriptParser.cpp -o $@

$(BUILD_DIR)/LightningEntityRegistry.o: ../Source/Script/LightningEntityRegistry.cpp ../Source/Script/LightningEntityRegistry.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Script/LightningEntityRegistry.cpp -o $@

$(BUILD_DIR)/LightningEntityManager.o: ../Source/Script/LightningEntityManager.cpp ../Source/Script/LightningEntityManager.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -DOMEGA_TEST_ENV -c ../Source/Script/LightningEntityManager.cpp -o $@

# .ozls writer (raylib-free). The Entity Properties panel writes stat edits back
# to the source .ozls through this.
$(BUILD_DIR)/OzlsWriter.o: ../Source/Script/OzlsWriter.cpp ../Source/Script/OzlsWriter.hpp ../Source/Script/LightningEntityDef.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Script/OzlsWriter.cpp -o $@

# 4. Compile raygui layer
$(BUILD_DIR)/raygui.o: Source/raygui/raygui.c | $(BUILD_DIR)
	$(COMP) -fpermissive $(CFLAGS) -c Source/raygui/raygui.c -shared -DRAYGUI_IMPLEMENTATION -DBUILD_LIBTYPE_SHARED $(LDFLAGS) -o $@

# 5. Link into final AngelEd binary
LIGHTNING_OBJS := $(addprefix $(BUILD_DIR)/, \
    LightningScriptContext.o LightningScriptParser.o \
    LightningEntityRegistry.o LightningEntityManager.o)
$(BUILD_DIR)/EditorIcons.o: Source/EditorIcons.cpp Source/EditorIcons.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/EditorIcons.cpp -o $@
# 5b. Core engine objects needed by the editor
$(BUILD_DIR)/OzPawnSystem.o: ../Source/Pawn/OzPawnSystem.cpp ../Source/Pawn/OzPawnSystem.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Pawn/OzPawnSystem.cpp -o $@

$(BUILD_DIR)/OzOzoneLoader.o: ../Source/World/OzOzoneLoader.cpp ../Source/World/OzOzoneLoader.hpp ../Source/World/OzoneParser.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/World/OzOzoneLoader.cpp -o $@

$(BUILD_DIR)/OzoneFrustum.o: ../Source/World/OzoneFrustum.cpp ../Source/World/OzoneFrustum.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/World/OzoneFrustum.cpp -o $@

$(BUILD_DIR)/OzoneHeightmap.o: ../Source/World/OzoneHeightmap.cpp ../Source/World/OzOzoneLoader.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/World/OzoneHeightmap.cpp -o $@

$(BUILD_DIR)/OzBsp.o: ../Source/Physics/OzBsp.cpp ../Source/Physics/OzBsp.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Physics/OzBsp.cpp -o $@

$(BUILD_DIR)/AutoConvex.o: ../Source/Physics/AutoConvex.cpp ../Source/Physics/AutoConvex.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Physics/AutoConvex.cpp -o $@

$(BUILD_DIR)/OzoneParser.o: ../Source/World/OzoneParser.cpp ../Source/World/OzoneParser.hpp ../Source/World/SurfaceFlags.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/World/OzoneParser.cpp -o $@

$(BUILD_DIR)/ZoneManager.o: ../Source/World/ZoneManager.cpp ../Source/World/ZoneManager.hpp ../Source/World/ZoneTypes.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/World/ZoneManager.cpp -o $@

$(BUILD_DIR)/SurfaceFlags.o: ../Source/World/SurfaceFlags.cpp ../Source/World/SurfaceFlags.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/World/SurfaceFlags.cpp -o $@

$(BUILD_DIR)/SurfaceMaterial.o: ../Source/Renderer/SurfaceMaterial.cpp ../Source/Renderer/SurfaceMaterial.hpp ../Source/World/SurfaceFlags.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Renderer/SurfaceMaterial.cpp -o $@

$(BUILD_DIR)/GameType.o: ../Source/World/GameType.cpp ../Source/World/GameType.hpp ../Source/World/LevelSettings.hpp ../Source/Script/LightningEntityRegistry.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/World/GameType.cpp -o $@

$(BUILD_DIR)/SoundManager.o: ../Source/Audio/SoundManager.cpp ../Source/Audio/SoundManager.hpp ../Source/Audio/DspReverb.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Audio/SoundManager.cpp -o $@

$(BUILD_DIR)/Log.o: ../Source/Log.cpp ../Source/Log.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Log.cpp -o $@

$(BUILD_DIR)/OzAssetMapper.o: ../Source/Renderer/OzAssetMapper.cpp ../Source/Renderer/OzAssetMapper.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Renderer/OzAssetMapper.cpp -o $@

# miniz (C compile; g++ would mangle symbols vs the extern "C" prototypes)
$(BUILD_DIR)/miniz.o: ../Source/miniz/miniz.c ../Source/miniz/miniz.h | $(BUILD_DIR)
	$(CCACHE_PREFIX) gcc $(OPTFLAGS) -std=c99 -I../Source/miniz -c ../Source/miniz/miniz.c -o $@

$(BUILD_DIR)/WorldChunk.o: ../Source/Physics/WorldChunk.cpp ../Source/Physics/WorldChunk.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Physics/WorldChunk.cpp -o $@

$(BUILD_DIR)/LitLightning.o: ../Source/Renderer/LitLightning.cpp ../Source/Renderer/LitLightning.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Renderer/LitLightning.cpp -o $@

$(BUILD_DIR)/Mesh.o: ../Source/Renderer/Mesh/Mesh.cpp ../Source/Renderer/Mesh/Mesh.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Renderer/Mesh/Mesh.cpp -o $@

# Material graph + registry. Same two objects the client links; the editor also
# needs them because it resolves `material=` props in its viewport.
$(BUILD_DIR)/OzMaterial.o: ../Source/Material/OzMaterial.cpp ../Source/Material/OzMaterial.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Material/OzMaterial.cpp -o $@

$(BUILD_DIR)/MaterialRegistry.o: ../Source/Material/MaterialRegistry.cpp ../Source/Material/MaterialRegistry.hpp ../Source/Material/OzMaterial.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Material/MaterialRegistry.cpp -o $@

$(BUILD_DIR)/SkeletalMesh.o: ../Source/Renderer/Mesh/SkeletalMesh.cpp ../Source/Renderer/Mesh/SkeletalMesh.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Renderer/Mesh/SkeletalMesh.cpp -o $@

$(BUILD_DIR)/MeshCache.o: ../Source/Renderer/Mesh/MeshCache.cpp ../Source/Renderer/Mesh/MeshCache.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Renderer/Mesh/MeshCache.cpp -o $@

$(BUILD_DIR)/OzAnimFormat.o: ../Source/Package/Anim/OzAnimFormat.cpp ../Source/Package/Anim/OzAnimFormat.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Package/Anim/OzAnimFormat.cpp -o $@

$(BUILD_DIR)/AnimatedMesh.o: ../Source/Renderer/Mesh/AnimatedMesh.cpp ../Source/Renderer/Mesh/AnimatedMesh.hpp ../Source/Package/Anim/OzAnimFormat.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Renderer/Mesh/AnimatedMesh.cpp -o $@

$(BUILD_DIR)/OzParticleSimulationManager.o: ../Source/Particle/OzParticleSimulationManager.cpp ../Source/Particle/OzParticleSimulationManager.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Particle/OzParticleSimulationManager.cpp -o $@

$(BUILD_DIR)/rlights.o: ../Source/Renderer/rlights/rlights.cpp ../Source/Renderer/rlights/rlights.h | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Renderer/rlights/rlights.cpp -o $@

# --- Resources/ layer ---------------------------------------------------------
# Real translation units, not unity fragments, and this is the one place the extra
# build wiring is accepted rather than avoided. AssetScope and PackageIO are each
# shared by two UI panels but used to be implemented INSIDE one of them (TexturePanel),
# reachable from the other only because the UI layer is a single translation unit.
# A shared contract with no header of its own is not a contract. See
# Wiki/Editor-Architecture-Refactor.md.
#
# CONSEQUENCE: adding a Resources/ file means adding a rule here AND a `g++ -c` line
# plus a link entry in .github/workflows/ci.yml, which compiles the editor by hand.
# That is the "added in one place, CI breaks silently" trap from AGENTS.md, and it is
# paid here deliberately - the same way the root Makefile and ci.yml list engine
# objects. Diff the two lists when changing either; they must agree apart from
# OTCustom_stub.o, which exists only because CI links a stub where the Makefile links
# -l:Custom.so.
RESOURCES_OBJS := $(BUILD_DIR)/AssetScope.o $(BUILD_DIR)/PackageIO.o $(BUILD_DIR)/AssetScan.o $(BUILD_DIR)/MaterialBake.o

# Subsystems/ promoted to REAL translation units in Phase F. These used to be unity
# fragments included by Main.cpp, which meant they inherited its include order and
# could not be compiled on their own. Add a file here AND in ci.yml.
SUBSYSTEMS_OBJS := $(BUILD_DIR)/SurfaceOps.o $(BUILD_DIR)/Placement.o $(BUILD_DIR)/PropsApply.o \
                  $(BUILD_DIR)/LevelState.o $(BUILD_DIR)/History.o

# Each promoted subsystem needs exactly three things: a header, a rule here, and a
# `g++ -c` line plus a link entry in .github/workflows/ci.yml. The lists must stay
# equal - diff them when changing either.

$(BUILD_DIR)/SurfaceOps.o: Source/Subsystems/SurfaceOps.cpp Source/Subsystems/SurfaceOps.hpp Source/Core/EditorState.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Subsystems/SurfaceOps.cpp -o $@

$(BUILD_DIR)/Placement.o: Source/Subsystems/Placement.cpp Source/Subsystems/Placement.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Subsystems/Placement.cpp -o $@

$(BUILD_DIR)/PropsApply.o: Source/Subsystems/PropsApply.cpp Source/Subsystems/PropsApply.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Subsystems/PropsApply.cpp -o $@

$(BUILD_DIR)/LevelState.o: Source/Subsystems/LevelState.cpp Source/Subsystems/LevelState.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Subsystems/LevelState.cpp -o $@

$(BUILD_DIR)/History.o: Source/Subsystems/History.cpp Source/Subsystems/History.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Subsystems/History.cpp -o $@

$(BUILD_DIR)/AssetScope.o: Source/Resources/AssetScope.cpp Source/Resources/AssetScope.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Resources/AssetScope.cpp -o $@

$(BUILD_DIR)/PackageIO.o: Source/Resources/PackageIO.cpp Source/Resources/PackageIO.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Resources/PackageIO.cpp -o $@

# AssetScan is the one Resources object a headless suite cannot link: it reads the
# filesystem and PackageAssetLoader. Its POLICY (sort + dedupe) lives in AssetScope.cpp
# precisely so that part stays testable - see tests/AssetScope.test.cpp.
$(BUILD_DIR)/AssetScan.o: Source/Resources/AssetScan.cpp Source/Resources/AssetScan.hpp Source/Resources/AssetScope.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Resources/AssetScan.cpp -o $@

# Material baker: evaluates a .ozmat graph and writes the PNGs. Editor-only (it
# decodes/encodes images through raylib) but shared by the Tools menu and the
# material panel, so it is a real Resources/ object.
$(BUILD_DIR)/MaterialBake.o: Source/Resources/MaterialBake.cpp Source/Resources/MaterialBake.hpp ../Source/Material/OzMaterial.hpp ../Source/Package/PackageAssetLoader.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Resources/MaterialBake.cpp -o $@

# Game-hierarchy engine objects the editor links because its own objects call
# them: OzPawnSystem emits EventBus facts (pickup.collect / zone.enter / entity.*),
# OzOzoneLoader injects trigger volumes into TriggerSystem and world entities
# through BoxedTypeRegistry, and LightningEntityManager subscribes the bus bridge
# and reads/writes ObjectiveSystem. All four are raylib-free. A line here MUST
# have a matching `g++ -c` + link entry in .github/workflows/ci.yml (which
# compiles the editor by hand); tools/check_editor_objects.sh enforces it.
#
# DELIBERATE EXCEPTION: Source/Game/{GameManager,GameDef,GameOpcodes,
# GameBoxedTypes}.cpp are NOT in the editor build. No AngelEd panel reads them
# yet (the Entity Properties game schemas are static tables in PropsPanel.cpp,
# not GameManager lookups), so linking them would be dead weight. If a panel
# ever needs GameManager, add the objects here AND to ci.yml in the same
# commit — check_editor_objects.sh will fail loudly on a mismatch either way.
$(BUILD_DIR)/EventBus.o: ../Source/Events/EventBus.cpp ../Source/Events/EventBus.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Events/EventBus.cpp -o $@

$(BUILD_DIR)/TriggerSystem.o: ../Source/Events/TriggerSystem.cpp ../Source/Events/TriggerSystem.hpp ../Source/Events/EventBus.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Events/TriggerSystem.cpp -o $@

$(BUILD_DIR)/ObjectiveSystem.o: ../Source/Events/ObjectiveSystem.cpp ../Source/Events/ObjectiveSystem.hpp ../Source/Events/EventBus.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Events/ObjectiveSystem.cpp -o $@

$(BUILD_DIR)/BoxedTypeRegistry.o: ../Source/Boxed/BoxedTypeRegistry.cpp ../Source/Boxed/BoxedTypeRegistry.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c ../Source/Boxed/BoxedTypeRegistry.cpp -o $@

CORE_OBJS := $(BUILD_DIR)/OzPawnSystem.o $(BUILD_DIR)/OzOzoneLoader.o $(BUILD_DIR)/OzoneFrustum.o $(BUILD_DIR)/OzoneHeightmap.o $(BUILD_DIR)/OzBsp.o \
             $(BUILD_DIR)/AutoConvex.o \
             $(BUILD_DIR)/OzoneParser.o $(BUILD_DIR)/ZoneManager.o $(BUILD_DIR)/SoundManager.o $(BUILD_DIR)/GameType.o \
             $(BUILD_DIR)/SurfaceFlags.o $(BUILD_DIR)/SurfaceMaterial.o $(BUILD_DIR)/Log.o $(BUILD_DIR)/OzAssetMapper.o \
             $(BUILD_DIR)/WorldChunk.o $(BUILD_DIR)/LitLightning.o $(BUILD_DIR)/Mesh.o $(BUILD_DIR)/SkeletalMesh.o \
             $(BUILD_DIR)/MeshCache.o $(BUILD_DIR)/AnimatedMesh.o $(BUILD_DIR)/OzAnimFormat.o \
             $(BUILD_DIR)/OzMaterial.o $(BUILD_DIR)/MaterialRegistry.o \
             $(BUILD_DIR)/OzParticleSimulationManager.o $(BUILD_DIR)/rlights.o $(BUILD_DIR)/miniz.o \
             $(BUILD_DIR)/EventBus.o $(BUILD_DIR)/TriggerSystem.o $(BUILD_DIR)/ObjectiveSystem.o \
             $(BUILD_DIR)/BoxedTypeRegistry.o \
             $(BUILD_DIR)/OzlsWriter.o

# 5c. Editor Windows resource
$(BUILD_DIR)/AngelEd.res: AngelEd.rc ../GameData/Tex/Editor/angelEd.ico | $(BUILD_DIR)
	windres AngelEd.rc -O coff -o $@

EDITOR_OBJS := $(BUILD_DIR)/raygui.o $(BUILD_DIR)/Main.o $(BUILD_DIR)/EditorEventBus.o $(BUILD_DIR)/EditorIcons.o $(BUILD_DIR)/AngelEd.res $(addprefix $(BUILD_DIR)/,$(WIN32_OBJS)) $(LIGHTNING_OBJS) $(CORE_OBJS) $(RESOURCES_OBJS) $(SUBSYSTEMS_OBJS)
AngelEd: $(EDITOR_OBJS) $(BUILD_DIR)/Custom.so
	$(COMP) $(LDEXTRA) $(EDITOR_OBJS) -o AngelEd $(CFLAGS) -L$(BUILD_DIR) -l:Custom.so $(LDFLAGS) -Wl,-rpath=$(BUILD_DIR)

clean:
	rm -f AngelEd
	rm -rf $(BUILD_DIR)
