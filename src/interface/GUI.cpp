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

}  // namespace nuke
