#include "WindowsCompat.hpp"
#include "raylib.h"
#include "Settings.hpp"
#include "Pawn/PlayerMovement.hpp"
#include "Pawn/Player.hpp"
#include "PPGIO.hpp"
#include "Pawn/Items.hpp"
#include "Renderer/Video.hpp"
#include "Renderer/TextSystem.hpp"
#include "WorldState.hpp"
// Entities.hpp removed

inline PlayerMovement g_playerMovement;

#include <string>
#include <iostream>
#include <fstream>
#include <chrono>
#include <utility>

using namespace std;

inline int R = 0;
inline int G = 0;
inline int B = 0;

inline int Direction = 1;
inline bool FadeDone = false;
Color FadeColor = (Color){R, G, B, 255};

void PlayFade()
{
    Direction = 1;
    R = 0;
    G = 0;
    B = 0;
    FadeDone = false;
}   



class WorldModelSet
{
    public:
        Texture2D Skybox;

        // Weapon object models (Object1-5, loaded from EntityDef mesh paths)
        Model objectModels[5]{};
        bool objectModelsLoaded[5] = {false, false, false, false, false};
};

inline WorldModelSet WorldModels;

// Audio state (GameSounds/AngelTechSoundData and every playback call) lives in
// Audio/SoundManager.hpp — include it instead of reaching for raylib sound
// handles directly.
