#ifndef ANGEL_ED_EDITOR_HPP
#define ANGEL_ED_EDITOR_HPP
#include "PPGIO.hpp"
#include "../../Source/Package/PackageAssetLoader.hpp"
#include "../../Source/Renderer/EngineBillboard.hpp"
#include "../../Source/Renderer/SurfaceMaterial.hpp"
#include "../../Source/Renderer/SkyMaterial.hpp"
#include "../../Source/Renderer/SkyPortal.hpp"   // also brings in Skybox.hpp
#include "../../Source/World/OzOzoneLoader.hpp"
#include "../../Source/Script/LightningEntityRegistry.hpp"
#include <cstring>
#include <cmath>
#include <filesystem>
#include <algorithm>
namespace fs = std::filesystem;

#define MaxCachedModels 200

// The viewport draws directly to the window (there is no offscreen Target any
// more). It used to keep a 320x200 render texture that nothing rendered into and
// then stretched it over the viewport - a leftover from an earlier pass that only
// survived because an uninitialised texture usually has alpha 0. That made the
// editor's output depend on driver luck and broke windowed resizing.

enum class LightingMode : uint8_t { LIT, UNLIT, WIREFRAME, DYNAMIC };

class Editor{
    public:
        Camera3D MainCamera = {0}; 
        Camera3D PreviewCamera = {0}; 
        LightingMode ViewMode = LightingMode::LIT;
        char Path[512] = {};
        // Environmental settings
        Color FogColor = {200, 200, 210, 255};
        float FogDensity = 0.02f;
        Color AmbientColor = {180, 180, 200, 255};
        float AmbientIntensity = 0.4f;
        // Window flag
        bool ShowEnvPanel = false;   // obsolete (the Zone window was removed); unused
        bool ShowWireframe = false;
        bool ShowSkybox = true;   // viewport skybox visibility (toolbar "Sky")
        // Shaders for lighting modes
        Shader LitFogShader = {0};
        Shader UnlitShader = {0};
        int FogStartLoc = -1;
        int FogEndLoc = -1;
        int FogDensityLoc = -1;
        int FogColorLoc = -1;
        int FogIntensityLoc = -1;
        int AmbientLoc = -1;
        int ViewPosLoc = -1;
};

static Editor OTEditor;

// Per-model entry in the dynamic model array
struct LoadedModel {
    Model model;
    Texture2D texture;
    std::string name;
    bool loaded = false;
};

class GameModels
{
    public:
        Texture2D Skybox;
        Vector3 HeightMapPosition;
        Vector3 HeightMapSize = {0, 0, 0};
        float HeightMapScale = 1.0f;
        Image HeightMapImage = {0};
        bool HeightMapReady = false;

        Model HeightMap;
        Texture2D HeightMapTexture;

        std::vector<LoadedModel> models;

        // Dynamically load all .obj files from <worldPath>/Models/
        void LoadModels(const std::string& worldPath) {
            for (auto& m : models) {
                if (m.loaded) {
                    if (m.texture.id > 0) UnloadTexture(m.texture);
                    UnloadModel(m.model);
                }
            }
            models.clear();

            fs::path dir = fs::path(worldPath) / "Models";
            if (!fs::exists(dir)) return;

            std::vector<fs::path> objFiles;
            for (auto& entry : fs::directory_iterator(dir)) {
                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".obj") objFiles.push_back(entry.path());
                }
            }
            std::sort(objFiles.begin(), objFiles.end());

            for (auto& objPath : objFiles) {
                LoadedModel lm;
                lm.name = objPath.stem().string();
                lm.model = LoadModel(objPath.string().c_str());
                if (lm.model.meshes == nullptr) {
                    lm.loaded = false;
                    models.push_back(lm);
                    continue;
                }
                std::string base = objPath.parent_path().string() + "/" + lm.name;
                std::string texPath = base + "_texture.png";
                if (!fs::exists(texPath)) texPath = base + ".png";
                if (fs::exists(texPath)) {
                    lm.texture = LoadTexture(texPath.c_str());
                    if (lm.texture.id > 0)
                        lm.model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = lm.texture;
                }
                if (OTEditor.LitFogShader.id > 0)
                    lm.model.materials[0].shader = OTEditor.LitFogShader;
                lm.loaded = true;
                models.push_back(lm);
            }
        }

        // Get a loaded model by 1-based WDL ModelId (Model1 â†’ 1, Model2 â†’ 2, ...)
        LoadedModel* GetModelByWDLId(int wdlId) {
            int idx = wdlId - 1;
            if (idx >= 0 && idx < (int)models.size() && models[idx].loaded)
                return &models[idx];
            return nullptr;
        }

        int GetModelCount() const { return (int)models.size(); }
        const char* GetModelName(int index) const {
            if (index < 0 || index >= (int)models.size()) return nullptr;
            return models[index].name.c_str();
        }
};

extern GameModels WDLModels;

class GameData{
    public:
        float X, Y, Z, R, S;
        int ModelId;
        bool Collision;
        void Init(){ X=Y=Z=R=S=0; ModelId=0; Collision=false; }
};

static int CachedModelCounter = 0;
static GameData CachedModels[MaxCachedModels];

class CollisionData{
    public:
        float X, Y, Z, W, H, L;
        void Init(){ X=Y=Z=W=H=L=0; }
};

static int CachedCollisionCounter = 0;
static CollisionData CachedCollision[MaxCachedModels];

// --- Heightmap sampling (terrain following) ---
inline float SampleHeightmapGroundY(float px, float pz) {
    if (!WDLModels.HeightMapReady || WDLModels.HeightMapImage.data == 0)
        return -99999.0f;
    Vector3 o = WDLModels.HeightMapPosition;
    float scale = WDLModels.HeightMapScale;
    float sx = WDLModels.HeightMapSize.x * scale;
    float sz = WDLModels.HeightMapSize.z * scale;
    int iw = WDLModels.HeightMapImage.width;
    int ih = WDLModels.HeightMapImage.height;
    if (iw < 1 || ih < 1) return -99999.0f;
    float hx = (px - o.x) / sx;
    float hz = (pz - o.z) / sz;
    float fx = hx * (float)(iw - 1);
    float fz = hz * (float)(ih - 1);
    int ix = (int)fx;
    int iz = (int)fz;
    if (ix < 0 || ix >= iw - 1 || iz < 0 || iz >= ih - 1)
        return o.y;
    float tx = fx - ix;
    float tz = fz - iz;
    uint8_t* p = (uint8_t*)WDLModels.HeightMapImage.data;
    float h00 = p[iz * iw + ix] / 255.0f;
    float h10 = p[iz * iw + ix + 1] / 255.0f;
    float h01 = p[(iz + 1) * iw + ix] / 255.0f;
    float h11 = p[(iz + 1) * iw + ix + 1] / 255.0f;
    float ht = h00 * (1-tx)*(1-tz) + h10 * tx*(1-tz) + h01 * (1-tx)*tz + h11 * tx*tz;
    return o.y + ht * WDLModels.HeightMapSize.y * scale;
}

// --- Pawn node types ---
enum class EditorNodeType { SPAWN, NPC, LIGHT, ZONE, PORTAL };
static const char* NodeTypeLabel(EditorNodeType t) {
    switch (t) {
        case EditorNodeType::SPAWN: return "Player Spawn";
        case EditorNodeType::NPC:   return "NPC Spawn";
        case EditorNodeType::LIGHT: return "Point Light";
        case EditorNodeType::ZONE:  return "Zone Volume";
        case EditorNodeType::PORTAL: return "Level Portal";
        default: return "?";
    }
}

inline void Init(){
    if (WDLModels.HeightMapImage.data) {
        UnloadImage(WDLModels.HeightMapImage);
        WDLModels.HeightMapImage = (Image){0};
    }
    WDLModels.HeightMapReady = false;

    OTEditor.MainCamera.position = (Vector3){ 18.0f, 10.0f, 18.0f };
    OTEditor.MainCamera.target = (Vector3){ 0.0f, 10.0f, 0.0f };   
    OTEditor.MainCamera.up = (Vector3){ 0.0f, 1.0f, 0.0f };          
    OTEditor.MainCamera.fovy = 60.0f;                         
    OTEditor.MainCamera.projection = CAMERA_PERSPECTIVE;

    OTEditor.PreviewCamera.position = (Vector3){ 5.0f, 4.0f, 5.0f };  
    OTEditor.PreviewCamera.target = (Vector3){ 0.0f, 0.0f, 0.0f };    
    OTEditor.PreviewCamera.up = (Vector3){ 0.0f, 1.0f, 0.0f };         
    OTEditor.PreviewCamera.fovy = 45.0f;                                
    OTEditor.PreviewCamera.projection = CAMERA_PERSPECTIVE;         

    // Initialize lit fog shader for editor.
    //
    // Use the PROJECT shader directory, not OTEditor.Path: that field is the
    // current WORLD directory (SetWorldDirectory writes it), so the old
    // `OTEditor.Path + "Shaders/Lights/"` resolved to
    // GameData/Worlds/<World>/Shaders/Lights/ and only worked by accident when
    // the package loader matched the file's basename. EnsureProjectRoot() has
    // already fixed the cwd to the repo root, so this is the stable path.
    std::string shaderBase = "GameData/Shaders/Lights/";
    OTEditor.LitFogShader = LoadShaderWithFallback(
        (shaderBase + "Lighting.vs").c_str(),
        (shaderBase + "LitFog.fs").c_str()
    );
    
    // Initialize fog uniforms for editor
    if (OTEditor.LitFogShader.id > 0) {
        OTEditor.FogStartLoc = GetShaderLocation(OTEditor.LitFogShader, "fogStart");
        OTEditor.FogEndLoc = GetShaderLocation(OTEditor.LitFogShader, "fogEnd");
        OTEditor.FogDensityLoc = GetShaderLocation(OTEditor.LitFogShader, "fogDensity");
        OTEditor.FogColorLoc = GetShaderLocation(OTEditor.LitFogShader, "fogColor");
        OTEditor.FogIntensityLoc = GetShaderLocation(OTEditor.LitFogShader, "fogIntensity");
        
        // Set default fog values
        float fogStart = 10.0f;
        float fogEnd = 100.0f;
        float fogDensity = 1.0f;
        float fogColor[3] = {0.7f, 0.7f, 0.8f};
        float fogIntensity = 1.0f;
        
        SetShaderValue(OTEditor.LitFogShader, OTEditor.FogStartLoc, &fogStart, SHADER_UNIFORM_FLOAT);
        SetShaderValue(OTEditor.LitFogShader, OTEditor.FogEndLoc, &fogEnd, SHADER_UNIFORM_FLOAT);
        SetShaderValue(OTEditor.LitFogShader, OTEditor.FogDensityLoc, &fogDensity, SHADER_UNIFORM_FLOAT);
        SetShaderValue(OTEditor.LitFogShader, OTEditor.FogColorLoc, fogColor, SHADER_UNIFORM_VEC3);
        SetShaderValue(OTEditor.LitFogShader, OTEditor.FogIntensityLoc, &fogIntensity, SHADER_UNIFORM_FLOAT);
        
        // Ambient uniform
        OTEditor.AmbientLoc = GetShaderLocation(OTEditor.LitFogShader, "ambient");
        OTEditor.ViewPosLoc = GetShaderLocation(OTEditor.LitFogShader, "viewPos");
        float ambient[4] = {(float)OTEditor.AmbientColor.r / 255.0f * OTEditor.AmbientIntensity,
                            (float)OTEditor.AmbientColor.g / 255.0f * OTEditor.AmbientIntensity,
                            (float)OTEditor.AmbientColor.b / 255.0f * OTEditor.AmbientIntensity,
                            1.0f};
        if (OTEditor.AmbientLoc >= 0)
            SetShaderValue(OTEditor.LitFogShader, OTEditor.AmbientLoc, ambient, SHADER_UNIFORM_VEC4);

        // PUBLISH the ambient, do not just push it to this one program.
        //
        // OzoneLoader owns the world's ambient (Set/GetWorldAmbient) precisely
        // because raylib has no GetShaderValue, and SurfaceMaterial::UpdateFrame
        // READS IT BACK from there rather than taking its own copy. The editor
        // never published, so OzoneLoader stayed at its header default
        // {0.1,0.1,0.1,1} while LitFog got ~{0.28,...} - and since both shaders
        // divide by 10, a flagged brush rendered at 0.01 next to a room at 0.028
        // in the SAME viewport frame. The client's Core.hpp has always published
        // correctly; the editor had a private copy nobody could read.
        OzoneLoader::Instance().SetWorldAmbient(ambient[0], ambient[1], ambient[2], ambient[3]);
    }

    // Load unlit shader for Unlit ViewMode (pass-through, no lighting)
    {
        const char* vs = "#version 330\nin vec3 vertexPosition;in vec2 vertexTexCoord;in vec4 vertexColor;uniform mat4 mvp;out vec2 fragTexCoord;out vec4 fragColor;void main(){gl_Position=mvp*vec4(vertexPosition,1.0);fragTexCoord=vertexTexCoord;fragColor=vertexColor;}";
        const char* fs = "#version 330\nin vec2 fragTexCoord;in vec4 fragColor;uniform sampler2D texture0;uniform vec4 colDiffuse;out vec4 finalColor;void main(){finalColor=texture(texture0,fragTexCoord)*colDiffuse*fragColor;}";
        OTEditor.UnlitShader = LoadShaderFromMemory(vs, fs);
    }

    // Pass lit fog shader to OzoneLoader for OZONE geometry
    OzoneLoader::Instance().SetLitFogShader(OTEditor.LitFogShader);

    // Initialize engine billboard system
    EngineBillboard::Init();

    // Per-face surface shader and the shared sky shader. These are PROJECT
    // assets, not world-local ones: OTEditor.Path is the current WORLD folder
    // (SetWorldDirectory writes it), so using it here looked for
    // GameData/Worlds/<World>/Shaders/* and the editor logged
    // "Failed to open text file ... World/Shaders/Sky.vs" for every world.
    // EnsureProjectRoot() has already fixed the cwd, so this path is stable.
    //
    // SkyMaterial needs the same directory as the client. oz::Skybox - shared by
    // both - silently falls back to raylib's default material when the sky
    // shader is missing, so an editor that forgot this would show an unfogged
    // sky with no diagnostic anywhere.
    {
        std::string sdir = "GameData/Shaders/";
        oz::SurfaceMaterial::Instance().Init(sdir.c_str());
        oz::SkyMaterial::Instance().Init(sdir.c_str());
    }

    if (IsPathFile(TextFormat("%s/Models/HeightMap.png", OTEditor.Path)))
    {
        WDLModels.HeightMapTexture = LoadTexture(TextFormat("%sModels/HeightMapTexture.png", OTEditor.Path));
        WDLModels.HeightMapImage = LoadImage(TextFormat("%sModels/HeightMap.png", OTEditor.Path));
        ImageFormat(&WDLModels.HeightMapImage, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
        int X = PullConfigValue(TextFormat("%sModels/HeightMapConfig.conf", OTEditor.Path), 0);
        int Y = PullConfigValue(TextFormat("%sModels/HeightMapConfig.conf", OTEditor.Path), 1);
        int Z = PullConfigValue(TextFormat("%sModels/HeightMapConfig.conf", OTEditor.Path), 2);
        WDLModels.HeightMapSize = (Vector3){(float)X, (float)Y, (float)Z};
        Mesh Mesh1 = GenMeshHeightmap(WDLModels.HeightMapImage, WDLModels.HeightMapSize);
        fprintf(stderr, "HM: %dx%d img=%dx%d\n", X, Z,
                WDLModels.HeightMapImage.width, WDLModels.HeightMapImage.height);
        WDLModels.HeightMap = LoadModelFromMesh(Mesh1);
        WDLModels.HeightMap.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = WDLModels.HeightMapTexture;
        WDLModels.HeightMap.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        if (OTEditor.LitFogShader.id > 0) {
            WDLModels.HeightMap.materials[0].shader = OTEditor.LitFogShader;
        }
        WDLModels.HeightMapReady = (WDLModels.HeightMapImage.data != nullptr);
    }

    // Dynamically load all .obj models from the world's Models/ directory
    WDLModels.LoadModels(OTEditor.Path);
}

class InEditor{
    public:
        bool DrawModel = false;
        float X = 0, Y = 0, Z = 0, S = 1, R = 0, L = 0, H = 0, W = 0;
        std::string ActivePickupName = "HealthVial";
        // Model Browser path captured at "Place in World" time. BOTH the ghost
        // preview and the commit read this, never g_editorPanels.selectedModel:
        // that index is reset to -1 by ScanModelBrowserFiles (import, Refresh),
        // so a rescan between the button and the click made the ghost disappear
        // and made the commit silently refuse with "select a model first".
        std::string ActiveModelPath;
        EditorNodeType ActiveNodeType = EditorNodeType::SPAWN;
        // CSG operation for OZONE brush placement: 0=SOLID, 1=ADD, 2=SUB, 3=INTERSECT, 4=DE_RESC
        int CSGOperation = 0;
};

static InEditor AngelTechEditor;

inline char ScriptEditorBuffer[1200];

inline void ConvertConstCharToCharArray(const char* constString, char* charArray, int arraySize) {
    std::strncpy(charArray, constString, arraySize - 1);
    charArray[arraySize - 1] = '\0';
}

inline void LoadEditor(const char* File){
    ifstream file(File);
    string fileContents;
    string line;
    while (getline(file, line)) { fileContents += line + "\n"; }
    file.close();
    ConvertConstCharToCharArray(fileContents.c_str(), ScriptEditorBuffer, 1200);
}

inline int EMID = 1;

// --- Helper: draw a WDL instruction line for current placed item ---
static wstring BuildWDLPlaceCommand(const wstring& prefix, int subId) {
    wstring cmd = prefix;
    if (subId >= 0) cmd += to_wstring(subId);
    cmd += L":" + to_wstring(AngelTechEditor.X) + L":" + to_wstring(AngelTechEditor.Y) + L":" +
           to_wstring(AngelTechEditor.Z) + L":" + to_wstring(AngelTechEditor.S) + L":" + to_wstring(AngelTechEditor.R) + L":";
    return cmd;
}

#endif // ANGEL_ED_EDITOR_HPP
