#ifndef CONFIG_H
#define CONFIG_H
#include "NukeAPI.h"
#include <string>

namespace nuke {

struct NukeWindow{
    int w = 1280, h = 720;
    std::string mainFont;
    // OS window properties (applied by the renderer at window creation). Mostly for
    // the game window; the editor leaves them at defaults (normal decorated window).
    std::string title = "NukeEngine";
    bool  decorated   = true;    // false => borderless
    bool  resizable   = true;
    bool  floating    = false;   // always-on-top
    bool  maximized   = false;
    bool  fullscreen  = false;
    bool  transparent = false;   // per-pixel alpha (needs renderer DComp support)
    float opacity     = 1.0f;    // whole-window opacity 0..1
    int   backend     = 0;       // render backend: 0 = D3D11, 1 = D3D12 (D3D12 enables ray tracing; restart to apply)
    bool hierarchy = true,
            console = true,
            browser = true,
            plugmgr = false,
            about = true,
            inspector = true,
            render = true;
};

// Global ray-tracing (RTX) reflection settings — engine-wide quality knobs, edited in Project Settings,
// persisted to config/main.json ["raytracing"]. The per-camera PostProcess "rtreflect" effect is just the
// on/off switch (which camera traces); these control HOW it traces.
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
    // CPU core the fixed-update (physics) thread is pinned to. -1 = auto (the LAST core,
    // away from core 0 where the OS/main thread lives); -2 = no pinning. config/main.json
    // "physicsCore".
    int physicsCore = -1;
	void reload(Config* instance);
	static Config* getSingleton();
};
}  // namespace nuke

#endif // CONFIG_H
