#pragma once
#ifndef NUKEE_EDITOR_HOOKS_H
#define NUKEE_EDITOR_HOOKS_H
#include "NukeAPI.h"
#include <functional>
#include <string>
#include <vector>

namespace nuke {

class Component;

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

// A small square icon button right BEHIND one reflected field's widget, in every inspector that
// draws reflected fields (world, prefab windows): a module's per-field action, e.g. a pin that
// writes the current value into the field. `click` gets the object the field belongs to and
// returns true when it changed it (the inspector marks the edit).
struct EditorFieldButton
{
	std::string id;                                  // unique key (unregister / re-register)
	std::string typeName;                            // TypeInfo name owning the field
	std::string field;                               // reflected field name
	std::string icon;                                // glyph (icon font, UTF-8)
	std::string tip;
	std::function<bool(void* obj)> click;
	std::function<void(void* obj)> update;           // optional: every frame the field's row is drawn, before the widget
};
// A module-drawn widget REPLACING the default one of a reflected field (a combo over a dynamic
// list, a custom picker). `draw` gets the owning object and the hidden ImGui id to use; the item
// width is already set. Returns true when it changed the object.
struct EditorFieldWidget
{
	std::string id;                                  // unique key (unregister / re-register)
	std::string typeName, field;
	std::function<bool(void* obj, const char* imguiId)> draw;
};
NUKEENGINE_API void RegisterFieldWidget(const EditorFieldWidget& w);
NUKEENGINE_API void UnregisterFieldWidget(const std::string& id);
NUKEENGINE_API const std::vector<EditorFieldWidget>& FieldWidgets();

NUKEENGINE_API void RegisterFieldButton(const EditorFieldButton& b);
NUKEENGINE_API void UnregisterFieldButton(const std::string& id);
NUKEENGINE_API const std::vector<EditorFieldButton>& FieldButtons();

// Purge every hook a module registered (see Modular.h PurgeModuleRegistrations).
NUKEENGINE_API void UnregisterEditorHooksOf(const std::string& moduleDll);

}  // namespace nuke

#endif // !NUKEE_EDITOR_HOOKS_H
