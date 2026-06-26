#pragma once
#ifndef NUKEE_PREFAB_H
#define NUKEE_PREFAB_H
#include "NukeAPI.h"
#include <string>

namespace nuke {

class Atom;

// A prefab (.nuprefab) is a saved Atom subtree (same JSON shape as a world atom: name +
// transform + components + children). Import writes one per model; instantiating reconstructs
// the tree. Reuses the world's atom (de)serialization, so components resolve the same way
// (e.g. MeshRenderer.meshGuid -> ResDB, missing-plugin types -> inert placeholders).
NUKEENGINE_API bool  SavePrefab(Atom* root, const std::string& path);
NUKEENGINE_API Atom* LoadPrefab(const std::string& path);   // nullptr on failure

}  // namespace nuke

#endif // !NUKEE_PREFAB_H
