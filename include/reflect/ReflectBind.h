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

// ---- reflected OBJECTS (task #67): EVERY reflected Model class is first-class in every
// scripting language — create, edit props, call methods, hand to components. Objects live
// in an engine-owned HANDLE table (id -> instance + TypeInfo); scripts hold only ids.
// Creation registers assets (Material/Texture/Mesh/...) into ResDB under a fresh guid, so
// components resolve them exactly like imported ones — nothing script-side is special.
NUKEENGINE_API unsigned long Reflect_CreateObject(const std::string& typeName);   // 0 = unknown/non-creatable
// Wrap an EXISTING engine-owned instance (a MeshRenderer's material, a ResDB asset) into
// a handle WITHOUT taking ownership. Null-safe: obj==nullptr -> 0.
NUKEENGINE_API unsigned long Reflect_WrapObject(void* obj, const std::string& typeName);
// Is this reflected type a ResDB ASSET (guid-identified, findable by name/guid)? The one
// place that answers it — Reflect_CreateObject's registration, Reflect_ObjectFromGuid's
// lookup and the C# wrapper generator (Find/FromGuid factories) all agree through this.
// Facades/singletons (World/Game/Log/Physics/Clock/...) are NOT assets — you never look
// one up by name.
NUKEENGINE_API bool Reflect_IsAssetType(const std::string& typeName);

// ResDB asset by guid -> handle (mesh/material/texture/clip/bonemap; "" kind = try all).
NUKEENGINE_API unsigned long Reflect_ObjectFromGuid(const std::string& guid);
NUKEENGINE_API std::string   Reflect_ObjectGuid(unsigned long id);   // "" when it has none
NUKEENGINE_API const char*   Reflect_ObjectType(unsigned long id);   // "" when the handle is dead
// Generic access on a handle (the same Field/Method machinery components use).
NUKEENGINE_API ReflectValue  Reflect_ObjectGet(unsigned long id, const std::string& field);
NUKEENGINE_API bool          Reflect_ObjectSet(unsigned long id, const std::string& field, const ReflectValue& v);
NUKEENGINE_API bool          Reflect_ObjectInvoke(unsigned long id, const std::string& method,
                                                  const ReflectValue* args, std::size_t n, ReflectValue& ret);

// Asset lookup by NAME (case-insensitive): internal asset names first (shader/material/
// mesh incl. "builtin:cube"/clip/texture), then any tracked path's file stem. User code
// says Find("world") / Find("bricks") — never a guid. `typeName` narrows to one class;
// "" tries all. 0 when nothing matches.
NUKEENGINE_API unsigned long Reflect_FindAsset(const std::string& typeName, const std::string& name);

// A component's OWNED reflected sub-object (the ONE spot to extend when a new owned
// object appears): "material" on a MeshRenderer -> its live Material instance. Returns
// the raw pointer + its TypeInfo, or null. The handle variant wraps it stale-safely.
NUKEENGINE_API void*         Reflect_SubObject(Component* c, const std::string& path, TypeInfo** ti);
NUKEENGINE_API unsigned long Reflect_ComponentObject(unsigned long atomId, unsigned long compId,
                                                     const std::string& path);

// Post-write hook for ASSET-REFERENCE fields on components: re-resolves the runtime
// pointers behind the guid (MeshRenderer's mesh/material) so a script-assigned asset
// takes effect this frame, not on reload. Call after Reflect_SetField on an asset field.
NUKEENGINE_API void          Reflect_ComponentFieldChanged(Component* c, const Field& f);

// Texture CONTENT — the blob channel (pixel data doesn't fit the value path). Expects
// tightly-packed RGBA8, len == w*h*4; sets size/format/mips and refreshes the GPU copy.
NUKEENGINE_API bool          Reflect_SetTexturePixels(unsigned long id, int w, int h,
                                                      const void* rgba, std::size_t len);

// Mesh CONTENT — procedural geometry from scripts. An unindexed TRIANGLE LIST, exactly
// the engine's mesh layout: numVerts (multiple of 3), verts = 3*numVerts floats
// (required), normals = 3*numVerts (null -> flat per-triangle normals are computed),
// uvs = 2*numVerts (null -> zeros). Bumps Mesh::version so the renderer re-uploads.
NUKEENGINE_API bool          Reflect_SetMeshGeometry(unsigned long id, int numVerts,
                                                     const float* verts, const float* normals,
                                                     const float* uvs);

}  // namespace nuke

#endif // !NUKEE_REFLECT_BIND_H
