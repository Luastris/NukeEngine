#pragma once
#ifndef NUKEE_API_IGUI_H
#define NUKEE_API_IGUI_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

// Retained/declarative runtime-UI layer (roadmap 2.5) on top of the immediate iGUI core.
//
// The game DECLARES widgets once (or keeps re-declaring — creation is idempotent by id)
// and polls events; the engine re-emits the whole tree through nuke::GUI() every frame
// (the GUI backend calls Ui::Emit() after the Component::OnGUI sweep). No imgui, no
// renderer types — this layer only speaks iGUI.
//
// Ids are user strings, globally unique. Widgets attach to a window by its id. Everything
// is safe from any thread (internal lock); scripts normally build the UI in start() and
// read events in update():
//   nuke.Ui.Window("hud", "HUD")
//   nuke.Ui.Button("hud.fire", "hud", "Fire!")
//   if nuke.Ui.Clicked("hud.fire") then ... end
class NUKEENGINE_API Ui
{
	NUKE_CLASS_NOCREATE(Ui, Object)
public:
	// --- declaration (create or update by id; parent = window id) ----------------------
	[[nuke::func]] static void Window(const std::string& id, const std::string& title);
	[[nuke::func]] static void Text(const std::string& id, const std::string& parent, const std::string& text);
	[[nuke::func]] static void Button(const std::string& id, const std::string& parent, const std::string& label);
	[[nuke::func]] static void Checkbox(const std::string& id, const std::string& parent, const std::string& label, bool value);
	[[nuke::func]] static void Slider(const std::string& id, const std::string& parent, const std::string& label,
	                                  double value, double lo, double hi);
	[[nuke::func]] static void Input(const std::string& id, const std::string& parent, const std::string& label,
	                                 const std::string& value);
	[[nuke::func]] static void Combo(const std::string& id, const std::string& parent, const std::string& label,
	                                 const std::string& items, double index);   // items ';'-separated, 0-based
	[[nuke::func]] static void Image(const std::string& id, const std::string& parent, const std::string& texGuid,
	                                 double w, double h);
	[[nuke::func]] static void Progress(const std::string& id, const std::string& parent, double fraction,
	                                    const std::string& overlay);
	[[nuke::func]] static void Separator(const std::string& id, const std::string& parent);

	// --- layout / lifetime --------------------------------------------------------------
	[[nuke::func]] static void SameLine(const std::string& id, bool on);       // widget joins the previous line
	[[nuke::func]] static void Show(const std::string& id, bool visible);      // window or widget
	[[nuke::func]] static void SetRect(const std::string& id, double x, double y, double w, double h);   // window
	[[nuke::func]] static void Remove(const std::string& id);                  // window removes its widgets too
	[[nuke::func]] static void Clear();                                        // drop the whole tree

	// --- state / events (Clicked & Changed latch until read) -----------------------------
	[[nuke::func]] static bool        Clicked(const std::string& id);          // button
	[[nuke::func]] static bool        Changed(const std::string& id);          // checkbox/slider/input/combo
	[[nuke::func]] static double      Value(const std::string& id);            // slider/combo index/checkbox 0|1/progress
	[[nuke::func]] static std::string TextOf(const std::string& id);           // input value / text content
	[[nuke::func]] static void        SetValue(const std::string& id, double v);
	[[nuke::func]] static void        SetText(const std::string& id, const std::string& text);   // text/input/overlay
	[[nuke::func]] static void        SetLabel(const std::string& id, const std::string& label);

	// Re-emit the whole tree through the immediate backend. Called by the GUI plugin once
	// per frame (after the OnGUI component sweep); harmless to call with no backend.
	static void Emit();
};

}  // namespace nuke

#endif // !NUKEE_API_IGUI_H
