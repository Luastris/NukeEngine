#pragma once
#ifndef NUKEE_REFLECT_BIND_H
#define NUKEE_REFLECT_BIND_H
#include "NukeAPI.h"
#include "Reflect.h"
#include <string>
#include <vector>

// Reflection -> scripting bridge (roadmap 0.8). ONE language-neutral layer over the
// reflection registry that every scripting backend (Lua today, C#/Mono later) mounts its
// bindings on, so per-class wrappers are never hand-written:
//   * types:      Reflect_ComponentTypes() / Registry_All()
//   * properties: Reflect_FindField() + Reflect_GetField()/Reflect_SetField()
//                 (generic access through Field::addr + the FT type tag)
//   * methods:    Reflect_FindMethod() + Reflect_Invoke()
//                 ([[nuke::func]]-tagged, generated type-erased invokers)
//   * factories:  Reflect_AddComponent() (TypeInfo::create + Atom::AddComponent)
// Handles are STALE-SAFE: a script holds (atomId, componentId) and resolves through the
// live World on every access (Reflect_ResolveComponent) — a dead handle yields null, it
// can never dereference freed memory.
// (ReflectValue — the language-neutral value these functions traffic in — lives in
// Reflect.h next to FT/Field/Method.)

namespace nuke {

class Atom;
class Component;

// Field lookup by name on a type, INCLUDING the base-class chain (walks TypeInfo::base
// through the registry). Null when the type has no such reflected field.
NUKEENGINE_API const Field* Reflect_FindField(const TypeInfo* type, const std::string& name);

// Method lookup by name on a type, INCLUDING the base-class chain. Null when absent.
NUKEENGINE_API const Method* Reflect_FindMethod(const TypeInfo* type, const std::string& name);

// Invoke a reflected method on an instance of its owning type. `args` count must equal
// m.params (each converted by the caller to the declared FT). False on arity mismatch or
// a method without an invoker; `ret.type == FT::Unknown` means the method returned void.
NUKEENGINE_API bool Reflect_Invoke(void* obj, const Method& m,
                                   const ReflectValue* args, std::size_t n, ReflectValue& ret);

// Generic property access on an instance of the field's owning type (the same void*
// contract the inspector uses: `obj` is the object the Field was registered for).
NUKEENGINE_API ReflectValue Reflect_GetField(void* obj, const Field& f);
NUKEENGINE_API bool         Reflect_SetField(void* obj, const Field& f, const ReflectValue& v);

// Names of every reflected type that can be created as a component (has a factory and
// derives from Component) — the script-side "what can I add" list.
NUKEENGINE_API std::vector<std::string> Reflect_ComponentTypes();

// First component of the given reflected type name on the atom (null if absent).
NUKEENGINE_API Component* Reflect_FindComponent(Atom* atom, const std::string& typeName);

// Create a component by reflected type name and attach it to the atom (factory +
// AddComponent/Init) — the same path the editor's Add-Component menu uses. Null when the
// type is unknown, non-creatable, or not a Component.
NUKEENGINE_API Component* Reflect_AddComponent(Atom* atom, const std::string& typeName);

// Resolve a stale-safe script handle against the CURRENT world: atom by id, then the
// component by id on it. Null when either side is gone.
NUKEENGINE_API Component* Reflect_ResolveComponent(unsigned long atomId, unsigned long componentId);

}  // namespace nuke

#endif // !NUKEE_REFLECT_BIND_H
