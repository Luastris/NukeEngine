#include "config.h"
#include "interface/Modular.h"   // RunRoot(): config/ lives in the run root

#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
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
    if (!loaded) { instance.reload(&instance); loaded = true; }   // load once, not on every call
    return &instance;
}

// config/ lives NEXT TO THE EXECUTABLE, never in the working directory (a game launched from
// a shortcut has an arbitrary CWD). Falls back to the CWD only if the exe path is unavailable.
bfs::path Config::userDataDir()
{
#ifdef _WIN32
    if (const char* e = std::getenv("APPDATA"))
        if (*e) return bfs::path(e);
#elif defined(__APPLE__)
    if (const char* e = std::getenv("HOME"))
        if (*e) return bfs::path(e) / "Library" / "Application Support";
#else
    if (const char* e = std::getenv("XDG_CONFIG_HOME"))
        if (*e) return bfs::path(e);
    if (const char* e = std::getenv("HOME"))
        if (*e) return bfs::path(e) / ".config";
#endif
    return bfs::path(".");   // last resort: portable-install style, next to the exe/CWD
}

bfs::path Config::writableDir()
{
    static bfs::path cached = []() -> bfs::path
    {
#ifndef _WIN32
        const bfs::path root = RunRoot();
        bool redirect = false;
        // An INSTALLED app must not write beside (or inside) its bundle: /Applications is
        // shared between every engine app, and bundle contents are signed. A run root
        // nested in a .app means exactly that layout.
        for (bfs::path p = root; !p.empty() && p != p.parent_path(); p = p.parent_path())
            if (p.extension() == ".app") { redirect = true; break; }
        if (!redirect)
        {
            // Not bundle-nested (dev tree / portable dir): probe writability — a system
            // install (/usr/share, /opt) is read-only and redirects too.
            boost::system::error_code ec;
            const bfs::path probe = root / ".nuke_write_probe";
            bfs::ofstream pf(probe, std::ios::trunc);
            if (pf) { pf.close(); bfs::remove(probe, ec); }
            else redirect = true;
        }
        if (redirect)
        {
            boost::system::error_code ec;
            bfs::path exe = boost::dll::program_location(ec);
            const std::string app = ec ? std::string("NukeEngine") : exe.stem().string();
            return userDataDir() / app;
        }
#endif
        return baseDir();   // Windows + dev trees: the run root, exactly as before
    }();
    return cached;
}

bfs::path Config::baseDir()
{
    // Run root = exe dir, except a macOS thin bundle resolves to the dir holding the .app
    // (config/ sits in the run dir next to it) — see RunRoot() in Modular.cpp.
    bfs::path root = RunRoot();
    if (root.empty()) return bfs::path(".");
    return root;
}

void Config::reload(Config* instance)
{
    boost::system::error_code ec;
    bfs::create_directories(writableDir() / "config", ec);

    // Saved (per-machine) config wins; the SHIPPED config beside the binaries is the
    // fallback — an installed bundle carries defaults it can never write to.
    bfs::path cfg = writableDir() / "config" / "main.json";
    if (!bfs::exists(cfg, ec))
        cfg = baseDir() / "config" / "main.json";
    bfs::ifstream f(cfg);
    if (!f)
    {
        cout << PREFIX_CONF << cfg.string() << " not found — using defaults." << endl;
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
        // (a legacy "title" key is ignored — the window title is not config, see NukeWindow)
        win.decorated   = w.value("decorated",   win.decorated);
        win.resizable   = w.value("resizable",   win.resizable);
        win.floating    = w.value("floating",    win.floating);
        win.maximized   = w.value("maximized",   win.maximized);
        // `mode` is the ONLY display-mode key, human-readable ("windowed"/"borderless"/
        // "exclusive"); a legacy 0/1/2 number or `fullscreen` bool is still accepted.
        if (w.contains("mode") && w["mode"].is_string())
        {
            const std::string m = w["mode"].get<std::string>();
            win.mode = (m == "borderless") ? 1 : (m == "exclusive") ? 2 : 0;
        }
        else if (w.contains("mode") && w["mode"].is_number())
            win.mode = w["mode"].get<int>();
        else
            win.mode = w.value("fullscreen", false) ? 2 : 0;
        win.fullscreen  = win.mode != 0;   // internal mirror only — never serialized
        win.transparent = w.value("transparent", win.transparent);
        win.opacity     = w.value("opacity",     win.opacity);
        win.backend     = w.value("backend",     win.backend);
        win.rayTracing  = w.value("rayTracing",  win.rayTracing);
        win.showFps     = w.value("showFps",     win.showFps);
        win.vsync       = w.value("vsync",       win.vsync);
        win.showConsole = w.value("showConsole", win.showConsole);
        cout << PREFIX_CONF << "Window size = [" << win.w << "x" << win.h << "]  backend="
             << (win.backend == 1 ? "D3D12" : win.backend == 2 ? "Vulkan" : "D3D11") << endl;
        cout << PREFIX_CONF << "FONT IS " << win.mainFont << endl;
    }

    if (root.contains("theme") && root["theme"].is_object())
        loadTheme(&instance->theme, root["theme"]);

    instance->physicsCore  = root.value("physicsCore",  instance->physicsCore);
    instance->logToConsole = root.value("logToConsole", instance->logToConsole);
    instance->gpuValidation = root.value("gpuValidation", instance->gpuValidation);

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
    boost::system::error_code ec;
    bfs::create_directories(writableDir() / "config", ec);
    // Saving from shipped defaults: carry the full file over so unrelated sections survive.
    const bfs::path dst = writableDir() / "config" / "main.json";
    if (!bfs::exists(dst, ec) && bfs::exists(baseDir() / "config" / "main.json", ec))
        bfs::copy_file(baseDir() / "config" / "main.json", dst, ec);
    saveWindowTo(dst.string());
}

// Persist the window block into an arbitrary json file (read-modify-write). In the EDITOR
// Game.Set* persists nothing — the shipped config is formed by the Package Project dialog.
void Config::saveWindowTo(const std::string& path)
{
    bfs::path cfg(path);
    boost::system::error_code ec;
    if (cfg.has_parent_path()) bfs::create_directories(cfg.parent_path(), ec);

    // Read-modify-write: preserve every other section (theme is large; raytracing/jobs too).
    json root = json::object();
    { bfs::ifstream f(cfg); if (f) { json parsed = json::parse(f, nullptr, false, true); if (!parsed.is_discarded() && parsed.is_object()) root = parsed; } }

    json& w = root["window"];   // creates the object if absent
    w["width"]       = window.w;
    w["height"]      = window.h;
    if (!window.mainFont.empty()) w["mainFont"] = window.mainFont;
    w.erase("title");   // legacy key: the window title is not config
    w["decorated"]   = window.decorated;
    w["resizable"]   = window.resizable;
    w["floating"]    = window.floating;
    w["maximized"]   = window.maximized;
    // Display mode is written as the WORD a user can read and edit; the legacy bool is erased.
    w["mode"]        = window.mode == 1 ? "borderless" : window.mode == 2 ? "exclusive" : "windowed";
    w.erase("fullscreen");
    w["transparent"] = window.transparent;
    w["opacity"]     = window.opacity;
    w["backend"]     = window.backend;
    w["rayTracing"]  = window.rayTracing;
    w["showFps"]     = window.showFps;
    w["vsync"]       = window.vsync;
    w["showConsole"] = window.showConsole;

    bfs::ofstream out(cfg, std::ios::trunc);
    if (!out) { cout << PREFIX_CONF << "saveWindow: cannot write " << path << endl; return; }
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
    // The build's architecture set (stamped at compile time) + the slice actually running —
    // printed every boot so nobody ever guesses what binary this is.
    {
#if defined(__arm64__) || defined(_M_ARM64)
        const char* running = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
        const char* running = "x86_64";
#else
        const char* running = "unknown";
#endif
#ifdef NUKE_BUILD_ARCHS
        cout << PREFIX_CONF << "build: " << NUKE_BUILD_ARCHS << " (running " << running << ")" << endl;
#else
        cout << PREFIX_CONF << "build: running " << running << endl;
#endif
    }
    cout << PREFIX_CONF << "CWD: " << bfs::current_path() << endl;
}

Config::~Config() {}

}  // namespace nuke
