#pragma once
#ifndef NUKEE_IGUI_H
#define NUKEE_IGUI_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

// Neutral style identifiers: the game never sees imgui enums; the backend maps these onto its own.
enum NukeUIColor
{
	NUKEUI_COL_TEXT = 0,
	NUKEUI_COL_WINDOW_BG,
	NUKEUI_COL_FRAME_BG,          // input/checkbox/slider background
	NUKEUI_COL_FRAME_BG_HOVERED,
	NUKEUI_COL_FRAME_BG_ACTIVE,
	NUKEUI_COL_TITLE_BG,
	NUKEUI_COL_TITLE_BG_ACTIVE,
	NUKEUI_COL_BUTTON,
	NUKEUI_COL_BUTTON_HOVERED,
	NUKEUI_COL_BUTTON_ACTIVE,
	NUKEUI_COL_CHECK_MARK,
	NUKEUI_COL_SLIDER_GRAB,
	NUKEUI_COL_BORDER,
	NUKEUI_COL_SEPARATOR,
	NUKEUI_COL_PROGRESS,          // progress-bar fill
	NUKEUI_COL_COUNT
};

enum NukeUIStyleVar
{
	NUKEUI_VAR_ALPHA = 0,         // x (whole-UI opacity)
	NUKEUI_VAR_WINDOW_ROUNDING,   // x
	NUKEUI_VAR_FRAME_ROUNDING,    // x
	NUKEUI_VAR_GRAB_ROUNDING,     // x
	NUKEUI_VAR_BORDER_SIZE,       // x
	NUKEUI_VAR_WINDOW_PADDING,    // x, y
	NUKEUI_VAR_FRAME_PADDING,     // x, y
	NUKEUI_VAR_ITEM_SPACING,      // x, y
	NUKEUI_VAR_COUNT
};

// Immediate-mode runtime GUI the game codes against; implemented by a backend module. Call these
// inside Component::OnGUI(). End() must ALWAYS follow Begin(), even when Begin() returned false.
class NUKEENGINE_API iGUI
{
public:
	virtual ~iGUI() {}
	virtual bool Begin(const char* name) = 0;   // window; returns false if collapsed (skip its body)
	virtual void End() = 0;
	virtual void Text(const char* s) = 0;
	virtual bool Button(const char* s) = 0;      // true on click
	virtual void SameLine() = 0;
	virtual void Separator() = 0;
	virtual bool Checkbox(const char* label, bool* v) = 0;
	virtual bool SliderFloat(const char* label, float* v, float lo, float hi) = 0;

	// ABI: appended with no-op defaults so older backends stay valid.
	virtual bool InputText(const char* label, char* buf, int bufCap) { return false; }   // true while edited
	virtual bool Combo(const char* label, int* current, const char* const* items, int count) { return false; }
	virtual void Image(const char* texGuid, float w, float h) {}   // engine texture asset; 0x0 = native size
	virtual void ProgressBar(float fraction, const char* overlay) {}
	// Persistent styling (skinning): applies to everything drawn from now on.
	virtual void StyleColor(int what /*NukeUIColor*/, float r, float g, float b, float a) {}
	virtual void StyleVar(int what /*NukeUIStyleVar*/, float x, float y) {}
	virtual void FontScale(float s) {}
	virtual void ResetStyle() {}
	// Window placement for the NEXT Begin() (retained layer / scripted layouts).
	virtual void SetNextWindowRect(float x, float y, float w, float h) {}
};

// The active backend. GUI() NEVER returns null — a no-op stub stands in when none is registered.
NUKEENGINE_API void  SetGUIBackend(iGUI* backend);
NUKEENGINE_API iGUI* GUI();

// Reflected script surface over the runtime GUI, auto-bound as nuke.Gui.* by every scripting
// backend's generic static binder. Value-carrying widgets return the NEW value.
class NUKEENGINE_API Gui
{
	NUKE_CLASS_NOCREATE(Gui, Object)
public:
	[[nuke::func]] static bool   Begin(const std::string& name);
	[[nuke::func]] static void   End();
	[[nuke::func]] static void   Text(const std::string& text);
	[[nuke::func]] static bool   Button(const std::string& label);
	[[nuke::func]] static void   SameLine();
	[[nuke::func]] static void   Separator();
	[[nuke::func]] static bool   Checkbox(const std::string& label, bool value);
	[[nuke::func]] static double Slider(const std::string& label, double value, double lo, double hi);
	// Combo items are ';'-separated; index is 0-based.
	[[nuke::func]] static std::string Input(const std::string& label, const std::string& value);
	[[nuke::func]] static double      Combo(const std::string& label, double index, const std::string& items);
	[[nuke::func]] static void        Image(const std::string& texGuid, double w, double h);
	[[nuke::func]] static void        Progress(double fraction, const std::string& overlay);
	// Styling by name, mapped onto NukeUIColor/NukeUIStyleVar; unknown names warn once.
	// Colors: text windowBg frameBg frameBgHovered frameBgActive titleBg titleBgActive button
	// buttonHovered buttonActive checkMark sliderGrab border separator progress.
	// Vars: alpha windowRounding frameRounding grabRounding borderSize windowPadding
	// framePadding itemSpacing (padding/spacing take x,y).
	[[nuke::func]] static void StyleColor(const std::string& name, double r, double g, double b, double a);
	[[nuke::func]] static void StyleVar(const std::string& name, double x, double y);
	[[nuke::func]] static void FontScale(double s);
	[[nuke::func]] static void ResetStyle();
};

}  // namespace nuke

#endif // !NUKEE_IGUI_H
