#include "config.h"

#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <iostream>

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
    instance.reload(&instance);
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
        win.transparent = w.value("transparent", win.transparent);
        win.opacity     = w.value("opacity",     win.opacity);
        cout << PREFIX_CONF << "Window size = [" << win.w << "x" << win.h << "]" << endl;
        cout << PREFIX_CONF << "FONT IS " << win.mainFont << endl;
    }

    if (root.contains("theme") && root["theme"].is_object())
        loadTheme(&instance->theme, root["theme"]);
}

Config::Config()
{
    cout << PREFIX_CONF << "CWD: " << bfs::current_path() << endl;
}

Config::~Config() {}

}  // namespace nuke
