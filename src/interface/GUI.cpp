#include "interface/iGUI.h"
#include <cctype>
#include <cstring>
#include <iostream>
#include <vector>

namespace nuke {

// No-op backend used when none is registered (edit mode, or NukeGUI not loaded).
// The v2 virtuals already default to no-ops in iGUI itself.
struct NullGUI : iGUI
{
	bool Begin(const char*) override { return false; }
	void End() override {}
	void Text(const char*) override {}
	bool Button(const char*) override { return false; }
	void SameLine() override {}
	void Separator() override {}
	bool Checkbox(const char*, bool*) override { return false; }
	bool SliderFloat(const char*, float*, float, float) override { return false; }
};

static NullGUI s_null;
static iGUI*   s_backend = nullptr;

void  SetGUIBackend(iGUI* backend) { s_backend = backend; }
iGUI* GUI() { return s_backend ? s_backend : &s_null; }

// --- reflected static facade (nuke.Gui.*) ---------------------------------------------
bool   Gui::Begin(const std::string& name)        { return GUI()->Begin(name.c_str()); }
void   Gui::End()                                 { GUI()->End(); }
void   Gui::Text(const std::string& text)         { GUI()->Text(text.c_str()); }
bool   Gui::Button(const std::string& label)      { return GUI()->Button(label.c_str()); }
void   Gui::SameLine()                            { GUI()->SameLine(); }
void   Gui::Separator()                           { GUI()->Separator(); }
bool   Gui::Checkbox(const std::string& label, bool value)
{
	GUI()->Checkbox(label.c_str(), &value);
	return value;
}
double Gui::Slider(const std::string& label, double value, double lo, double hi)
{
	float v = (float)value;
	GUI()->SliderFloat(label.c_str(), &v, (float)lo, (float)hi);
	return v;
}

// --- v2 widgets (2.5) -------------------------------------------------------------------
std::string Gui::Input(const std::string& label, const std::string& value)
{
	char buf[1024];
	const size_t n = value.size() < sizeof(buf) - 1 ? value.size() : sizeof(buf) - 1;
	std::memcpy(buf, value.data(), n);
	buf[n] = 0;
	GUI()->InputText(label.c_str(), buf, (int)sizeof(buf));
	return std::string(buf);
}

// Split ';'-separated items into a c-string array the seam takes (kept simple: a combo
// is a handful of entries, per-frame rebuild is nothing).
static std::vector<const char*> SplitItems(const std::string& items, std::vector<std::string>& keep)
{
	keep.clear();
	size_t start = 0;
	while (start <= items.size())
	{
		size_t e = items.find(';', start);
		if (e == std::string::npos) e = items.size();
		keep.push_back(items.substr(start, e - start));
		start = e + 1;
	}
	if (!keep.empty() && keep.back().empty()) keep.pop_back();   // trailing ';'
	std::vector<const char*> ptrs;
	ptrs.reserve(keep.size());
	for (const std::string& s : keep) ptrs.push_back(s.c_str());
	return ptrs;
}

double Gui::Combo(const std::string& label, double index, const std::string& items)
{
	std::vector<std::string> keep;
	std::vector<const char*> ptrs = SplitItems(items, keep);
	if (ptrs.empty()) return index;
	int cur = (int)index;
	if (cur < 0) cur = 0;
	if (cur >= (int)ptrs.size()) cur = (int)ptrs.size() - 1;
	GUI()->Combo(label.c_str(), &cur, ptrs.data(), (int)ptrs.size());
	return (double)cur;
}

void Gui::Image(const std::string& texGuid, double w, double h)
{
	GUI()->Image(texGuid.c_str(), (float)w, (float)h);
}

void Gui::Progress(double fraction, const std::string& overlay)
{
	GUI()->ProgressBar((float)fraction, overlay.c_str());
}

// --- styling by name ---------------------------------------------------------------------
static int ColorByName(const std::string& n)
{
	static const std::pair<const char*, int> map[] = {
		{ "text",           NUKEUI_COL_TEXT },
		{ "windowbg",       NUKEUI_COL_WINDOW_BG },
		{ "framebg",        NUKEUI_COL_FRAME_BG },
		{ "framebghovered", NUKEUI_COL_FRAME_BG_HOVERED },
		{ "framebgactive",  NUKEUI_COL_FRAME_BG_ACTIVE },
		{ "titlebg",        NUKEUI_COL_TITLE_BG },
		{ "titlebgactive",  NUKEUI_COL_TITLE_BG_ACTIVE },
		{ "button",         NUKEUI_COL_BUTTON },
		{ "buttonhovered",  NUKEUI_COL_BUTTON_HOVERED },
		{ "buttonactive",   NUKEUI_COL_BUTTON_ACTIVE },
		{ "checkmark",      NUKEUI_COL_CHECK_MARK },
		{ "slidergrab",     NUKEUI_COL_SLIDER_GRAB },
		{ "border",         NUKEUI_COL_BORDER },
		{ "separator",      NUKEUI_COL_SEPARATOR },
		{ "progress",       NUKEUI_COL_PROGRESS },
	};
	std::string low;
	low.reserve(n.size());
	for (char c : n) low += (char)std::tolower((unsigned char)c);
	for (const auto& m : map) if (low == m.first) return m.second;
	return -1;
}

static int VarByName(const std::string& n)
{
	static const std::pair<const char*, int> map[] = {
		{ "alpha",          NUKEUI_VAR_ALPHA },
		{ "windowrounding", NUKEUI_VAR_WINDOW_ROUNDING },
		{ "framerounding",  NUKEUI_VAR_FRAME_ROUNDING },
		{ "grabrounding",   NUKEUI_VAR_GRAB_ROUNDING },
		{ "bordersize",     NUKEUI_VAR_BORDER_SIZE },
		{ "windowpadding",  NUKEUI_VAR_WINDOW_PADDING },
		{ "framepadding",   NUKEUI_VAR_FRAME_PADDING },
		{ "itemspacing",    NUKEUI_VAR_ITEM_SPACING },
	};
	std::string low;
	low.reserve(n.size());
	for (char c : n) low += (char)std::tolower((unsigned char)c);
	for (const auto& m : map) if (low == m.first) return m.second;
	return -1;
}

void Gui::StyleColor(const std::string& name, double r, double g, double b, double a)
{
	const int what = ColorByName(name);
	if (what < 0) { std::cout << "[Gui]\t\tunknown style color '" << name << "'" << std::endl; return; }
	GUI()->StyleColor(what, (float)r, (float)g, (float)b, (float)a);
}

void Gui::StyleVar(const std::string& name, double x, double y)
{
	const int what = VarByName(name);
	if (what < 0) { std::cout << "[Gui]\t\tunknown style var '" << name << "'" << std::endl; return; }
	GUI()->StyleVar(what, (float)x, (float)y);
}

void Gui::FontScale(double s)   { GUI()->FontScale((float)s); }
void Gui::ResetStyle()          { GUI()->ResetStyle(); }

}  // namespace nuke
