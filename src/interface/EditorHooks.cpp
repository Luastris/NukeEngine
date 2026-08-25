#include "interface/EditorHooks.h"
#include <algorithm>

namespace nuke {

static std::vector<EditorToggle>& Toggles() { static std::vector<EditorToggle> p; return p; }
static std::vector<EditorViewportOverlay>& Overlays() { static std::vector<EditorViewportOverlay> p; return p; }

void RegisterEditorToggle(const EditorToggle& t)
{
	if (t.id.empty() || !t.get || !t.set) return;
	for (EditorToggle& e : Toggles())
		if (e.id == t.id) { e = t; return; }   // re-register (hot reload) = replace
	Toggles().push_back(t);
}

void UnregisterEditorToggle(const std::string& id)
{
	Toggles().erase(std::remove_if(Toggles().begin(), Toggles().end(),
	                [&](const EditorToggle& e) { return e.id == id; }), Toggles().end());
}

const std::vector<EditorToggle>& EditorToggles() { return Toggles(); }

void RegisterViewportOverlay(const EditorViewportOverlay& o)
{
	if (o.id.empty() || !o.draw) return;
	for (EditorViewportOverlay& e : Overlays())
		if (e.id == o.id) { e = o; return; }
	Overlays().push_back(o);
}

void UnregisterViewportOverlay(const std::string& id)
{
	Overlays().erase(std::remove_if(Overlays().begin(), Overlays().end(),
	                 [&](const EditorViewportOverlay& e) { return e.id == id; }), Overlays().end());
}

const std::vector<EditorViewportOverlay>& ViewportOverlays() { return Overlays(); }

static std::vector<std::string>& Curves() { static std::vector<std::string> p; return p; }

void RegisterCurveComponent(const std::string& typeName)
{
	if (typeName.empty()) return;
	for (const std::string& c : Curves())
		if (c == typeName) return;
	Curves().push_back(typeName);
}

const std::vector<std::string>& CurveComponents() { return Curves(); }

}  // namespace nuke
