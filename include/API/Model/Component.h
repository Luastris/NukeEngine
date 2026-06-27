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
struct TypeInfo;   // reflection

// A dynamic, per-instance property value (e.g. a script's exported var). Pure data — no
// UI, no engine logic; the editor draws these, the runtime just carries them.
struct NukeVar
{
	enum class Kind { None, Number, Bool, String } kind = Kind::None;
	double      num = 0.0;
	bool        b   = false;
	std::string str;
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
	Transform* transform = nullptr;
	Atom* atom = nullptr;   // owning Atom (back-reference), set by the component's Init
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

};
}  // namespace nuke

#endif
