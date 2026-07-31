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
class iRender;     // scene-render hook (OnRender) — POD seam
struct TypeInfo;   // reflection

// Phases the scene-render hook fires during a camera pass (see World::Render); the camera's
// view/proj is already bound. RTScene fires between beginRTScene/buildRTScene, when RT is
// available. APPEND-ONLY enum: modules compiled against the old set must keep their values.
enum class RenderPhase { Opaque = 0, Transparent = 1, Overlay = 2, RTScene = 3 };

// A dynamic, per-instance property value (e.g. a script's exported var). Pure data.
// AtomRef references a live atom by STABLE id (never a name), so serialization travels by id.
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
    std::string modOrigin;   // mod that added this component ("" = native); RUNTIME only, never serialized
	Transform* transform = nullptr;
	Atom* atom = nullptr;   // owning Atom (back-reference), set by the component's Init
	// Tick interval: Update() runs every Nth frame (1 = every frame), staggered by component
	// id so same-interval components spread across frames. FixedUpdate is NOT affected.
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

	// Per-instance dynamic properties (e.g. a Lua script's exported vars); empty = none.
	// DATA ONLY — the editor renders/edits them, SetDynamicProp writes one back.
	virtual std::vector<DynProp> DynamicProps() { return {}; }
	virtual void SetDynamicProp(const std::string& /*name*/, const NukeVar& /*v*/) {}

	// Runtime immediate-mode UI hook, called each frame by the GUI backend while playing;
	// draw with nuke::GUI()->... (see interface/iGUI.h).
	virtual void OnGUI() {}

	// Physics contact hooks; `other` = the other atom of the pair. Trigger colliders get the
	// Trigger pair, everything else the Collision pair.
	// THREADING: dispatched on the FIXED-UPDATE thread — a scripting component must queue and
	// flush into its VM on the game thread, never enter it here.
	// ABI: new virtuals are appended at the END of the class; keep it that way.
	virtual void OnCollisionEnter(Atom* other) {}
	virtual void OnCollisionExit(Atom* other) {}
	virtual void OnTriggerEnter(Atom* other) {}
	virtual void OnTriggerExit(Atom* other) {}

	// The sibling Animator fires this on every component of its atom when the playhead
	// crosses a clip event. GAME thread, game lock held (same contract as OnGUI).
	virtual void OnAnimEvent(const char* name) {}

	// Scene-render hook: called for every ENABLED component at each RenderPhase of a camera
	// pass; a component that draws issues iRender seam calls using its own transform.
	// ABI: new virtuals are appended at the END.
	virtual void OnRender(iRender* /*r*/, RenderPhase /*phase*/) {}

	// Event-bus delivery (nuke::Events): every queued event reaches every ENABLED component
	// once per frame from World::Update; filter by name. GAME thread, game lock held.
	// ABI: appended at the END of the vtable.
	virtual void OnEvent(const std::string& /*name*/, const std::string& /*payload*/) {}

	// Called right before the reflected props are serialized: components whose LIVE state
	// lives outside the props re-encode it here. Keep it cheap; runs on every world save.
	// ABI: appended at the END of the vtable.
	virtual void OnBeforeSave() {}
};
}  // namespace nuke

#endif
