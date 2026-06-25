#pragma once
#ifndef NUKEE_COMPONENT_H
#define NUKEE_COMPONENT_H
#include "NukeAPI.h"

namespace nuke {
class Atom;
class Transform;
class Script;
class Camera;
class Light;
struct TypeInfo;   // reflection


class NUKEENGINE_API Component
{
public:
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

};
}  // namespace nuke

#endif
