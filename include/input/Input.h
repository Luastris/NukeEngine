#pragma once
#ifndef NUKEE_INPUT_H
#define NUKEE_INPUT_H
#include "NukeAPI.h"
#include "input/InputTypes.h"
#include "API/Model/Vector.h"   // Vector2 (Axis2 actions)
#include "reflect/Reflect.h"    // NUKE_CLASS (reflected facade)
#include <string>
#include <vector>
#include <functional>

namespace nuke {

// The gameplay input system: raw controls (string ids fed by device providers) -> abstract ACTIONS
// through BINDINGS grouped into swappable CONTEXTS. Query by polling or subscribe from C++.
// Distinct from the editor's menu-shortcut `Hotkeys` registry.
class NUKEENGINE_API Input
{
	NUKE_CLASS_NOCREATE(Input, Object)
public:
	// ---- reflected QUERY (poll from gameplay/scripts) ------------------------------------------------
	[[nuke::func]] static bool    Pressed(const std::string& action);       // edge down this frame
	[[nuke::func]] static bool    Held(const std::string& action);          // currently down
	[[nuke::func]] static bool    Released(const std::string& action);      // edge up this frame
	[[nuke::func]] static bool    Tapped(const std::string& action);        // quick press+release (touch-safe)
	[[nuke::func]] static bool    LongPressed(const std::string& action);   // crossed the long-press threshold
	[[nuke::func]] static bool    DoublePressed(const std::string& action);
	[[nuke::func]] static float   Value(const std::string& action);         // Axis1 (Bool reads 0/1)
	[[nuke::func]] static Vector2 Axis2(const std::string& action);         // Axis2 (stick / WASD)

	// ---- reflected CONTEXTS (hot-swap key maps on the fly) ------------------------------------------
	[[nuke::func]] static void PushContext(const std::string& name);        // activate (respects priority)
	[[nuke::func]] static void PopContext(const std::string& name);         // deactivate
	[[nuke::func]] static void SetContextActive(const std::string& name, bool on);
	[[nuke::func]] static bool ContextActive(const std::string& name);

	// ---- reflected RAW controls ---------------------------------------------------------------------
	[[nuke::func]] static void  SetControl(const std::string& id, float value);   // feed a control (0/1 or -1..1)
	[[nuke::func]] static float Control(const std::string& id);                   // current raw value

	// ---- reflected CURSOR ---------------------------------------------------------------------------
	// Raw pixel position in GAME-SCREEN space (same space as Screen.Width/Height, top-left origin).
	[[nuke::func]] static double MouseX();
	[[nuke::func]] static double MouseY();

	// Cursor mode: 0 Normal (visible, free), 1 Hidden (invisible, free), 2 Locked (invisible +
	// pinned to the window center, raw deltas), 3 Confined (visible, clamped to the window).
	[[nuke::func]] static void SetCursorMode(int mode);
	[[nuke::func]] static int  CursorMode();
	// True while the OS cursor is on screen (Normal/Confined). Gate camera look on this so a
	// visible menu cursor doesn't also spin the camera.
	[[nuke::func]] static bool CursorVisible();

	// ---- reflected USER REMAPS ----------------------------------------------------------------------
	// Reflection can't marshal InputBinding/vectors, so the model crosses as JSON strings using the
	// .nuinput schema. MapJson = the live model to draw a rebind UI from; ControlsJson = every known
	// raw control id (press-to-bind); RebindJson replaces the user binding for (context, action).
	[[nuke::func]] static std::string MapJson();
	[[nuke::func]] static std::string ControlsJson();
	[[nuke::func]] static void        RebindJson(const std::string& context, const std::string& bindingJson);
	[[nuke::func]] static void        ClearUserBindings(const std::string& context, const std::string& action);
	[[nuke::func]] static std::string SaveUserBindings();                        // -> persist (game settings file)
	[[nuke::func]] static void        LoadUserBindings(const std::string& json); // re-apply over the defaults

	// ===== engine / plugin API (NOT reflected) =======================================================
	// Register a device provider; `poll` runs once per frame BEFORE action evaluation. Event-driven
	// providers just SetControl from their callbacks and register a no-op.
	static void RegisterProvider(const std::string& name, std::function<void()> poll);

	// Author the model in code (or load an authored .nuinput). Bindings reference an action by name.
	static void DefineAction(const std::string& name, ActionValueType type);
	static void DefineContext(const std::string& name, int priority);
	static void AddBinding(const std::string& context, const InputBinding& b);
	static bool LoadAsset(const std::string& path);             // .nuinput (JSON file) -> actions/contexts/bindings
	static bool LoadAssetFromString(const std::string& json);   // same, from in-memory bytes (packed content)

	// Called ONCE per frame by the host loop: polls providers, computes control edges/timings, evaluates
	// active contexts by priority, updates action states, fires callbacks.
	static void Update(double dt);

	// C++/plugin event callbacks: fire `cb` whenever `action` reaches `phase`. Returns a sub id for Unsubscribe.
	static int  OnAction(const std::string& action, InputPhase phase, std::function<void()> cb);
	static void Unsubscribe(int subId);

	// User remaps, native side: add/replace a binding for (context, action).
	static void        Rebind(const std::string& context, const InputBinding& b);
	// Replace a whole context's binding list — one action can span several bindings (WASD), which a
	// per-action override can't represent. Persisted by SaveUserBindings.
	static void        SetUserContext(const std::string& context, const std::vector<InputBinding>& bindings);

	// Introspection for the remap editor.
	static std::vector<InputAction>  ListActions();
	static std::vector<InputContext> ListContexts();
	static std::vector<std::string>  ListControls();   // every known raw control id (for press-to-bind)
	static void                      Clear();   // drop all actions/contexts/bindings

	// Data-only input map: parse/edit/serialize a .nuinput WITHOUT touching the live singleton.
	// ApplyMap pushes a whole map in as a per-context replace — repeatable, never duplicates bindings.
	struct InputMapData { std::vector<InputAction> actions; std::vector<InputContext> contexts; };
	static InputMapData ParseMapString(const std::string& json);
	static std::string  SerializeMap(const InputMapData& map);
	static void         ApplyMap(const InputMapData& map);
};

}  // namespace nuke
#endif // !NUKEE_INPUT_H
