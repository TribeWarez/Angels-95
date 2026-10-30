// =============================================================================
// Core/EditorMenus.cpp
//
// The native menu bar construction.
//
// FRAGMENT - not a standalone translation unit. Included by Main.cpp, which is the
// single TU for AngelEd's core layer. See Wiki/Editor-Architecture-Refactor.md.
// =============================================================================

#include "../EditorKeys.hpp"

static void CreateEditorMenuBar() {
#ifdef _WIN32
    HWND hWnd = (HWND)GetWindowHandle();
    if (!hWnd) return;

    // Menu hints come from the SAME table the shortcuts read, so a rebind cannot
    // leave the menu advertising a key that no longer does anything.
    auto menuLabel = [](const char* text, EdAction a) {
        return std::string(text) + "\t" + EdKeys::ShortcutText(a);
    };

    // Subclass the raylib/GLFW window so we can intercept menu WM_COMMAND
    g_originalWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)EditorWndProc);

    HMENU hMenu = CreateMenu();
    HMENU hFile = CreatePopupMenu();
    AppendMenuA(hFile, MF_STRING, IDM_NEW, menuLabel("&New", EdAction::New).c_str());
    AppendMenuA(hFile, MF_STRING, IDM_OPEN, menuLabel("&Open...", EdAction::Open).c_str());
    AppendMenuA(hFile, MF_STRING, IDM_SAVE, menuLabel("&Save", EdAction::Save).c_str());
    AppendMenuA(hFile, MF_STRING, IDM_SAVE_AS, menuLabel("Save &As...", EdAction::SaveAs).c_str());
    AppendMenuA(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFile, MF_STRING, IDM_PLAY_TEST, menuLabel("&Play Test", EdAction::Play).c_str());
    AppendMenuA(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFile, MF_STRING, IDM_EXIT, menuLabel("E&xit", EdAction::Exit).c_str());
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hFile, "&File");

    HMENU hEdit = CreatePopupMenu();
    AppendMenuA(hEdit, MF_STRING, IDM_UNDO, menuLabel("&Undo", EdAction::Undo).c_str());
    AppendMenuA(hEdit, MF_STRING, IDM_REDO, menuLabel("&Redo", EdAction::Redo).c_str());
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hEdit, "&Edit");

    HMENU hView = CreatePopupMenu();
    AppendMenuA(hView, MF_STRING, IDM_MODEL_BRW, menuLabel("Model &Browser", EdAction::ModelBrowser).c_str());
    AppendMenuA(hView, MF_STRING, IDM_SOUND_MGR, menuLabel("&Sound Manager", EdAction::SoundManager).c_str());
    AppendMenuA(hView, MF_STRING, IDM_TEXTURE_MGR, menuLabel("&Texture Manager", EdAction::TextureManager).c_str());
    AppendMenuA(hView, MF_STRING, IDM_PAWN_MGR, menuLabel("&Pawn Manager", EdAction::PawnManager).c_str());
    AppendMenuA(hView, MF_STRING, IDM_SCRIPT_MGR, menuLabel("&Script Manager", EdAction::ScriptManager).c_str());
    AppendMenuA(hView, MF_STRING, IDM_MATERIAL_GRAPH, "Material &Graph Explorer");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    // No Zone Properties / Light Properties entries. Their per-zone rows live in
    // Entity Properties (right-click a zone or light) and their level-state rows
    // in the WorldGraph "Map (level)" row. Both old windows duplicated that state
    // in a global that Apply could silently drop.
    AppendMenuA(hView, MF_STRING, IDM_NODE_PANEL, "&Node Panel");
    AppendMenuA(hView, MF_STRING, IDM_PICKUP_PANEL, menuLabel("&Pickups", EdAction::PickupPanel).c_str());
    AppendMenuA(hView, MF_STRING, IDM_HEIGHTMAP, menuLabel("&Heightmap Editor", EdAction::Heightmap).c_str());
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING, IDM_WORLD_GRAPH, menuLabel("World &Hierarchy", EdAction::Hierarchy).c_str());
    AppendMenuA(hView, MF_STRING, IDM_LEVEL_LIST, "Level &List / Campaign");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    // Lit-preview environment switches. Checkable, and persisted in [Views];
    // they only mask the shader uniforms, so a user can inspect geometry in a
    // dark/foggy level without editing its zone or level values.
    AppendMenuA(hView, MF_STRING, IDM_VIEW_AMBIENT, "Show &Ambient");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_FOG, "Show &Fog");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    // Panel geometry is user data (System/AngelEd.ini [Layout]), written when the
    // editor exits. Save makes that explicit - it is the only way to keep a layout
    // you just arranged without quitting - and Reset restores the built-ins. Both
    // live here because they are a pair, and because "panel layout" is a View
    // concern, not an editor preference like Unreal-style input.
    AppendMenuA(hView, MF_STRING, IDM_SAVE_LAYOUT, "&Save Pane Layout");
    AppendMenuA(hView, MF_STRING, IDM_RESET_LAYOUT, "&Reset Pane Layout");
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hView, "&View");

    // Tools. The material graph is authored as text today; these two make the
    // bake pipeline usable end-to-end from the editor. The node canvas lands in
    // its own panel next to them.
    HMENU hTools = CreatePopupMenu();
    AppendMenuA(hTools, MF_STRING, IDM_BAKE_MATERIALS, "&Bake All Materials");
    AppendMenuA(hTools, MF_STRING, IDM_RELOAD_MATERIALS, "&Reload Material Textures");
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hTools, "&Tools");

    HMENU hCam = CreatePopupMenu();
    AppendMenuA(hCam, MF_STRING, IDM_RESET_CAM, "&Reset Camera\tHome");
    AppendMenuA(hCam, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hCam, MF_STRING, IDM_VIEW_TOP, "&Top\tNumpad 7");
    AppendMenuA(hCam, MF_STRING, IDM_VIEW_BOTTOM, "&Bottom\tNumpad 1");
    AppendMenuA(hCam, MF_STRING, IDM_VIEW_RIGHT, "&Right\tNumpad 3");
    AppendMenuA(hCam, MF_STRING, IDM_VIEW_LEFT, "&Left\tNumpad 9");
    AppendMenuA(hCam, MF_STRING, IDM_VIEW_PERSPECTIVE, "&Perspective\tNumpad 5");
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hCam, "&Camera");

    HMENU hSettings = CreatePopupMenu();
    AppendMenuA(hSettings, MF_STRING, IDM_FULLSCREEN, "Toggle &Fullscreen\tF11");
    AppendMenuA(hSettings, MF_STRING, IDM_MAXIMIZE, "&Maximize Window (Windowed Full Size)");
    AppendMenuA(hSettings, MF_SEPARATOR, 0, NULL);
    // UnrealEd-style input: mouse+modifier transforms instead of (or alongside)
    // the gizmo. Opt-in because it takes Ctrl/Shift away from the gizmo's own
    // modifiers, and because 1/2/3 become movement speed rather than placement
    // modes while it is on.
    AppendMenuA(hSettings, MF_STRING, IDM_UNREAL_MOVE, "&Unreal-Style Brush Movement");
    HMENU hSpeed = CreatePopupMenu();
    AppendMenuA(hSpeed, MF_STRING, IDM_UNREAL_SPEED_SLOW, "&Slow\t1");
    AppendMenuA(hSpeed, MF_STRING, IDM_UNREAL_SPEED_MED,  "&Medium\t2");
    AppendMenuA(hSpeed, MF_STRING, IDM_UNREAL_SPEED_FAST, "&Fast\t3");
    AppendMenuA(hSettings, MF_POPUP, (UINT_PTR)hSpeed, "Brush Move &Speed");
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hSettings, "&Settings");

    HMENU hHelp = CreatePopupMenu();
    AppendMenuA(hHelp, MF_STRING, IDM_KEYMAP, "&Keyboard Shortcuts...");
    AppendMenuA(hHelp, MF_STRING, IDM_ABOUT, "&About AngelEd");
    AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hHelp, "&Help");

    SetMenu(hWnd, hMenu);
#endif
}
