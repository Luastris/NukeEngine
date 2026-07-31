#pragma once
#ifndef NUKEE_PREFAB_H
#define NUKEE_PREFAB_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

class Atom;

// A prefab (.nuprefab) is a saved Atom subtree (same JSON shape as a world atom); save/load
// reuse the world's atom (de)serialization, so components resolve the same way.
NUKEENGINE_API bool  SavePrefab(Atom* root, const std::string& path);
NUKEENGINE_API Atom* LoadPrefab(const std::string& path);   // nullptr on failure
NUKEENGINE_API Atom* LoadPrefabFromString(const std::string& text);   // packed content (3.2)

// In-memory variants of the same atom-subtree (de)serialization (used by editor undo deltas).
NUKEENGINE_API std::string SaveAtomToString(Atom* root);
NUKEENGINE_API Atom*       LoadAtomFromString(const std::string& json);   // nullptr on failure
// Clone variant (editor copy/paste/duplicate): FRESH ids for the whole subtree, internal
// AtomRef props remapped to the clones, external refs untouched. The source stays live.
NUKEENGINE_API Atom*       CloneAtomFromString(const std::string& json);  // nullptr on failure

// The prefab file's own GUID (its root "prefab" field), or "" for pre-link prefabs.
NUKEENGINE_API std::string PrefabGuid(const std::string& path);
NUKEENGINE_API std::string PrefabGuidFromString(const std::string& text);

// The SCRIPT-facing face of the prefab API: spawn saved subtrees at runtime.
class NUKEENGINE_API Prefabs
{
	NUKE_CLASS_NOCREATE(Prefabs, Object)
public:
	// Reconstruct a .nuprefab from the project CONTENT (content-relative path) into the current
	// world root, through the layered resolution (raw project or pak + mods) with fresh stable
	// ids. Returns the new root atom, null on failure.
	[[nuke::func]] static Atom* Spawn(const std::string& contentRelPath);
};

}  // namespace nuke

#endif // !NUKEE_PREFAB_H
