#pragma once
#ifndef NUKEE_REFLECT_BIND_H
#define NUKEE_REFLECT_BIND_H
#include "NukeAPI.h"
#include "Reflect.h"
#include <string>
#include <vector>

// Reflection -> scripting bridge: one language-neutral layer over the reflection registry that
// every scripting backend mounts its bindings on. Handles are stale-safe — a script holds
// (atomId, componentId) and resolves through the live World on every access, so a dead handle
// yields null rather than dereferencing freed memory. ReflectValue lives in Reflect.h.

namespace nuke {

class Atom;
class Component;

// Field lookup by name on a type, INCLUDING the base-class chain. Null when absent.
NUKEENGINE_API const Field* Reflect_FindField(const TypeInfo* type, const std::string& name);

// Method lookup by name on a type, INCLUDING the base-class chain. Null when absent.
NUKEENGINE_API const Method* Reflect_FindMethod(const TypeInfo* type, const std::string& name);

// Invoke a reflected method on an instance of its owning type; `args` count must equal m.params.
// False on arity mismatch or a method without an invoker; ret.type == FT::Unknown means void.
NUKEENGINE_API bool Reflect_Invoke(void* obj, const Method& m,
                                   const ReflectValue* args, std::size_t n, ReflectValue& ret);

// Generic property access; `obj` must be an instance of the field's owning type.
NUKEENGINE_API ReflectValue Reflect_GetField(void* obj, const Field& f);
NUKEENGINE_API bool         Reflect_SetField(void* obj, const Field& f, const ReflectValue& v);

// List props (std::vector<T>) don't fit in a ReflectValue, so they cross as a JSON array string —
// the same encoding the serializer uses. Get returns "[...]"; Set replaces the whole list and
// returns false on malformed json or a non-list field.
NUKEENGINE_API std::string Reflect_GetFieldJson(void* obj, const Field& f);
NUKEENGINE_API bool        Reflect_SetFieldJson(void* obj, const Field& f, const std::string& jsonText);

// Names of every reflected type creatable as a component (has a factory and derives from Component).
NUKEENGINE_API std::vector<std::string> Reflect_ComponentTypes();

// First component of the given reflected type name on the atom (null if absent).
NUKEENGINE_API Component* Reflect_FindComponent(Atom* atom, const std::string& typeName);

// Create a component by reflected type name and attach it to the atom. Null when the type is
// unknown, non-creatable, or not a Component.
NUKEENGINE_API Component* Reflect_AddComponent(Atom* atom, const std::string& typeName);

// Resolve a stale-safe script handle against the CURRENT world. Null when either side is gone.
NUKEENGINE_API Component* Reflect_ResolveComponent(unsigned long atomId, unsigned long componentId);

// ---- reflected OBJECTS ------------------------------------------------------------------
// Reflected Model classes live in an engine-owned handle table (id -> instance + TypeInfo);
// scripts hold only ids. Creation registers assets into ResDB under a fresh guid, so components
// resolve them exactly like imported ones.
NUKEENGINE_API unsigned long Reflect_CreateObject(const std::string& typeName);   // 0 = unknown/non-creatable
// Wrap an EXISTING engine-owned instance into a handle WITHOUT taking ownership. Null-safe.
NUKEENGINE_API unsigned long Reflect_WrapObject(void* obj, const std::string& typeName);
// Is this reflected type a ResDB asset (guid-identified, findable by name/guid)? Facades and
// singletons are not assets. The single source of truth for creation, lookup and generators.
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

// Asset lookup by NAME (case-insensitive): internal asset names first, then any tracked path's
// file stem. `typeName` narrows to one class; "" tries all. 0 when nothing matches.
NUKEENGINE_API unsigned long Reflect_FindAsset(const std::string& typeName, const std::string& name);

// A component's owned reflected sub-object ("material" on a MeshRenderer -> its live Material).
// Returns the raw pointer + its TypeInfo, or null; the handle variant wraps it stale-safely.
NUKEENGINE_API void*         Reflect_SubObject(Component* c, const std::string& path, TypeInfo** ti);
NUKEENGINE_API unsigned long Reflect_ComponentObject(unsigned long atomId, unsigned long compId,
                                                     const std::string& path);

// Post-write hook for asset-reference fields: re-resolves the runtime pointers behind the guid so
// a script-assigned asset takes effect this frame. Call after Reflect_SetField on an asset field.
NUKEENGINE_API void          Reflect_ComponentFieldChanged(Component* c, const Field& f);

// Set texture content. Expects tightly-packed RGBA8, len == w*h*4; sets size/format/mips and
// refreshes the GPU copy.
NUKEENGINE_API bool          Reflect_SetTexturePixels(unsigned long id, int w, int h,
                                                      const void* rgba, std::size_t len);

// Set mesh geometry as an unindexed TRIANGLE LIST: numVerts (multiple of 3), verts = 3*numVerts
// floats (required), normals = 3*numVerts (null -> flat per-triangle normals), uvs = 2*numVerts
// (null -> zeros). Bumps Mesh::version so the renderer re-uploads.
NUKEENGINE_API bool          Reflect_SetMeshGeometry(unsigned long id, int numVerts,
                                                     const float* verts, const float* normals,
                                                     const float* uvs);

}  // namespace nuke

#endif // !NUKEE_REFLECT_BIND_H
