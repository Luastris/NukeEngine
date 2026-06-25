#pragma once
#ifndef NUKEE_SCENE_H
#define NUKEE_SCENE_H
#include "NukeAPI.h"
#include "Atom.h"
#include <memory>

namespace nuke {

class iRender;

class NUKEENGINE_API World
{
protected:
	bc::list<Atom*> *hierarchy = nullptr;
public:
	std::string name = "Default scene";
    

	World();

	Atom* Get(const char* name);
	bc::list<Atom*>& GetHierarchy();
	void Add(Atom* go);

	void Start();

	void Update();              // game logic (Play mode)
	void Render(iRender* r);    // draw pass: one render per camera (Edit + Play)

	// Ray-pick the nearest Atom (with a MeshRenderer) hit by a world-space ray.
	// Returns nullptr on miss. Used by the editor viewport for click-to-select.
	Atom* Pick(const Vector3& origin, const Vector3& dir);

	// Text (.nuworld JSON) scene serialization via reflection. The editor camera is
	// excluded from save and preserved across load (it is editor infrastructure).
	std::string SaveToString();                  // serialize to JSON text (also used for PIE snapshots)
	void        LoadFromString(const std::string& data);
	void SaveToFile(const std::string& path);
	void LoadFromFile(const std::string& path);
};

}  // namespace nuke

#endif // !NUKEE_SCENE_H
