#include "config.h"

#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace nuke {

namespace bfs = boost::filesystem;
using json = nlohmann::json;
using std::cout;
using std::endl;

#define PREFIX_CONF "[config]\t\t"

// --- small helpers -----------------------------------------------------------
static confColor jColor(const json& j)
{
    confColor c{ 1, 1, 1, 1 };
    if (j.is_object())
    {
        c.x = j.value("r", 1.0f);
        c.y = j.value("g", 1.0f);
        c.z = j.value("b", 1.0f);
        c.w = j.value("a", 1.0f);
    }
    return c;
}

static confUiVec jVec(const json& j)
{
    confUiVec v{ 0, 0 };
    if (j.is_object())
    {
        v.x = j.value("x", 0);
        v.y = j.value("y", 0);
    }
    return v;
}

static void loadTheme(NukeTheme* t, const json& j)
{
    t->WindowPadding     = jVec(j.value("WindowPadding",     json::object()));
    t->FramePadding      = jVec(j.value("FramePadding",      json::object()));
    t->ItemSpacing       = jVec(j.value("ItemSpacing",       json::object()));
    t->ItemInnerSpacing  = jVec(j.value("ItemInnerSpacing",  json::object()));

    t->WindowRounding    = j.value("WindowRounding",    0.0f);
    t->FrameRounding     = j.value("FrameRounding",     0.0f);
    t->IndentSpacing     = j.value("IndentSpacing",     0.0f);
    t->ScrollbarSize     = j.value("ScrollbarSize",     0.0f);
    t->ScrollbarRounding = j.value("ScrollbarRounding", 0.0f);
    t->GrabMinSize       = j.value("GrabMinSize",       0.0f);
    t->GrabRounding      = j.value("GrabRounding",      0.0f);

    auto col = [&](const char* k) { return jColor(j.value(k, json::object())); };
    t->ImGuiCol_Text                 = col("ImGuiCol_Text");
    t->ImGuiCol_TextDisabled         = col("ImGuiCol_TextDisabled");
    t->ImGuiCol_WindowBg             = col("ImGuiCol_WindowBg");
    t->ImGuiCol_ChildWindowBg        = col("ImGuiCol_ChildWindowBg");
    t->ImGuiCol_PopupBg              = col("ImGuiCol_PopupBg");
    t->ImGuiCol_Border               = col("ImGuiCol_Border");
    t->ImGuiCol_BorderShadow         = col("ImGuiCol_BorderShadow");
    t->ImGuiCol_FrameBg              = col("ImGuiCol_FrameBg");
    t->ImGuiCol_FrameBgHovered       = col("ImGuiCol_FrameBgHovered");
    t->ImGuiCol_FrameBgActive        = col("ImGuiCol_FrameBgActive");
    t->ImGuiCol_TitleBg              = col("ImGuiCol_TitleBg");
    t->ImGuiCol_TitleBgCollapsed     = col("ImGuiCol_TitleBgCollapsed");
    t->ImGuiCol_TitleBgActive        = col("ImGuiCol_TitleBgActive");
    t->ImGuiCol_MenuBarBg            = col("ImGuiCol_MenuBarBg");
    t->ImGuiCol_ScrollbarBg          = col("ImGuiCol_ScrollbarBg");
    t->ImGuiCol_ScrollbarGrab        = col("ImGuiCol_ScrollbarGrab");
    t->ImGuiCol_ScrollbarGrabHovered = col("ImGuiCol_ScrollbarGrabHovered");
    t->ImGuiCol_ScrollbarGrabActive  = col("ImGuiCol_ScrollbarGrabActive");
    t->ImGuiCol_CheckMark            = col("ImGuiCol_CheckMark");
    t->ImGuiCol_SliderGrab           = col("ImGuiCol_SliderGrab");
    t->ImGuiCol_SliderGrabActive     = col("ImGuiCol_SliderGrabActive");
    t->ImGuiCol_Button               = col("ImGuiCol_Button");
    t->ImGuiCol_ButtonHovered        = col("ImGuiCol_ButtonHovered");
    t->ImGuiCol_ButtonActive         = col("ImGuiCol_ButtonActive");
    t->ImGuiCol_Header               = col("ImGuiCol_Header");
    t->ImGuiCol_HeaderHovered        = col("ImGuiCol_HeaderHovered");
    t->ImGuiCol_HeaderActive         = col("ImGuiCol_HeaderActive");
    t->ImGuiCol_Column               = col("ImGuiCol_Column");
    t->ImGuiCol_ColumnHovered        = col("ImGuiCol_ColumnHovered");
    t->ImGuiCol_ColumnActive         = col("ImGuiCol_ColumnActive");
    t->ImGuiCol_ResizeGrip           = col("ImGuiCol_ResizeGrip");
    t->ImGuiCol_ResizeGripHovered    = col("ImGuiCol_ResizeGripHovered");
    t->ImGuiCol_ResizeGripActive     = col("ImGuiCol_ResizeGripActive");
    t->ImGuiCol_PlotLines            = col("ImGuiCol_PlotLines");
    t->ImGuiCol_PlotLinesHovered     = col("ImGuiCol_PlotLinesHovered");
    t->ImGuiCol_PlotHistogram        = col("ImGuiCol_PlotHistogram");
    t->ImGuiCol_PlotHistogramHovered = col("ImGuiCol_PlotHistogramHovered");
    t->ImGuiCol_TextSelectedBg       = col("ImGuiCol_TextSelectedBg");
    t->ImGuiCol_ModalWindowDarkening = col("ImGuiCol_ModalWindowDarkening");

    t->isLoaded = true;
}

Config* Config::getSingleton()
{
    static Config instance;
    static bool   loaded = false;
    if (!loaded) { instance.reload(&instance); loaded = true; }   // load once — NOT every call (was re-reading the file + log-spamming per frame)
    return &instance;
}

void Config::reload(Config* instance)
{
    bfs::path configDir("config");
    if (!bfs::exists(configDir))
        bfs::create_directory(configDir);

    bfs::path cfg("./config/main.json");
    bfs::ifstream f(cfg);
    if (!f)
    {
        cout << PREFIX_CONF << "config/main.json not found — using defaults." << endl;
        return;
    }

    // allow_exceptions = false, ignore_comments = true (so // and /* */ are tolerated).
    json root = json::parse(f, nullptr, false, true);
    if (root.is_discarded())
    {
        cout << PREFIX_CONF << "config/main.json: parse error — using defaults." << endl;
        return;
    }

    if (root.contains("window") && root["window"].is_object())
    {
        const json& w = root["window"];
        NukeWindow& win = instance->window = NukeWindow();
        win.w           = w.value("width",       win.w);
        win.h           = w.value("height",      win.h);
        win.mainFont    = w.value("mainFont",    win.mainFont);
        win.title       = w.value("title",       win.title);
        win.decorated   = w.value("decorated",   win.decorated);
        win.resizable   = w.value("resizable",   win.resizable);
        win.floating    = w.value("floating",    win.floating);
        win.maximized   = w.value("maximized",   win.maximized);
        win.fullscreen  = w.value("fullscreen",  win.fullscreen);
        // `mode` is authoritative; a config predating it derives from the legacy `fullscreen`
        // bool (true -> exclusive). `fullscreen` is then kept in sync with mode.
        win.mode        = w.value("mode", win.fullscreen ? 2 : 0);
        win.fullscreen  = win.mode != 0;
        win.transparent = w.value("transparent", win.transparent);
        win.opacity     = w.value("opacity",     win.opacity);
        win.backend     = w.value("backend",     win.backend);
        win.vsync       = w.value("vsync",       win.vsync);
        win.showConsole = w.value("showConsole", win.showConsole);
        cout << PREFIX_CONF << "Window size = [" << win.w << "x" << win.h << "]  backend=" << (win.backend == 1 ? "D3D12" : "D3D11") << endl;
        cout << PREFIX_CONF << "FONT IS " << win.mainFont << endl;
    }

    if (root.contains("theme") && root["theme"].is_object())
        loadTheme(&instance->theme, root["theme"]);

    instance->physicsCore  = root.value("physicsCore",  instance->physicsCore);
    instance->logToConsole = root.value("logToConsole", instance->logToConsole);

    if (root.contains("jobs") && root["jobs"].is_object())
    {
        const json& j = root["jobs"];
        instance->jobWorkers  = j.value("workers",  instance->jobWorkers);
        instance->jobPinCores = j.value("pinCores", instance->jobPinCores);
    }

    if (root.contains("raytracing") && root["raytracing"].is_object())
    {
        const json& rt = root["raytracing"];
        NukeRT& r = instance->rt = NukeRT();
        r.intensity   = rt.value("intensity",   r.intensity);
        r.maxDist     = rt.value("maxDist",     r.maxDist);
        r.bounces     = rt.value("bounces",     r.bounces);
        r.roughCutoff = rt.value("roughCutoff", r.roughCutoff);
    }
}

void Config::saveWindow()
{
    bfs::path configDir("config");
    boost::system::error_code ec;
    if (!bfs::exists(configDir, ec)) bfs::create_directory(configDir, ec);
    bfs::path cfg("./config/main.json");

    // Read-modify-write: preserve every other section (theme is large; raytracing/jobs too).
    json root = json::object();
    { bfs::ifstream f(cfg); if (f) { json parsed = json::parse(f, nullptr, false, true); if (!parsed.is_discarded() && parsed.is_object()) root = parsed; } }

    json& w = root["window"];   // creates the object if absent
    w["width"]       = window.w;
    w["height"]      = window.h;
    if (!window.mainFont.empty()) w["mainFont"] = window.mainFont;
    w["title"]       = window.title;
    w["decorated"]   = window.decorated;
    w["resizable"]   = window.resizable;
    w["floating"]    = window.floating;
    w["maximized"]   = window.maximized;
    w["mode"]        = window.mode;
    w["fullscreen"]  = window.fullscreen;   // legacy mirror (mode != 0)
    w["transparent"] = window.transparent;
    w["opacity"]     = window.opacity;
    w["backend"]     = window.backend;
    w["vsync"]       = window.vsync;
    w["showConsole"] = window.showConsole;

    bfs::ofstream out(cfg, std::ios::trunc);
    if (!out) { cout << PREFIX_CONF << "saveWindow: cannot write config/main.json" << endl; return; }
    out << root.dump(2);
    cout << PREFIX_CONF << "window config saved (mode=" << window.mode << ", " << window.w << "x" << window.h << ")" << endl;
}

void Config::SetConsoleWindowVisible(bool visible)
{
#ifdef _WIN32
    HWND con = GetConsoleWindow();
    if (!con) return;   // no console attached (e.g. windows-subsystem build)
    // NEVER hide a console we SHARE with a launching terminal — that would hide the user's
    // cmd/powershell. GetConsoleProcessList > 1 means another process is attached too.
    DWORD pids[4];
    DWORD n = GetConsoleProcessList(pids, 4);
    if (!visible && n > 1)
    {
        cout << PREFIX_CONF << "showConsole=false ignored (console shared with a terminal)" << endl;
        return;
    }
    ShowWindow(con, visible ? SW_SHOW : SW_HIDE);
#else
    (void)visible;
#endif
}

Config::Config()
{
    cout << PREFIX_CONF << "CWD: " << bfs::current_path() << endl;
}

Config::~Config() {}

}  // namespace nuke
