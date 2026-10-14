#include "interface/EditorHooks.h"
#include "interface/Modular.h"   // RegisteringModule: entries remember their module
#include <algorithm>
#include <map>

namespace nuke {

static std::vector<EditorToggle>& Toggles() { static std::vector<EditorToggle> p; return p; }
static std::vector<EditorViewportOverlay>& Overlays() { static std::vector<EditorViewportOverlay> p; return p; }
// entry id -> owning module dll ("" = engine/editor own): purge support
static std::map<std::string, std::string>& ToggleOwner()  { static std::map<std::string, std::string> m; return m; }
static std::map<std::string, std::string>& OverlayOwner() { static std::map<std::string, std::string> m; return m; }
static std::map<std::string, std::string>& CurveOwner()   { static std::map<std::string, std::string> m; return m; }
static std::vector<EditorFieldButton>& FieldBtns() { static std::vector<EditorFieldButton> p; return p; }
static std::map<std::string, std::string>& FieldBtnOwner() { static std::map<std::string, std::string> m; return m; }

void RegisterFieldButton(const EditorFieldButton& b)
{
	if (b.id.empty() || b.typeName.empty() || b.field.empty() || !b.click) return;
	FieldBtnOwner()[b.id] = RegisteringModule();
	for (EditorFieldButton& e : FieldBtns())
		if (e.id == b.id) { e = b; return; }
	FieldBtns().push_back(b);
}
void UnregisterFieldButton(const std::string& id)
{
	FieldBtns().erase(std::remove_if(FieldBtns().begin(), FieldBtns().end(),
	                  [&](const EditorFieldButton& e) { return e.id == id; }), FieldBtns().end());
}
const std::vector<EditorFieldButton>& FieldButtons() { return FieldBtns(); }

static std::vector<EditorFieldWidget>& FieldWids() { static std::vector<EditorFieldWidget> p; return p; }
static std::map<std::string, std::string>& FieldWidOwner() { static std::map<std::string, std::string> m; return m; }
void RegisterFieldWidget(const EditorFieldWidget& w)
{
	if (w.id.empty() || w.typeName.empty() || w.field.empty() || !w.draw) return;
	FieldWidOwner()[w.id] = RegisteringModule();
	for (EditorFieldWidget& e : FieldWids())
		if (e.id == w.id) { e = w; return; }
	FieldWids().push_back(w);
}
void UnregisterFieldWidget(const std::string& id)
{
	FieldWids().erase(std::remove_if(FieldWids().begin(), FieldWids().end(),
	                  [&](const EditorFieldWidget& e) { return e.id == id; }), FieldWids().end());
}
const std::vector<EditorFieldWidget>& FieldWidgets() { return FieldWids(); }

void RegisterEditorToggle(const EditorToggle& t)
{
	if (t.id.empty() || !t.get || !t.set) return;
	ToggleOwner()[t.id] = RegisteringModule();
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
	OverlayOwner()[o.id] = RegisteringModule();
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
	CurveOwner()[typeName] = RegisteringModule();
	for (const std::string& c : Curves())
		if (c == typeName) return;
	Curves().push_back(typeName);
}

const std::vector<std::string>& CurveComponents() { return Curves(); }

void UnregisterEditorHooksOf(const std::string& moduleDll)
{
	if (moduleDll.empty()) return;
	auto owned = [&](std::map<std::string, std::string>& owner, const std::string& id)
	{
		auto it = owner.find(id);
		if (it == owner.end() || it->second != moduleDll) return false;
		owner.erase(it);
		return true;
	};
	Toggles().erase(std::remove_if(Toggles().begin(), Toggles().end(),
	                [&](const EditorToggle& e) { return owned(ToggleOwner(), e.id); }), Toggles().end());
	Overlays().erase(std::remove_if(Overlays().begin(), Overlays().end(),
	                 [&](const EditorViewportOverlay& e) { return owned(OverlayOwner(), e.id); }), Overlays().end());
	Curves().erase(std::remove_if(Curves().begin(), Curves().end(),
	               [&](const std::string& c) { return owned(CurveOwner(), c); }), Curves().end());
	FieldBtns().erase(std::remove_if(FieldBtns().begin(), FieldBtns().end(),
	                  [&](const EditorFieldButton& e) { return owned(FieldBtnOwner(), e.id); }), FieldBtns().end());
	FieldWids().erase(std::remove_if(FieldWids().begin(), FieldWids().end(),
	                  [&](const EditorFieldWidget& e) { return owned(FieldWidOwner(), e.id); }), FieldWids().end());
}

}  // namespace nuke
