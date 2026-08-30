#pragma once
#ifndef NUKEE_EDITOR_HOOKS_H
#define NUKEE_EDITOR_HOOKS_H
#include "NukeAPI.h"
#include <functional>
#include <string>
#include <vector>

namespace nuke {

// Generic editor extension points: a module registers, the editor renders — the editor never
// knows who registered. Modules unregister by id in Shutdown (callbacks die with the DLL).

// A checkbox in the viewport view/snap popup.
struct EditorToggle
{
	std::string id;                     // unique key (used to unregister / re-register)
	std::string label;
	std::string tip;                    // tooltip, "" = none
	std::function<bool()> get;
	std::function<void(bool)> set;
};
NUKEENGINE_API void RegisterEditorToggle(const EditorToggle& t);
NUKEENGINE_API void UnregisterEditorToggle(const std::string& id);
NUKEENGINE_API const std::vector<EditorToggle>& EditorToggles();

// Viewport rect + editor-camera world->clip, handed to overlay hooks every frame the EDIT
// viewport draws (never while a game camera is possessed). Hooks run inside the viewport
// window, so ImGui::GetWindowDrawList() targets it.
struct EditorViewportCtx
{
	float x = 0, y = 0, w = 0, h = 0;   // viewport rect in screen coordinates
	float viewProj[16] = {};            // column-major
};
struct EditorViewportOverlay
{
	std::string id;
	std::function<void(const EditorViewportCtx&)> draw;
};
NUKEENGINE_API void RegisterViewportOverlay(const EditorViewportOverlay& o);
NUKEENGINE_API void UnregisterViewportOverlay(const std::string& id);
NUKEENGINE_API const std::vector<EditorViewportOverlay>& ViewportOverlays();

// Component types whose reflected `points` FloatList edits with the viewport curve gizmo
// (drag/insert/delete handles + undo). The engine Spline is built in; modules register theirs.
NUKEENGINE_API void RegisterCurveComponent(const std::string& typeName);
NUKEENGINE_API const std::vector<std::string>& CurveComponents();

// Purge every hook a module registered (see Modular.h PurgeModuleRegistrations).
NUKEENGINE_API void UnregisterEditorHooksOf(const std::string& moduleDll);

}  // namespace nuke

#endif // !NUKEE_EDITOR_HOOKS_H
