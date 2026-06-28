#pragma once
#ifndef NUKEE_IGUI_H
#define NUKEE_IGUI_H
#include "NukeAPI.h"

namespace nuke {

// Immediate-mode runtime GUI interface the GAME codes against (declared by the engine, brought to life
// by a backend module — NukeGUI). Backend-agnostic: the game never depends on imgui or the renderer.
// Call these inside Component::OnGUI(). Extend with more widgets as needed.
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
};

// The active backend. GUI() NEVER returns null — a no-op stub is used when no backend is registered
// (so game code calling GUI() in edit mode / without NukeGUI loaded is safe).
NUKEENGINE_API void  SetGUIBackend(iGUI* backend);
NUKEENGINE_API iGUI* GUI();

}  // namespace nuke

#endif // !NUKEE_IGUI_H
