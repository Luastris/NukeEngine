#pragma once
#ifndef NUKEE_COMPONENT_H
#define NUKEE_COMPONENT_H
#include "NukeAPI.h"
#include "ID.h"
#include <string>
#include <vector>

namespace nuke {
class Atom;
class Transform;
class Script;
class Camera;
class Light;
class iRender;     // scene-render hook (OnRender) — POD seam, forward-declared to keep this header light
struct TypeInfo;   // reflection

// Phases the scene-render hook fires during a camera pass (see World::Render). A component draws via
// iRender seams using its own transform — the camera's view/proj is already bound. This is how MODULE
// components (particles, custom draws) get into the render without World::Render hardcoding them; the
// core Sprite/Decal keep their own batched path.
enum class RenderPhase { Opaque = 0, Transparent = 1, Overlay = 2 };

// A dynamic, per-instance property value (e.g. a script's exported var). Pure data — no
// UI, no engine logic; the editor draws these, the runtime just carries them.
// AtomRef = a REFERENCE to a live atom by STABLE id (never a name/string): scripts see a
// live atom object, the inspector draws the picker, serialization travels by id and
// resolves stale-safe after load. Lua declares one with `nuke.AtomRef()`; C# with a
// member of type `Atom`.
struct NukeVar
{
	enum class Kind { None, Number, Bool, String, AtomRef } kind = Kind::None;
	double      num = 0.0;
	bool        b   = false;
	std::string str;
	long long   refId = 0;   // AtomRef: the stable atom id (0 = none)
};

struct DynProp
{
	std::string name;
	NukeVar     value;
	NukeVar     def;   // declared default (for an editor reset button)
};


class NUKEENGINE_API Component
{
public:
    ID id;                  // per-component identity (multiple components of one type per atom, e.g. scripts)
    bool enabled = true;
    // Which MOD added this component (world-merge provenance, RUNTIME only — never
    // serialized): "" = native. The inspector badges non-native components with it.
    std::string modOrigin;
	Transform* transform = nullptr;
	Atom* atom = nullptr;   // owning Atom (back-reference), set by the component's Init
	// Tick INTERVAL (6.8): Update() runs every Nth frame (1 = every frame, today's default).
	// Staggered by the component id, so a colony of same-interval components spreads across
	// frames instead of spiking on the same one. Serialized next to `enabled`; editable in
	// the inspector; scripts: the component proxy's `tickEvery`. FixedUpdate is NOT affected
	// (physics cadence is sacred).
	int tickEvery = 1;
    char* name;
    Component(const char* _name = "Component") : name((char*)_name){}
	virtual void Init(Atom* parent) = 0;
	virtual void Destroy() = 0;
	virtual void Update() = 0;
	virtual void FixedUpdate() = 0;
	virtual void Pause() = 0;
	virtual void Reset() = 0;
	virtual TypeInfo* GetType() { return nullptr; }   // reflection schema (NUKE_TYPE overrides)

	// Per-instance dynamic properties (e.g. a Lua script's exported vars). DATA ONLY —
	// the editor renders/edits them; the engine and a shipped Player just carry them.
	// Empty = none. SetDynamicProp writes one back (the editor calls it on edit/reset).
	virtual std::vector<DynProp> DynamicProps() { return {}; }
	virtual void SetDynamicProp(const std::string& /*name*/, const NukeVar& /*v*/) {}

	// Runtime immediate-mode UI hook. Called each frame by the GUI backend (NukeGUI) while playing;
	// draw with nuke::GUI()->... (see interface/iGUI.h).
	virtual void OnGUI() {}

	// Physics contact hooks, dispatched by World's FIXED-UPDATE THREAD right after
	// iPhysics::step. `other` = the other atom of the contact pair. Pairs where either
	// collider is a trigger get the Trigger pair; everything else gets the Collision pair.
	// THREADING: these run on the fixed thread — a scripting component must QUEUE and
	// flush into its VM on the game thread (see ScriptComponent), never call in here.
	// ABI: new virtuals are appended at the END of the class; keep it that way.
	virtual void OnCollisionEnter(Atom* other) {}
	virtual void OnCollisionExit(Atom* other) {}
	virtual void OnTriggerEnter(Atom* other) {}
	virtual void OnTriggerExit(Atom* other) {}

	// Animation event (roadmap 3.1): the sibling Animator fires this on every component
	// of its atom when the playhead crosses a clip event. GAME thread, game lock held —
	// scripting components may enter their VM directly (same contract as OnGUI).
	virtual void OnAnimEvent(const char* name) {}

	// Scene-render hook: World::Render calls this for every ENABLED component at each RenderPhase during a
	// camera pass. Default no-op — a component that draws overrides it and issues iRender seam calls
	// (drawSprite/drawDecal/drawDebugLine/...) using its own transform. Lets module components render
	// without the engine's render loop knowing them. ABI: new virtuals are appended at the END.
	virtual void OnRender(iRender* /*r*/, RenderPhase /*phase*/) {}

	// Event-bus delivery (nuke::Events, Phase 6.3): every queued event reaches every ENABLED
	// component once per frame from World::Update. GAME thread, game lock held — scripting
	// components may enter their VM directly (ScriptComponent → Lua `onEvent(self, name,
	// payload)`, CSharpScript → C# `OnEvent(string, string)`). Default no-op; filter by name.
	// ABI: appended at the END of the vtable.
	virtual void OnEvent(const std::string& /*name*/, const std::string& /*payload*/) {}

	// Save is about to serialize this component (SaveAtom, right before the reflected props
	// are read): components whose LIVE state lives outside the props re-encode it here —
	// the Tilemap packs its cell layers into its hidden data prop, script components (6.6)
	// pull live script fields back into `props`. Keep it cheap; called on every world save.
	// ABI: appended at the END of the vtable.
	virtual void OnBeforeSave() {}
};
}  // namespace nuke

#endif
