# Detect OS for cross-platform support
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  PIC := -fPIC
  # Where raylib.h lives. Previously RAYLIB_INC was only ever set in the Windows arm, so
  # on Linux every raylib-dependent target found raylib.h solely because /usr/local/include
  # is a gcc default search path. That made a non-default prefix - which build.sh's own
  # CMake line and every distro packaging guide produce - fail with a bare
  # "raylib.h: No such file". pkg-config is the portable answer; the two probes are
  # ordered most-specific first and both may be empty.
  RAYLIB_INC := $(shell pkg-config --cflags raylib 2>/dev/null)
  ifeq ($(strip $(RAYLIB_INC)),)
    RAYLIB_INC := $(shell test -f /usr/local/include/raylib.h && echo -I/usr/local/include)
  endif
  ifeq ($(strip $(RAYLIB_INC)),)
    RAYLIB_INC := $(shell test -f /usr/include/raylib.h && echo -I/usr/include)
  endif
  # -pthread, not -lpthread: it also defines _REENTRANT, which libstdc++'s thread
  # headers check. Video.hpp's title-screen fetch worker calls pthread_create directly.
  LDFLAGS := -lraylib -lGL -lm -pthread -ldl -lrt -lX11
  RPATH := -Wl,-rpath=.
  EXE :=
  SERVER_LIBS := -lm -pthread
else
  # Windows (MINGW/MSYS/CYGWIN)
  PIC :=
  # raylib library path (detect w64devkit; fall back to default search path)
  RAYLIB_LIB := $(wildcard C:/raylib/w64devkit/lib/libraylib.a)
  ifneq ($(RAYLIB_LIB),)
    RAYLIB_DIR := -LC:/raylib/w64devkit/lib
    RAYLIB_INC := -IC:/raylib/w64devkit/include
  else
    RAYLIB_DIR :=
    RAYLIB_INC :=
  endif
  LDFLAGS := $(RAYLIB_DIR) -lraylib -lopengl32 -lgdi32 -lwinmm -lws2_32 -lwinhttp -lm
  RPATH :=
  EXE := .exe
  SERVER_LIBS := -lm -lws2_32 -lwinhttp
endif

# Windows resource files (icon embedding) only apply to Windows builds;
# GNU windres is not guaranteed on Linux runners.
ifneq ($(EXE),)
RES_95  = $(BUILD_DIR)/Angels95.res
RES_SRV = $(BUILD_DIR)/AngelServ.res
else
RES_95  =
RES_SRV =
endif

# --- Build mode: release (default) or debug ---
#   release: -O3          shipping build
#   debug:   -O0 -g       fast compiles + symbols for iteration
# Usage:  make MODE=debug OTENGINE
MODE ?= release
ifeq ($(MODE),debug)
  OPTFLAGS := -O0 -g
else ifeq ($(MODE),release)
  OPTFLAGS := -O3
else
  $(error MODE must be 'release' or 'debug' (got '$(MODE)'))
endif

# Optional compiler cache (ccache) and faster linker (LLD), auto-detected via
# `command -v`. Only probed when a POSIX shell is present (UNAME_S is set by the
# `uname` probe above); this keeps cmd.exe-based make quiet.
# Disable with CCACHE= / LDEXTRA=, or force with CCACHE=<path> / LDEXTRA=-fuse-ld=lld.
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

# -MMD -MP emits a .d file next to every .o naming every header that went into
# it; -include then reads those back so make knows the .o is stale. This exists
# because the hand-maintained header prerequisites were INCOMPLETE and silently
# so: OzOzoneLoader.o listed .cpp/.hpp/OzoneParser.hpp but NOT
# Source/World/SurfaceFlags.hpp, which its own header includes. So changing a flag
# bit - or the backdrop gain - left a stale OzOzoneLoader.o in build/, and a
# renumber compiled cleanly against a renderer that disagreed with the header.
# Nothing warned; the flag just did nothing.
#
# Hand-written prerequisites rot exactly this way (a new #include is not a build
# edit). Generated ones cannot. The explicit prerequisites below are harmless but
# are no longer load-bearing.
DEPFLAGS := -MMD -MP

CFLAGS := $(OPTFLAGS) --std=c++20 $(PIC) $(RAYLIB_INC) $(DEPFLAGS)
COMP := $(CCACHE_PREFIX) g++
CC := $(CCACHE_PREFIX) gcc
SERVER_CXX := $(CCACHE_PREFIX) g++
# $(PIC) was missing here while miniz.o below is built WITH it, so AngelServ linked a mix
# of PIC and non-PIC objects. Harmless on the default x86-64 toolchain; a toolchain that
# defaults to -no-pie with strict relocation checks rejects it. Also carries $(RAYLIB_INC)
# because LightningEntityRegistry.cpp includes PackageAssetLoader.hpp, which pulls
# raylib.h - see the note there on why that include has not been removed yet.
SERVER_FLAGS := $(OPTFLAGS) --std=c++20 $(PIC) $(RAYLIB_INC) $(DEPFLAGS)

BUILD_DIR := build

# Read back the generated dependency files. This is MAKE's -include, not the
# compiler's: a .d file is makefile syntax, and make's -include silently ignores
# a missing file (which is what you want on a clean tree). Passing these to the
# COMPILER's -include instead makes gcc try to preprocess makefile text as C++,
# which fails with "'build' does not name a type" on the first target line.
#
# .DEFAULT_GOAL is REQUIRED and must come first. A -included .d file begins with
# a target line; if it is read before this Makefile names a default goal, make
# adopts the .d file's first target as the default. Symptom: bare `make` builds
# ONE object and exits while `make OTENGINE` works - which reads as a flaky build
# rather than a default-goal hijack.
.DEFAULT_GOAL := all
-include $(wildcard $(BUILD_DIR)/*.d)
# Every object Angels95 links. This list is the SINGLE source of truth: OTENGINE
# depends on $(OBJS), so a new object added here is picked up by the link
# automatically. It previously kept a second hand-maintained copy in the OTENGINE
# prerequisite list, and the two drifted -- cf3159c added PlayerProfile.o here but
# not there, so the link failed on undefined PlayerProfileManager symbols in CI.
# miniz.o is C (see the note on its rule) but is an ordinary link input here.
OBJS := $(addprefix $(BUILD_DIR)/, \
          raygui.o miniz.o Main.o Network.o Log.o Client.o \
          OzAssetMapper.o OzPawnSystem.o GameUi.o SlotBar.o InventoryBehaviour.o WeaponBehaviour.o PlayerController.o PickupPawns.o \
          OzOzoneLoader.o OzoneFrustum.o OzoneHeightmap.o OzoneParser.o OzBsp.o AutoConvex.o WorldChunk.o PlayerPhysics.o \
          ZoneManager.o SoundManager.o GameType.o SurfaceFlags.o \
          LightningScriptContext.o LightningScriptParser.o \
          LightningEntityRegistry.o LightningEntityManager.o \
          LitLightning.o SurfaceMaterial.o Mesh.o SkeletalMesh.o MeshCache.o AnimatedMesh.o OzAnimFormat.o \
          ViewModel.o PlayerModel.o OzParticleSimulationManager.o rlights.o UiHandler.o \
          PlayerProfile.o \
          EventBus.o TriggerSystem.o ObjectiveSystem.o BoxedTypeRegistry.o \
          GameDef.o GameManager.o GameBoxedTypes.o GameOpcodes.o)

.PHONY: all clean test help
all: OTENGINE AngelServ AngelMaster ozpack

# Build-speed helpers:
#   MODE=debug     -O0 -g instead of -O3 (much faster compiles for iteration)
#   CCACHE=...     compiler cache; auto-detected when 'ccache' is on PATH
#   LDEXTRA=...    extra linker driver flags; -fuse-ld=lld auto-set when available
help:
	@echo "Targets: OTENGINE AngelServ AngelMaster ozpack test clean"
	@echo "MODE      = $(MODE)      (release | debug)"
	@echo "OPTFLAGS  = $(OPTFLAGS)"
	@echo "ccache    = $(if $(CCACHE),$(CCACHE),<not found>)"
	@echo "LDEXTRA   = $(if $(LDEXTRA),$(LDEXTRA),<none>)"
	@echo ""
	@echo "Fast iteration:  make -j$$(nproc) MODE=debug OTENGINE"

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 1. Compile Main Game Logic
$(BUILD_DIR)/Main.o: Source/Main.cpp Source/*.hpp Source/UI/*.hpp Source/Package/*.hpp Source/Pawn/*.hpp Source/Pawn/AngelPlayer/*.hpp Source/Renderer/*.hpp Source/Audio/*.hpp Source/Menu/*.hpp Source/Server/Master/*.hpp Source/Physics/*.hpp Source/Client/*.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Main.cpp -o $@

# 4. Compile raygui helper
$(BUILD_DIR)/raygui.o: Source/Renderer/raygui/raygui.c | $(BUILD_DIR)
	$(COMP) -fpermissive $(CFLAGS) -c Source/Renderer/raygui/raygui.c -DRAYGUI_IMPLEMENTATION -o $@

# 4b. Compile miniz (public-domain zlib substitute used by OzPackage).
# Must be compiled as C (gcc): g++ would C++-mangle the definitions while
# consumers see extern "C" prototypes from miniz.h.
$(BUILD_DIR)/miniz.o: Source/miniz/miniz.c Source/miniz/miniz.h | $(BUILD_DIR)
	$(CC) $(OPTFLAGS) -std=c99 $(PIC) -ISource/miniz -c Source/miniz/miniz.c -o $@

# 5. Compile Network library (used by both client and server)
$(BUILD_DIR)/Network.o: Source/Network/Network.cpp Source/Network/Network.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Network/Network.cpp -o $@

# 5b. Compile Log system
$(BUILD_DIR)/Log.o: Source/Log.cpp Source/Log.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Log.cpp -o $@

# 5c. Compile Client networking
$(BUILD_DIR)/Client.o: Source/Client/Client.cpp Source/Client/Client.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Client/Client.cpp -o $@

# 5c-player. Player profile slots (raylib-free: also usable by AngelEd/server).
$(BUILD_DIR)/PlayerProfile.o: Source/PlayerProfile.cpp Source/PlayerProfile.hpp Source/IniConfig.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/PlayerProfile.cpp -o $@

# 5d. Compile the Oz* subsystem modules
$(BUILD_DIR)/OzAssetMapper.o: Source/Renderer/OzAssetMapper.cpp Source/Renderer/OzAssetMapper.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Renderer/OzAssetMapper.cpp -o $@

$(BUILD_DIR)/OzPawnSystem.o: Source/Pawn/OzPawnSystem.cpp Source/Pawn/OzPawnSystem.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Pawn/OzPawnSystem.cpp -o $@

$(BUILD_DIR)/GameUi.o: Source/Pawn/AngelPlayer/GameUi.cpp Source/Pawn/AngelPlayer/GameUi.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Pawn/AngelPlayer/GameUi.cpp -o $@

$(BUILD_DIR)/InventoryBehaviour.o: Source/Pawn/AngelPlayer/InventoryBehaviour.cpp Source/Pawn/AngelPlayer/InventoryBehaviour.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Pawn/AngelPlayer/InventoryBehaviour.cpp -o $@

$(BUILD_DIR)/SlotBar.o: Source/Pawn/AngelPlayer/SlotBar.cpp Source/Pawn/AngelPlayer/SlotBar.hpp Source/Pawn/AngelPlayer/GameUi.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Pawn/AngelPlayer/SlotBar.cpp -o $@

$(BUILD_DIR)/WeaponBehaviour.o: Source/Pawn/AngelPlayer/WeaponBehaviour.cpp Source/Pawn/AngelPlayer/WeaponBehaviour.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Pawn/AngelPlayer/WeaponBehaviour.cpp -o $@

$(BUILD_DIR)/PlayerController.o: Source/Pawn/AngelPlayer/PlayerController.cpp Source/Pawn/AngelPlayer/PlayerController.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Pawn/AngelPlayer/PlayerController.cpp -o $@

$(BUILD_DIR)/PickupPawns.o: Source/Pawn/PickupPawns.cpp Source/Pawn/PickupPawns.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Pawn/PickupPawns.cpp -o $@

$(BUILD_DIR)/LitLightning.o: Source/Renderer/LitLightning.cpp Source/Renderer/LitLightning.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Renderer/LitLightning.cpp -o $@

# 5d-mesh. Compile the oz::Mesh taxonomy render primitives
$(BUILD_DIR)/Mesh.o: Source/Renderer/Mesh/Mesh.cpp Source/Renderer/Mesh/Mesh.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Renderer/Mesh/Mesh.cpp -o $@

$(BUILD_DIR)/SkeletalMesh.o: Source/Renderer/Mesh/SkeletalMesh.cpp Source/Renderer/Mesh/SkeletalMesh.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Renderer/Mesh/SkeletalMesh.cpp -o $@

$(BUILD_DIR)/MeshCache.o: Source/Renderer/Mesh/MeshCache.cpp Source/Renderer/Mesh/MeshCache.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Renderer/Mesh/MeshCache.cpp -o $@

# 5d-anim. Vertex-keyframe (morph) animation format + runtime mesh
$(BUILD_DIR)/OzAnimFormat.o: Source/Package/Anim/OzAnimFormat.cpp Source/Package/Anim/OzAnimFormat.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Package/Anim/OzAnimFormat.cpp -o $@

$(BUILD_DIR)/AnimatedMesh.o: Source/Renderer/Mesh/AnimatedMesh.cpp Source/Renderer/Mesh/AnimatedMesh.hpp Source/Package/Anim/OzAnimFormat.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Renderer/Mesh/AnimatedMesh.cpp -o $@

$(BUILD_DIR)/ViewModel.o: Source/Renderer/ViewModel.cpp Source/Renderer/ViewModel.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Renderer/ViewModel.cpp -o $@

$(BUILD_DIR)/PlayerModel.o: Source/Renderer/PlayerModel.cpp Source/Renderer/PlayerModel.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Renderer/PlayerModel.cpp -o $@

# 5d-ui. Native menu bar / platform UI handler
$(BUILD_DIR)/UiHandler.o: Source/UI/UiHandler.cpp Source/UI/UiHandler.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/UI/UiHandler.cpp -o $@

# 5d-particle. Isolated ParticleEmitter simulation
$(BUILD_DIR)/OzParticleSimulationManager.o: Source/Particle/OzParticleSimulationManager.cpp Source/Particle/OzParticleSimulationManager.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Particle/OzParticleSimulationManager.cpp -o $@

$(BUILD_DIR)/rlights.o: Source/Renderer/rlights/rlights.cpp Source/Renderer/rlights/rlights.h | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Renderer/rlights/rlights.cpp -o $@

# 5i. OzOzoneLoader (OzWorld format loader)
$(BUILD_DIR)/OzOzoneLoader.o: Source/World/OzOzoneLoader.cpp Source/World/OzOzoneLoader.hpp Source/World/OzoneParser.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/World/OzOzoneLoader.cpp -o $@

$(BUILD_DIR)/OzoneFrustum.o: Source/World/OzoneFrustum.cpp Source/World/OzoneFrustum.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/World/OzoneFrustum.cpp -o $@

$(BUILD_DIR)/OzoneHeightmap.o: Source/World/OzoneHeightmap.cpp Source/World/OzOzoneLoader.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/World/OzoneHeightmap.cpp -o $@

# 5e. Compile OzoneParser (used by both client and server)
$(BUILD_DIR)/OzoneParser.o: Source/World/OzoneParser.cpp Source/World/OzoneParser.hpp Source/World/SurfaceFlags.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/World/OzoneParser.cpp -o $@

# 5e2. Zone volume storage + runtime queries (extracted from PawnSystem)
$(BUILD_DIR)/ZoneManager.o: Source/World/ZoneManager.cpp Source/World/ZoneManager.hpp Source/World/ZoneTypes.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/World/ZoneManager.cpp -o $@

# 5e2b. Game-mode taxonomy + ruleset data (raylib-free; server + editor + tests)
$(BUILD_DIR)/GameType.o: Source/World/GameType.cpp Source/World/GameType.hpp Source/World/LevelSettings.hpp Source/Script/LightningEntityRegistry.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/World/GameType.cpp -o $@

# 5e2c. Surface flag registry + per-face surface props (raylib-free; links the
# OZONE parser, the server's worldcheck, the editor and the tests)
$(BUILD_DIR)/SurfaceFlags.o: Source/World/SurfaceFlags.cpp Source/World/SurfaceFlags.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/World/SurfaceFlags.cpp -o $@

# 5e2d. Surface shader CPU half (per-face uniforms + GL state bracketing)
$(BUILD_DIR)/SurfaceMaterial.o: Source/Renderer/SurfaceMaterial.cpp Source/Renderer/SurfaceMaterial.hpp Source/World/SurfaceFlags.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Renderer/SurfaceMaterial.cpp -o $@

# 5e3. Audio facade (one-shots, world music, sound zones, reverb, script SFX)
$(BUILD_DIR)/SoundManager.o: Source/Audio/SoundManager.cpp Source/Audio/SoundManager.hpp Source/Audio/DspReverb.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Audio/SoundManager.cpp -o $@

# 5f. Compile CSG/BSP processor
$(BUILD_DIR)/OzBsp.o: Source/Physics/OzBsp.cpp Source/Physics/OzBsp.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Physics/OzBsp.cpp -o $@

# 5f-b. AutoConvex (voxel-derived convex collision proxies for meshes/brushes).
# Raylib-free, so it also builds into the headless test_autoconvex target.
$(BUILD_DIR)/AutoConvex.o: Source/Physics/AutoConvex.cpp Source/Physics/AutoConvex.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Physics/AutoConvex.cpp -o $@

# 5g. Compile WorldChunk spatial partition
$(BUILD_DIR)/WorldChunk.o: Source/Physics/WorldChunk.cpp Source/Physics/WorldChunk.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Physics/WorldChunk.cpp -o $@

# 5g-b. Player/collision physics module
$(BUILD_DIR)/PlayerPhysics.o: Source/Physics/PlayerPhysics.cpp Source/Physics/PlayerPhysics.hpp Source/Physics/PhysicsInfo.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Physics/PlayerPhysics.cpp -o $@

# 5h. Compile LightningScript system
$(BUILD_DIR)/LightningScriptContext.o: Source/Script/LightningScriptContext.cpp Source/Script/LightningScriptContext.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Script/LightningScriptContext.cpp -o $@

$(BUILD_DIR)/LightningScriptParser.o: Source/Script/LightningScriptParser.cpp Source/Script/LightningScriptParser.hpp Source/Script/LightningEntityDef.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Script/LightningScriptParser.cpp -o $@

$(BUILD_DIR)/LightningEntityRegistry.o: Source/Script/LightningEntityRegistry.cpp Source/Script/LightningEntityRegistry.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Script/LightningEntityRegistry.cpp -o $@

$(BUILD_DIR)/LightningEntityManager.o: Source/Script/LightningEntityManager.cpp Source/Script/LightningEntityManager.hpp | $(BUILD_DIR)
	$(COMP) $(CFLAGS) -c Source/Script/LightningEntityManager.cpp -o $@

# 6a. Compile Windows resource files
ifneq ($(EXE),)
$(BUILD_DIR)/Angels95.res: Source/Angels95.rc GameData/Tex/Engine/angels95.ico | $(BUILD_DIR)
	windres Source/Angels95.rc -O coff -o $@

$(BUILD_DIR)/AngelServ.res: Source/AngelServ.rc GameData/Tex/Engine/AngelServ.ico | $(BUILD_DIR)
	windres Source/AngelServ.rc -O coff -o $@
endif

# 6b. Build Game Binary
OTENGINE: $(RES_95) $(OBJS)
	$(COMP) $(LDEXTRA) $^ -o Angels95$(EXE) $(CFLAGS) $(LDFLAGS) $(RPATH)

# 7. Build AngelServ (dedicated server, no raylib; miniz for OZWN package reads)
$(BUILD_DIR)/GameState.o: Source/Server/GameState.cpp Source/Server/GameState.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Server/GameState.cpp -o $@

AngelServ: $(RES_SRV) $(BUILD_DIR)/Network.o $(BUILD_DIR)/GameState.o $(BUILD_DIR)/GameType.o $(BUILD_DIR)/SurfaceFlags.o $(BUILD_DIR)/Log.o $(BUILD_DIR)/LightningEntityRegistry.o $(BUILD_DIR)/LightningScriptParser.o $(BUILD_DIR)/LightningScriptContext.o $(BUILD_DIR)/EventBus.o $(BUILD_DIR)/ObjectiveSystem.o $(BUILD_DIR)/GameManager.o $(BUILD_DIR)/miniz.o Source/Server/Server.cpp Source/Server/ServerHttp.cpp Source/Server/ServerInternal.hpp Source/Network/Network.hpp Source/World/OzoneParser.hpp Source/World/OzoneParser.cpp Source/Server/Master/MasterClient.hpp Source/Network/MasterProtocol.hpp Source/Network/MasterHttp.hpp
	$(SERVER_CXX) $(SERVER_FLAGS) $(RES_SRV) $(BUILD_DIR)/Network.o $(BUILD_DIR)/GameState.o $(BUILD_DIR)/GameType.o $(BUILD_DIR)/SurfaceFlags.o $(BUILD_DIR)/Log.o $(BUILD_DIR)/LightningEntityRegistry.o $(BUILD_DIR)/LightningScriptParser.o $(BUILD_DIR)/LightningScriptContext.o $(BUILD_DIR)/EventBus.o $(BUILD_DIR)/ObjectiveSystem.o $(BUILD_DIR)/GameManager.o $(BUILD_DIR)/miniz.o Source/Server/Server.cpp Source/Server/ServerHttp.cpp Source/World/OzoneParser.cpp -o AngelServ$(EXE) $(SERVER_LIBS) $(LDEXTRA)

# 7b. Build AngelMaster (standalone master server, no raylib)
AngelMaster: $(BUILD_DIR)/Log.o Source/Server/Master/Master.cpp Source/Network/MasterProtocol.hpp Source/Network/MasterHttp.hpp
	$(SERVER_CXX) $(SERVER_FLAGS) $(BUILD_DIR)/Log.o Source/Server/Master/Master.cpp -o AngelMaster$(EXE) $(SERVER_LIBS) $(LDEXTRA)

# 8. Build OzPack (standalone packer/unpacker, no raylib)
ozpack: Source/OzPack.cpp Source/Package/OzPackage.hpp Source/miniz/miniz.h $(BUILD_DIR)/miniz.o
	$(SERVER_CXX) $(SERVER_FLAGS) Source/OzPack.cpp $(BUILD_DIR)/miniz.o -o OzPack$(EXE) $(SERVER_LIBS) $(LDEXTRA)

# 8b. Engine-generic gameplay systems + Angels95 game layer (SCAFFOLD, Phase 0).
#     These are raylib-free and currently inert (nothing emits/subscribes yet);
#     they are compiled + linked so the skeleton exists and Phase 1/2 only fill
#     in bodies. Keep them raylib-free so a future test_events suite can link them.
$(BUILD_DIR)/EventBus.o: Source/Events/EventBus.cpp Source/Events/EventBus.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Events/EventBus.cpp -o $@

$(BUILD_DIR)/TriggerSystem.o: Source/Events/TriggerSystem.cpp Source/Events/TriggerSystem.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Events/TriggerSystem.cpp -o $@

$(BUILD_DIR)/ObjectiveSystem.o: Source/Events/ObjectiveSystem.cpp Source/Events/ObjectiveSystem.hpp Source/Events/EventBus.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Events/ObjectiveSystem.cpp -o $@

$(BUILD_DIR)/BoxedTypeRegistry.o: Source/Boxed/BoxedTypeRegistry.cpp Source/Boxed/BoxedTypeRegistry.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Boxed/BoxedTypeRegistry.cpp -o $@

$(BUILD_DIR)/GameDef.o: Source/Game/GameDef.cpp Source/Game/GameDef.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Game/GameDef.cpp -o $@

$(BUILD_DIR)/GameManager.o: Source/Game/GameManager.cpp Source/Game/GameManager.hpp Source/Game/GameDef.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Game/GameManager.cpp -o $@

$(BUILD_DIR)/GameBoxedTypes.o: Source/Game/GameBoxedTypes.cpp Source/Game/GameBoxedTypes.hpp Source/Boxed/BoxedTypeRegistry.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Game/GameBoxedTypes.cpp -o $@

$(BUILD_DIR)/GameOpcodes.o: Source/Game/GameOpcodes.cpp Source/Game/GameOpcodes.hpp | $(BUILD_DIR)
	$(SERVER_CXX) $(SERVER_FLAGS) -c Source/Game/GameOpcodes.cpp -o $@

# 9. Unit tests (no raylib dependency)
TEST_FLAGS := -O0 -g --std=c++20 -DOMEGA_TEST_ENV
test_context: tests/LightningScriptContext.test.cpp Source/Script/LightningScriptContext.cpp Source/Log.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@

test_parser: tests/LightningScriptParser.test.cpp Source/Script/LightningScriptParser.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@

test_registry: tests/LightningEntityRegistry.test.cpp Source/Script/LightningEntityRegistry.cpp Source/Script/LightningScriptParser.cpp Source/Script/LightningScriptContext.cpp Source/Log.cpp
	$(SERVER_CXX) $(TEST_FLAGS) $(RAYLIB_INC) -ISource $^ $(BUILD_DIR)/miniz.o -o $@

test_entity_manager: tests/LightningEntityManager.test.cpp Source/Script/LightningEntityManager.cpp Source/Script/LightningEntityRegistry.cpp Source/Script/LightningScriptContext.cpp Source/Script/LightningScriptParser.cpp Source/Audio/SoundManager.cpp Source/World/ZoneManager.cpp Source/Events/EventBus.cpp Source/Events/ObjectiveSystem.cpp Source/Log.cpp
	$(COMP) $(TEST_FLAGS) $(RAYLIB_INC) -ISource $^ $(BUILD_DIR)/miniz.o -o $@ $(LDFLAGS) $(LDEXTRA)

# OMEGA_PAWNSYSTEM_TEST opts LightningEntityManager.cpp back into the real
# PawnSystem/raylib melee path (test_pawn_system links raylib + OzPawnSystem.cpp).
# Without it the file compiles headless and the melee resolution is stubbed out.
test_pawn_system: tests/OzPawnSystem.test.cpp Source/Pawn/OzPawnSystem.cpp Source/World/ZoneManager.cpp Source/Audio/SoundManager.cpp Source/Physics/OzBsp.cpp Source/Physics/WorldChunk.cpp Source/Physics/PlayerPhysics.cpp Source/Log.cpp Source/Renderer/OzAssetMapper.cpp Source/Script/LightningEntityManager.cpp Source/Script/LightningEntityRegistry.cpp Source/Script/LightningScriptContext.cpp Source/Script/LightningScriptParser.cpp Source/Events/EventBus.cpp Source/Events/ObjectiveSystem.cpp Source/Renderer/Mesh/Mesh.cpp Source/Renderer/Mesh/SkeletalMesh.cpp Source/Renderer/Mesh/MeshCache.cpp Source/Renderer/Mesh/AnimatedMesh.cpp Source/Package/Anim/OzAnimFormat.cpp Source/Particle/OzParticleSimulationManager.cpp Source/Renderer/LitLightning.cpp Source/Renderer/rlights/rlights.cpp
	$(COMP) $(TEST_FLAGS) -DOMEGA_PAWNSYSTEM_TEST $(RAYLIB_INC) -ISource $^ $(BUILD_DIR)/miniz.o -o $@ $(LDFLAGS) $(LDEXTRA)

test_ozone_parser: tests/OzoneParser.test.cpp Source/World/OzoneParser.cpp Source/World/SurfaceFlags.cpp Source/Log.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ $(BUILD_DIR)/miniz.o -o $@

# Surface flag registry + per-face surface props. Raylib-free, so headless.
test_surface: tests/Surface.test.cpp Source/World/SurfaceFlags.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@

# CSG + collision queries. Raylib-free, so headless: OzBsp and WorldChunk are
# plain math. Covers the per-brush split budget, the rotation-aware collision
# AABB, and the XZ box query the obstacle test needs.
test_csg: tests/Csg.test.cpp Source/Physics/OzBsp.cpp Source/Physics/WorldChunk.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@

# Triangle winding for the hand-built brush meshes. Header-only, so nothing to
# link: BrushMesh.hpp is raylib-free precisely so this can exist. A cylinder
# shipped with both caps inside-out and no test could see it (the builder needs a
# GL context), which is what these assertions are for.
test_brushmesh: tests/BrushMesh.test.cpp Source/World/BrushMesh.hpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@

# Game-mode taxonomy + ruleset data. Raylib-free; links the registry because
# GameTypeInfoFromOverride resolves EntityType::GAMETYPE .ozls defs.
test_gametype: tests/GameType.test.cpp Source/World/GameType.cpp Source/Script/LightningEntityRegistry.cpp Source/Script/LightningScriptParser.cpp Source/Script/LightningScriptContext.cpp Source/Log.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ $(BUILD_DIR)/miniz.o -o $@

# AngelEd's editor event bus. Deliberately the ONLY AngelEd code in the test
# link, and deliberately raylib-free + Win32-free: the 58 EditorPanelState
# action* fields it replaces were reachable only from inside a WM_COMMAND handler
# in a raylib frame loop, so none of that behaviour could be tested headlessly.
test_editorbus: tests/EditorEventBus.test.cpp AngelEd/Source/Core/EditorEventBus.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@

# The two-root asset tree shared by the Model Browser and the Texture Manager. The
# first automated coverage the asset browser has had: BuildAssetScope was implemented
# inside UI/Panels/TexturePanel.cpp while ModelPanel.cpp also called it, so it was
# shared code living in one of its consumers. AGENTS.md asserted the two panels
# "cannot drift" but nothing checked the tree was CORRECT.
# Raylib-free and Win32-free by construction - AssetScope.hpp includes only
# <string> and <vector> - which is the only reason this suite is possible.
# No CI wiring needed: `make test` already runs in the Linux job.
test_assetscope: tests/AssetScope.test.cpp AngelEd/Source/Resources/AssetScope.cpp AngelEd/Source/Resources/AssetScope.hpp
	$(SERVER_CXX) $(TEST_FLAGS) -I. $^ -o $@

# AutoConvex is raylib-free, so this suite builds headless with SERVER_CXX.
test_autoconvex: tests/AutoConvex.test.cpp Source/Physics/AutoConvex.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@

# Static auditor for .ozone worlds: it links OzoneParser from the exact
# single-source-of-truth source, then checks geometry bounds, lighting, mesh
# scale, asset references, playerstart sanity and zone naming. Pass a world
# file: make worldcheck && ./worldcheck GameData/Worlds/<World>/World.ozone.
# For exact model-bounds checking, generate the JSON first with:
#   python3 tools/glb_bounds.py > bounds.json
#   ./worldcheck GameData/Worlds/<World>/World.ozone --glb-bounds bounds.json
# CI runs worldcheck on every shipped world.
worldcheck: tools/worldcheck.cpp Source/World/OzoneParser.cpp Source/World/SurfaceFlags.cpp Source/Log.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource tools/worldcheck.cpp Source/World/OzoneParser.cpp Source/World/SurfaceFlags.cpp Source/Log.cpp $(BUILD_DIR)/miniz.o -o $@

# Phase-1 engine-generic systems: event bus, objectives, triggers. Raylib-free,
# so the whole suite is headless.
test_events: tests/Events.test.cpp Source/Events/EventBus.cpp Source/Events/ObjectiveSystem.cpp Source/Events/TriggerSystem.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@

# Phase-2 game hierarchy: registry -> GameManager over the real GameData/Game
# defs. Needs RAYLIB_INC to compile the registry (PackageAssetLoader pulls
# raylib.h) but links no raylib.
test_game: tests/GameManager.test.cpp Source/Game/GameManager.cpp Source/Events/ObjectiveSystem.cpp Source/Events/EventBus.cpp Source/Script/LightningEntityRegistry.cpp Source/Script/LightningScriptParser.cpp Source/Script/LightningScriptContext.cpp Source/Log.cpp
	$(SERVER_CXX) $(TEST_FLAGS) $(RAYLIB_INC) -ISource $^ $(BUILD_DIR)/miniz.o -o $@

test_join_uri: tests/JoinUri.test.cpp Source/Client/JoinUri.hpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource tests/JoinUri.test.cpp -o $@
test_master: tests/Master.test.cpp Source/Network/MasterProtocol.hpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource tests/Master.test.cpp -o $@

test_network: tests/Network.test.cpp Source/Network/Network.cpp Source/Log.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@ $(SERVER_LIBS)

# Client.cpp + PlayerProfile.cpp are raylib-free (Client.hpp only pulls
# Network.hpp), so the pickup round-trip cases drive the REAL client handler and
# the REAL auth handshake over loopback rather than a hand-rolled stub.
test_game_state: tests/GameState.test.cpp Source/Server/GameState.cpp Source/World/GameType.cpp Source/World/OzoneParser.cpp Source/World/SurfaceFlags.cpp Source/Network/Network.cpp Source/Log.cpp Source/Script/LightningEntityRegistry.cpp Source/Script/LightningScriptParser.cpp Source/Script/LightningScriptContext.cpp Source/Game/GameManager.cpp Source/Events/ObjectiveSystem.cpp Source/Events/EventBus.cpp Source/Client/Client.cpp Source/PlayerProfile.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ $(BUILD_DIR)/miniz.o -o $@ $(SERVER_LIBS)

test_ozanim: tests/OzAnim.test.cpp Source/Package/Anim/OzAnimFormat.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@

# .ozls write path (AngelEd Property Window). Links the parser too because the
# round-trip cases assert that what the writer emits parses back identically.
test_ozls_writer: tests/OzlsWriter.test.cpp Source/Script/OzlsWriter.cpp Source/Script/LightningScriptParser.cpp
	$(SERVER_CXX) $(TEST_FLAGS) -ISource $^ -o $@

# The suites, in the order they run. ONE list: the `test` target's prerequisites
# and its runner both read it, so adding a suite cannot leave it built-but-not-run
# (the old shape spelled the list twice - once as prerequisites, once as `-./suite`
# lines - and the two had to be kept in step by hand).
TEST_SUITES := test_parser test_context test_registry test_entity_manager \
               test_pawn_system test_ozone_parser test_join_uri test_master \
               test_network test_game_state test_ozanim test_ozls_writer \
               test_autoconvex test_csg test_brushmesh test_surface test_gametype \
               test_editorbus test_assetscope test_events test_game

# The editor is built twice from two hand-maintained object lists - AngelEd/Makefile
# and the inline `g++` list in ci.yml - because CI does not use the Makefile. They
# drift silently: an object compiled but not linked produces no error, just missing
# code. tools/check_editor_objects.sh compares them. Text-only, so it runs on both
# CI jobs and locally.
check-editor-objects:
	@sh tools/check_editor_objects.sh

# Names of suites that failed, written by the runner and read by the summary.
# A FILE rather than a shell variable because each recipe line is its own shell.
TEST_FAIL_FILE := $(BUILD_DIR)/failed_suites.txt

# Run every suite, report all failures, and FAIL if any suite did.
#
# Each suite is invoked through `|| echo ... >> $(TEST_FAIL_FILE)` instead of the
# old `-./suite`. The `-` prefix made make ignore the exit code, which was
# deliberate (one broken suite must not hide the rest) but had a hole: a suite that
# CRASHES - segfault, uncaught exception, abort - never reaches its summary line and
# prints no `FAIL`, and its status was discarded, so `make test` reported success
# and CI stayed green. That is not hypothetical: a malformed-vec3 input produced
# exit 0xC0000409 with a green log, and a CI step that greps for `FAIL` cannot see a
# crash by construction. The summary below makes a crash fail the build exactly the
# way an assertion does, WITHOUT giving up on running the remaining suites.
test: check-editor-objects $(TEST_SUITES) worldcheck
	@mkdir -p $(BUILD_DIR)
	@rm -f $(TEST_FAIL_FILE)
	@for t in $(TEST_SUITES); do \
	  echo ""; \
	  echo "=== $$t ==="; \
	  ./$$t || echo "  $$t" >> $(TEST_FAIL_FILE); \
	done
	@echo ""
	@echo "=== worldcheck (.ozone auditor, every shipped world) ==="
	@for w in GameData/Worlds/*/World.ozone; do \
	  ./worldcheck "$$w" || echo "  worldcheck $$w" >> $(TEST_FAIL_FILE); \
	done
	@echo ""
	@echo "=== worldcheck --asset-refs (every .ozls/.cfg path reference) ==="
	# GameData-wide, not world-scoped: run it once. Dangling paths whose basename
	# is in tools/assetrefs-allow.txt (pre-existing missing content) are counted,
	# any other dangling path fails the build. This is the gate for folder moves.
	@./worldcheck GameData/Worlds/EngineTest/World.ozone --asset-refs \
	  || echo "  worldcheck --asset-refs" >> $(TEST_FAIL_FILE)
	@echo ""
	@if [ -s $(TEST_FAIL_FILE) ]; then \
	  echo "**************"; \
	  echo "FAILED SUITES:"; \
	  cat $(TEST_FAIL_FILE); \
	  rm -f $(TEST_FAIL_FILE); \
	  echo "**************"; \
	  exit 1; \
	fi
	@rm -f $(TEST_FAIL_FILE)
	@echo "All suites passed."

clean:
	rm -rf $(BUILD_DIR) *.exe AngelServ Angels95 AngelMaster OzPack *.o AngelEd/*.o AngelEd/Source/*.o test_context test_parser test_registry test_ozone_parser test_join_uri test_master test_network test_game_state test_ozanim test_ozls_writer test_autoconvex test_csg test_brushmesh test_surface test_gametype test_editorbus test_assetscope test_events test_game worldcheck
