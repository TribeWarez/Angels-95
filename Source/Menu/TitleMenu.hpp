#pragma once
#include "raylib.h"
#include "../Renderer/raygui/raygui.h"
#include "../Network/Network.hpp"
#include "InternetBrowser.hpp"
#include "../Client/MasterList.hpp"
#include "../Audio/SoundManager.hpp"
#include "../PlayerProfile.hpp"
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

// Index 0 is "Unassigned" (PlayerProfile::team == 0); 1..7 map to the teams.
// Keep this in sync with TEAM_COLOR_NAMES below.
static const Color TEAM_COLORS[8] = {
    {90, 90, 100, 255},    // Unassigned
    {200, 40, 40, 255},   {40, 100, 220, 255},  {40, 180, 60, 255},
    {200, 170, 30, 255},  {60, 180, 200, 255},  {160, 50, 200, 255},
    {220, 130, 30, 255}
};
static const char* TEAM_COLOR_NAMES[8] = {"Unassigned","Red","Blue","Green","Gold","Azure","Purple","Orange"};
static const int   TEAM_COLOR_COUNT = 8;

struct WorldEntry { std::string name; std::string dirName; };

struct TitleMenuTextures {
    Texture2D titleBg{0};
    Texture2D fTL{0}, fT{0}, fTR{0};
    Texture2D fL{0}, fArea{0}, fR{0};
    Texture2D fBL{0}, fB{0}, fBR{0};
    Texture2D barL{0}, barTile{0}, barWin{0}, barMax{0};
    Texture2D menuLine{0}, clientArea{0};
    Texture2D btnNormal{0}, btnHover{0}, btnClicked{0};
    bool loaded = false;

    void Load() {
        if (loaded) return;
        titleBg = LoadTextureWithFallback("GameData/Tex/Title/Title.png");
        fTL = LoadTexture("GameData/Tex/Menu/BlueMenuTL.png");
        fT  = LoadTexture("GameData/Tex/Menu/BlueMenuT.png");
        fTR = LoadTexture("GameData/Tex/Menu/BlueMenuTR.png");
        fL  = LoadTexture("GameData/Tex/Menu/BlueMenuL.png");
        fArea = LoadTexture("GameData/Tex/Menu/BlueMenuArea.png");
        fR  = LoadTexture("GameData/Tex/Menu/BlueMenuR.png");
        fBL = LoadTexture("GameData/Tex/Menu/BlueMenuBL.png");
        fB  = LoadTexture("GameData/Tex/Menu/BlueMenuB.png");
        fBR = LoadTexture("GameData/Tex/Menu/BlueMenuBR.png");
        barL    = LoadTexture("GameData/Tex/Menu/BlueBarL.png");
        barTile = LoadTexture("GameData/Tex/Menu/BlueBarTile.png");
        barWin  = LoadTexture("GameData/Tex/Menu/BlueBarWin.png");
        barMax  = LoadTexture("GameData/Tex/Menu/BlueBarMax.png");
        menuLine = LoadTexture("GameData/Tex/Menu/BlueMenuLine.png");
        clientArea = LoadTexture("GameData/Tex/Menu/BlueClientArea.png");
        btnNormal  = LoadTexture("GameData/Tex/Title/menu_button.png");
        btnHover   = LoadTexture("GameData/Tex/Title/menu_button_hover.png");
        btnClicked = LoadTexture("GameData/Tex/Title/menu_button_clicked.png");
        loaded = true;
    }

    void Unload() {
        if (!loaded) return;
        auto u = [](Texture2D& t) { if (t.id) UnloadTexture(t); t = Texture2D{0}; };
        u(titleBg); u(fTL); u(fT); u(fTR); u(fL); u(fArea); u(fR); u(fBL); u(fB); u(fBR);
        u(barL); u(barTile); u(barWin); u(barMax);
        u(menuLine); u(clientArea); u(btnNormal); u(btnHover); u(btnClicked);
        loaded = false;
    }
};

static TitleMenuTextures& GetMenuTex() { static TitleMenuTextures tex; tex.Load(); return tex; }

static std::vector<WorldEntry> ScanWorlds() {
    std::vector<WorldEntry> worlds;
    // Try multiple base directories in case CWD differs
    const char* candidates[] = {"GameData/Worlds", "../GameData/Worlds", "System/../GameData/Worlds"};
    for (auto base : candidates) {
        if (!fs::exists(base)) continue;
        for (auto& entry : fs::directory_iterator(base)) {
            if (!entry.is_directory()) continue;
            std::string dirName = entry.path().filename().string();
            // Skip non-world directories (they won't have world data)
            if (dirName == "Legacy") continue;
            std::string oz  = entry.path().string() + "/World.ozone";
            if (fs::exists(oz) && dirName != "." && dirName != "..") {
                bool dup = false;
                for (auto& w : worlds) if (w.dirName == dirName) { dup = true; break; }
                if (!dup) worlds.push_back({dirName, dirName});
            }
        }
        if (!worlds.empty()) break; // found some, stop searching
    }
    // Fallback: scan compiled zones in System/Data/Zones/
    if (worlds.empty()) {
        const char* zoneDirs[] = {"System/Data/Zones", "../System/Data/Zones"};
        for (auto zd : zoneDirs) {
            if (!fs::exists(zd)) continue;
            for (auto& entry : fs::directory_iterator(zd)) {
                if (!entry.is_regular_file()) continue;
                std::string fname = entry.path().filename().string();
                if (fname.rfind("world_", 0) == 0 && fname.size() > 6) {
                    std::string dirName = fname.substr(6);
                    size_t dot = dirName.rfind(".ozone");
                    if (dot != std::string::npos) dirName = dirName.substr(0, dot);
                    bool dup = false;
                    for (auto& w : worlds) if (w.dirName == dirName) { dup = true; break; }
                    if (!dup) worlds.push_back({dirName, dirName});
                }
            }
            if (!worlds.empty()) break;
        }
    }
    // Ultimate fallback: hardcoded known worlds
    if (worlds.empty()) {
        const char* known[] = {"Dessert_Dreams", "EngineTest"};
        for (auto name : known) {
            // Verify the world actually has an ozone file somewhere
            std::string paths[] = {
                std::string("GameData/Worlds/") + name + "/World.ozone",
                std::string("../GameData/Worlds/") + name + "/World.ozone",
                std::string("System/Data/Zones/world_") + name + ".ozone",
                std::string("../System/Data/Zones/world_") + name + ".ozone"
            };
            for (auto& p : paths) {
                if (fs::exists(p)) {
                    worlds.push_back({name, name});
                    break;
                }
            }
        }
    }
    return worlds;
}

enum PaneType : int {
    PANE_CAMPAIGN = 0,
    PANE_PRACTICE,
    PANE_SERAPHIC,
    PANE_STATS,
    PANE_SETTINGS,
    PANE_CHARACTER,
    PANE_HELP,
    PANE_MULTIPLAYER
};

struct Pane {
    Rectangle rect;
    PaneType type;
    bool dragging = false;
    Vector2 dragOffset{0};
};

class TitleMenu {
public:
    TitleMenu() {
        GetMenuTex();
        m_worlds = ScanWorlds();
        // The manager is initialised once at client startup (see Source/Main.cpp).
        // Active() is safe to read even if that has not run: it falls back to a
        // built-in default profile.
        LoadActiveProfileIntoFields();
        m_masterUrls = master::LoadMasterUrls("System/Angels95.ini");
        m_internet.SetMasters(m_masterUrls);
    }

    bool Tick() {
        if (WindowShouldClose()) { m_exit = true; return true; }

        SoundManager::Instance().UpdateStream(OmegaTechData.HomeScreenMusic);

        // LAN/internet browsers: pump while the Multiplayer pane is open.
        bool multiOpen = false;
        for (auto& p : m_panes) if (p.type == PANE_MULTIPLAYER) { multiOpen = true; break; }
        if (multiOpen) {
            LanUpdate();
            m_internet.Update();
            // First time the browser is shown with Internet selected, fetch it.
            if (m_serverSource == 1 && !m_internetAutoScanned) {
                m_internetAutoScanned = true;
                if (!m_internet.HasResult() && !m_internet.IsScanning()) {
                    m_internet.SetMasters(m_masterUrls);
                    m_internet.Refresh();
                }
            }
        } else {
            if (m_lanScanning) LanStop();
            m_internet.Cancel();
            m_internetAutoScanned = false;
        }

        m_sw = GetScreenWidth();
        m_sh = GetScreenHeight();
        Vector2 mp = GetMousePosition();

        UpdateLayout();
        UpdateInput(mp);

        BeginTextureMode(Target);
        ClearBackground(BLACK);

        DrawBackground();
        Draw9SliceFrame();
        DrawTitleBar();
        DrawSeparator(m_titleBar);
        DrawMenuBar();
        DrawSeparator(m_menuBar);
        DrawContent(m_contentArea);
        DrawSeparator(m_helpBar);
        DrawHelpBar();

        if (m_dropdownOpen && m_openTab >= 0)
            DrawDropdown(m_openTab);

        EndTextureMode();

        BeginDrawing();
        DrawTexturePro(Target.texture,
            (Rectangle){0, 0, Target.texture.width, -Target.texture.height},
            (Rectangle){0, 0, m_sw, m_sh},
            (Vector2){0, 0}, 0.f, WHITE);
        EndDrawing();

        return m_exit;
    }

    bool ShouldExit() const { return m_exit; }
    bool ShouldLoadGame() const { return m_loadGame; }
    bool ShouldJoinServer() const { return m_joinServer; }
    bool ShouldStartServer() const { return m_startServer; }
    // nullptr when nothing is selected: callers use this as a truthiness guard
    // before copying, and std::string::c_str() is never null (an empty string
    // used to look "selected", wiping g_world_to_load on a plain server join).
    const char* GetSelectedWorld() const {
        return m_selectedWorld.empty() ? nullptr : m_selectedWorld.c_str();
    }
    const char* GetJoinIP() const { return m_joinIP; }
    int GetJoinPort() const { return std::atoi(m_hostPortBuffer); }

private:
    int m_sw = 1280, m_sh = 720;
    static constexpr int BORDER = 4, TITLE_H = 24, MENU_H = 24, HELP_H = 22;
    static constexpr int SEP_H = 2, CELL_W = 160, CELL_H = 48, CELL_GAP = 8, FRAME_M = 8;

    Rectangle m_frameRect, m_innerRect, m_titleBar, m_menuBar, m_contentArea, m_helpBar;

    static constexpr int TAB_COUNT = 8;
    static const char* TAB_NAMES[TAB_COUNT];
    Rectangle m_tabRects[TAB_COUNT];
    int m_hoveredTab = -1, m_openTab = -1, m_hoveredItem = -1;
    bool m_dropdownOpen = false;
    Rectangle m_dropdownRect{0};
    std::vector<Rectangle> m_itemRects;

    bool m_exit = false, m_loadGame = false, m_joinServer = false, m_startServer = false;
    char m_joinIP[32] = "127.0.0.1";
    std::string m_selectedWorld;
    std::vector<WorldEntry> m_worlds;

    char m_charName[64];
    // Profile edit buffers. m_charName doubles as the active profile's name so
    // there is a single source of truth; these two back the remaining fields.
    char m_modelPath[128] = "";
    char m_voiceSet[64]  = "";
    bool m_modelPathEditing = false;
    bool m_voiceSetEditing  = false;
    // Index into TEAM_COLORS (0 = Unassigned). Mirrors PlayerProfile::team.
    int m_teamColor = 0;
    int m_gameTypeFilter = 0, m_settingsSubPage = 0;
    char m_joinIPBuffer[64] = "127.0.0.1";
    char m_hostPortBuffer[16] = "27015";
    // raygui textboxes share one global cursor index; forcing editMode=true
    // every frame left it stale/out-of-range and corrupted the buffer (garbage
    // hosts like "z"). Track focus per box and toggle on the return value.
    bool m_joinIpEditing = false;
    bool m_joinPortEditing = false;
    bool m_hostPortEditing = false;
    bool m_charNameEditing = false;
    std::string m_selectedServerWorld;
    int m_multiplayerSubPage = 0; // 0=Join, 1=Host

    // LAN server browser (Join tab)
    net::NetworkDiscovery m_lanDiscovery;
    bool m_lanScanning = false;
    double m_lastLanProbe = 0.0;
    std::vector<net::DiscoveredServer> m_lanServers;
    int m_lanHover = -1;

    // Internet server browser (Join tab)
    int m_serverSource = 0;   // 0 = LAN, 1 = Internet
    bool m_internetAutoScanned = false;
    master::InternetBrowser m_internet;
    std::vector<std::string> m_masterUrls;
    int m_onlineHover = -1;

    void LanStartScan() {
        if (!m_lanDiscovery.is_running()) {
            m_lanDiscovery.init("Angels95", "0.2.1", 0); // 0 = client mode (probe only)
            m_lanDiscovery.start();
        }
        m_lanDiscovery.send_request();
        m_lastLanProbe = GetTime();
        m_lanScanning = true;
    }

    void LanStop() {
        if (m_lanDiscovery.is_running()) m_lanDiscovery.stop();
        m_lanScanning = false;
        m_lanServers.clear();
    }

    void LanUpdate() {
        if (!m_lanScanning) return;
        m_lanDiscovery.update();
        double now = GetTime();
        if (now - m_lastLanProbe > 1.5) {
            m_lanDiscovery.send_request();
            m_lastLanProbe = now;
        }
        m_lanDiscovery.poll_discovered(m_lanServers);
        if (m_lanHover >= (int)m_lanServers.size()) m_lanHover = -1;
    }

    Rectangle m_minBtn, m_maxBtn, m_closeBtn;
    bool m_dragging = false;
    bool m_dragPane = false;
    int m_dragPaneIdx = -1;
    Vector2 m_dragStart{0};

    // Pane system
    std::vector<Pane> m_panes;
    int m_cascadeX = 40, m_cascadeY = 40;
    static constexpr int PANE_TITLE_H = 20;
    static constexpr int PANE_BORDER = 4;

    void UpdateLayout() {
        m_frameRect = {(float)FRAME_M, (float)FRAME_M, (float)(m_sw - FRAME_M*2), (float)(m_sh - FRAME_M*2)};
        m_innerRect = {m_frameRect.x + BORDER, m_frameRect.y + BORDER, m_frameRect.width - BORDER*2, m_frameRect.height - BORDER*2};

        float iy = m_innerRect.y;
        m_titleBar = {m_innerRect.x, iy, m_innerRect.width, TITLE_H}; iy += TITLE_H;
        m_menuBar = {m_innerRect.x, iy + SEP_H, m_innerRect.width, MENU_H}; iy += SEP_H + MENU_H;
        m_contentArea = {m_innerRect.x, iy + SEP_H, m_innerRect.width, m_innerRect.height - TITLE_H - SEP_H - MENU_H - SEP_H - SEP_H - HELP_H};
        iy += SEP_H + m_contentArea.height;
        m_helpBar = {m_innerRect.x, iy + SEP_H, m_innerRect.width, HELP_H};

        float bx = m_titleBar.x + m_titleBar.width - 16;
        m_closeBtn = {bx, m_titleBar.y, 16, 16}; bx -= 18;
        m_maxBtn = {bx, m_titleBar.y, 16, 16}; bx -= 18;
        m_minBtn = {bx, m_titleBar.y, 16, 16};

        float tabW = m_menuBar.width / TAB_COUNT;
        for (int i = 0; i < TAB_COUNT; i++)
            m_tabRects[i] = {m_menuBar.x + tabW * i, m_menuBar.y, tabW, m_menuBar.height};
    }

    void UpdateInput(Vector2 mp) {
        m_hoveredTab = -1;
        for (int i = 0; i < TAB_COUNT; i++)
            if (CheckCollisionPointRec(mp, m_tabRects[i])) { m_hoveredTab = i; break; }

        m_hoveredItem = -1;

        #ifdef _WIN32
        // Main window chrome buttons
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (CheckCollisionPointRec(mp, m_closeBtn)) { PostMessage(GetActiveWindow(), WM_CLOSE, 0, 0); return; }
            if (CheckCollisionPointRec(mp, m_maxBtn)) { ToggleFullscreen(); return; }
            if (CheckCollisionPointRec(mp, m_minBtn)) { ShowWindow(GetActiveWindow(), SW_MINIMIZE); return; }
            if (CheckCollisionPointRec(mp, m_titleBar)) { m_dragging = true; m_dragStart = mp; return; }
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) { m_dragging = false; m_dragPane = false; }
        if (m_dragging) {
            Vector2 d = {mp.x - m_dragStart.x, mp.y - m_dragStart.y};
            if (d.x != 0 || d.y != 0) {
                RECT r; GetWindowRect(GetActiveWindow(), &r);
                SetWindowPos(GetActiveWindow(), 0, r.left + (int)d.x, r.top + (int)d.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
                m_dragStart = mp;
            }
            return;
        }
#else
        // No native window chrome outside Windows
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (CheckCollisionPointRec(mp, m_maxBtn)) { ToggleFullscreen(); return; }
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) { m_dragging = false; m_dragPane = false; }
#endif

        // Pane dragging
        if (m_dragPane && m_dragPaneIdx >= 0 && m_dragPaneIdx < (int)m_panes.size()) {
            m_panes[m_dragPaneIdx].rect.x = mp.x - m_panes[m_dragPaneIdx].dragOffset.x;
            m_panes[m_dragPaneIdx].rect.y = mp.y - m_panes[m_dragPaneIdx].dragOffset.y;
            return;
        }

        // Dropdown tab clicks
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (m_hoveredTab >= 0) {
                SoundManager::Instance().PlayUIClick();
                if (m_openTab == m_hoveredTab && m_dropdownOpen) { m_dropdownOpen = false; m_openTab = -1; }
                else { m_openTab = m_hoveredTab; m_dropdownOpen = true; BuildDropdownItems(); }
                return;
            }
        }

        // Dropdown menu clicks
        if (m_dropdownOpen && m_openTab >= 0) {
            if (CheckCollisionPointRec(mp, m_dropdownRect)) {
                for (size_t i = 0; i < m_itemRects.size(); i++)
                    if (CheckCollisionPointRec(mp, m_itemRects[i])) { m_hoveredItem = (int)i; break; }
                if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && m_hoveredItem >= 0) {
                    SoundManager::Instance().PlayUIClick();
                    HandleDropdownClick(m_openTab, m_hoveredItem);
                    return;
                }
            } else if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) { m_dropdownOpen = false; m_openTab = -1; }
        }

        // Pane input handling (front to back)
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) || IsMouseButtonReleased(MOUSE_LEFT_BUTTON))
            HandlePaneInput(mp);
    }

    void BuildDropdownItems() {
        m_itemRects.clear();
        if (m_openTab < 0) return;
        float x = m_tabRects[m_openTab].x;
        float y = m_tabRects[m_openTab].y + m_tabRects[m_openTab].height;
        int count = GetDropdownItemCount(m_openTab);
        m_dropdownRect = {x, y, 180, count * 26.0f + 4};
        for (int i = 0; i < count; i++)
            m_itemRects.push_back({x + 2, y + 2 + i * 26.0f, 176, 24});
    }

    int GetDropdownItemCount(int tab) {
        switch (tab) {
            case 0: return 2; case 1: return 3; case 4: return 3;
            case 2: case 3: case 5: case 6: case 7: return 1;
            default: return 0;
        }
    }

    const char* GetDropdownItemName(int tab, int item) {
        switch (tab) {
            case 0: return item == 0 ? "Start New Campaign" : "Load Campaign Game";
            case 1: return item == 0 ? "Start New Game" : (item == 1 ? "Load Game" : "Quick Match");
            case 2: return "Enter Seraphic Realm";
            case 3: return "View Statistics";
            case 4: return item == 0 ? "Video Settings" : (item == 1 ? "Audio Settings" : "Controls");
            case 5: return "Configure Character";
            case 6: return "About Angels95";
            case 7: return "Multiplayer Lobby";
            default: return "";
        }
    }

    static const char* GetPaneTitle(PaneType t) {
        switch (t) {
            case PANE_CAMPAIGN:  return "Campaign Missions";
            case PANE_PRACTICE:  return "Practice Session";
            case PANE_SERAPHIC:  return "Seraphic Realm";
            case PANE_STATS:     return "Player Statistics";
            case PANE_SETTINGS:  return "Settings";
            case PANE_CHARACTER: return "Character Configuration";
            case PANE_HELP:      return "About Angels95";
            case PANE_MULTIPLAYER: return "Multiplayer Lobby";
            default:             return "";
        }
    }

    void OpenPane(PaneType type) {
        // If a pane of this type already exists, bring it to front
        for (size_t i = 0; i < m_panes.size(); i++) {
            if (m_panes[i].type == type) {
                BringPaneToFront((int)i);
                return;
            }
        }
        int pw, ph;
        GetPaneSize(type, pw, ph);
        Rectangle r;
        r.width = (float)pw;
        r.height = (float)ph;
        r.x = m_contentArea.x + m_cascadeX;
        r.y = m_contentArea.y + m_cascadeY;
        // Clamp inside content area
        if (r.x + r.width > m_contentArea.x + m_contentArea.width - 10)
            r.x = m_contentArea.x + 20;
        if (r.y + r.height > m_contentArea.y + m_contentArea.height - 10)
            r.y = m_contentArea.y + 20;
        // Cascade position for next pane
        m_cascadeX += 24;
        m_cascadeY += 24;
        if (m_cascadeX + pw > (int)m_contentArea.width - 60) m_cascadeX = 40;
        if (m_cascadeY + ph > (int)m_contentArea.height - 60) m_cascadeY = 40;
        // For settings pane, reset sub-page
        if (type == PANE_SETTINGS) m_settingsSubPage = 0;
        m_panes.push_back({r, type, false, {0, 0}});
    }

    void GetPaneSize(int type, int& w, int& h) {
        switch (type) {
            case PANE_CAMPAIGN:  w = 640; h = 380; break;
            case PANE_PRACTICE:  w = 640; h = 380; break;
            case PANE_SERAPHIC:  w = 460; h = 260; break;
            case PANE_STATS:     w = 360; h = 240; break;
            case PANE_SETTINGS:  w = 480; h = 340; break;
            case PANE_CHARACTER: w = 440; h = 260; break;
            case PANE_HELP:      w = 520; h = 360; break;
            case PANE_MULTIPLAYER: w = 520; h = 440; break;
            default:             w = 400; h = 300; break;
        }
    }

    void BringPaneToFront(int idx) {
        if (idx < 0 || idx >= (int)m_panes.size()) return;
        Pane p = m_panes[idx];
        m_panes.erase(m_panes.begin() + idx);
        m_panes.push_back(p);
    }

    int GetPaneAtPoint(Vector2 mp) {
        for (int i = (int)m_panes.size() - 1; i >= 0; i--)
            if (CheckCollisionPointRec(mp, m_panes[i].rect))
                return i;
        return -1;
    }

    Rectangle GetPaneTitleBarRect(const Pane& p) {
        return {p.rect.x + PANE_BORDER, p.rect.y + PANE_BORDER, p.rect.width - PANE_BORDER * 2, PANE_TITLE_H};
    }

    Rectangle GetPaneCloseBtnRect(const Pane& p) {
        Rectangle tb = GetPaneTitleBarRect(p);
        return {tb.x + tb.width - 16, tb.y, 14, tb.height};
    }

    Rectangle GetPaneContentRect(const Pane& p) {
        Rectangle tb = GetPaneTitleBarRect(p);
        return {
            p.rect.x + PANE_BORDER + 2,
            tb.y + tb.height + 2,
            p.rect.width - PANE_BORDER * 2 - 4,
            p.rect.height - PANE_BORDER - tb.height - 6
        };
    }

    void HandlePaneInput(Vector2 mp) {
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            // Check close buttons first (front to back)
            for (int i = (int)m_panes.size() - 1; i >= 0; i--) {
                Rectangle closeBtn = GetPaneCloseBtnRect(m_panes[i]);
                if (CheckCollisionPointRec(mp, closeBtn)) {
                    SoundManager::Instance().PlayUIClick();
                    m_panes.erase(m_panes.begin() + i);
                    return;
                }
            }

            // Check title bar for drag (front to back)
            for (int i = (int)m_panes.size() - 1; i >= 0; i--) {
                Rectangle tb = GetPaneTitleBarRect(m_panes[i]);
                if (CheckCollisionPointRec(mp, tb)) {
                    m_dragPane = true;
                    m_dragPaneIdx = i;
                    m_panes[i].dragging = true;
                    m_panes[i].dragOffset = {mp.x - m_panes[i].rect.x, mp.y - m_panes[i].rect.y};
                    BringPaneToFront(i);
                    return;
                }
            }

            // Check content area clicks (front to back)
            int paneIdx = GetPaneAtPoint(mp);
            if (paneIdx >= 0) {
                BringPaneToFront(paneIdx);
                Pane& p = m_panes[paneIdx];
                Rectangle cr = GetPaneContentRect(p);
                Vector2 localMp = {mp.x - cr.x, mp.y - cr.y};
                if (CheckCollisionPointRec(mp, cr)) {
                    HandlePaneContentClick(p.type, mp, cr);
                }
                return;
            }
        }
    }

    void HandlePaneContentClick(PaneType type, Vector2 mp, Rectangle contentRect) {
        switch (type) {
            case PANE_MULTIPLAYER: {
                float bx = contentRect.x + 12, by = contentRect.y + 40;
                auto checkBtn = [&](int idx, Rectangle r) {
                    if (CheckCollisionPointRec(mp, r)) {
                        SoundManager::Instance().PlayUIClick();
                        m_multiplayerSubPage = idx;
                        // Leaving a page must drop textbox focus so a hidden
                        // box can't keep editing with a stale cursor index.
                        m_joinIpEditing = m_joinPortEditing = m_hostPortEditing = false;
                    }
                };
                checkBtn(0, {bx, by, 140, 32});
                checkBtn(1, {bx, by + 40, 140, 32});
                break;
            }
            case PANE_CAMPAIGN:
            case PANE_PRACTICE: {
                bool campaign = (type == PANE_CAMPAIGN);
                int cols = std::max(1, (int)((contentRect.width - 24) / (CELL_W + CELL_GAP)));
                int sx = (int)(contentRect.x + 12);
                int sy = (int)(contentRect.y + 64);
                for (size_t i = 0; i < m_worlds.size(); i++) {
                    int r = (int)(i / cols), c = (int)(i % cols);
                    Rectangle cell = {(float)(sx + c * (CELL_W + CELL_GAP)), (float)(sy + r * (CELL_H + CELL_GAP)), (float)CELL_W, (float)CELL_H};
                    if (CheckCollisionPointRec(mp, cell)) {
                        SoundManager::Instance().PlayUIClick();
                        m_selectedWorld = m_worlds[i].dirName;
                        if (!campaign) { m_loadGame = false; }
                        m_exit = true;
                        return;
                    }
                }
                break;
            }
            case PANE_SETTINGS: {
                // Settings sub-page buttons
                float bx = contentRect.x + 12, by = contentRect.y + 40;
                auto checkBtn = [&](int idx, Rectangle r) {
                    if (CheckCollisionPointRec(mp, r)) {
                        SoundManager::Instance().PlayUIClick();
                        m_settingsSubPage = idx;
                    }
                };
                checkBtn(0, {bx, by, 140, 32});
                checkBtn(1, {bx, by + 40, 140, 32});
                checkBtn(2, {bx, by + 80, 140, 32});
                break;
            }
            case PANE_CHARACTER: {
                // Geometry comes from CharGeom so these hit rects can never
                // drift from the ones DrawCharacterPage actually painted.
                auto& pm = PlayerProfileManager::Instance();
                const CharLayout L = CharGeom(contentRect);
                const float x = contentRect.x + 16;
                const int listW = (int)(contentRect.width - 32);

                // Profile rows select the active slot.
                for (int i = 0; i < L.visibleRows; i++) {
                    Rectangle row = {x, (float)(L.rowY + i * L.rowH), (float)listW, (float)L.rowH};
                    if (CheckCollisionPointRec(mp, row)) {
                        SoundManager::Instance().PlayUIClick();
                        pm.SetActive(i);
                        LoadActiveProfileIntoFields();
                        return;
                    }
                }

                // Team swatches.
                for (int i = 0; i < TEAM_COLOR_COUNT; i++) {
                    Rectangle sw = {(float)(L.teamX + i * L.swStep), (float)L.teamY,
                                    (float)L.swSize, (float)L.swSize};
                    if (CheckCollisionPointRec(mp, sw)) {
                        SoundManager::Instance().PlayUIClick();
                        m_teamColor = i;
                        // Persist immediately so the swatch survives a pane close
                        // without an explicit Save.
                        SaveFieldsIntoActiveProfile();
                        break;
                    }
                }
                break;
            }
            default: break;
        }
    }

    void HandleDropdownClick(int tab, int item) {
        m_dropdownOpen = false; m_openTab = -1;
        switch (tab) {
            case 0:
                if (item == 0) OpenPane(PANE_CAMPAIGN);
                else if (item == 1) {
                    if (IsPathFile("GameData/Saves/TF.sav")) { LoadSave(); LoadFlag = true; m_exit = true; }
                }
                break;
            case 1:
                if (item == 0) OpenPane(PANE_PRACTICE);
                else if (item == 1) {
                    if (IsPathFile("GameData/Saves/TF.sav")) { LoadSave(); LoadFlag = true; m_exit = true; }
                }
                else if (item == 2 && !m_worlds.empty()) {
                    m_selectedWorld = m_worlds[0].dirName; m_exit = true;
                }
                break;
            case 2: OpenPane(PANE_SERAPHIC); break;
            case 3: OpenPane(PANE_STATS); break;
            case 4: OpenPane(PANE_SETTINGS); break;
            case 5: OpenPane(PANE_CHARACTER); break;
            case 6: OpenPane(PANE_HELP); break;
            case 7: OpenPane(PANE_MULTIPLAYER); break;
            default: break;
        }
    }

    void DrawBackground() {
        auto& tex = GetMenuTex();
        // Animated title background (MPEG1) — fall back to static Title.png.
        ray_video_t& vid = OmegaTechData.HomeScreenVideo;
        if (vid.ok && vid.texture.id != 0) {
            ray_video_update(&vid, GetFrameTime());
            if (vid.texture.id != 0) {
                // Cover-fit the video to the screen, preserving aspect.
                float vw = (float)vid.texture.width, vh = (float)vid.texture.height;
                float scale = std::max(m_sw / vw, m_sh / vh);
                float dw = vw * scale, dh = vh * scale;
                DrawTexturePro(vid.texture,
                    {0, 0, vw, vh},
                    {(m_sw - dw) * 0.5f, (m_sh - dh) * 0.5f, dw, dh},
                    {0, 0}, 0.f, WHITE);
                return;
            }
        }
        if (vid.ok)
            ray_video_update(&vid, GetFrameTime()); // keep decoding until first frame
        if (tex.titleBg.id)
            for (int x = 0; x < m_sw; x += tex.titleBg.width)
                for (int y = 0; y < m_sh; y += tex.titleBg.height)
                    DrawTexture(tex.titleBg, x, y, WHITE);
    }

    void Draw9SlicePanel(Rectangle r, Texture2D tl, Texture2D t, Texture2D tr,
                         Texture2D l, Texture2D area, Texture2D rgt,
                         Texture2D bl, Texture2D b, Texture2D br) {
        if (r.width < 8 || r.height < 8) return;
        auto d9 = [](Texture2D tex, Rectangle src, Rectangle dst) {
            if (tex.id) DrawTexturePro(tex, src, dst, {0,0}, 0, WHITE);
        };
        d9(tl, {0,0,(float)tl.width,(float)tl.height}, {r.x,r.y,4,4});
        d9(tr, {0,0,(float)tr.width,(float)tr.height}, {r.x+r.width-4,r.y,4,4});
        d9(bl, {0,0,(float)bl.width,(float)bl.height}, {r.x,r.y+r.height-4,4,4});
        d9(br, {0,0,(float)br.width,(float)br.height}, {r.x+r.width-4,r.y+r.height-4,4,4});
        d9(t, {0,0,(float)t.width,(float)t.height}, {r.x+4,r.y,r.width-8,4});
        d9(b, {0,0,(float)b.width,(float)b.height}, {r.x+4,r.y+r.height-4,r.width-8,4});
        d9(l, {0,0,(float)l.width,(float)l.height}, {r.x,r.y+4,4,r.height-8});
        d9(rgt,{0,0,(float)rgt.width,(float)rgt.height}, {r.x+r.width-4,r.y+4,4,r.height-8});
        d9(area,{0,0,(float)area.width,(float)area.height}, {r.x+4,r.y+4,r.width-8,r.height-8});
    }

    void Draw9SliceFrame() {
        auto& t = GetMenuTex();
        Draw9SlicePanel(m_frameRect, t.fTL, t.fT, t.fTR, t.fL, t.fArea, t.fR, t.fBL, t.fB, t.fBR);
    }

    void DrawTitleBar() {
        auto& t = GetMenuTex();
        Rectangle r = m_titleBar;
        DrawRectangleRec(r, (Color){0,16,48,255});
        if (t.barL.id) DrawTexturePro(t.barL, {0,0,16,16}, {r.x, r.y, 16, r.height}, {0,0}, 0, WHITE);
        if (t.barTile.id) {
            float tw = r.width - 16 - 16*3 - 8;
            if (tw > 0) DrawTexturePro(t.barTile, {0,0,1,16}, {r.x+16, r.y, tw, r.height}, {0,0}, 0, WHITE);
        }
        DrawText("Angels95", (int)(r.x + 20), (int)(r.y + 4), 14, WHITE);
        DrawWinButton(m_minBtn, 0);
        DrawWinButton(m_maxBtn, 1);
        DrawWinButton(m_closeBtn, 2);
        if (t.barWin.id)
            DrawTexturePro(t.barWin, {0,0,16,16}, {r.x + r.width - 16*3 - 8 - 20, r.y, 16, r.height}, {0,0}, 0, WHITE);
    }

    void DrawWinButton(Rectangle r, int type) {
        bool hover = CheckCollisionPointRec(GetMousePosition(), r);
        Color bg = hover ? (Color){60,80,120,255} : (Color){20,30,60,255};
        if (type == 2) bg = hover ? (Color){180,40,40,255} : (Color){100,20,20,255};
        DrawRectangleRec(r, bg);
        DrawRectangleLinesEx(r, 1, (Color){100,130,180,255});
        int cx = (int)(r.x + r.width/2), cy = (int)(r.y + r.height/2);
        if (type == 0) DrawRectangle(cx-4, cy, 8, 2, WHITE);
        else if (type == 1) DrawRectangleLines(cx-4, cy-3, 8, 6, WHITE);
        else { DrawLine(cx-3, cy-3, cx+3, cy+3, WHITE); DrawLine(cx+3, cy-3, cx-3, cy+3, WHITE); }
    }

    void DrawSeparator(Rectangle ref) {
        Rectangle s = {m_innerRect.x, ref.y - SEP_H, m_innerRect.width, SEP_H};
        auto& t = GetMenuTex();
        if (t.menuLine.id) DrawTexturePro(t.menuLine, {0,0,128,2}, s, {0,0}, 0, WHITE);
        else DrawRectangleRec(s, (Color){60,100,160,200});
    }

    void DrawMenuBar() {
        DrawRectangleRec(m_menuBar, (Color){8,16,40,255});
        for (int i = 0; i < TAB_COUNT; i++) {
            Rectangle tr = m_tabRects[i];
            bool hover = (i == m_hoveredTab);
            bool active = (i == m_openTab);
            if (active) DrawRectangleRec(tr, (Color){30,60,100,255});
            else if (hover) DrawRectangleRec(tr, (Color){20,40,70,255});
            Color tc = active ? WHITE : (hover ? WHITE : (Color){180,200,220,255});
            int fs = 12;
            DrawText(TAB_NAMES[i], (int)(tr.x + (tr.width - MeasureText(TAB_NAMES[i], fs))/2), (int)(tr.y + (tr.height - fs)/2), fs, tc);
        }
    }

    void DrawDropdown(int tab) {
        auto& t = GetMenuTex();
        Draw9SlicePanel(m_dropdownRect, t.fTL, t.fT, t.fTR, t.fL, t.fArea, t.fR, t.fBL, t.fB, t.fBR);
        int count = GetDropdownItemCount(tab);
        for (int i = 0; i < count && i < (int)m_itemRects.size(); i++) {
            Rectangle ir = m_itemRects[i];
            bool hover = (i == m_hoveredItem);
            if (hover) DrawRectangleRec(ir, (Color){40,80,140,200});
            DrawText(GetDropdownItemName(tab, i), (int)(ir.x + 6), (int)(ir.y + 6), 12, hover ? WHITE : (Color){200,210,230,255});
            if (i < count - 1) DrawLine((int)ir.x, (int)(ir.y + ir.height - 1), (int)(ir.x + ir.width), (int)(ir.y + ir.height - 1), (Color){60,80,120,150});
        }
    }

    // ── Pane rendering ──────────────────────────────────────────────

    void DrawContent(Rectangle area) {
        auto& t = GetMenuTex();
        if (t.clientArea.id) DrawTexturePro(t.clientArea, {0,0,128,1}, area, {0,0}, 0, WHITE);
        else DrawRectangleRec(area, (Color){4,8,24,220});

        for (auto& p : m_panes) {
            DrawPane(p);
        }

        if (m_panes.empty()) {
            const char* msg = "Select an action from the menu above";
            DrawText(msg, (int)(area.x + (area.width - MeasureText(msg, 16)) / 2),
                     (int)(area.y + area.height / 2 - 10), 16, (Color){100,130,160,150});
        }
    }

    void DrawPane(const Pane& p) {
        auto& t = GetMenuTex();
        // Draw 9-slice frame
        Draw9SlicePanel(p.rect, t.fTL, t.fT, t.fTR, t.fL, t.fArea, t.fR, t.fBL, t.fB, t.fBR);

        // Draw pane title bar
        Rectangle tb = GetPaneTitleBarRect(p);
        DrawRectangleRec(tb, (Color){0,16,48,255});
        if (t.barL.id)
            DrawTexturePro(t.barL, {0,0,16,16}, {tb.x, tb.y, 16, tb.height}, {0,0}, 0, WHITE);
        if (t.barTile.id && tb.width > 32) {
            float tw = tb.width - 16 - 16 - 2;
            if (tw > 0)
                DrawTexturePro(t.barTile, {0,0,1,16}, {tb.x + 16, tb.y, tw, tb.height}, {0,0}, 0, WHITE);
        }
        DrawWinButton(GetPaneCloseBtnRect(p), 2);
        const char* title = GetPaneTitle(p.type);
        DrawText(title, (int)(tb.x + 20), (int)(tb.y + 4), 12, WHITE);

        // Draw pane content with scissoring
        Rectangle cr = GetPaneContentRect(p);
        BeginScissorMode((int)cr.x, (int)cr.y, (int)cr.width, (int)cr.height);
        DrawPaneContent(p.type, cr);
        EndScissorMode();
    }

    void DrawPaneContent(PaneType type, Rectangle area) {
        switch (type) {
            case PANE_CAMPAIGN:  DrawCampaignPage(area); break;
            case PANE_PRACTICE:  DrawPracticeSessionPage(area); break;
            case PANE_SERAPHIC:  DrawSeraphicPage(area); break;
            case PANE_STATS:     DrawStatsPage(area); break;
            case PANE_SETTINGS:  DrawSettingsPage(area); break;
            case PANE_CHARACTER: DrawCharacterPage(area); break;
            case PANE_HELP:      DrawHelpPage(area); break;
            case PANE_MULTIPLAYER: DrawMultiplayerPage(area); break;
        }
    }

    // ── Page drawing functions (no input handling) ──────────────────

    void DrawPageHeader(Rectangle area, const char* title) {
        DrawText(title, (int)(area.x + 12), (int)(area.y + 8), 18, WHITE);
        DrawLine((int)(area.x + 12), (int)(area.y + 30), (int)(area.x + area.width - 12), (int)(area.y + 30), (Color){60,100,160,150});
    }

    void DrawLevelSelectGrid(Rectangle area, bool campaignMode) {
        DrawPageHeader(area, campaignMode ? "Campaign Missions" : "Practice Session");
        DrawText("Game Type: ", (int)(area.x + 12), (int)(area.y + 38), 12, LIGHTGRAY);
        float fx = area.x + 12 + MeasureText("Game Type: ", 12) + 4;
        Rectangle filterRect = {fx, (float)(area.y + 36), 120, 20};
        GuiDropdownBox(filterRect, "All Types;Campaign;Deathmatch;CTF", &m_gameTypeFilter, false);

        int cols = std::max(1, (int)((area.width - 24) / (CELL_W + CELL_GAP)));
        int sx = (int)(area.x + 12), sy = (int)(area.y + 64);
        Vector2 mp = GetMousePosition();

        for (size_t i = 0; i < m_worlds.size(); i++) {
            int r = (int)(i / cols), c = (int)(i % cols);
            Rectangle cell = {(float)(sx + c * (CELL_W + CELL_GAP)), (float)(sy + r * (CELL_H + CELL_GAP)), (float)CELL_W, (float)CELL_H};
            bool hover = CheckCollisionPointRec(mp, cell);
            DrawRectangleRounded(cell, 0.15f, 8, hover ? (Color){40,80,140,180} : (Color){12,24,50,200});
            DrawRectangleRoundedLinesEx(cell, 0.15f, 8, 1.0f, hover ? (Color){100,160,220,200} : (Color){40,60,100,150});
            const char* wn = m_worlds[i].name.c_str();
            int fs = 13;
            DrawText(wn, (int)(cell.x + (cell.width - MeasureText(wn, fs))/2), (int)(cell.y + (cell.height - fs)/2), fs, hover ? WHITE : (Color){180,200,230,255});
        }
        if (m_worlds.empty())
            DrawText("No worlds found in GameData/Worlds/", (int)(area.x + 12), (int)(area.y + 64), 14, (Color){180,180,180,200});
    }

    void DrawCampaignPage(Rectangle area) { DrawLevelSelectGrid(area, true); }
    void DrawPracticeSessionPage(Rectangle area) { DrawLevelSelectGrid(area, false); }

    void DrawSeraphicPage(Rectangle area) {
        DrawPageHeader(area, "Seraphic Realm");
        const char* l[] = {"The Seraphic Realm awaits...","","A celestial battlefield where angels descend","to challenge the worthy.","","Coming Soon","","Prepare yourself for the ascent."};
        int y = (int)(area.y + 40);
        for (int i = 0; i < 8; i++) { DrawText(l[i], (int)(area.x + 16), y, 14, (Color){160,180,200,220}); y += 22; }
    }

    void DrawStatsPage(Rectangle area) {
        DrawPageHeader(area, "Player Statistics");
        char buf[128]; int y = (int)(area.y + 40);
        snprintf(buf, sizeof(buf), "Deaths: %d", OmegaTechData.Deaths); DrawText(buf, (int)(area.x + 16), y, 14, LIGHTGRAY); y += 24;
        snprintf(buf, sizeof(buf), "Level: %d", OmegaTechData.LevelIndex); DrawText(buf, (int)(area.x + 16), y, 14, LIGHTGRAY); y += 24;
        DrawText("More statistics coming soon...", (int)(area.x + 16), y + 20, 13, (Color){120,140,160,180});
    }

    void DrawSettingsPage(Rectangle area) {
        DrawPageHeader(area, "Settings");
        auto drawBtn = [&](int idx, const char* label, Rectangle r) {
            bool h = CheckCollisionPointRec(GetMousePosition(), r);
            auto& t = GetMenuTex();
            DrawTexturePro(h ? t.btnHover : t.btnNormal, {0,0,64,64}, r, {0,0}, 0, h ? WHITE : (Color){200,200,200,255});
            DrawText(label, (int)(r.x + (r.width - MeasureText(label, 13))/2), (int)(r.y + 9), 13, h ? WHITE : LIGHTGRAY);
        };
        float bx = area.x + 12, by = area.y + 40;
        drawBtn(0, "Video", {bx, by, 140, 32});
        drawBtn(1, "Audio", {bx, by + 40, 140, 32});
        drawBtn(2, "Controls", {bx, by + 80, 140, 32});

        float sx = bx + 160, sw = area.x + area.width - sx - 12;
        Rectangle sub = {sx, by, sw, area.y + area.height - by - 8};
        DrawRectangleRec(sub, (Color){8,16,36,180});
        int y = (int)(sub.y + 4);
        switch (m_settingsSubPage) {
            case 0: {
                DrawText("Video Settings", (int)(sx + 8), y, 14, WHITE); y += 24;
                bool fs = IsWindowFullscreen();
                if (GuiButton({sx+8, (float)y, 140, 22}, fs ? "Fullscreen: ON" : "Fullscreen: OFF")) ToggleFullscreen();
                y += 30;
                DrawText("Press Alt+Enter to toggle fullscreen", (int)(sx + 8), y, 11, (Color){140,160,180,200});
                break;
            }
            case 1: {
                DrawText("Audio Settings", (int)(sx + 8), y, 14, WHITE); y += 24;
                // Drive the persisted AudioSlider (0..100) so the value survives
                // SaveClientSettings() on exit.
                float vol = GuiSlider({sx+8, (float)y, 200, 20}, "Volume", NULL, AudioSlider, 0.0f, 100.0f);
                if (vol != AudioSlider) { AudioSlider = vol; ApplyMasterVolume(); }
                break;
            }
            case 2: {
                DrawText("Controls", (int)(sx + 8), y, 14, WHITE); y += 24;
                const char* c[] = {"WASD - Move","Mouse - Look","Left Click - Fire","E - Interact","Tab - Inventory","Escape - Pause","` - Console"};
                for (int i = 0; i < 7; i++) { DrawText(c[i], (int)(sx + 8), y, 13, LIGHTGRAY); y += 20; }
                break;
            }
        }
    }

    // Single source of truth for Character-pane geometry. The draw pass and the
    // click handler must agree, and the row count depends on how many profiles
    // exist — so both read these values instead of hardcoding offsets.
    struct CharLayout {
        int  visibleRows = 0;
        int  rowY   = 0;    // top of the first profile row
        int  rowH   = 18;
        int  teamX  = 0;    // left edge of the first team swatch
        int  teamY  = 0;
        int  swStep = 26;
        int  swSize = 22;
        int  saveY  = 0;    // top of the Save/New/Delete button row
    };

    CharLayout CharGeom(Rectangle area) const {
        CharLayout L;
        L.rowY = (int)area.y + 62;                 // below the page header + "Profile:" label
        const int listBottom = (int)(area.y + area.height) - 150;
        for (int i = 0; i < PlayerProfileManager::Instance().Count(); i++) {
            if (L.rowY + L.rowH * (i + 1) > listBottom) break;
            L.visibleRows++;
        }
        int y = L.rowY + L.rowH * L.visibleRows + 24;   // + list bottom padding
        y += 30;                                          // name row
        y += 30;                                          // model row
        y += 30;                                          // voice row
        L.teamX = (int)area.x + 16 + 60;
        L.teamY = y;
        y += 26;                                          // swatch row + team caption
        L.saveY = y + 26;
        return L;
    }

    void DrawCharacterPage(Rectangle area) {
        DrawPageHeader(area, "Character Configuration");
        float x = area.x + 16;
        int y = (int)area.y + 42;
        auto& pm = PlayerProfileManager::Instance();
        const CharLayout L = CharGeom(area);

        // --- Profile slots -------------------------------------------------
        DrawText("Profile:", (int)x, y, 14, WHITE);

        const int listW = (int)(area.width - 32);
        for (int i = 0; i < L.visibleRows; i++) {
            const PlayerProfile& p = pm.Get(i);
            Rectangle row = {x, (float)(L.rowY + i * L.rowH), (float)listW, (float)L.rowH};
            bool active = (i == pm.ActiveIndex());
            bool hover = CheckCollisionPointRec(GetMousePosition(), row);
            DrawRectangleRec(row, active ? (Color){40,80,140,200}
                                        : (hover ? (Color){24,44,84,200} : (Color){12,24,50,160}));
            if (active) DrawRectangleLinesEx(row, 1, (Color){120,190,255,220});

            char label[192];
            snprintf(label, sizeof(label), "%s%s%s",
                     active ? "> " : "  ", p.displayName.c_str(),
                     p.team > 0 ? TextFormat("  [%s]", TEAM_COLOR_NAMES[
                         (p.team < TEAM_COLOR_COUNT) ? p.team : 0]) : "");
            DrawText(label, (int)(row.x + 4), (int)(row.y + 3), 12,
                     active ? WHITE : (Color){180,200,230,220});
        }
        if (L.visibleRows == 0)
            DrawText("(no profiles)", (int)(x + 4), L.rowY, 12, (Color){140,160,180,180});

        // --- Fields for the active slot -----------------------------------
        y = L.rowY + L.rowH * L.visibleRows + 24;
        DrawText("Name:", (int)x, y, 14, WHITE);
        Rectangle nameRect = {x + 60, (float)y - 2, 200, 22};
        // GuiTextBox returns true on focus enter/exit; toggling here keeps the
        // shared cursor index valid and lets exactly one box consume key input.
        if (GuiTextBox(nameRect, m_charName, sizeof(m_charName), m_charNameEditing))
            m_charNameEditing = !m_charNameEditing;
        y += 30;

        DrawText("Model:", (int)x, y, 14, WHITE);
        Rectangle modelRect = {x + 60, (float)y - 2, 260, 22};
        if (GuiTextBox(modelRect, m_modelPath, sizeof(m_modelPath), m_modelPathEditing))
            m_modelPathEditing = !m_modelPathEditing;
        y += 30;

        DrawText("Voice Set:", (int)x, y, 14, WHITE);
        Rectangle voiceRect = {x + 60, (float)y - 2, 200, 22};
        if (GuiTextBox(voiceRect, m_voiceSet, sizeof(m_voiceSet), m_voiceSetEditing))
            m_voiceSetEditing = !m_voiceSetEditing;
        y += 30;

        DrawText("Team:", (int)x, L.teamY + 4, 14, WHITE);
        for (int i = 0; i < TEAM_COLOR_COUNT; i++) {
            Rectangle sw = {(float)(L.teamX + i * L.swStep), (float)L.teamY,
                            (float)L.swSize, (float)L.swSize};
            bool h = CheckCollisionPointRec(GetMousePosition(), sw);
            DrawRectangleRec(sw, TEAM_COLORS[i]);
            if (i == m_teamColor) DrawRectangleLinesEx(sw, 2, WHITE);
            else if (h) DrawRectangleLinesEx(sw, 1, LIGHTGRAY);
        }
        DrawText(TEAM_COLOR_NAMES[(m_teamColor >= 0 && m_teamColor < TEAM_COLOR_COUNT)
                                  ? m_teamColor : 0],
                 (int)x, L.teamY + L.swSize + 4, 12, LIGHTGRAY);

        // --- Actions -------------------------------------------------------
        if (GuiButton({x, (float)L.saveY, 90, 24}, "Save"))
            SaveFieldsIntoActiveProfile();
        if (GuiButton({x + 98, (float)L.saveY, 90, 24}, "New")) {
            // Seed the new slot from the current fields, then make it active.
            PlayerProfile p;
            p.displayName = m_charName;
            p.modelPath   = m_modelPath;
            p.voiceSet    = m_voiceSet;
            p.team        = m_teamColor;
            pm.Add(p);
            LoadActiveProfileIntoFields();
        }
        if (GuiButton({x + 196, (float)L.saveY, 90, 24}, "Delete")) {
            if (pm.Count() > 1) {
                pm.Remove(pm.ActiveIndex());
                LoadActiveProfileIntoFields();
            }
        }

        // DrawText takes a const char*, so the concatenation has to be
        // materialised first: "Saved to " + DefaultPath() has no operator+, and
        // std::string + const char* yields a std::string, which DrawText also
        // cannot take.
        const std::string savePath = std::string("Saved to ") + PlayerProfileManager::DefaultPath();
        DrawText(savePath.c_str(), (int)x, L.saveY + 28, 11, (Color){120,150,180,190});
    }

    void DrawMultiplayerPage(Rectangle area) {
        DrawPageHeader(area, "Multiplayer Lobby");
        float bx = area.x + 12, by = area.y + 40;
        auto drawTabBtn = [&](int idx, const char* label, Rectangle r) {
            bool h = CheckCollisionPointRec(GetMousePosition(), r);
            auto& t = GetMenuTex();
            DrawTexturePro(h ? t.btnHover : t.btnNormal, {0,0,64,64}, r, {0,0}, 0, h ? WHITE : (Color){200,200,200,255});
            DrawText(label, (int)(r.x + (r.width - MeasureText(label, 13))/2), (int)(r.y + 9), 13, h ? WHITE : LIGHTGRAY);
        };
        drawTabBtn(0, "Join Game", {bx, by, 140, 32});
        drawTabBtn(1, "Host Game", {bx, by + 40, 140, 32});

        float sx = bx + 160, sw = area.x + area.width - sx - 12;
        Rectangle sub = {sx, by, sw, area.y + area.height - by - 8};
        DrawRectangleRec(sub, (Color){8,16,36,180});
        int y = (int)(sub.y + 4);

        if (m_multiplayerSubPage == 0) {
            DrawText("Join a Game Server", (int)(sx + 8), y, 14, WHITE); y += 24;
            DrawText("Server IP:", (int)(sx + 8), y + 4, 12, LIGHTGRAY);
            // raygui textboxes must only be in edit mode while focused; forcing
            // editMode=true every frame corrupts the buffer (shared cursor index
            // desync), which produced garbage hosts like "z".
            Rectangle ipRect = {sx + 80, (float)y, 150, 22};
            DrawText("Port:", (int)(sx + 240), y + 4, 12, LIGHTGRAY);
            Rectangle portRect = {sx + 276, (float)y, 70, 22};
            if (GuiTextBox(ipRect, m_joinIPBuffer, sizeof(m_joinIPBuffer), m_joinIpEditing))
                m_joinIpEditing = !m_joinIpEditing;
            if (GuiTextBox(portRect, m_hostPortBuffer, sizeof(m_hostPortBuffer), m_joinPortEditing))
                m_joinPortEditing = !m_joinPortEditing;
            y += 30;
            if (GuiButton({sx + 8, (float)y, 140, 24}, "Connect")) {
                SoundManager::Instance().PlayUIClick();
                // Commit the typed/selected values (authoritative join target)
                std::strncpy(m_joinIP, m_joinIPBuffer, sizeof(m_joinIP) - 1);
                m_joinIP[sizeof(m_joinIP) - 1] = '\0';
                m_joinIpEditing = false;
                m_joinPortEditing = false;
                m_joinServer = true;
                m_exit = true;
            }
            y += 30;

            // Discovery source: LAN broadcast or internet master server.
            DrawText("Source:", (int)(sx + 8), y + 5, 12, LIGHTGRAY);
            if (GuiButton({sx + 60, (float)y, 64, 22}, "LAN")) {
                if (m_serverSource != 0) SoundManager::Instance().PlayUIClick();
                m_serverSource = 0;
            }
            if (GuiButton({sx + 130, (float)y, 82, 22}, "Internet")) {
                if (m_serverSource != 1) {
                    SoundManager::Instance().PlayUIClick();
                    m_serverSource = 1;
                    // Auto-fetch the master list the first time Internet is opened
                    if (!m_internet.HasResult() && !m_internet.IsScanning()) {
                        m_internet.SetMasters(m_masterUrls);
                        m_internet.Refresh();
                    }
                }
            }
            y += 30;

            if (m_serverSource == 0) {
                if (GuiButton({sx + 8, (float)y, 140, 24}, "Scan LAN")) {
                    SoundManager::Instance().PlayUIClick();
                    LanStartScan();
                }
            } else {
                if (GuiButton({sx + 8, (float)y, 160, 24}, "Scan Internet")) {
                    SoundManager::Instance().PlayUIClick();
                    m_internet.SetMasters(m_masterUrls);
                    m_internet.Refresh();
                }
                const char* st = m_internet.IsScanning() ? "querying masters..."
                    : (!m_internet.Error().empty() ? m_internet.Error().c_str()
                       : (m_internet.HasResult() ? "master list loaded" : "not scanned"));
                DrawText(st, (int)(sx + 176), (int)(y + 7), 11, (Color){150,170,190,220});
            }
            y += 30;

            // Server list
            {
                int listH = (int)(sub.y + sub.height - y - 26);
                if (listH > 30) {
                    const auto& onlineList = m_internet.Servers();
                    const char* label = (m_serverSource == 0)
                        ? (m_lanScanning ? TextFormat("Servers on LAN (%d):", (int)m_lanServers.size())
                                         : "Servers on LAN:")
                        : (m_internet.IsScanning() ? "Internet servers (scanning...)"
                                                   : TextFormat("Internet servers (%d):", (int)onlineList.size()));
                    DrawText(label, (int)(sx + 8), y, 12, LIGHTGRAY);
                    y += 18;
                    Rectangle listRect = {sx + 4, (float)y, sub.width - 8, (float)(sub.y + sub.height - y - 22)};
                    DrawRectangleRec(listRect, (Color){0, 0, 0, 120});
                    float ry = listRect.y + 2;
                    const float rowH = 20.0f;

                    if (m_serverSource == 0) {
                        m_lanHover = -1;
                        for (int i = 0; i < (int)m_lanServers.size(); i++) {
                            if (ry + rowH > listRect.y + listRect.height) break;
                            const auto& s = m_lanServers[i];
                            Rectangle row = {listRect.x + 2, ry, listRect.width - 4, rowH};
                            bool hov = CheckCollisionPointRec(GetMousePosition(), row);
                            if (hov) m_lanHover = i;
                            if (hov) DrawRectangleRec(row, (Color){60, 110, 180, 90});
                            char rowText[200];
                            snprintf(rowText, sizeof(rowText), "%s  %s:%u  (%u/%u)",
                                     s.name.c_str(), s.ip.c_str(), (unsigned)s.port,
                                     s.cur_players, s.max_players);
                            DrawText(rowText, (int)(row.x + 4), (int)(ry + 4), 11,
                                     hov ? WHITE : (Color){170, 190, 210, 230});
                            if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                                SoundManager::Instance().PlayUIClick();
                                snprintf(m_joinIPBuffer, sizeof(m_joinIPBuffer), "%s", s.ip.c_str());
                                snprintf(m_hostPortBuffer, sizeof(m_hostPortBuffer), "%u", (unsigned)s.port);
                                // Keep the authoritative join target in sync with
                                // the clicked row (m_joinIP is what GetJoinIP reads).
                                snprintf(m_joinIP, sizeof(m_joinIP), "%s", s.ip.c_str());
                            }
                            ry += rowH;
                        }
                        if (m_lanServers.empty())
                            DrawText(m_lanScanning ? "scanning..." : "no servers found",
                                     (int)(listRect.x + 6), (int)(listRect.y + 6), 11,
                                     (Color){110, 130, 150, 200});
                    } else {
                        m_onlineHover = -1;
                        for (int i = 0; i < (int)onlineList.size(); i++) {
                            if (ry + rowH > listRect.y + listRect.height) break;
                            const auto& os = onlineList[i];
                            const auto& s = os.entry;
                            Rectangle row = {listRect.x + 2, ry, listRect.width - 4, rowH};
                            bool hov = CheckCollisionPointRec(GetMousePosition(), row);
                            if (hov) m_onlineHover = i;
                            if (hov) DrawRectangleRec(row, (Color){60, 110, 180, 90});
                            char ping[16] = "";
                            if (os.ping_ms >= 0) snprintf(ping, sizeof(ping), "  %dms", os.ping_ms);
                            char rowText[256];
                            snprintf(rowText, sizeof(rowText), "%s  %s:%u  (%u/%u)  %s%s%s",
                                     s.name.empty() ? "server" : s.name.c_str(),
                                     s.ip.c_str(), (unsigned)s.port,
                                     s.players, s.maxplayers,
                                     s.map.c_str(), ping, s.password ? "  [pw]" : "");
                            DrawText(rowText, (int)(row.x + 4), (int)(ry + 4), 11,
                                     hov ? WHITE : (Color){170, 190, 210, 230});
                            if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                                SoundManager::Instance().PlayUIClick();
                                snprintf(m_joinIPBuffer, sizeof(m_joinIPBuffer), "%s", s.ip.c_str());
                                snprintf(m_hostPortBuffer, sizeof(m_hostPortBuffer), "%u", (unsigned)s.port);
                                // Keep the authoritative join target in sync (GetJoinIP reads m_joinIP)
                                snprintf(m_joinIP, sizeof(m_joinIP), "%s", s.ip.c_str());
                            }
                            ry += rowH;
                        }
                        if (onlineList.empty()) {
                            const char* msg = m_internet.IsScanning() ? "scanning..."
                                : (!m_internet.HasResult() ? "not scanned" : "no servers found");
                            DrawText(msg, (int)(listRect.x + 6), (int)(listRect.y + 6), 11,
                                     (Color){110, 130, 150, 200});
                        }
                    }
                    y = listRect.y + listRect.height;
                }
            }

            DrawText("Click a server to fill IP and port, then Connect.",
                     (int)(sx + 8), (int)(sub.y + sub.height - 16), 11, (Color){140,160,180,200});
        } else {
            DrawText("Host a Game Server", (int)(sx + 8), y, 14, WHITE); y += 24;
            DrawText("World:", (int)(sx + 8), y + 4, 12, LIGHTGRAY);
            // World dropdown
            {
                static int worldSel = 0;
                if (worldSel >= (int)m_worlds.size()) worldSel = 0;
                std::string wl;
                for (size_t i = 0; i < m_worlds.size(); i++) {
                    if (i) wl += ";";
                    wl += m_worlds[i].name;
                }
                if (!wl.empty()) {
                    GuiDropdownBox({sx + 60, (float)y, sw - 68, 22}, wl.c_str(), &worldSel, false);
                    if (worldSel >= 0 && worldSel < (int)m_worlds.size())
                        m_selectedServerWorld = m_worlds[worldSel].dirName;
                }
            }
            y += 30;
            DrawText("Port:", (int)(sx + 8), y + 4, 12, LIGHTGRAY);
            Rectangle hostPortRect = {sx + 80, (float)y, 80, 22};
            if (GuiTextBox(hostPortRect, m_hostPortBuffer, sizeof(m_hostPortBuffer), m_hostPortEditing))
                m_hostPortEditing = !m_hostPortEditing;
            y += 34;
            if (GuiButton({sx + 8, (float)y, 140, 24}, "Start Server")) {
                SoundManager::Instance().PlayUIClick();
                m_hostPortEditing = false;
                if (!m_selectedServerWorld.empty()) {
                    m_selectedWorld = m_selectedServerWorld;
                    m_startServer = true;
                    m_exit = true;
                }
            }
            y += 30;
            DrawText("Select a world and port, then click", (int)(sx + 8), y, 11, (Color){140,160,180,200}); y += 14;
            DrawText("Start Server to host a game.", (int)(sx + 8), y, 11, (Color){140,160,180,200});
        }
    }

    void DrawHelpPage(Rectangle area) {
        DrawPageHeader(area, "About Angels95 / OmegaTech Engine");
        const char* l[] = {
            "Angels95 - Powered by the OzWorld Engine - based on OmegaTech Engine","","A first-person shooter built with raylib 5.5 and C++20.","",
            "Controls:","  WASD  - Movement","  Mouse - Look around","  Left Click - Fire weapon",
            "  E     - Interact / Use","  Tab   - Inventory","  Escape - Pause menu","  `     - Console","",
            "Features:","  - LightningScript scripting system","  - OZONE world format with CSG brushes",
            "  - Dynamic NPC system with FSM AI","  - Custom UDP networking","  - Particle effects system","  - OzPackage asset management"
        };
        int y = (int)(area.y + 40);
        for (int i = 0; i < 21; i++) { DrawText(l[i], (int)(area.x + 16), y, 13, (Color){160,180,200,220}); y += 18; }
    }

    // Copy the active profile into the edit buffers. Called on construction and
    // whenever the selection changes, so the fields always show the active slot.
    void LoadActiveProfileIntoFields() {
        const PlayerProfile& p = PlayerProfileManager::Instance().Active();
        snprintf(m_charName,   sizeof(m_charName),   "%s", p.displayName.c_str());
        snprintf(m_modelPath,  sizeof(m_modelPath),  "%s", p.modelPath.c_str());
        snprintf(m_voiceSet,   sizeof(m_voiceSet),   "%s", p.voiceSet.c_str());
        // team is 1-based on the profile, 0-based as a TEAM_COLORS index.
        m_teamColor = (p.team >= 0 && p.team < TEAM_COLOR_COUNT) ? p.team : 0;
    }

    // Write the edit buffers back into the active profile and persist.
    void SaveFieldsIntoActiveProfile() {
        PlayerProfile p = PlayerProfileManager::Instance().Active();
        p.displayName = m_charName;
        p.modelPath   = m_modelPath;
        p.voiceSet    = m_voiceSet;
        p.team        = m_teamColor;
        PlayerProfileManager::Instance().Update(PlayerProfileManager::Instance().ActiveIndex(), p);
    }

    void DrawHelpBar() {
        DrawRectangleRec(m_helpBar, (Color){6,12,32,230});
        const char* h = GetContextHelp();
        DrawText(h, (int)(m_helpBar.x + 8), (int)(m_helpBar.y + 5), 12, (Color){160,190,220,220});
    }

    const char* GetContextHelp() {
        if (m_dropdownOpen && m_openTab >= 0 && m_hoveredItem >= 0) {
            if (m_openTab == 0) return m_hoveredItem == 0 ? "Begin a new campaign mission" : "Load a previously saved campaign game";
            if (m_openTab == 1) return m_hoveredItem == 0 ? "Start a new practice session" : (m_hoveredItem == 1 ? "Load a saved game" : "Jump straight into a random world");
            if (m_openTab == 2) return "Enter the Seraphic Realm - celestial PvP arena (coming soon)";
            if (m_openTab == 3) return "View your player statistics and achievements";
            if (m_openTab == 4) return m_hoveredItem == 0 ? "Configure video and graphics" : (m_hoveredItem == 1 ? "Adjust audio volume" : "Configure keyboard and mouse");
            if (m_openTab == 5) return "Customize your character";
            if (m_openTab == 6) return "About Angels95 and the OmegaTech Engine";
            if (m_openTab == 7) return "Join or host a multiplayer game";
        }
        if (!m_panes.empty()) {
            int idx = GetPaneAtPoint(GetMousePosition());
            if (idx >= 0) {
                switch (m_panes[idx].type) {
                    case PANE_CAMPAIGN:  return "Campaign mission selection";
                    case PANE_PRACTICE:  return "Practice session world selection";
                    case PANE_SERAPHIC:  return "The Seraphic Realm - coming soon";
                    case PANE_STATS:     return "Your player statistics";
                    case PANE_SETTINGS:  return "Configure game settings";
                    case PANE_CHARACTER: return "Customize your character";
                    case PANE_HELP:      return "About Angels95 / OmegaTech Engine";
                    case PANE_MULTIPLAYER: return "Join or host a multiplayer game";
                }
            }
        }
        return "Select an action from the menu above";
    }
};

const char* TitleMenu::TAB_NAMES[TitleMenu::TAB_COUNT] = {
    "Campaign", "Practice Session", "Seraphic Realm",
    "Stats", "Settings", "Character", "Help", "Multiplayer"
};
