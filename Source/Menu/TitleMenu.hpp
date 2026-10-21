#pragma once
// ---------------------------------------------------------------------------
// Menu/TitleMenu.hpp
//
// The title screen. This is a SINGLE-SHELL menu:
//   title bar -> one nav bar -> one page -> one footer
//
// The old implementation was a fake desktop with draggable floating panes. Every
// page was drawn every frame and several pages handled clicks during draw, so a
// background pane could receive a click through the front one; the Character
// pane's Save/New/Delete buttons were outside its own content rect, and the
// campaign list was clipped by a fixed pane height. Those are structural, not
// cosmetic, so the pane system is gone.
//
// Rules this file follows:
//   * one active page, so click-through cannot happen;
//   * all geometry comes from MenuMetrics (responsive/DPI aware);
//   * a menu entry either works or is hidden (no "coming soon");
//   * long lists scroll and keep the keyboard selection visible;
//   * the menu draws directly to the screen, not into the game's Target, so a
//     window resize cannot leave it stretched.
//
// Character models/portraits come from CharacterRoster + CharacterPreview, which
// render the REAL shipped meshes. The only two player models in the tree are
// Plague_Arcanist.glb and Character_Killer_01.glb; the roster is data-driven so
// more entries are just new sections in characters.cfg.
// ---------------------------------------------------------------------------
#include "raylib.h"
#include "../Renderer/raygui/raygui.h"
#include "../Network/Network.hpp"
#include "InternetBrowser.hpp"
#include "../Client/MasterList.hpp"
#include "../Client/JoinUri.hpp"
#include "../Audio/SoundManager.hpp"
#include "../GameplayFlags.hpp"
#include "../Version.hpp"
#include "../Input/Keybinds.hpp"
#include "../Game/GameManager.hpp"
#include "../PlayerProfile.hpp"
#include "MenuStyle.hpp"
#include "MenuMetrics.hpp"
#include "CharacterRoster.hpp"
#include "CharacterPreview.hpp"
#include "ServerBrowserModel.hpp"
#include "../Settings.hpp"

using angelsmenu::CharacterDef;
using angelsmenu::CharacterRoster;

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Index 0 is "Unassigned" (PlayerProfile::team == 0); 1..7 map to colours.
// Kept here rather than in the profile manager because it is presentation. The
// server has no team concept yet; this is a cosmetic colour, not a team.
static const Color TEAM_COLORS[8] = {
    {90, 90, 100, 255},    // Unassigned
    {200, 40, 40, 255},    {40, 100, 220, 255},  {40, 180, 60, 255},
    {200, 170, 30, 255},   {60, 180, 200, 255},  {160, 50, 200, 255},
    {220, 130, 30, 255}
};
static const char* TEAM_COLOR_NAMES[8] = {
    "Unassigned","Red","Blue","Green","Gold","Azure","Purple","Orange"
};
static const int TEAM_COLOR_COUNT = 8;

struct WorldEntry {
    std::string name;
    std::string dirName;
    std::string mode;
    std::string sector;
};

// ---------------------------------------------------------------------------
// Title menu texture bundle. EVERY texture goes through the package-aware
// fallback: the old loader used LoadTexture() for the frame/bar/button skin, so
// a packaged-only install lost the entire menu chrome.
// ---------------------------------------------------------------------------
struct TitleMenuTextures {
    Texture2D titleBg{0};
    Texture2D heading{0};
    Texture2D fTL{0}, fT{0}, fTR{0};
    Texture2D fL{0}, fArea{0}, fR{0};
    Texture2D fBL{0}, fB{0}, fBR{0};
    Texture2D barL{0}, barTile{0}, barWin{0}, barMax{0};
    Texture2D menuLine{0}, clientArea{0};
    Texture2D btnNormal{0}, btnHover{0}, btnClicked{0};
    bool loaded = false;

    void Load() {
        if (loaded) return;
        auto t = [](const char* p) { return LoadTextureWithFallback(p); };
        titleBg  = t("GameData/Tex/Title/Title.png");
        heading  = t("GameData/Tex/Title/menu_heading.png");
        fTL = t("GameData/Tex/Menu/BlueMenuTL.png");
        fT  = t("GameData/Tex/Menu/BlueMenuT.png");
        fTR = t("GameData/Tex/Menu/BlueMenuTR.png");
        fL  = t("GameData/Tex/Menu/BlueMenuL.png");
        fArea = t("GameData/Tex/Menu/BlueMenuArea.png");
        fR  = t("GameData/Tex/Menu/BlueMenuR.png");
        fBL = t("GameData/Tex/Menu/BlueMenuBL.png");
        fB  = t("GameData/Tex/Menu/BlueMenuB.png");
        fBR = t("GameData/Tex/Menu/BlueMenuBR.png");
        barL    = t("GameData/Tex/Menu/BlueBarL.png");
        barTile = t("GameData/Tex/Menu/BlueBarTile.png");
        barWin  = t("GameData/Tex/Menu/BlueBarWin.png");
        barMax  = t("GameData/Tex/Menu/BlueBarMax.png");
        menuLine = t("GameData/Tex/Menu/BlueMenuLine.png");
        clientArea = t("GameData/Tex/Menu/BlueClientArea.png");
        btnNormal  = t("GameData/Tex/Title/menu_button.png");
        btnHover   = t("GameData/Tex/Title/menu_button_hover.png");
        btnClicked = t("GameData/Tex/Title/menu_button_clicked.png");
        loaded = true;
    }

    void Unload() {
        if (!loaded) return;
        auto u = [](Texture2D& t) { if (t.id) UnloadTexture(t); t = Texture2D{0}; };
        u(titleBg); u(heading); u(fTL); u(fT); u(fTR); u(fL); u(fArea); u(fR); u(fBL); u(fB); u(fBR);
        u(barL); u(barTile); u(barWin); u(barMax);
        u(menuLine); u(clientArea); u(btnNormal); u(btnHover); u(btnClicked);
        loaded = false;
    }
};

static std::vector<WorldEntry> ScanWorlds() {
    std::vector<WorldEntry> worlds;
    const char* candidates[] = {"GameData/Worlds", "../GameData/Worlds", "System/../GameData/Worlds"};
    for (auto base : candidates) {
        if (!fs::exists(base)) continue;
        for (auto& entry : fs::directory_iterator(base)) {
            if (!entry.is_directory()) continue;
            std::string dirName = entry.path().filename().string();
            if (dirName == "Legacy") continue;
            std::string oz = entry.path().string() + "/World.ozone";
            if (fs::exists(oz) && dirName != "." && dirName != "..") {
                bool dup = false;
                for (auto& w : worlds)
                    if (w.dirName == dirName) { dup = true; break; }
                if (!dup) worlds.push_back({dirName, dirName, {}, {}});
            }
        }
        if (!worlds.empty()) break;
    }
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
                    if (dot != std::string::npos) dirName.resize(dot);
                    bool dup = false;
                    for (auto& w : worlds)
                        if (w.dirName == dirName) { dup = true; break; }
                    if (!dup) worlds.push_back({dirName, dirName, {}, {}});
                }
            }
            if (!worlds.empty()) break;
        }
    }
    auto& manager = oz::game::GameManager::Instance();
    if (manager.Loaded()) {
        for (auto& w : worlds) {
            if (const oz::game::MissionDef* md = manager.FindMissionByWorld(w.dirName)) {
                w.name = md->name;
                w.mode = md->mode;
                w.sector = md->sector;
            }
        }
    }
    std::sort(worlds.begin(), worlds.end(), [](const WorldEntry& a, const WorldEntry& b) {
        return a.name < b.name;
    });
    return worlds;
}

class TitleMenu {
public:
    TitleMenu() {
        m_textures.Load();
        m_worlds = ScanWorlds();
        m_masters = master::LoadMasterUrls("System/Angels95.ini");
        m_internet.SetMasters(m_masters);
        m_browser.LoadState(kBrowserStatePath);
        m_filter.source = angelsmenu::ServerFilter::Lan;

        // The roster is normally loaded once in AngelTechInit; load it here too
        // so the menu is usable in a tool/test harness that skipped that block.
        if (!CharacterRoster::Instance().Loaded()) LoadRoster();

        LoadProfileFields();
        CharacterRoster& roster = CharacterRoster::Instance();
        const std::string id = roster.ResolveSelection(m_activeProfile.modelPath);
        if (!id.empty()) m_selectedCharacter = id;
        else if (!roster.All().empty()) m_selectedCharacter = roster.All().front().id;

        // Normalise the stored value: an empty/legacy path is replaced by the
        // resolved roster id, so the profile sent on the wire names a character
        // the server can relay to other clients.
        if (!m_selectedCharacter.empty() && m_activeProfile.modelPath != m_selectedCharacter) {
            m_activeProfile.modelPath = m_selectedCharacter;
            PlayerProfileManager::Instance().Update(m_profileSlot, m_activeProfile);
        }

        m_preview.SetShader(AngelTechData.Lights);
        m_lastInternetRefresh = GetTime();
    }

    ~TitleMenu() {
        m_internet.Cancel();
        if (m_lanScanning) m_lanDiscovery.stop();
        m_preview.Shutdown();
        m_textures.Unload();
    }

    // Developer capture (--menu-shot): open a page directly, bypassing Home.
    void SetStartPage(int page) {
        if (page < 0 || page >= (int)PageId::Count) return;
        m_page = (PageId)page;
        m_navFocus = page;
        m_lastPage = PageId::Count;
        if (m_page == PageId::Multiplayer) {
            m_multiTab = MultiTab::Join;
            m_filter.source = angelsmenu::ServerFilter::Lan;
            LanStart();
        }
    }

    bool Tick() {
        if (WindowShouldClose()) { m_exit = true; return true; }

        SoundManager::Instance().UpdateStream(AngelTechData.HomeScreenMusic);

        m_metrics = angelsmenu::MenuMetrics::Compute(
            (float)GetScreenWidth(), (float)GetScreenHeight(),
            GetWindowScaleDPI().y, UiScale);

        UpdateWindowDrag();
        PumpNetwork();
        UpdateToast(GetFrameTime());
        m_pageFade = std::min(1.0f, m_pageFade + GetFrameTime() * 6.0f);

        if (m_page != m_lastPage) {
            m_focus = Focus::None;
            m_capturingKey = false;
            m_lastPage = m_page;
        }

        m_suppressClick = false;
        HandleGlobalInput();
        // A modal owns all clicks. Pages still draw behind it, but their
        // immediate-mode buttons must not activate through the dim layer.
        if (m_showAbout || m_confirmDelete) m_suppressClick = true;

        // The live preview renders into its own render texture before the 2D
        // pass. Rendering it inside BeginDrawing would be legal but makes the
        // 3D/2D batch boundaries depend on page code.
        if (m_page == PageId::Character && !m_showAbout && !m_confirmDelete) {
            RenderCharacterPreview(GetFrameTime());
            // Warm every card here, not from inside DrawCharacterPage: a nested
            // BeginTextureMode inside the 2D pass is exactly the kind of state
            // leak this rewrite is removing.
            for (const auto& c : CharacterRoster::Instance().All())
                m_preview.Portrait(c);
        } else if (m_page == PageId::Home && !m_showAbout && !m_confirmDelete) {
            m_preview.Portrait(SelectedCharacter());
        }

        BeginDrawing();
        ClearBackground(BLACK);
        DrawBackground();
        DrawShell();
        DrawPage();
        if (m_pageFade < 0.999f) {
            const float a = (1.0f - m_pageFade) * 180.0f;
            DrawRectangleRec(R(m_metrics.content), (Color){2, 6, 18, (unsigned char)a});
        }
        DrawFooter();
        if (m_showAbout) DrawAboutModal();
        if (m_confirmDelete) DrawDeleteProfileModal();
        DrawToast();
        EndDrawing();

        return m_done || m_exit;
    }

    // --- Result interface used by PlayHomeScreen --------------------------
    bool ShouldExit() const { return m_exit; }
    bool ShouldQuit() const { return m_result == Result::Quit; }
    bool ShouldLoadGame() const {
        return m_result == Result::ContinueSession || m_result == Result::ContinueCampaign;
    }
    bool ShouldJoinServer() const { return m_result == Result::JoinServer; }
    bool ShouldStartServer() const { return m_result == Result::HostServer; }
    const char* GetCoopMission() const {
        return m_resultMission.empty() ? nullptr : m_resultMission.c_str();
    }
    const char* GetSelectedMission() const {
        return m_resultMission.empty() ? nullptr : m_resultMission.c_str();
    }
    const char* GetSelectedWorld() const {
        return m_resultWorld.empty() ? nullptr : m_resultWorld.c_str();
    }
    const char* GetJoinIP() const { return m_joinIp; }
    int GetJoinPort() const {
        if (m_result == Result::HostServer && m_resultPort > 0) return m_resultPort;
        return std::atoi(m_joinPort);
    }
    const char* GetHostServerName() const {
        return m_serverName[0] ? m_serverName : nullptr;
    }
    bool IsCampaignContinue() const { return m_result == Result::ContinueCampaign; }

private:
    enum class PageId : int { Home = 0, Play, Multiplayer, Character, Settings, Count };
    enum class PlayTab : int { Campaign = 0, Practice };
    enum class MultiTab : int { Join = 0, Host, Coop };
    enum class SettingsTab : int { Video = 0, Audio, Controls, Gameplay, Window };
    enum class Result : int {
        None = 0, PlayWorld, ContinueSession, ContinueCampaign,
        JoinServer, HostServer, Quit
    };
    enum class Focus : int {
        None = 0, JoinIp, JoinPort, HostPort, ServerName, ProfileName, Search
    };

    static constexpr const char* kBrowserStatePath = "System/ServerBrowser.ini";
    static constexpr int kNavCount = 5;

    // --- textures/theme ---------------------------------------------------
    TitleMenuTextures m_textures;
    angelsmenu::MenuMetrics m_metrics;

    // --- navigation/state -------------------------------------------------
    PageId m_page = PageId::Home;
    PageId m_lastPage = PageId::Count;
    PlayTab m_playTab = PlayTab::Campaign;
    MultiTab m_multiTab = MultiTab::Join;
    SettingsTab m_settingsTab = SettingsTab::Video;
    int m_navHover = -1;
    int m_navFocus = 0;
    float m_pageFade = 1.0f;
    bool m_done = false;
    bool m_exit = false;
    Result m_result = Result::None;
    std::string m_resultWorld;
    std::string m_resultMission;
    int m_resultPort = 0;
    std::string m_error;
    std::string m_toast;
    float m_toastTimer = 0.0f;
    bool m_showAbout = false;
    bool m_confirmDelete = false;
    bool m_suppressClick = false;
    Focus m_focus = Focus::None;
    bool m_capturingKey = false;
    int m_captureAction = -1;
    int m_captureArmFrames = 0;

    // --- profile/character ------------------------------------------------
    PlayerProfile m_activeProfile;
    std::string m_selectedCharacter;
    char m_profileName[64] = "";
    int m_profileColor = 0;
    int m_profileSlot = 0;
    float m_previewYaw = 0.0f;
    float m_previewSpin = 0.0f;
    bool m_previewDragging = false;
    angelsmenu::CharacterPreview m_preview;
    Texture2D m_livePreviewTex{0};

    // --- worlds -----------------------------------------------------------
    std::vector<WorldEntry> m_worlds;
    int m_selectedWorld = -1;
    int m_hostWorld = 0;
    int m_worldScroll = 0;
    std::string m_selectedMission;

    // --- multiplayer ------------------------------------------------------
    char m_joinIp[64] = "127.0.0.1";
    char m_joinPort[16] = "27015";
    char m_hostPort[16] = "27015";
    char m_serverName[64] = "Angels95 Server";
    std::vector<std::string> m_masters;
    master::InternetBrowser m_internet;
    net::NetworkDiscovery m_lanDiscovery;
    bool m_lanScanning = false;
    double m_lastLanProbe = 0.0;
    std::vector<net::DiscoveredServer> m_lanServers;
    angelsmenu::ServerBrowserModel m_browser;
    angelsmenu::ServerFilter m_filter;
    char m_search[64] = "";
    int m_serverScroll = 0;
    int m_selectedServer = -1;
    std::string m_selectedServerKey;
    double m_lastInternetRefresh = 0.0;
    double m_lastServerClick = 0.0;
    int m_lastServerClickIndex = -1;
    bool m_internetAutoScanned = false;

    // --- window chrome ----------------------------------------------------
    bool m_draggingWindow = false;
    Vector2 m_dragStart{0};

    // ---------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------
    static std::string Trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    int FS(float base) const { return (int)std::round(base * m_metrics.scale); }

    static Rectangle R(const angelsmenu::UiRect& r) {
        return {r.x, r.y, r.w, r.h};
    }

    static angelsmenu::UiRect U(const Rectangle& r) {
        return {r.x, r.y, r.width, r.height};
    }

    bool MouseIn(angelsmenu::UiRect r) const {
        return r.Contains(GetMousePosition().x, GetMousePosition().y);
    }

    bool Clickable() const { return !m_suppressClick && !m_capturingKey; }

    void Toast(const std::string& msg) { m_toast = msg; m_toastTimer = 3.0f; }

    void UpdateToast(float dt) {
        if (m_toastTimer > 0.0f) {
            m_toastTimer -= dt;
            if (m_toastTimer <= 0.0f) m_toast.clear();
        }
    }

    // A page button. Uses the same palette as the in-game settings panel.
    bool Button(angelsmenu::UiRect r, const char* label, bool active = false,
                bool enabled = true, int size = 0) {
        const Rectangle rr = R(r);
        const bool hover = enabled && MouseIn({r.x, r.y, r.w, r.h});
        const Color bg = !enabled ? (Color){14, 20, 34, 200}
                        : active ? angelsui::kBtnActive
                        : hover ? angelsui::kBtnHover
                                : angelsui::kBtnNormal;
        DrawRectangleRec(rr, bg);
        DrawRectangleLinesEx(rr, 1, enabled ? (active ? angelsui::kEdge : angelsui::kBorder)
                                            : angelsui::kBorderDim);
        const int fs = size > 0 ? size : FS(13);
        const int tw = MeasureText(label, fs);
        DrawText(label, (int)(r.x + (r.w - tw) * 0.5f),
                 (int)(r.y + (r.h - fs) * 0.5f), fs,
                 enabled ? (active || hover ? angelsui::kText : angelsui::kTextSoft)
                         : angelsui::kTextDim);
        if (hover && enabled && Clickable() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            m_focus = Focus::None;
            SoundManager::Instance().PlayUIClick();
            return true;
        }
        return false;
    }

    bool TextField(Rectangle r, char* buf, size_t cap, Focus focus, int size = 0) {
        if (m_showAbout || m_confirmDelete) {
            // A modal dims the page; do not let a click-through focus a text
            // box behind it. Draw the value read-only instead of calling
            // raygui, which would still process the click.
            DrawRectangleRec(r, angelsui::kBtnNormal);
            DrawRectangleLinesEx(r, 1, angelsui::kBorderDim);
            DrawText(buf, (int)(r.x + 6), (int)(r.y + 5), FS(12), angelsui::kTextDim);
            return false;
        }
        const bool active = (m_focus == focus);
        if (GuiTextBox(r, buf, (int)cap, active)) {
            if (active) m_focus = Focus::None;
            else {
                m_focus = focus;
                m_capturingKey = false;
            }
        }
        // Draw a focus underline; raygui's own focus state is a shared cursor.
        if (active)
            DrawRectangle((int)r.x, (int)(r.y + r.height), (int)r.width, 2,
                          angelsui::kEdge);
        (void)size;
        return active;
    }

    std::string FitText(const std::string& text, int fontSize, float maxW) const {
        if (maxW <= 0.0f) return {};
        if (MeasureText(text.c_str(), fontSize) <= (int)maxW) return text;
        std::string out = text;
        while (!out.empty() &&
               MeasureText((out + "...").c_str(), fontSize) > (int)maxW)
            out.pop_back();
        return out + "...";
    }

    void Label(const char* text, float x, float y, int size, Color color) {
        DrawText(text, (int)x, (int)y, size, color);
    }

    void SectionHeader(const char* text, Rectangle r) {
        Label(text, r.x, r.y, FS(16), angelsui::kText);
        DrawLine((int)r.x, (int)(r.y + FS(22)), (int)(r.x + r.width),
                 (int)(r.y + FS(22)), (Color){60, 100, 160, 150});
    }

    void Card(angelsmenu::UiRect r, bool selected = false) {
        const Rectangle rr = R(r);
        DrawRectangleRec(rr, selected ? (Color){24, 46, 84, 235}
                                      : (Color){10, 18, 36, 225});
        DrawRectangleLinesEx(rr, selected ? 2.0f : 1.0f,
                             selected ? angelsui::kEdge : angelsui::kBorderDim);
    }
    void Card(Rectangle r, bool selected = false) { Card(U(r), selected); }

    void ScrollList(angelsmenu::UiRect area, int contentRows, int rowH, int& scroll,
                    int& selected) {
        (void)selected;
        const int visible = std::max(1, (int)(area.h / (float)rowH));
        const int maxScroll = std::max(0, contentRows - visible);
        if (MouseIn(area)) {
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) scroll -= (int)wheel;
        }
        scroll = std::clamp(scroll, 0, maxScroll);
        // A smaller scrollbar on the right edge. It is intentionally decorative
        // (the wheel and keyboard own the value) but makes overflow discoverable.
        if (maxScroll > 0) {
            const float trackH = area.h - 4.0f;
            const float thumbH = std::max(18.0f, trackH * (float)visible / (float)contentRows);
            const float t = (float)scroll / (float)maxScroll;
            const float thumbY = area.y + 2.0f + (trackH - thumbH) * t;
            DrawRectangle((int)(area.Right() - 6.0f), (int)(area.y + 2.0f), 3,
                          (int)trackH, (Color){20, 32, 54, 200});
            DrawRectangle((int)(area.Right() - 6.0f), (int)thumbY, 3,
                          (int)thumbH, angelsui::kBorder);
        }
    }

    // ---------------------------------------------------------------------
    // Profile / roster
    // ---------------------------------------------------------------------
    void LoadRoster() {
        std::string text = LoadFileTextWithFallback("GameData/Pawn/Player/characters.cfg");
        CharacterRoster& roster = CharacterRoster::Instance();
        roster.LoadFromText(text);
        for (const auto& w : roster.Warnings())
            OZ_WARN("CharacterRoster: %s", w.c_str());
        if (!roster.Loaded())
            OZ_WARN("CharacterRoster: no characters.cfg entries; using the Player def");
    }

    void LoadProfileFields() {
        m_activeProfile = PlayerProfileManager::Instance().Active();
        m_profileSlot = PlayerProfileManager::Instance().ActiveIndex();
        std::snprintf(m_profileName, sizeof(m_profileName), "%s",
                      m_activeProfile.displayName.c_str());
        m_profileColor = std::clamp(m_activeProfile.team, 0, TEAM_COLOR_COUNT - 1);
    }

    void SaveProfileFields() {
        PlayerProfile p = m_activeProfile;
        std::string name = Trim(m_profileName);
        if (name.empty()) name = "Player";
        // Match the server's display-name rules so the local list and the
        // scoreboard cannot disagree about control characters/whitespace.
        name = net::SanitizeDisplayName(name, 63);
        if (name.empty()) name = "Player";
        p.displayName = name;
        p.modelPath = m_selectedCharacter;
        p.team = std::clamp(m_profileColor, 0, TEAM_COLOR_COUNT - 1);
        p.voiceSet.clear();   // no voice content exists; don't ship a dead field
        PlayerProfileManager::Instance().Update(m_profileSlot, p);
        m_activeProfile = p;
        std::snprintf(m_profileName, sizeof(m_profileName), "%s", p.displayName.c_str());
    }

    const CharacterDef& SelectedCharacter() const {
        CharacterRoster& roster = CharacterRoster::Instance();
        if (const CharacterDef* c = roster.Find(m_selectedCharacter)) return *c;
        return roster.Default();
    }

    void SelectCharacter(const std::string& id) {
        m_selectedCharacter = id;
        m_activeProfile.modelPath = id;
        PlayerProfileManager::Instance().Update(m_profileSlot, m_activeProfile);
        m_previewYaw = 0.0f;
    }

    void RenderCharacterPreview(float dt) {
        if (!m_previewDragging) m_previewSpin += dt * 12.0f;
        if (m_previewDragging) m_previewYaw += GetMouseDelta().x * 0.4f;
        else m_previewYaw = m_previewSpin;
        const CharacterDef& def = SelectedCharacter();
        angelsmenu::UiRect body = m_metrics.PageBody();
        angelsmenu::UiRect preview = PreviewRect(body);
        m_livePreviewTex = m_preview.LivePreview(
            def, {preview.x, preview.y, preview.w, preview.h}, m_previewYaw, dt);
    }

    angelsmenu::UiRect PreviewRect(const angelsmenu::UiRect& body) const {
        if (body.w >= FS(700)) {
            return {body.x, body.y, body.w * 0.56f, body.h};
        }
        return {body.x, body.y, body.w, body.h * 0.46f};
    }

    // ---------------------------------------------------------------------
    // Worlds / save discovery
    // ---------------------------------------------------------------------
    const WorldEntry* FindWorld(const std::string& dir) const {
        for (const auto& w : m_worlds)
            if (w.dirName == dir) return &w;
        return nullptr;
    }

    std::string ReadCampaignMission() const {
        std::ifstream f("GameData/Saves/Campaign.sav");
        if (!f) return {};
        std::string line, firstLegacy;
        while (std::getline(f, line)) {
            line = Trim(line);
            if (line.empty()) continue;
            if (line.rfind("mission=", 0) == 0) return Trim(line.substr(8));
            if (firstLegacy.empty() && line.find('=') == std::string::npos)
                firstLegacy = line;
        }
        // Legacy format: the whole file is the mission name.
        return firstLegacy;
    }

    std::string ReadSavedWorld() const {
        std::ifstream f("GameData/Saves/POS.sav");
        if (!f) return {};
        std::string line;
        std::getline(f, line);
        // x:y:z:level:world: -- the world field was appended after level.
        std::vector<std::string> fields;
        std::string cur;
        for (char c : line) {
            if (c == ':') { fields.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        fields.push_back(cur);
        if (fields.size() >= 5) return Trim(fields[4]);
        return {};
    }

    bool ContinueInfo(std::string& world, std::string& mission, bool& campaign) const {
        campaign = false;
        mission = ReadCampaignMission();
        auto& gm = oz::game::GameManager::Instance();
        if (!mission.empty() && gm.Loaded()) {
            if (const oz::game::MissionDef* md = gm.FindMission(mission)) {
                world = md->world;
                campaign = !world.empty();
                if (campaign) return true;
            }
        }
        world = ReadSavedWorld();
        if (!world.empty() && FindWorld(world)) return true;
        return false;
    }

    // ---------------------------------------------------------------------
    // Network/server browser
    // ---------------------------------------------------------------------
    void LanStart() {
        if (!m_lanDiscovery.is_running())
            m_lanDiscovery.init("Angels95", "0.2.1", 0);
        m_lanDiscovery.start();
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
        const double now = GetTime();
        if (now - m_lastLanProbe > 1.5) {
            m_lanDiscovery.send_request();
            m_lastLanProbe = now;
        }
        m_lanDiscovery.poll_discovered(m_lanServers);
    }

    void PumpNetwork() {
        const bool serverPage = (m_page == PageId::Multiplayer);
        if (serverPage) {
            LanUpdate();
            m_internet.Update();
            // Auto-refresh the internet list while the page is open, but never
            // while a scan is already running.
            if (m_filter.source != angelsmenu::ServerFilter::Lan &&
                !m_internet.IsScanning() &&
                GetTime() - m_lastInternetRefresh > 30.0) {
                RefreshInternet();
            }
            RebuildServerRows();
        } else {
            if (m_lanScanning) LanStop();
            m_internet.Cancel();
            m_internetAutoScanned = false;
        }
    }

    void RefreshInternet() {
        m_lastInternetRefresh = GetTime();
        if (m_internet.IsScanning()) return;
        m_internet.SetMasters(m_masters);
        m_internet.Refresh();
    }

    void RebuildServerRows() {
        std::vector<angelsmenu::ServerRow> lan;
        lan.reserve(m_lanServers.size());
        for (const auto& s : m_lanServers) {
            angelsmenu::ServerRow r;
            r.key = {s.ip, s.port};
            r.name = s.name;
            r.map = s.map;
            r.version = s.version;
            r.players = (int)s.cur_players;
            r.maxPlayers = (int)s.max_players;
            r.password = s.password;
            r.lan = true;
            r.live = s.cur_players > 0;
            lan.push_back(std::move(r));
        }
        std::vector<angelsmenu::ServerRow> net;
        net.reserve(m_internet.Servers().size());
        for (const auto& os : m_internet.Servers()) {
            angelsmenu::ServerRow r;
            r.key = {os.entry.ip, os.entry.port};
            r.name = os.entry.name.empty() ? "server" : os.entry.name;
            r.map = os.entry.map;
            r.version = os.entry.gamever;
            r.protocol = (int)os.entry.protocol;
            r.players = (int)os.entry.players;
            r.maxPlayers = (int)os.entry.maxplayers;
            r.pingMs = os.ping_ms;
            r.password = os.entry.password;
            r.internet = true;
            r.live = os.live;
            net.push_back(std::move(r));
        }
        m_browser.SetLan(std::move(lan));
        m_browser.SetInternet(std::move(net));

        // Rows are rebuilt every frame while the page is open, so an index is
        // not a stable selection. Re-find by ip:port; if the server vanished,
        // drop the selection instead of silently pointing at a different row.
        m_selectedServer = -1;
        if (!m_selectedServerKey.empty()) {
            for (int i = 0; i < (int)m_browser.Rows().size(); ++i) {
                if (angelsmenu::ServerBrowserModel::KeyString(m_browser.Rows()[i].key)
                        == m_selectedServerKey) {
                    m_selectedServer = i;
                    break;
                }
            }
            if (m_selectedServer < 0) m_selectedServerKey.clear();
        }
    }

    const angelsmenu::ServerRow* SelectedServerRow() const {
        if (m_selectedServer < 0 || m_selectedServer >= (int)m_browser.Rows().size())
            return nullptr;
        return &m_browser.Rows()[m_selectedServer];
    }

    void ConnectToSelectedOrTyped() {
        const angelsmenu::ServerRow* row = SelectedServerRow();
        std::string ip = row ? row->key.ip : m_joinIp;
        int port = row ? row->key.port : std::atoi(m_joinPort);
        if (ip.empty() || port <= 0 || port > 65535) {
            m_error = "Enter a valid server address and port.";
            return;
        }
        // A non-zero protocol that differs is a hard wire mismatch: the server
        // drops CLIENT_AUTH and the player only sees a generic timeout. Say
        // why instead of letting them retry forever. 0 = legacy/unknown
        // (a master that predates the `proto` field) and is still allowed.
        if (row && row->protocol > 0 &&
            row->protocol != (int)net::PROTOCOL_VERSION) {
            m_error = "Server " + (row->version.empty() ? std::string("(unknown build)")
                                                       : row->version) +
                      " speaks protocol " + std::to_string(row->protocol) +
                      "; this client speaks " +
                      std::to_string((int)net::PROTOCOL_VERSION) + ".";
            return;
        }
        std::string host;
        if (!JoinUri::ParseHostPort(ip, host, port, port) || host.empty()) {
            m_error = "That address is not a valid host.";
            return;
        }
        std::snprintf(m_joinIp, sizeof(m_joinIp), "%s", host.c_str());
        std::snprintf(m_joinPort, sizeof(m_joinPort), "%d", port);
        if (row) m_browser.AddRecent(row->key);
        m_browser.SaveState(kBrowserStatePath);
        m_result = Result::JoinServer;
        m_resultPort = port;
        m_done = true;
    }

    void ApplyFilterFromUI() {
        std::snprintf(m_search, sizeof(m_search), "%s", m_filter.query.c_str());
    }

    void SyncFilterFromText() {
        m_filter.query = m_search;
    }

    // ---------------------------------------------------------------------
    // Window chrome / global input
    // ---------------------------------------------------------------------
    angelsmenu::UiRect CloseBtnRect() const {
        return {m_metrics.titleBar.Right() - FS(28), m_metrics.titleBar.y + FS(8),
                FS(18), FS(18)};
    }
    angelsmenu::UiRect MaxBtnRect() const {
        return {CloseBtnRect().x - FS(22), CloseBtnRect().y, FS(18), FS(18)};
    }
    angelsmenu::UiRect MinBtnRect() const {
        return {MaxBtnRect().x - FS(22), CloseBtnRect().y, FS(18), FS(18)};
    }

    void UpdateWindowDrag() {
        if (!m_draggingWindow) return;
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) { m_draggingWindow = false; return; }
        Vector2 mp = GetMousePosition();
        Vector2 d = {mp.x - m_dragStart.x, mp.y - m_dragStart.y};
        if (d.x != 0 || d.y != 0) {
#ifdef _WIN32
            RECT r; GetWindowRect(GetActiveWindow(), &r);
            SetWindowPos(GetActiveWindow(), 0, r.left + (int)d.x, r.top + (int)d.y,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER);
#endif
            m_dragStart = mp;
        }
    }

    void HandleGlobalInput() {
        Vector2 mp = GetMousePosition();

        // Window chrome always wins over page content.
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (MouseIn({CloseBtnRect().x, CloseBtnRect().y, CloseBtnRect().w, CloseBtnRect().h})) {
                m_suppressClick = true;
                m_exit = true;
                g_quitRequested = true;
                return;
            }
            if (MouseIn({MaxBtnRect().x, MaxBtnRect().y, MaxBtnRect().w, MaxBtnRect().h})) {
                m_suppressClick = true;
#ifdef _WIN32
                ShowWindow(GetActiveWindow(), SW_MAXIMIZE);
#else
                ToggleFullscreen();
#endif
                return;
            }
            if (MouseIn({MinBtnRect().x, MinBtnRect().y, MinBtnRect().w, MinBtnRect().h})) {
                m_suppressClick = true;
#ifdef _WIN32
                ShowWindow(GetActiveWindow(), SW_MINIMIZE);
#endif
                return;
            }
            if (MouseIn({m_metrics.titleBar.x, m_metrics.titleBar.y,
                         m_metrics.titleBar.w, m_metrics.titleBar.h})) {
                m_draggingWindow = true;
                m_dragStart = mp;
                m_suppressClick = true;
                return;
            }
        }

        // Escape: modal first, then one page back, then release text focus.
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (m_showAbout) { m_showAbout = false; m_suppressClick = true; return; }
            if (m_confirmDelete) { m_confirmDelete = false; m_suppressClick = true; return; }
            if (m_capturingKey) { m_capturingKey = false; m_suppressClick = true; return; }
            if (m_focus != Focus::None) { m_focus = Focus::None; m_suppressClick = true; return; }
            if (m_page != PageId::Home) { m_page = PageId::Home; m_pageFade = 0.0f; m_suppressClick = true; return; }
        }

        // F1 = About. F5 = refresh servers when the browser is visible.
        if (IsKeyPressed(KEY_F1)) { m_showAbout = true; return; }
        if (IsKeyPressed(KEY_F5) && m_page == PageId::Multiplayer) {
            RefreshInternet();
            LanStart();
            return;
        }

        // Modal input is handled by the modal draw/close paths. Everything
        // below (nav, page focus) must not run through the dim layer.
        if (m_showAbout || m_confirmDelete) return;

        // Text/keybind capture owns the keyboard. Navigation must not steal
        // Enter/arrows while the user is typing or rebinding.
        if (m_focus != Focus::None || m_capturingKey) return;

        // Nav clicks.
        for (int i = 0; i < kNavCount; ++i) {
            auto r = m_metrics.NavItem(i, kNavCount);
            if (MouseIn(r)) m_navHover = i;
            if (MouseIn(r) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                m_navFocus = i;
                m_page = (PageId)i;
                m_pageFade = 0.0f;
                m_suppressClick = true;
                SoundManager::Instance().PlayUIClick();
                return;
            }
        }
        // Left/right walks the nav bar; Enter opens it.
        if (IsKeyPressed(KEY_LEFT)) { m_navFocus = (m_navFocus + kNavCount - 1) % kNavCount; }
        if (IsKeyPressed(KEY_RIGHT)) { m_navFocus = (m_navFocus + 1) % kNavCount; }
        if (IsKeyPressed(KEY_ENTER)) {
            m_page = (PageId)m_navFocus;
            m_pageFade = 0.0f;
        }
    }

    // ---------------------------------------------------------------------
    // Drawing shell
    // ---------------------------------------------------------------------
    void DrawBackground() {
        auto& tex = m_textures;
        ray_video_t& vid = AngelTechData.HomeScreenVideo;
        if (vid.ok && vid.texture.id != 0) {
            ray_video_update(&vid, GetFrameTime());
            if (vid.texture.id != 0) {
                const float vw = (float)vid.texture.width;
                const float vh = (float)vid.texture.height;
                const float s = std::max((float)GetScreenWidth() / vw,
                                         (float)GetScreenHeight() / vh);
                const float dw = vw * s, dh = vh * s;
                DrawTexturePro(vid.texture, {0, 0, vw, vh},
                               {(GetScreenWidth() - dw) * 0.5f,
                                (GetScreenHeight() - dh) * 0.5f, dw, dh},
                               {0, 0}, 0.0f, WHITE);
            }
        } else if (vid.ok) {
            ray_video_update(&vid, GetFrameTime());
        } else if (tex.titleBg.id) {
            for (int x = 0; x < GetScreenWidth(); x += tex.titleBg.width)
                for (int y = 0; y < GetScreenHeight(); y += tex.titleBg.height)
                    DrawTexture(tex.titleBg, x, y, WHITE);
        } else {
            DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(),
                                   (Color){8, 14, 34, 255}, (Color){2, 4, 10, 255});
        }
        // Darken the video so text is readable, and tint the whole screen a
        // little toward the menu navy.
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){2, 6, 18, 110});
    }

    void DrawFrameEdges() {
        auto& t = m_textures;
        const angelsmenu::UiRect f = m_metrics.frame;
        const float e = std::max(4.0f, std::round(8.0f * m_metrics.scale));
        auto d = [&](Texture2D tex, Rectangle src, Rectangle dst) {
            if (tex.id) DrawTexturePro(tex, src, dst, {0, 0}, 0, WHITE);
        };
        // Fill the client area with a translucent panel first; the old code drew
        // the opaque BlueMenuArea over it, which hid the title video entirely.
        DrawRectangleRec({f.x + e, f.y + e, f.w - e * 2, f.h - e * 2},
                         (Color){4, 8, 20, 190});
        d(t.fTL, {0, 0, (float)t.fTL.width, (float)t.fTL.height}, {f.x, f.y, e, e});
        d(t.fTR, {0, 0, (float)t.fTR.width, (float)t.fTR.height}, {f.Right() - e, f.y, e, e});
        d(t.fBL, {0, 0, (float)t.fBL.width, (float)t.fBL.height}, {f.x, f.Bottom() - e, e, e});
        d(t.fBR, {0, 0, (float)t.fBR.width, (float)t.fBR.height}, {f.Right() - e, f.Bottom() - e, e, e});
        d(t.fT, {0, 0, (float)t.fT.width, (float)t.fT.height}, {f.x + e, f.y, f.w - e * 2, e});
        d(t.fB, {0, 0, (float)t.fB.width, (float)t.fB.height}, {f.x + e, f.Bottom() - e, f.w - e * 2, e});
        d(t.fL, {0, 0, (float)t.fL.width, (float)t.fL.height}, {f.x, f.y + e, e, f.h - e * 2});
        d(t.fR, {0, 0, (float)t.fR.width, (float)t.fR.height}, {f.Right() - e, f.y + e, e, f.h - e * 2});
        DrawRectangleLinesEx({f.x, f.y, f.w, f.h}, 1, angelsui::kBorder);
    }

    void DrawShell() {
        DrawFrameEdges();
        DrawTitleBar();
        DrawNavBar();
    }

    void DrawTitleBar() {
        const angelsmenu::UiRect tb = m_metrics.titleBar;
        DrawRectangleRec({tb.x, tb.y, tb.w, tb.h}, (Color){0, 16, 48, 235});
        DrawLine((int)tb.x, (int)tb.Bottom(), (int)tb.Right(), (int)tb.Bottom(),
                 angelsui::kBorder);

        // Branding: the title art if present, otherwise a clean wordmark.
        int tx = (int)(tb.x + FS(14));
        const int ty = (int)(tb.y + (tb.h - FS(18)) * 0.5f);
        if (m_textures.titleBg.id) {
            DrawText("ANGELS95", tx, ty, FS(18), angelsui::kText);
            tx += MeasureText("ANGELS95", FS(18)) + FS(10);
            DrawText("// ANGELTECH", tx, ty + FS(2), FS(10),
                     (Color){120, 160, 200, 210});
        } else {
            DrawText("ANGELS95", tx, ty, FS(18), angelsui::kText);
        }

        const char* profile = PlayerProfileManager::Instance().Active().displayName.c_str();
        const std::string profileLabel = FitText(profile, FS(12),
            (float)MinBtnRect().x - (float)tx - FS(20));
        int pw = MeasureText(profileLabel.c_str(), FS(12));
        DrawText(profileLabel.c_str(), (int)(MinBtnRect().x - pw - FS(16)), ty + FS(2),
                 FS(12), angelsui::kTextDim);

        DrawWinButton(MinBtnRect(), 0);
        DrawWinButton(MaxBtnRect(), 1);
        DrawWinButton(CloseBtnRect(), 2);
    }

    void DrawWinButton(angelsmenu::UiRect r, int type) {
        const bool hover = MouseIn(r);
        Color bg = hover ? angelsui::kBtnHover : angelsui::kBtnNormal;
        if (type == 2) bg = hover ? angelsui::kCloseHover : angelsui::kClose;
        DrawRectangleRec({r.x, r.y, r.w, r.h}, bg);
        DrawRectangleLinesEx({r.x, r.y, r.w, r.h}, 1, angelsui::kBorder);
        const int cx = (int)r.CenterX(), cy = (int)r.CenterY();
        if (type == 0) DrawRectangle(cx - FS(4), cy, FS(8), std::max(1, FS(1)), angelsui::kText);
        else if (type == 1) DrawRectangleLines(cx - FS(4), cy - FS(3), FS(8), FS(6), angelsui::kText);
        else {
            DrawLine(cx - FS(3), cy - FS(3), cx + FS(3), cy + FS(3), angelsui::kText);
            DrawLine(cx + FS(3), cy - FS(3), cx - FS(3), cy + FS(3), angelsui::kText);
        }
    }

    void DrawNavBar() {
        const angelsmenu::UiRect nb = m_metrics.navBar;
        DrawRectangleRec({nb.x, nb.y, nb.w, nb.h}, (Color){8, 16, 40, 235});
        static const char* names[kNavCount] = {
            "Home", "Play", "Multiplayer", "Character", "Settings"
        };
        for (int i = 0; i < kNavCount; ++i) {
            auto r = m_metrics.NavItem(i, kNavCount);
            const bool active = ((int)m_page == i);
            const bool hover = (i == m_navHover);
            if (active) DrawRectangleRec({r.x, r.y, r.w, r.h}, (Color){30, 60, 100, 255});
            else if (hover) DrawRectangleRec({r.x, r.y, r.w, r.h}, (Color){20, 40, 70, 255});
            if (active) DrawRectangle((int)r.x, (int)r.Bottom() - 2, (int)r.w, 2,
                                      angelsui::kEdge);
            const int fs = FS(13);
            const char* label = (m_metrics.compact && std::strcmp(names[i], "Multiplayer") == 0)
                                ? "Online" : names[i];
            const int tw = MeasureText(label, fs);
            DrawText(label, (int)(r.x + (r.w - tw) * 0.5f),
                     (int)(r.y + (r.h - fs) * 0.5f), fs,
                     active ? WHITE : (hover ? angelsui::kText : angelsui::kTextSoft));
        }
        DrawLine((int)nb.x, (int)nb.Bottom(), (int)nb.Right(), (int)nb.Bottom(),
                 (Color){60, 100, 160, 120});
    }

    void DrawFooter() {
        const angelsmenu::UiRect fb = m_metrics.footer;
        DrawRectangleRec({fb.x, fb.y, fb.w, fb.h}, (Color){6, 12, 32, 230});
        DrawLine((int)fb.x, (int)fb.y, (int)fb.Right(), (int)fb.y,
                 (Color){60, 100, 160, 120});
        const char* help = ContextHelp();
        DrawText(help, (int)(fb.x + FS(12)),
                 (int)(fb.y + (fb.h - FS(11)) * 0.5f), FS(11),
                 (Color){150, 180, 210, 220});
        const char* version = TextFormat("%s  |  F1 About  |  Esc Back",
                                         oz::kBuildVersion);
        int vw = MeasureText(version, FS(10));
        DrawText(version, (int)(fb.Right() - vw - FS(12)),
                 (int)(fb.y + (fb.h - FS(10)) * 0.5f), FS(10),
                 (Color){110, 135, 165, 210});
    }

    const char* ContextHelp() const {
        switch (m_page) {
            case PageId::Home: return "Continue a save, start a campaign, or jump into co-op.";
            case PageId::Play: return m_playTab == PlayTab::Campaign
                ? "Select a campaign mission, then Launch." : "Select any world, then Load World.";
            case PageId::Multiplayer: return m_multiTab == MultiTab::Join
                ? "Pick a server, or type an address, then Join." : (m_multiTab == MultiTab::Host
                    ? "Choose a world and port, then Start Server." : "Host a mission or join a co-op game.");
            case PageId::Character: return "Choose a character and colour. Cosmetics are visible to other players in co-op.";
            case PageId::Settings: return "Player settings. Developer settings live in the in-game panel.";
            default: return "";
        }
    }

    // ---------------------------------------------------------------------
    // Page dispatch
    // ---------------------------------------------------------------------
    void DrawPage() {
        const angelsmenu::UiRect body = m_metrics.PageBody();
        switch (m_page) {
            case PageId::Home:        DrawHomePage(body); break;
            case PageId::Play:        DrawPlayPage(body); break;
            case PageId::Multiplayer: DrawMultiplayerPage(body); break;
            case PageId::Character:   DrawCharacterPage(body); break;
            case PageId::Settings:    DrawSettingsPage(body); break;
            default: break;
        }
    }

    // ---------------------------------------------------------------------
    // Home
    // ---------------------------------------------------------------------
    void DrawHomePage(angelsmenu::UiRect body) {
        const bool wide = m_metrics.wide;
        const float leftW = wide ? body.w * 0.52f : body.w;
        const float x = body.x;
        float y = body.y + FS(8);

        if (m_textures.heading.id) {
            const float hh = FS(74);
            const float hw = hh * (float)m_textures.heading.width /
                             (float)m_textures.heading.height;
            DrawTexturePro(m_textures.heading,
                           {0, 0, (float)m_textures.heading.width,
                            (float)m_textures.heading.height},
                           {x, y - FS(10), hw, hh}, {0, 0}, 0.0f, WHITE);
            y += FS(70);
        } else {
            DrawText("ANGELS95", (int)x, (int)y, FS(34), angelsui::kText);
            y += FS(42);
        }
        DrawText("Angels95 //  co-op campaign and skirmish",
                 (int)x, (int)y, FS(13), angelsui::kTextDim);
        y += FS(28);

        std::string cWorld, cMission;
        bool cCampaign = false;
        const bool hasContinue = ContinueInfo(cWorld, cMission, cCampaign);
        const float bw = std::min(leftW - FS(20), (float)FS(360));
        const float bh = FS(38);
        const float gap = FS(8);

        if (hasContinue) {
            std::string contLabel = cCampaign
                ? std::string("Continue Campaign  -  ") + cMission
                : std::string("Continue  -  ") + cWorld;
            contLabel = FitText(contLabel, FS(13), bw - FS(16));
            if (Button({x, y, bw, bh}, contLabel.c_str(), true)) {
                m_result = cCampaign ? Result::ContinueCampaign : Result::ContinueSession;
                m_resultWorld = cWorld;
                m_resultMission = cMission;
                m_done = true;
            }
            y += bh + gap;
        }

        if (Button({x, y, bw, bh}, "New Campaign")) {
            auto& gm = oz::game::GameManager::Instance();
            if (!gm.Loaded()) {
                m_error = "Campaign hierarchy is not loaded (GameData/Game).";
            } else {
                const std::string firstWorld = gm.FirstMissionWorld();
                const oz::game::MissionDef* first = gm.FindMissionByWorld(firstWorld);
                if (firstWorld.empty() || !first) {
                    m_error = "No campaign starting mission is defined.";
                } else {
                    m_result = Result::PlayWorld;
                    m_resultWorld = firstWorld;
                    m_resultMission = first->name;
                    m_done = true;
                }
            }
        }
        y += bh + gap;

        if (Button({x, y, bw, bh}, "Multiplayer")) {
            m_page = PageId::Multiplayer; m_multiTab = MultiTab::Join; m_pageFade = 0.0f;
        }
        y += bh + gap;
        if (Button({x, y, bw, bh}, "Co-op Campaign")) {
            m_page = PageId::Multiplayer; m_multiTab = MultiTab::Coop; m_pageFade = 0.0f;
        }
        y += bh + gap;
        if (Button({x, y, bw, bh}, "Character")) {
            m_page = PageId::Character; m_pageFade = 0.0f;
        }
        y += bh + gap;
        if (Button({x, y, bw, bh}, "Settings")) {
            m_page = PageId::Settings; m_pageFade = 0.0f;
        }
        y += bh + gap;
        if (Button({x, y, bw, bh}, "Quit", false, true)) {
            m_result = Result::Quit;
            m_done = true;
        }

        if (!m_error.empty()) {
            DrawText(m_error.c_str(), (int)x, (int)(y + bh + FS(10)), FS(11),
                     (Color){240, 150, 120, 230});
        }

        if (wide) {
            const CharacterDef& def = SelectedCharacter();
            angelsmenu::UiRect card = {body.x + body.w * 0.58f, body.y,
                                       body.w * 0.42f, body.h};
            Card(card, true);
            const float pad = FS(12);
            const float portraitW = card.w - pad * 2;
            const float portraitH = card.h - FS(96);
            m_preview.DrawPortrait(def, {card.x + pad, card.y + pad,
                                         portraitW, portraitH});
            const int nameFs = FS(20);
            DrawText(def.name.c_str(), (int)(card.x + pad),
                     (int)(card.y + card.h - FS(66)), nameFs, angelsui::kText);
            DrawText(PlayerProfileManager::Instance().Active().displayName.c_str(),
                     (int)(card.x + pad), (int)(card.y + card.h - FS(38)),
                     FS(12), angelsui::kTextDim);
            if (Button({card.x + card.w - FS(120), card.y + card.h - FS(42),
                        FS(104), FS(28)}, "Change")) {
                m_page = PageId::Character;
                m_pageFade = 0.0f;
            }
        }
    }

    // ---------------------------------------------------------------------
    // Play (Campaign / Practice)
    // ---------------------------------------------------------------------
    void DrawPlayPage(angelsmenu::UiRect body) {
        DrawTabBar(body, {"Campaign", "Practice"},
                   (int)m_playTab, 2, [&](int i) { m_playTab = (PlayTab)i; });
        angelsmenu::UiRect listArea = {body.x, body.y + FS(38), body.w, body.h - FS(38)};

        if (m_playTab == PlayTab::Campaign) DrawCampaignList(listArea);
        else DrawPracticeList(listArea);
    }

    void DrawTabBar(angelsmenu::UiRect body, const std::vector<const char*>& labels,
                    int selected, int count, void (*onClick)(int)) {
        (void)count;
        float x = body.x;
        const float h = FS(30);
        for (int i = 0; i < (int)labels.size(); ++i) {
            const int fs = FS(13);
            const float w = (float)MeasureText(labels[i], fs) + FS(28);
            angelsmenu::UiRect r = {x, body.y, w, h};
            const bool active = (i == selected);
            if (Button(r, labels[i], active)) onClick(i);
            x += w + FS(6);
        }
        DrawLine((int)body.x, (int)(body.y + h + FS(4)),
                 (int)(body.x + body.w), (int)(body.y + h + FS(4)),
                 (Color){60, 100, 160, 120});
    }

    // Overload for lambdas (the function-pointer version above cannot capture).
    template <typename Fn>
    void DrawTabBar(angelsmenu::UiRect body, const std::vector<const char*>& labels,
                    int selected, int count, Fn onClick) {
        (void)count;
        float x = body.x;
        const float h = FS(30);
        for (int i = 0; i < (int)labels.size(); ++i) {
            const int fs = FS(13);
            const float w = (float)MeasureText(labels[i], fs) + FS(28);
            angelsmenu::UiRect r = {x, body.y, w, h};
            const bool active = (i == selected);
            if (Button(r, labels[i], active)) onClick(i);
            x += w + FS(6);
        }
        DrawLine((int)body.x, (int)(body.y + h + FS(4)),
                 (int)(body.x + body.w), (int)(body.y + h + FS(4)),
                 (Color){60, 100, 160, 120});
    }

    void DrawCampaignList(angelsmenu::UiRect area) {
        auto& gm = oz::game::GameManager::Instance();
        if (!gm.Loaded()) {
            DrawText("Campaign data did not load (GameData/Game).",
                     (int)area.x, (int)area.y + FS(8), FS(14),
                     (Color){240, 170, 140, 230});
            DrawText("Practice still works: choose the Practice tab and load any world.",
                     (int)area.x, (int)area.y + FS(30), FS(11),
                     (Color){180, 160, 140, 220});
            return;
        }

        const int rowH = FS(26);
        const int headerH = FS(28);
        int contentRows = 0;
        for (const auto& sector : gm.Sectors()) contentRows += 1 + (int)sector.missions.size();
        angelsmenu::UiRect list = {area.x, area.y + FS(10), area.w * 0.62f,
                                   area.h - FS(54)};
        Card(list);
        ScrollList({list.x, list.y, list.w, list.h - FS(6)}, contentRows, rowH,
                   m_worldScroll, m_selectedWorld);

        BeginScissorMode((int)list.x, (int)list.y, (int)list.w, (int)list.h);
        float y = list.y + FS(6) - m_worldScroll * rowH;
        for (const auto& sector : gm.Sectors()) {
            if (y + rowH > list.y && y < list.Bottom()) {
                char hdr[160];
                std::snprintf(hdr, sizeof(hdr), "%d. %s", sector.numerology, sector.name.c_str());
                DrawText(hdr, (int)(list.x + FS(10)), (int)y + FS(5), FS(13),
                         (Color){255, 220, 140, 255});
            }
            y += headerH;
            for (const auto& missionName : sector.missions) {
                if (y + rowH > list.y && y < list.Bottom()) {
                    const oz::game::MissionDef* md = gm.FindMission(missionName);
                    if (md) {
                        Rectangle row = {list.x + FS(8), y, list.w - FS(16), (float)rowH};
                        const bool sel = (m_selectedMission == md->name);
                        const bool hover = MouseIn({row.x, row.y, row.width, row.height});
                        if (sel) DrawRectangleRec(row, (Color){24, 48, 92, 220});
                        else if (hover) DrawRectangleRec(row, (Color){30, 60, 110, 180});
                        DrawText(TextFormat("%s  [%s]", md->name.c_str(), md->mode.c_str()),
                                 (int)(row.x + FS(8)), (int)(row.y + FS(5)), FS(12),
                                 sel || hover ? WHITE : angelsui::kTextSoft);
                        if (hover && Clickable() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                            m_selectedMission = md->name;
                            m_selectedWorld = -1;
                            m_suppressClick = true;
                            SoundManager::Instance().PlayUIClick();
                        }
                    }
                }
                y += rowH;
            }
            y += FS(6);
        }
        EndScissorMode();

        // Details / launch.
        angelsmenu::UiRect detail = {list.Right() + FS(12), list.y,
                                     area.Right() - list.Right() - FS(12), list.h};
        Card(detail);
        const oz::game::MissionDef* md = gm.FindMission(m_selectedMission);
        float dy = detail.y + FS(12);
        if (!md) {
            DrawText("Select a mission", (int)(detail.x + FS(12)), (int)dy, FS(14),
                     angelsui::kTextDim);
        } else {
            DrawText(md->name.c_str(), (int)(detail.x + FS(12)), (int)dy, FS(18), angelsui::kText);
            dy += FS(26);
            DrawText(TextFormat("%s  //  %s", md->sector.c_str(), md->mode.c_str()),
                     (int)(detail.x + FS(12)), (int)dy, FS(11), angelsui::kTextDim);
            dy += FS(22);
            DrawText(TextFormat("World: %s", md->world.c_str()),
                     (int)(detail.x + FS(12)), (int)dy, FS(11), angelsui::kTextSoft);
            dy += FS(26);
        }
        if (md && Button({detail.x + FS(12), detail.Bottom() - FS(44),
                          detail.w - FS(24), FS(32)}, "Launch Mission", true)) {
            m_result = Result::PlayWorld;
            m_resultWorld = md->world;
            m_resultMission = md->name;
            m_done = true;
        }
    }

    void DrawPracticeList(angelsmenu::UiRect area) {
        const int rowH = FS(30);
        const int visible = std::max(1, (int)((area.h - FS(58)) / rowH));
        const int maxScroll = std::max(0, (int)m_worlds.size() - visible);
        if (MouseIn({area.x, area.y, area.w, area.h})) {
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) m_worldScroll -= (int)wheel;
        }
        m_worldScroll = std::clamp(m_worldScroll, 0, maxScroll);

        angelsmenu::UiRect list = {area.x, area.y + FS(10), area.w * 0.62f,
                                   area.h - FS(54)};
        Card(list);
        if (m_worlds.empty()) {
            DrawText("No worlds found in GameData/Worlds or System/Data/Zones.",
                     (int)(list.x + FS(12)), (int)(list.y + FS(12)), FS(12),
                     (Color){240, 170, 140, 230});
            return;
        }
        BeginScissorMode((int)list.x, (int)list.y, (int)list.w, (int)list.h);
        float y = list.y + FS(6) - m_worldScroll * rowH;
        for (int i = 0; i < (int)m_worlds.size(); ++i) {
            if (y + rowH > list.y && y < list.Bottom()) {
                const WorldEntry& w = m_worlds[i];
                Rectangle row = {list.x + FS(8), y, list.w - FS(16), (float)rowH - FS(2)};
                const bool sel = (m_selectedWorld == i);
                const bool hover = MouseIn({row.x, row.y, row.width, row.height});
                if (sel) DrawRectangleRec(row, (Color){24, 48, 92, 220});
                else if (hover) DrawRectangleRec(row, (Color){30, 60, 110, 180});
                DrawText(w.name.c_str(), (int)(row.x + FS(8)), (int)(row.y + FS(6)),
                         FS(13), sel || hover ? WHITE : angelsui::kTextSoft);
                if (!w.mode.empty()) {
                    const int tw = MeasureText(w.mode.c_str(), FS(10));
                    DrawText(w.mode.c_str(), (int)(row.x + row.width - tw - FS(8)),
                             (int)(row.y + FS(8)), FS(10), angelsui::kTextDim);
                }
                if (hover && Clickable() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    m_selectedWorld = i;
                    m_suppressClick = true;
                    SoundManager::Instance().PlayUIClick();
                }
            }
            y += rowH;
        }
        EndScissorMode();

        angelsmenu::UiRect detail = {list.Right() + FS(12), list.y,
                                     area.Right() - list.Right() - FS(12), list.h};
        Card(detail);
        const float dx = detail.x + FS(12);
        float dy = detail.y + FS(12);
        if (m_selectedWorld >= 0 && m_selectedWorld < (int)m_worlds.size()) {
            const WorldEntry& w = m_worlds[m_selectedWorld];
            DrawText(w.name.c_str(), (int)dx, (int)dy, FS(20), angelsui::kText);
            dy += FS(30);
            DrawText(TextFormat("Folder: %s", w.dirName.c_str()), (int)dx, (int)dy,
                     FS(11), angelsui::kTextDim);
            dy += FS(20);
            if (!w.sector.empty())
                DrawText(TextFormat("Sector: %s", w.sector.c_str()), (int)dx, (int)dy,
                         FS(11), angelsui::kTextDim);
            if (Button({dx, detail.Bottom() - FS(44), detail.w - FS(24), FS(32)},
                       "Load World", true)) {
                m_result = Result::PlayWorld;
                m_resultWorld = w.dirName;
                m_resultMission.clear();
                m_done = true;
            }
        } else {
            DrawText("Select a world", (int)dx, (int)dy, FS(14), angelsui::kTextDim);
        }
    }

    // ---------------------------------------------------------------------
    // Multiplayer
    // ---------------------------------------------------------------------
    void DrawMultiplayerPage(angelsmenu::UiRect body) {
        DrawTabBar(body, {"Join", "Host", "Co-op"}, (int)m_multiTab, 3,
                   [&](int i) {
                       m_multiTab = (MultiTab)i;
                       m_error.clear();
                       if (m_multiTab == MultiTab::Join && !m_internetAutoScanned) {
                           m_internetAutoScanned = true;
                           RefreshInternet();
                       }
                   });
        angelsmenu::UiRect area = {body.x, body.y + FS(40), body.w, body.h - FS(40)};
        if (m_multiTab == MultiTab::Join) DrawJoinTab(area);
        else if (m_multiTab == MultiTab::Host) DrawHostTab(area);
        else DrawCoopTab(area);
    }

    void DrawJoinTab(angelsmenu::UiRect area) {
        const float leftW = m_metrics.wide ? area.w * 0.62f : area.w;
        angelsmenu::UiRect left = {area.x, area.y, leftW, area.h};
        angelsmenu::UiRect right = {left.Right() + FS(12), area.y,
                                    area.Right() - left.Right() - FS(12), area.h};

        // Direct address row.
        float y = left.y;
        DrawText("Address", (int)left.x, (int)y, FS(12), angelsui::kTextDim);
        TextField({left.x + FS(64), y - FS(4), left.w * 0.42f, FS(24)},
                  m_joinIp, sizeof(m_joinIp), Focus::JoinIp);
        DrawText("Port", (int)(left.x + FS(64) + left.w * 0.42f + FS(10)), (int)y,
                 FS(12), angelsui::kTextDim);
        TextField({left.x + FS(64) + left.w * 0.42f + FS(48), y - FS(4), FS(72), FS(24)},
                  m_joinPort, sizeof(m_joinPort), Focus::JoinPort);
        if (Button({left.Right() - FS(110), y - FS(4), FS(100), FS(24)}, "Join", true,
                   std::atoi(m_joinPort) > 0)) {
            ConnectToSelectedOrTyped();
        }
        y += FS(34);

        // Source tabs.
        const char* sources[] = {"LAN", "Internet", "Favorites"};
        const int sourceIds[] = {angelsmenu::ServerFilter::Lan,
                                 angelsmenu::ServerFilter::Internet,
                                 angelsmenu::ServerFilter::Favorites};
        float sx = left.x;
        for (int i = 0; i < 3; ++i) {
            const float w = (float)MeasureText(sources[i], FS(11)) + FS(24);
            if (Button({sx, y, w, FS(24)}, sources[i], m_filter.source == sourceIds[i],
                       true, FS(11))) {
                m_filter.source = sourceIds[i];
                if (i == 1 && !m_internet.HasResult() && !m_internet.IsScanning())
                    RefreshInternet();
            }
            sx += w + FS(6);
        }
        if (m_filter.source == angelsmenu::ServerFilter::Lan) {
            if (Button({sx + FS(8), y, FS(90), FS(24)}, m_lanScanning ? "Scanning..." : "Scan LAN",
                       false, !m_lanScanning, FS(11))) {
                LanStart();
            }
        } else {
            if (Button({sx + FS(8), y, FS(90), FS(24)},
                       m_internet.IsScanning() ? "Scanning..." : "Scan Internet",
                       false, !m_internet.IsScanning(), FS(11))) {
                RefreshInternet();
            }
        }
        y += FS(32);

        // Search + filters.
        TextField({left.x, y, left.w * 0.48f, FS(24)}, m_search, sizeof(m_search),
                  Focus::Search);
        if (m_search[0] == '\0') m_filter.query.clear();
        else SyncFilterFromText();
        static const char* sorts[] = {"Players", "Ping", "Name", "Map"};
        angelsmenu::UiRect sortRect = {left.x + left.w * 0.50f, y, FS(110), FS(24)};
        m_filter.sort = angelsui::Cycle(R(sortRect), sorts, 4, m_filter.sort);
        if (Button({sortRect.Right() + FS(8), y, FS(90), FS(24)},
                   m_filter.hideFull ? "Hide Full: ON" : "Hide Full",
                   m_filter.hideFull, true, FS(10))) m_filter.hideFull = !m_filter.hideFull;
        if (Button({sortRect.Right() + FS(8) + FS(94), y, FS(96), FS(24)},
                   m_filter.hidePassword ? "No PW: ON" : "No PW",
                   m_filter.hidePassword, true, FS(10))) m_filter.hidePassword = !m_filter.hidePassword;
        y += FS(32);

        // Header.
        const float listW = left.w;
        const float rowH = FS(24);
        const int visible = std::max(1, (int)((left.Bottom() - y - FS(8)) / rowH));
        const auto visibleRows = m_browser.Query(m_filter);
        const int maxScroll = std::max(0, (int)visibleRows.size() - visible);
        if (MouseIn({left.x, y, listW, left.Bottom() - y})) {
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) m_serverScroll -= (int)wheel;
        }
        m_serverScroll = std::clamp(m_serverScroll, 0, maxScroll);

        DrawRectangleRec({left.x, y, listW, left.Bottom() - y}, (Color){0, 0, 0, 110});
        const float colName = listW * 0.42f;
        const float colMap = listW * 0.26f;
        const float colPl = listW * 0.12f;
        const float colPing = listW * 0.10f;
        DrawText("NAME", (int)(left.x + FS(6)), (int)y + FS(4), FS(10), angelsui::kTextDim);
        DrawText("MAP", (int)(left.x + colName), (int)y + FS(4), FS(10), angelsui::kTextDim);
        DrawText("PLAYERS", (int)(left.x + colName + colMap), (int)y + FS(4), FS(10), angelsui::kTextDim);
        DrawText("PING", (int)(left.x + colName + colMap + colPl), (int)y + FS(4), FS(10), angelsui::kTextDim);
        DrawText("TYPE", (int)(left.x + colName + colMap + colPl + colPing),
                 (int)y + FS(4), FS(10), angelsui::kTextDim);
        y += FS(20);

        BeginScissorMode((int)left.x, (int)y, (int)listW, (int)(left.Bottom() - y - FS(4)));
        for (int i = 0; i < visible && m_serverScroll + i < (int)visibleRows.size(); ++i) {
            const size_t rowIndex = visibleRows[m_serverScroll + i];
            const angelsmenu::ServerRow& r = m_browser.Rows()[rowIndex];
            Rectangle row = {left.x, y + FS(2), listW, rowH - FS(2)};
            const bool sel = ((int)rowIndex == m_selectedServer);
            const bool hover = MouseIn({row.x, row.y, row.width, row.height});
            if (sel) DrawRectangleRec(row, (Color){30, 60, 110, 220});
            else if (hover) DrawRectangleRec(row, (Color){20, 40, 72, 180});
            const Color c = sel || hover ? WHITE : angelsui::kTextSoft;
            const std::string rowName = FitText(r.name, FS(11), colName - FS(12));
            DrawText(rowName.c_str(), (int)(row.x + FS(6)), (int)(row.y + FS(5)), FS(11), c);
            DrawText(r.map.c_str(), (int)(row.x + colName), (int)(row.y + FS(5)), FS(10), angelsui::kTextDim);
            DrawText(TextFormat("%d/%d", r.players, r.maxPlayers),
                     (int)(row.x + colName + colMap), (int)(row.y + FS(5)), FS(10), c);
            const char* ping = r.pingMs >= 0 ? TextFormat("%dms", r.pingMs) : "-";
            Color pc = r.pingMs >= 0 && r.pingMs < 60 ? (Color){120, 230, 140, 230}
                     : r.pingMs >= 0 && r.pingMs < 120 ? (Color){235, 210, 120, 230}
                     : r.pingMs >= 0 ? (Color){235, 130, 120, 230} : angelsui::kTextDim;
            DrawText(ping, (int)(row.x + colName + colMap + colPl), (int)(row.y + FS(5)), FS(10), pc);
            const char* type = r.favorite ? "FAV" : (r.lan && r.internet ? "LAN+NET" : (r.lan ? "LAN" : "NET"));
            DrawText(type, (int)(row.x + colName + colMap + colPl + colPing),
                     (int)(row.y + FS(5)), FS(9), r.favorite ? (Color){255, 210, 100, 240} : angelsui::kTextDim);
            if (r.password) {
                const int lw = MeasureText(type, FS(9));
                DrawText("[pw]", (int)(row.x + colName + colMap + colPl + colPing + lw + FS(6)),
                         (int)(row.y + FS(5)), FS(9), (Color){230, 160, 120, 220});
            }
            if (hover && Clickable() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                m_selectedServer = (int)rowIndex;
                m_selectedServerKey = angelsmenu::ServerBrowserModel::KeyString(r.key);
                std::snprintf(m_joinIp, sizeof(m_joinIp), "%s", r.key.ip.c_str());
                std::snprintf(m_joinPort, sizeof(m_joinPort), "%u", (unsigned)r.key.port);
                m_suppressClick = true;
                SoundManager::Instance().PlayUIClick();
                const double now = GetTime();
                if (m_lastServerClickIndex == (int)rowIndex && now - m_lastServerClick < 0.35) {
                    ConnectToSelectedOrTyped();
                }
                m_lastServerClick = now;
                m_lastServerClickIndex = (int)rowIndex;
            }
            y += rowH;
        }
        EndScissorMode();

        if (visibleRows.empty()) {
            const char* msg;
            if (m_filter.source == angelsmenu::ServerFilter::Lan)
                msg = m_lanScanning ? "Scanning the LAN..." : "No LAN servers. Press Scan LAN.";
            else if (m_filter.source == angelsmenu::ServerFilter::Favorites)
                msg = "No favourite servers in the current list.";
            else
                msg = m_internet.IsScanning() ? "Querying masters..." :
                      (!m_internet.HasResult() ? "Not scanned. Press Scan Internet."
                                               : "No servers match the filter.");
            DrawText(msg, (int)(left.x + FS(10)), (int)(y + FS(10)), FS(11),
                     (Color){150, 170, 190, 210});
        }
        if (!m_internet.Error().empty() &&
            m_filter.source != angelsmenu::ServerFilter::Lan) {
            DrawText(m_internet.Error().c_str(), (int)(left.x + FS(10)),
                     (int)(left.Bottom() - FS(18)), FS(10),
                     (Color){235, 150, 130, 220});
        }

        DrawServerDetails(right);
    }

    void DrawServerDetails(angelsmenu::UiRect right) {
        if (right.w < FS(120) || right.h < FS(80)) return;
        Card(right);
        const angelsmenu::ServerRow* r = SelectedServerRow();
        float x = right.x + FS(12);
        float y = right.y + FS(12);
        DrawText("SERVER", (int)x, (int)y, FS(11), angelsui::kTextDim);
        y += FS(22);
        if (!r) {
            DrawText("Select a server to see its details.", (int)x, (int)y, FS(11),
                     angelsui::kTextDim);
            return;
        }
        const std::string serverName = FitText(r->name, FS(17), right.w - FS(24));
        DrawText(serverName.c_str(), (int)x, (int)y, FS(17), angelsui::kText);
        y += FS(26);
        DrawText(TextFormat("%s:%u", r->key.ip.c_str(), (unsigned)r->key.port),
                 (int)x, (int)y, FS(11), angelsui::kTextSoft);
        y += FS(20);
        DrawText(TextFormat("Map: %s", r->map.empty() ? "unknown" : r->map.c_str()),
                 (int)x, (int)y, FS(11), angelsui::kTextDim);
        y += FS(18);
        DrawText(TextFormat("Players: %d/%d", r->players, r->maxPlayers),
                 (int)x, (int)y, FS(11), angelsui::kTextDim);
        y += FS(18);
        DrawText(TextFormat("Ping: %s", r->pingMs >= 0 ? TextFormat("%dms", r->pingMs) : "-"),
                 (int)x, (int)y, FS(11), angelsui::kTextDim);
        y += FS(18);
        DrawText(TextFormat("Build: %s", r->version.empty() ? "unknown" : r->version.c_str()),
                 (int)x, (int)y, FS(11), angelsui::kTextDim);
        y += FS(18);
        const bool incompatible =
            r->protocol > 0 && r->protocol != (int)net::PROTOCOL_VERSION;
        const std::string protoLine = std::string("Protocol: ") +
            (r->protocol > 0 ? std::to_string(r->protocol) : std::string("unknown")) +
            (incompatible ? "  (INCOMPATIBLE)" : "");
        DrawText(protoLine.c_str(), (int)x, (int)y, FS(11),
                 incompatible ? (Color){235, 130, 120, 230} : angelsui::kTextDim);
        y += FS(18);
        DrawText(TextFormat("Source: %s", r->lan && r->internet ? "LAN + Internet" : (r->lan ? "LAN" : "Internet")),
                 (int)x, (int)y, FS(11), angelsui::kTextDim);
        if (r->password) {
            y += FS(18);
            DrawText("Password protected", (int)x, (int)y, FS(11), (Color){235, 160, 120, 220});
        }
        if (Button({x, right.Bottom() - FS(76), right.w - FS(24), FS(28)},
                   r->favorite ? "Remove Favourite" : "Add Favourite", r->favorite)) {
            m_browser.ToggleFavorite(r->key);
            m_browser.SaveState(kBrowserStatePath);
        }
        if (Button({x, right.Bottom() - FS(42), right.w - FS(24), FS(30)},
                   "Join Server", true, !incompatible)) {
            ConnectToSelectedOrTyped();
        }
    }

    bool HostBinaryAvailable() const {
#ifdef _WIN32
        return fs::exists("System/AngelServ.exe") || fs::exists("AngelServ.exe");
#else
        return fs::exists("./AngelServ") || fs::exists("AngelServ");
#endif
    }

    void DrawHostTab(angelsmenu::UiRect area) {
        const float leftW = m_metrics.wide ? area.w * 0.56f : area.w;
        angelsmenu::UiRect left = {area.x, area.y, leftW, area.h};
        angelsmenu::UiRect right = {left.Right() + FS(12), area.y,
                                    area.Right() - left.Right() - FS(12), area.h};

        DrawText("World", (int)left.x, (int)left.y, FS(12), angelsui::kTextDim);
        float y = left.y + FS(22);
        angelsmenu::UiRect list = {left.x, y, left.w, left.h - FS(120)};
        Card(list);
        const int rowH = FS(26);
        const int visible = std::max(1, (int)((list.h - FS(8)) / rowH));
        if (MouseIn({list.x, list.y, list.w, list.h})) {
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) m_worldScroll -= (int)wheel;
        }
        m_worldScroll = std::clamp(m_worldScroll, 0, std::max(0, (int)m_worlds.size() - visible));
        BeginScissorMode((int)list.x, (int)list.y, (int)list.w, (int)list.h);
        float ry = list.y + FS(6) - m_worldScroll * rowH;
        for (int i = 0; i < (int)m_worlds.size(); ++i) {
            if (ry + rowH > list.y && ry < list.Bottom()) {
                Rectangle row = {list.x + FS(8), ry, list.w - FS(16), (float)rowH - FS(2)};
                const bool sel = (m_hostWorld == i);
                if (sel) DrawRectangleRec(row, (Color){30, 60, 110, 220});
                else if (MouseIn(U(row))) DrawRectangleRec(row, (Color){20, 40, 72, 180});
                DrawText(m_worlds[i].name.c_str(), (int)(row.x + FS(8)),
                         (int)(row.y + FS(6)), FS(12),
                         sel ? WHITE : angelsui::kTextSoft);
                if (MouseIn(U(row)) && Clickable() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    m_hostWorld = i;
                    m_suppressClick = true;
                }
            }
            ry += rowH;
        }
        EndScissorMode();

        y = list.Bottom() + FS(10);
        DrawText("Port", (int)left.x, (int)y, FS(12), angelsui::kTextDim);
        TextField({left.x + FS(48), y - FS(4), FS(80), FS(24)}, m_hostPort,
                  sizeof(m_hostPort), Focus::HostPort);
        DrawText("Server name", (int)(left.x + FS(150)), (int)y, FS(12), angelsui::kTextDim);
        TextField({left.x + FS(238), y - FS(4), left.w - FS(246), FS(24)},
                  m_serverName, sizeof(m_serverName), Focus::ServerName);
        y += FS(34);
        const bool valid = m_hostWorld >= 0 && m_hostWorld < (int)m_worlds.size() &&
                           std::atoi(m_hostPort) > 0 && std::atoi(m_hostPort) <= 65535;
        if (Button({left.x, y, FS(160), FS(32)}, "Start Server", true, valid)) {
            const int port = std::atoi(m_hostPort);
            if (!valid) {
                m_error = "Choose a world and a port in 1..65535.";
            } else if (!HostBinaryAvailable()) {
                m_error = "AngelServ was not found. Build it or run from System/.";
            } else {
                m_error.clear();
                m_result = Result::HostServer;
                m_resultWorld = m_worlds[m_hostWorld].dirName;
                m_resultPort = port;
                m_done = true;
            }
        }
        if (!m_error.empty())
            DrawText(m_error.c_str(), (int)(left.x + FS(170)), (int)(y + FS(10)),
                     FS(11), (Color){235, 150, 130, 220});

        Card(right);
        float x = right.x + FS(12);
        float yy = right.y + FS(12);
        DrawText("HOST A GAME", (int)x, (int)yy, FS(11), angelsui::kTextDim);
        yy += FS(24);
        DrawText("Plain co-op", (int)x, (int)yy, FS(17), angelsui::kText);
        yy += FS(24);
        DrawText("The server starts in the selected world and", (int)x, (int)yy, FS(11),
                 angelsui::kTextDim);
        yy += FS(16);
        DrawText("clients join it from the Join tab.", (int)x, (int)yy, FS(11),
                 angelsui::kTextDim);
        yy += FS(28);
        DrawText("For a campaign mission pin, use the Co-op tab.", (int)x, (int)yy,
                 FS(11), angelsui::kTextSoft);
        yy += FS(24);
        DrawText("The host needs System/AngelServ.exe next to the client.", (int)x,
                 (int)yy, FS(10), (Color){150, 170, 190, 210});
    }

    void DrawCoopTab(angelsmenu::UiRect area) {
        auto& gm = oz::game::GameManager::Instance();
        if (!gm.Loaded()) {
            DrawText("Campaign data did not load (GameData/Game).", (int)area.x,
                     (int)area.y, FS(14), (Color){240, 170, 140, 230});
            return;
        }
        const float listW = m_metrics.wide ? area.w * 0.62f : area.w;
        angelsmenu::UiRect list = {area.x, area.y, listW, area.h - FS(100)};
        Card(list);
        const int rowH = FS(24);
        int contentRows = 0;
        for (const auto& s : gm.Sectors()) contentRows += 1 + (int)s.missions.size();
        int unusedSelection = 0;
        ScrollList({list.x, list.y, list.w, list.h}, contentRows, rowH, m_worldScroll,
                   unusedSelection);
        BeginScissorMode((int)list.x, (int)list.y, (int)list.w, (int)list.h);
        float y = list.y + FS(6) - m_worldScroll * rowH;
        for (const auto& sector : gm.Sectors()) {
            if (y + rowH > list.y && y < list.Bottom()) {
                DrawText(TextFormat("%d. %s", sector.numerology, sector.name.c_str()),
                         (int)(list.x + FS(10)), (int)(y + FS(4)), FS(12),
                         (Color){255, 220, 140, 255});
            }
            y += FS(26);
            for (const auto& missionName : sector.missions) {
                const oz::game::MissionDef* md = gm.FindMission(missionName);
                if (!md) continue;
                if (y + rowH > list.y && y < list.Bottom()) {
                    Rectangle row = {list.x + FS(8), y, list.w - FS(16), (float)rowH - FS(2)};
                    const bool sel = (m_selectedMission == md->name);
                    if (sel) DrawRectangleRec(row, (Color){24, 48, 92, 220});
                    else if (MouseIn(U(row))) DrawRectangleRec(row, (Color){30, 60, 110, 180});
                    DrawText(TextFormat("%s  [%s]", md->name.c_str(), md->mode.c_str()),
                             (int)(row.x + FS(8)), (int)(row.y + FS(4)), FS(11),
                             sel ? WHITE : angelsui::kTextSoft);
                    if (MouseIn(U(row)) && Clickable() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                        m_selectedMission = md->name;
                        m_suppressClick = true;
                    }
                }
                y += rowH;
            }
            y += FS(4);
        }
        EndScissorMode();

        float by = list.Bottom() + FS(10);
        DrawText("Port", (int)(area.x), (int)(by), FS(11), angelsui::kTextDim);
        TextField({area.x + FS(44), by - FS(4), FS(72), FS(24)}, m_hostPort,
                  sizeof(m_hostPort), Focus::HostPort);
        by += FS(34);
        const oz::game::MissionDef* md = gm.FindMission(m_selectedMission);
        if (Button({area.x, by, FS(150), FS(32)}, "Host Co-op", true, md != nullptr)) {
            if (md) {
                if (!HostBinaryAvailable()) {
                    m_error = "AngelServ was not found. Build it or run from System/.";
                } else {
                    m_error.clear();
                    m_result = Result::HostServer;
                    m_resultMission = md->name;
                    m_resultWorld = md->world;
                    m_resultPort = std::atoi(m_hostPort);
                    if (m_resultPort <= 0 || m_resultPort > 65535) m_resultPort = 27015;
                    m_done = true;
                }
            }
        }
        if (Button({area.x + FS(160), by, FS(150), FS(32)}, "Join Co-op")) {
            m_multiTab = MultiTab::Join;
            m_filter.source = angelsmenu::ServerFilter::Internet;
            m_internetAutoScanned = true;
            RefreshInternet();
        }
        if (md)
            DrawText(TextFormat("Host: %s", md->name.c_str()),
                     (int)(area.x + FS(324)), (int)(by + FS(8)), FS(12),
                     angelsui::kTextSoft);
        if (!m_error.empty())
            DrawText(m_error.c_str(), (int)(area.x), (int)(by + FS(40)), FS(11),
                     (Color){235, 150, 130, 220});
    }

    // ---------------------------------------------------------------------
    // Character
    // ---------------------------------------------------------------------
    void DrawCharacterPage(angelsmenu::UiRect body) {
        CharacterRoster& roster = CharacterRoster::Instance();
        const CharacterDef& def = SelectedCharacter();
        angelsmenu::UiRect preview = PreviewRect(body);
        angelsmenu::UiRect side;
        if (body.w >= FS(700)) {
            side = {preview.Right() + FS(12), body.y, body.Right() - preview.Right() - FS(12), body.h};
        } else {
            side = {body.x, preview.Bottom() + FS(10), body.w, body.Bottom() - preview.Bottom() - FS(10)};
        }

        Card(preview, true);
        // The live preview was rendered before BeginDrawing; draw its texture.
        Texture2D live = m_livePreviewTex;
        if (live.id) {
            DrawTexturePro(live, {0, 0, (float)live.width, (float)-live.height},
                           {preview.x + 1, preview.y + 1, preview.w - 2, preview.h - 2},
                           {0, 0}, 0.0f, WHITE);
        } else {
            DrawText("Preview unavailable", (int)(preview.x + FS(12)),
                     (int)(preview.y + FS(12)), FS(13), angelsui::kTextDim);
        }
        if (MouseIn(preview) && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            if (!m_previewDragging) { m_previewDragging = true; }
        } else if (!IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            m_previewDragging = false;
        }

        // Name/bio panel over the preview's bottom.
        const float nameH = FS(72);
        DrawRectangleGradientV((int)preview.x, (int)(preview.Bottom() - nameH),
                               (int)preview.w, (int)nameH,
                               (Color){0, 0, 0, 0}, (Color){0, 0, 0, 210});
        DrawText(def.name.c_str(), (int)(preview.x + FS(16)),
                 (int)(preview.Bottom() - FS(50)), FS(24), angelsui::kText);
        DrawText(def.bio.empty() ? "Cosmetic character - visible to other players in co-op."
                                 : def.bio.c_str(),
                 (int)(preview.x + FS(16)), (int)(preview.Bottom() - FS(22)), FS(10),
                 angelsui::kTextDim);

        // Profile slots.
        float y = side.y;
        DrawText("PROFILE", (int)side.x, (int)y, FS(10), angelsui::kTextDim);
        y += FS(18);
        auto& pm = PlayerProfileManager::Instance();
        const int rowH = FS(24);
        for (int i = 0; i < pm.Count() && i < 4; ++i) {
            Rectangle row = {side.x, y + i * rowH, side.w, (float)rowH - 2};
            const bool active = (i == pm.ActiveIndex());
            if (active) DrawRectangleRec(row, (Color){30, 60, 110, 220});
            else if (MouseIn(U(row))) DrawRectangleRec(row, (Color){20, 40, 72, 160});
            DrawText(TextFormat("%s%s", active ? "> " : "  ",
                                pm.Get(i).displayName.c_str()),
                     (int)(row.x + FS(6)), (int)(row.y + FS(4)), FS(11),
                     active ? WHITE : angelsui::kTextSoft);
            if (MouseIn(U(row)) && Clickable() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                pm.SetActive(i);
                LoadProfileFields();
                m_selectedCharacter = roster.ResolveSelection(m_activeProfile.modelPath);
                if (m_selectedCharacter.empty() && !roster.All().empty())
                    m_selectedCharacter = roster.All().front().id;
                m_suppressClick = true;
            }
        }
        y += rowH * 4 + FS(8);

        DrawText("CHARACTER", (int)side.x, (int)y, FS(10), angelsui::kTextDim);
        y += FS(18);
        const float cardW = FS(96), cardH = FS(124);
        float cx = side.x;
        for (int i = 0; i < (int)roster.All().size(); ++i) {
            const CharacterDef& c = roster.All()[i];
            if (cx + cardW > side.Right()) break;
            Rectangle card = {cx, y, cardW, cardH};
            Card(card, c.id == m_selectedCharacter);
            m_preview.DrawPortrait(c, {card.x + 2, card.y + 2,
                                       card.width - 4, card.height - FS(22)});
            const std::string cardLabel = FitText(c.name, FS(9), card.width - FS(8));
            DrawText(cardLabel.c_str(), (int)(card.x + FS(4)),
                     (int)(card.y + card.height - FS(16)), FS(9), angelsui::kTextSoft);
            if (MouseIn(U(card)) && Clickable() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                SelectCharacter(c.id);
                m_suppressClick = true;
            }
            cx += cardW + FS(6);
        }
        if (roster.All().empty()) {
            DrawText("No characters.cfg - using the default model.",
                     (int)side.x, (int)y, FS(10), (Color){235, 170, 140, 220});
        }
        y += cardH + FS(12);

        DrawText("NAME", (int)side.x, (int)y, FS(10), angelsui::kTextDim);
        y += FS(16);
        TextField({side.x, y, std::min(side.w, (float)FS(260)), FS(24)}, m_profileName,
                  sizeof(m_profileName), Focus::ProfileName);
        y += FS(32);

        DrawText("COLOUR", (int)side.x, (int)y, FS(10), angelsui::kTextDim);
        y += FS(16);
        for (int i = 0; i < TEAM_COLOR_COUNT; ++i) {
            Rectangle sw = {side.x + i * FS(24), y, FS(20), FS(20)};
            DrawRectangleRec(sw, TEAM_COLORS[i]);
            if (i == m_profileColor) DrawRectangleLinesEx(sw, 2, WHITE);
            else if (MouseIn(U(sw))) DrawRectangleLinesEx(sw, 1, angelsui::kTextDim);
            if (MouseIn(U(sw)) && Clickable() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                m_profileColor = i;
                SaveProfileFields();
                m_suppressClick = true;
            }
        }
        DrawText(TEAM_COLOR_NAMES[std::clamp(m_profileColor, 0, TEAM_COLOR_COUNT - 1)],
                 (int)(side.x + TEAM_COLOR_COUNT * FS(24) + FS(8)),
                 (int)(y + FS(4)), FS(10), angelsui::kTextDim);
        y += FS(28);

        if (Button({side.x, y, FS(96), FS(26)}, "Save Profile")) {
            SaveProfileFields();
            Toast("Profile saved");
        }
        if (Button({side.x + FS(104), y, FS(96), FS(26)}, "New Profile")) {
            PlayerProfile p;
            p.displayName = "Player";
            p.modelPath = m_selectedCharacter;
            p.team = m_profileColor;
            pm.Add(p);
            LoadProfileFields();
            m_selectedCharacter = pm.Active().modelPath;
            Toast("Profile created");
        }
        if (Button({side.x + FS(208), y, FS(96), FS(26)}, "Delete Profile",
                   false, pm.Count() > 1)) {
            m_confirmDelete = true;
        }
    }

    void DrawDeleteProfileModal() {
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 170});
        Rectangle panel = {(sw - FS(380)) * 0.5f, (sh - FS(190)) * 0.5f, FS(380), FS(190)};
        Card(panel, true);
        DrawText("Delete this profile?", (int)(panel.x + FS(16)),
                 (int)(panel.y + FS(16)), FS(18), angelsui::kText);
        DrawText(PlayerProfileManager::Instance().Active().displayName.c_str(),
                 (int)(panel.x + FS(16)), (int)(panel.y + FS(48)), FS(13),
                 angelsui::kTextDim);
        DrawText("This cannot be undone.", (int)(panel.x + FS(16)),
                 (int)(panel.y + FS(70)), FS(11), (Color){230, 150, 130, 220});
        const bool oldSuppress = m_suppressClick;
        m_suppressClick = false;
        if (Button({panel.x + FS(16), panel.y + panel.height - FS(46), FS(150), FS(30)},
                   "Delete", true)) {
            PlayerProfileManager::Instance().Remove(PlayerProfileManager::Instance().ActiveIndex());
            LoadProfileFields();
            m_confirmDelete = false;
            Toast("Profile deleted");
        }
        if (Button({panel.x + panel.width - FS(166), panel.y + panel.height - FS(46), FS(150), FS(30)},
                   "Cancel")) {
            m_confirmDelete = false;
        }
        m_suppressClick = oldSuppress;
    }

    // ---------------------------------------------------------------------
    // Settings
    // ---------------------------------------------------------------------
    void DrawSettingsPage(angelsmenu::UiRect body) {
        const char* tabs[] = {"Video", "Audio", "Controls", "Gameplay", "Window"};
        float x = body.x;
        const float tabH = FS(28);
        for (int i = 0; i < 5; ++i) {
            const int fs = FS(12);
            const float w = (float)MeasureText(tabs[i], fs) + FS(24);
            if (Button({x, body.y, w, tabH}, tabs[i], (int)m_settingsTab == i, true, fs))
                m_settingsTab = (SettingsTab)i;
            x += w + FS(6);
        }
        angelsmenu::UiRect area = {body.x, body.y + tabH + FS(12), body.w,
                                   body.h - tabH - FS(12)};
        switch (m_settingsTab) {
            case SettingsTab::Video: DrawVideoSettings(area); break;
            case SettingsTab::Audio: DrawAudioSettings(area); break;
            case SettingsTab::Controls: DrawControlsSettings(area); break;
            case SettingsTab::Gameplay: DrawGameplaySettings(area); break;
            case SettingsTab::Window: DrawWindowSettings(area); break;
        }
    }

    void SettingRow(angelsmenu::UiRect area, int& y, const char* label) {
        DrawText(label, (int)area.x, (int)(y + FS(4)), FS(12), angelsui::kTextDim);
    }

    void DrawVideoSettings(angelsmenu::UiRect area) {
        float x = area.x, y = area.y;
        const float w = std::min(area.w, (float)FS(420));
        if (Button({x, y, w, FS(28)}, IsWindowFullscreen() ? "Fullscreen: ON" : "Fullscreen: OFF",
                   IsWindowFullscreen())) ToggleFullscreen();
        y += FS(36);
        if (Button({x, y, w, FS(28)}, VSYNCToggle ? "VSync: ON" : "VSync: OFF", VSYNCToggle)) {
            VSYNCToggle = !VSYNCToggle;
            if (VSYNCToggle) SetWindowState(FLAG_VSYNC_HINT);
            else ClearWindowState(FLAG_VSYNC_HINT);
        }
        y += FS(36);
        if (Button({x, y, w, FS(28)}, MXAAToggle ? "MSAA x4: ON (restart)" : "MSAA x4: OFF (restart)",
                   MXAAToggle)) {
            MXAAToggle = !MXAAToggle;
        }
        y += FS(36);
        DrawText("UI Scale", (int)x, (int)y, FS(12), angelsui::kTextDim);
        UiScale = angelsui::Slider({x + FS(80), y - FS(4), w - FS(80), FS(24)},
                                   TextFormat("%.2f", UiScale), UiScale, 0.75f, 2.0f);
        y += FS(34);
        static const char* filterNames[] = {"Point", "Bilinear", "Trilinear", "Aniso x4", "Aniso x8", "Aniso x16"};
        DrawText("Texture Filter", (int)x, (int)y, FS(12), angelsui::kTextDim);
        TextureFilterMode = angelsui::Cycle({x + FS(110), y - FS(4), FS(150), FS(24)},
                                            filterNames, 6, TextureFilterMode);
        y += FS(30);
        DrawText("Applies to textures loaded after the next world load.",
                 (int)x, (int)y, FS(10), (Color){130, 150, 175, 210});
        y += FS(28);
        const char* sizes[] = {"720p", "1080p", "1440p", "4K"};
        const int sw[] = {1280, 1920, 2560, 3840};
        const int sh[] = {720, 1080, 1440, 2160};
        for (int i = 0; i < 4; ++i) {
            float bw = (w - FS(3) * FS(6)) / 4.0f;
            if (Button({x + i * (bw + FS(6)), y, bw, FS(28)}, sizes[i], false, true, FS(10))) {
                ConfigWindowWidth = sw[i];
                ConfigWindowHeight = sh[i];
                SetWindowSize(sw[i], sh[i]);
            }
        }
    }

    void DrawAudioSettings(angelsmenu::UiRect area) {
        float x = area.x, y = area.y;
        const float w = std::min(area.w, (float)FS(420));
        AudioSlider = angelsui::Slider({x, y, w, FS(24)}, TextFormat("Master  %.0f", AudioSlider),
                                       AudioSlider, 0.0f, 100.0f);
        ApplyMasterVolume();
        y += FS(34);
        MusicVolume = angelsui::Slider({x, y, w, FS(24)}, TextFormat("Music  %.0f", MusicVolume),
                                       MusicVolume, 0.0f, 100.0f);
        SoundManager::Instance().SetMusicGain(MusicVolume / 100.0f);
        y += FS(34);
        SfxVolume = angelsui::Slider({x, y, w, FS(24)}, TextFormat("Sound  %.0f", SfxVolume),
                                     SfxVolume, 0.0f, 100.0f);
        SoundManager::Instance().SetSfxGain(SfxVolume / 100.0f);
    }

    void DrawControlsSettings(angelsmenu::UiRect area) {
        float x = area.x, y = area.y;
        const float w = std::min(area.w, (float)FS(420));
        DrawText("Sensitivity", (int)x, (int)y, FS(12), angelsui::kTextDim);
        g_mouseSensitivity = angelsui::Slider({x + FS(90), y - FS(4), w - FS(90), FS(24)},
                                              TextFormat("%.2f", g_mouseSensitivity),
                                              g_mouseSensitivity, 0.1f, 5.0f);
        y += FS(32);
        if (Button({x, y, w, FS(26)}, g_invertY ? "Invert Y: ON" : "Invert Y: OFF", g_invertY))
            g_invertY = !g_invertY;
        y += FS(34);

        const int rowH = FS(24);
        const int listBottom = (int)(area.Bottom() - FS(36));
        int visible = (listBottom - (int)y) / rowH;
        if (visible < 3) visible = 3;
        static int scrollRow = 0;
        if (MouseIn({x, y, area.w, area.Bottom() - y})) {
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f) scrollRow -= (int)wheel;
        }
        const int maxScroll = std::max(0, Keybinds::kActionCount - visible);
        scrollRow = std::clamp(scrollRow, 0, maxScroll);
        BeginScissorMode((int)x, (int)y, (int)area.w, (int)(area.Bottom() - y));
        for (int r = 0; r < visible; ++r) {
            const int i = scrollRow + r;
            if (i >= Keybinds::kActionCount) break;
            const int rowY = (int)y + r * rowH;
            const auto& def = Keybinds::kDefs[i];
            DrawText(def.label, (int)(x + FS(6)), rowY + FS(5), FS(11), angelsui::kTextSoft);
            Rectangle keyBox = {area.Right() - FS(150), (float)rowY, FS(140), (float)rowH - FS(2)};
            const bool hover = MouseIn({keyBox.x, keyBox.y, keyBox.width, keyBox.height});
            const bool capturing = (m_capturingKey && m_captureAction == i);
            DrawRectangleRec(keyBox, capturing ? (Color){90, 70, 40, 255}
                                             : hover ? (Color){50, 70, 100, 255}
                                                     : (Color){20, 34, 60, 255});
            const std::string keyText = capturing ? "Press a key..."
                : Keybinds::KeyName(Keybinds::Key(def.action));
            DrawText(keyText.c_str(), (int)(keyBox.x + FS(6)), rowY + FS(5), FS(11),
                     capturing ? ORANGE : WHITE);
            if (!capturing && hover && Clickable() && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                m_capturingKey = true;
                m_captureAction = i;
                m_captureArmFrames = 2;
                m_suppressClick = true;
            }
        }
        EndScissorMode();

        if (m_capturingKey) {
            if (m_captureArmFrames > 0) m_captureArmFrames--;
            else {
                const int key = Keybinds::CapturePressedKey();
                if (key == KEY_ESCAPE) {
                    m_capturingKey = false;
                } else if (key >= 0) {
                    Keybinds::Assign(Keybinds::kDefs[m_captureAction].action, key);
                    m_capturingKey = false;
                }
            }
        }
        if (Button({x, area.Bottom() - FS(30), FS(130), FS(26)}, "Reset Defaults")) {
            Keybinds::ResetDefaults();
            m_capturingKey = false;
        }
        DrawText(m_capturingKey ? "Capture: press a key. Esc cancels."
                                : "Click a binding to change it. Conflicts swap.",
                 (int)(x + FS(140)), (int)(area.Bottom() - FS(22)), FS(10),
                 angelsui::kTextDim);
    }

    void DrawGameplaySettings(angelsmenu::UiRect area) {
        float x = area.x, y = area.y;
        const float w = std::min(area.w, (float)FS(420));
        if (Button({x, y, w, FS(28)}, WeaponHand ? "Weapon Hand: LEFT" : "Weapon Hand: RIGHT",
                   WeaponHand != 0)) {
            WeaponHand = WeaponHand ? 0 : 1;
            g_weaponHand = WeaponHand;
        }
        y += FS(36);
        if (Button({x, y, w, FS(28)}, GoreEnabled ? "Gore: ON" : "Gore: OFF", GoreEnabled)) {
            GoreEnabled = !GoreEnabled;
            g_goreEnabled = GoreEnabled;
        }
        y += FS(36);
        if (Button({x, y, w, FS(28)}, HeadBob ? "Head Bob: ON" : "Head Bob: OFF", HeadBob))
            HeadBob = !HeadBob;
        y += FS(40);
        DrawText("Crosshair", (int)x, (int)y, FS(13), angelsui::kText);
        y += FS(22);
        CrosshairStyleSel = angelsui::Cycle({x, y, w, FS(26)}, kCrosshairStyleNames,
                                            CROSSHAIR_STYLE_COUNT, CrosshairStyleSel);
        y += FS(34);
        CrosshairVisible = !Button({x, y, w, FS(26)},
                                   CrosshairVisible ? "Crosshair: ON" : "Crosshair: OFF",
                                   CrosshairVisible) ? CrosshairVisible : !CrosshairVisible;
        y += FS(34);
        CrosshairSize = (int)angelsui::Slider({x, y, w, FS(24)},
                                              TextFormat("Size  %d", CrosshairSize),
                                              (float)CrosshairSize, 2.0f, 48.0f);
        y += FS(32);
        CrosshairGap = (int)angelsui::Slider({x, y, w, FS(24)},
                                             TextFormat("Gap  %d", CrosshairGap),
                                             (float)CrosshairGap, 0.0f, 48.0f);
    }

    void DrawWindowSettings(angelsmenu::UiRect area) {
        float x = area.x, y = area.y;
        const float w = std::min(area.w, (float)FS(420));
        if (Button({x, y, w, FS(28)},
                   BorderlessWindow ? "Borderless (Alt+Drag to move)" : "Windowed (title bar)",
                   BorderlessWindow)) {
            BorderlessWindow = !BorderlessWindow;
            if (BorderlessWindow) SetWindowState(FLAG_WINDOW_UNDECORATED);
            else ClearWindowState(FLAG_WINDOW_UNDECORATED);
        }
        y += FS(40);
        DrawText("Window size is persisted to System/Angels95.ini.",
                 (int)x, (int)y, FS(11), angelsui::kTextDim);
        y += FS(20);
        DrawText(TextFormat("Current: %dx%d", GetScreenWidth(), GetScreenHeight()),
                 (int)x, (int)y, FS(11), angelsui::kTextDim);
    }

    // ---------------------------------------------------------------------
    // About / toast
    // ---------------------------------------------------------------------
    void DrawAboutModal() {
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 170});
        Rectangle panel = {(sw - FS(520)) * 0.5f, (sh - FS(330)) * 0.5f, FS(520), FS(330)};
        Card(panel, true);
        DrawText("ANGELS95", (int)(panel.x + FS(20)), (int)(panel.y + FS(18)),
                 FS(26), angelsui::kText);
        DrawText("Powered by the AngelTech Engine  //  raylib 5.5  //  C++20",
                 (int)(panel.x + FS(20)), (int)(panel.y + FS(52)), FS(11),
                 angelsui::kTextDim);
        DrawText("TribeWarez 2026  //  @EC, JF, ZT, NC, LC",
                 (int)(panel.x + FS(20)), (int)(panel.y + FS(70)), FS(11),
                 angelsui::kTextDim);
        const char* lines[] = {
            "",
            "Menu",
            "  Mouse / arrows / Enter, Esc goes back (F1 this page).",
            "  Play: Campaign and Practice.",
            "  Multiplayer: Join, Host, and Co-op.",
            "  Character: choose the model other players see.",
            "  Settings: player settings; developer settings are in-game.",
            "",
            "In-game",
            "  Move / Jump / Sprint / Crouch - rebindable in Settings > Controls.",
            "  Drop Item is G; Q is the spellbook.",
            "  Pause is Escape (rebindable).",
        };
        float y = panel.y + FS(96);
        for (const char* line : lines) {
            DrawText(line, (int)(panel.x + FS(20)), (int)y, FS(11),
                     line[0] ? angelsui::kTextSoft : angelsui::kTextDim);
            y += FS(15);
        }
        bool close = false;
        {
            const bool oldSuppress = m_suppressClick;
            m_suppressClick = false;
            close = Button({panel.x + panel.width - FS(120),
                            panel.y + panel.height - FS(44), FS(100), FS(30)},
                           "Close", true);
            m_suppressClick = oldSuppress;
        }
        if (close || IsKeyPressed(KEY_ESCAPE)) {
            m_showAbout = false;
        }
    }

    void DrawToast() {
        if (m_toast.empty() || m_toastTimer <= 0.0f) return;
        const int fs = FS(12);
        const int tw = MeasureText(m_toast.c_str(), fs);
        Rectangle r = {(GetScreenWidth() - tw) * 0.5f - FS(16),
                       (float)GetScreenHeight() - FS(92), (float)tw + FS(32), FS(32)};
        DrawRectangleRec(r, (Color){20, 50, 40, 235});
        DrawRectangleLinesEx(r, 1, (Color){80, 180, 120, 220});
        DrawText(m_toast.c_str(), (int)(r.x + FS(16)), (int)(r.y + FS(9)), fs,
                 (Color){200, 245, 220, 255});
    }
};
