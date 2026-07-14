#pragma once
#ifndef NUKEE_PREFAB_H
#define NUKEE_PREFAB_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

class Atom;

// A prefab (.nuprefab) is a saved Atom subtree (same JSON shape as a world atom: name +
// transform + components + children). Import writes one per model; instantiating reconstructs
// the tree. Reuses the world's atom (de)serialization, so components resolve the same way
// (e.g. MeshRenderer.meshGuid -> ResDB, missing-plugin types -> inert placeholders).
NUKEENGINE_API bool  SavePrefab(Atom* root, const std::string& path);
NUKEENGINE_API Atom* LoadPrefab(const std::string& path);   // nullptr on failure
NUKEENGINE_API Atom* LoadPrefabFromString(const std::string& text);   // packed content (3.2)

// In-memory variants of the same atom-subtree (de)serialization (used by editor undo deltas).
NUKEENGINE_API std::string SaveAtomToString(Atom* root);
NUKEENGINE_API Atom*       LoadAtomFromString(const std::string& json);   // nullptr on failure

// The prefab file's own GUID (its root "prefab" field), or "" for pre-link prefabs.
NUKEENGINE_API std::string PrefabGuid(const std::string& path);
NUKEENGINE_API std::string PrefabGuidFromString(const std::string& text);

// The SCRIPT-facing face of the prefab API ([[nuke::func]]-reflected statics — Lua
// nuke.Prefabs.*, C# Prefabs.*): spawn saved subtrees at runtime.
class NUKEENGINE_API Prefabs
{
	NUKE_CLASS_NOCREATE(Prefabs, Object)
public:
	// Reconstruct a .nuprefab from the project CONTENT (content-relative path, e.g.
	// "Prefabs/Enemy.nuprefab") into the current world root — reads through the engine's
	// layered resolution (raw project or mounted pak + mods), fresh stable ids, components
	// resolved like a world load. Returns the new root atom (null on failure); place it
	// via its Transform, parent it via SetParent/Reparent.
	[[nuke::func]] static Atom* Spawn(const std::string& contentRelPath);
};

}  // namespace nuke

#endif // !NUKEE_PREFAB_H
