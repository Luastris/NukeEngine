#ifndef CONFIG_H
#define CONFIG_H
#include <boost/filesystem/path.hpp>
#include "NukeAPI.h"
#include <string>

namespace nuke {

// Window display mode (Game.SetWindowMode / NukeWindow::mode). Reflected; stored as int on the
// POD config / WindowDesc so it crosses the render seam and JSON as a plain number.
enum class WindowMode : int
{
    Windowed             = 0,   // regular window (decoration per `decorated`)
    BorderlessFullscreen = 1,   // undecorated window covering the monitor at desktop res (no mode switch)
    ExclusiveFullscreen  = 2,   // real fullscreen; the monitor switches to the window's resolution
};

struct NukeWindow{
    int w = 1280, h = 720;
    std::string mainFont;
    // OS window properties, applied by the renderer at window creation. No `title` here — the
    // window title is not config (the game window carries the project name, bound at packaging).
    bool  decorated   = true;    // false => borderless
    bool  resizable   = true;
    bool  floating    = false;   // always-on-top
    bool  maximized   = false;
    // 0 = windowed, 1 = borderless fullscreen, 2 = exclusive fullscreen. main.json "mode" is
    // serialized as the readable word "windowed"/"borderless"/"exclusive" (legacy forms still load).
    int   mode        = 0;
    bool  fullscreen  = false;   // internal mirror of mode != 0 (cross-DLL layout); never serialized
    bool  transparent = false;   // per-pixel alpha (creation-time; applied on next launch)
    float opacity     = 1.0f;    // whole-window opacity 0..1
    int   backend     = 0;       // 0 = D3D11, 1 = D3D12 (ray tracing), 2 = Vulkan (restart to apply)
    bool  showConsole = true;    // show the process's own OS console window; a console shared with a launching terminal is never hidden
    bool hierarchy = true,
            console = true,
            browser = true,
            plugmgr = false,
            about = true,
            inspector = true,
            render = true;
    // ABI: cross-DLL struct — new fields are APPENDED at the END, never inserted mid-struct.
    bool  rayTracing  = true;    // false = force the raster path even on RT-capable GPUs ("rayTracing")
    bool  showFps     = true;    // Player: append "N FPS (x.x ms)" to the window title ("showFps")
    bool  vsync       = true;    // cap the main present to the display refresh ("vsync"); live-toggleable
};

// Engine-wide ray-tracing reflection quality, persisted to config/main.json ["raytracing"].
struct NukeRT{
    float intensity   = 1.0f;     // reflection strength
    float maxDist     = 100.0f;   // max ray distance (world units)
    int   bounces     = 3;        // recursion depth (mirror-in-mirror; 1 = single reflection)
    float roughCutoff = 0.6f;     // reflections fade out toward this roughness (sharp RT = smooth surfaces)
};

struct confUiVec{
      int x,y;
};


struct confColor{
      float x,y,z,w;
};


struct NukeTheme{
    bool isLoaded = false;

    struct confUiVec WindowPadding;
    struct confUiVec FramePadding;
    struct confUiVec ItemSpacing;
    struct confUiVec ItemInnerSpacing;
    float WindowRounding;
    float FrameRounding;
    float IndentSpacing;
    float ScrollbarSize;
    float ScrollbarRounding;
    float GrabMinSize;
    float GrabRounding;

    struct confColor ImGuiCol_Text;
    struct confColor ImGuiCol_TextDisabled;
    struct confColor ImGuiCol_WindowBg;
    struct confColor ImGuiCol_ChildWindowBg;
    struct confColor ImGuiCol_PopupBg;
    struct confColor ImGuiCol_Border;
    struct confColor ImGuiCol_BorderShadow;
    struct confColor ImGuiCol_FrameBg;
    struct confColor ImGuiCol_FrameBgHovered;
    struct confColor ImGuiCol_FrameBgActive;
    struct confColor ImGuiCol_TitleBg;
    struct confColor ImGuiCol_TitleBgCollapsed;
    struct confColor ImGuiCol_TitleBgActive;
    struct confColor ImGuiCol_MenuBarBg;
    struct confColor ImGuiCol_ScrollbarBg;
    struct confColor ImGuiCol_ScrollbarGrab;
    struct confColor ImGuiCol_ScrollbarGrabHovered;
    struct confColor ImGuiCol_ScrollbarGrabActive;
    //struct confColor ImGuiCol_ComboBg;
    struct confColor ImGuiCol_CheckMark;
    struct confColor ImGuiCol_SliderGrab;
    struct confColor ImGuiCol_SliderGrabActive;
    struct confColor ImGuiCol_Button;
    struct confColor ImGuiCol_ButtonHovered;
    struct confColor ImGuiCol_ButtonActive;
    struct confColor ImGuiCol_Header;
    struct confColor ImGuiCol_HeaderHovered;
    struct confColor ImGuiCol_HeaderActive;
    struct confColor ImGuiCol_Column;
    struct confColor ImGuiCol_ColumnHovered;
    struct confColor ImGuiCol_ColumnActive;
    struct confColor ImGuiCol_ResizeGrip;
    struct confColor ImGuiCol_ResizeGripHovered;
    struct confColor ImGuiCol_ResizeGripActive;
    //struct confColor ImGuiCol_CloseButton;
    //struct confColor ImGuiCol_CloseButtonHovered;
    //struct confColor ImGuiCol_CloseButtonActive;
    struct confColor ImGuiCol_PlotLines;
    struct confColor ImGuiCol_PlotLinesHovered;
    struct confColor ImGuiCol_PlotHistogram;
    struct confColor ImGuiCol_PlotHistogramHovered;
    struct confColor ImGuiCol_TextSelectedBg;
    struct confColor ImGuiCol_ModalWindowDarkening;
};

class NUKEENGINE_API Config
{
private:
	Config();
	~Config();
public:
    NukeWindow window {};
    NukeTheme theme{};
    NukeRT    rt{};
    // Core the fixed-update (physics) thread is pinned to. -1 = auto (last core), -2 = no pinning.
    int physicsCore = -1;
    // Job workers (-1 = auto: one per core, minus core 0 and the physics core) + per-core pinning.
    int  jobWorkers  = -1;
    bool jobPinCores = true;
    // Echo logs to the OS console. Windows console writes are slow enough to cost frame time under
    // heavy logging; false stops the echo (the editor's in-app Console panel is unaffected).
    bool logToConsole = true;
    // D3D12 GPU validation layer + DRED breadcrumbs (Debug builds only; heavy per-command cost).
    // Enabled by either this flag or the NUKE_GPU_VALIDATION env var.
    bool gpuValidation = false;
	void reload(Config* instance);
	// Show/hide the process's OWN OS console window. No-op if the console is SHARED with a
	// launching terminal (>1 attached process), and no-op off Windows.
	static void SetConsoleWindowVisible(bool visible);
	// Persist config/main.json, updating ONLY the "window" object and leaving every other
	// section exactly as on disk.
	void saveWindow();
	// Same, into an arbitrary json file.
	void saveWindowTo(const std::string& path);
	// The directory config/ resolves against — NEXT TO THE EXECUTABLE, never the CWD.
	static boost::filesystem::path baseDir();
	static Config* getSingleton();
};
}  // namespace nuke

#endif // CONFIG_H
