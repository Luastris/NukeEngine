#include "interface/iGUI.h"

namespace nuke {

// No-op backend used when none is registered (edit mode, or NukeGUI not loaded).
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

}  // namespace nuke
