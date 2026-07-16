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

// THE gameplay input system. Raw controls (string ids fed by device PROVIDERS: keyboard/mouse/gamepad in
// core, wheels/touch/MIDI as plugins) -> abstract ACTIONS through BINDINGS grouped into swappable
// CONTEXTS. Query (poll) or subscribe (C++). Distinct from the editor's menu-shortcut `Hotkeys` registry.
//
// The QUERY + CONTEXT surface is [[nuke::func]]-reflected, so Lua/C# use it 1:1 (nuke.Input.* / Input.*).
// The plugin/engine surface (provider registration, callbacks, binding structs) is plain C++ — not
// script-marshalable, and only native code needs it.
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

	// ---- reflected CONTEXTS (hot-swap key maps on the fly, from anywhere) ----------------------------
	[[nuke::func]] static void PushContext(const std::string& name);        // activate (respects priority)
	[[nuke::func]] static void PopContext(const std::string& name);         // deactivate
	[[nuke::func]] static void SetContextActive(const std::string& name, bool on);
	[[nuke::func]] static bool ContextActive(const std::string& name);

	// ---- reflected RAW controls (a script provider / diagnostics) ------------------------------------
	[[nuke::func]] static void  SetControl(const std::string& id, float value);   // feed a control (0/1 or -1..1)
	[[nuke::func]] static float Control(const std::string& id);                   // current raw value

	// ---- reflected CURSOR (6.7): raw pixel position in GAME-SCREEN space -----------------------------
	// Same space as Screen.Width/Height: the viewport panel in the editor (PIE), the window in the
	// player — top-left origin. Feed it to Camera.ScreenRayOrigin/Dir for click-to-world picking.
	[[nuke::func]] static double MouseX();
	[[nuke::func]] static double MouseY();

	// ---- reflected USER REMAPS (an in-game rebind screen, written in scripts) -------------------------
	// Reflection can't marshal InputBinding/vectors, so the model crosses as JSON strings (same schema
	// as .nuinput). MapJson = the LIVE model (actions/contexts/bindings incl. applied user overrides) to
	// draw the rebind UI from; ControlsJson = every known raw control id (press-to-bind: poll Control()
	// on each until one goes active); RebindJson replaces the user binding for (context, action).
	[[nuke::func]] static std::string MapJson();
	[[nuke::func]] static std::string ControlsJson();
	[[nuke::func]] static void        RebindJson(const std::string& context, const std::string& bindingJson);
	[[nuke::func]] static void        ClearUserBindings(const std::string& context, const std::string& action);
	[[nuke::func]] static std::string SaveUserBindings();                        // -> persist (game settings file)
	[[nuke::func]] static void        LoadUserBindings(const std::string& json); // re-apply over the defaults

	// ===== engine / plugin API (NOT reflected) =======================================================
	// A device PROVIDER: `poll` runs once per frame BEFORE action evaluation (for polled devices like
	// gamepads to read state and SetControl). Event-driven providers (keyboard/mouse) just SetControl
	// from their callbacks and register a no-op (or nothing).
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

	// USER remaps, native side (structs): add/replace a binding for (context, action). The reflected
	// JSON twins above route here. Save/Load/Clear are declared in the reflected block.
	static void        Rebind(const std::string& context, const InputBinding& b);
	// Replace a whole context's binding list (the remap editor edits the full list — WASD is 4 bindings
	// under one action, which a per-action override can't represent). Persisted by SaveUserBindings.
	static void        SetUserContext(const std::string& context, const std::vector<InputBinding>& bindings);

	// Introspection for the remap editor.
	static std::vector<InputAction>  ListActions();
	static std::vector<InputContext> ListContexts();
	static std::vector<std::string>  ListControls();   // every known raw control id (for press-to-bind)
	static void                      Clear();   // drop all actions/contexts/bindings (project switch)

	// Data-only input map: the .nuinput ASSET editor parses/edits/serializes a map WITHOUT touching the
	// live singleton (the file is the asset; live state is gameplay). ApplyMap pushes a whole map into the
	// live system (wholesale per-context replace — safe to call repeatedly, never duplicates bindings).
	struct InputMapData { std::vector<InputAction> actions; std::vector<InputContext> contexts; };
	static InputMapData ParseMapString(const std::string& json);
	static std::string  SerializeMap(const InputMapData& map);
	static void         ApplyMap(const InputMapData& map);
};

}  // namespace nuke
#endif // !NUKEE_INPUT_H
