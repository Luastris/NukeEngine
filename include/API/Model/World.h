#pragma once
#ifndef NUKEE_SCENE_H
#define NUKEE_SCENE_H
#include "NukeAPI.h"
#include "Atom.h"
#include <boost/thread/recursive_mutex.hpp>
#include <memory>

namespace nuke {

class iRender;

class NUKEENGINE_API World
{
protected:
	bc::list<Atom*> *hierarchy = nullptr;
public:
	std::string name = "Default scene";

	// World-level render settings (edited in the World Settings window, saved in .nuworld -> "settings").
	// Shadow GLOBALS (which lights cast is per-Light); pushed to the renderer each frame in Render().
	struct Settings
	{
		int   shadowRes        = 2048;    // shadow map resolution (1024/2048/4096)
		float shadowDistance   = 60.0f;   // directional ortho extent / range
		float shadowDepthBias  = 0.0015f;
		float shadowNormalBias = 0.0f;
		float shadowSoftness   = 1.0f;    // PCF kernel scale
		bool  frustumCull      = true;    // skip drawing objects outside the camera frustum (toggle)
		// Physics (drives the fixed-step loop in Update; pushed to the physics service).
		float gravity[3] = { 0.0f, -9.81f, 0.0f };
		float fixedDt    = 1.0f / 60.0f;  // fixed simulation timestep (seconds)
	};
	Settings settings;

	// Auxiliary world (editor-owned asset previews): rendered IN ADDITION to the current
	// scene each frame, so it skips the global heavy passes (RT scene build) — the live
	// scene rebuilds them right after and must stay the last writer. NOT serialized.
	bool auxiliary = false;

	World();

	Atom* Get(const char* name);
	Atom* GetById(long id);     // recursive lookup by stable atom id
	bc::list<Atom*>& GetHierarchy();
	void Add(Atom* go);

	void Start();

	void Update();              // game logic, once per frame (Play mode); takes the game lock
	// ONE fixed step (settings.fixedDt): sync bodies -> iPhysics::step -> pull dynamic
	// poses into Transforms -> Atom::FixedUpdate. Runs even without a physics provider
	// (gameplay fixed ticks are independent). Driven by AppInstance's FIXED-FREQUENCY
	// THREAD — never by the render loop; the cadence does not depend on the frame rate.
	void FixedUpdate();
	void Render(iRender* r);    // draw pass: one render per camera (Edit + Play)

	// THE GAME LOCK — the "shared world/VM" contract between the game thread and the
	// fixed-update thread (how big engines serialize script/world access while physics
	// solves in parallel): every entry that touches the hierarchy or enters the script VM
	// holds it — Update(), FixedUpdate()'s world phases (the Jolt solve itself runs
	// OUTSIDE the lock, overlapping the frame), LoadFromString/Clear, and the runtime-GUI
	// OnGUI sweep (NukeGUI). RECURSIVE so a script may re-enter (e.g. LoadWorld from Lua).
	void LockGame();
	void UnlockGame();

private:
	boost::recursive_mutex gameLock;
public:

	// Ray-pick the nearest Atom (with a MeshRenderer) hit by a world-space ray.
	// Returns nullptr on miss. Used by the editor viewport for click-to-select.
	Atom* Pick(const Vector3& origin, const Vector3& dir);

	// Text (.nuworld JSON) scene serialization via reflection. The editor camera is
	// excluded from save and preserved across load (it is editor infrastructure).
	std::string SaveToString();                  // serialize to JSON text (also used for PIE snapshots)
	void        LoadFromString(const std::string& data);
	void SaveToFile(const std::string& path);
	void LoadFromFile(const std::string& path);
	void Clear();   // drop all atoms except the Editor Camera (for "New World")
	// Move an atom under a new parent (nullptr = scene root). Detaches from its current location
	// (old parent's children or the root list) first; ignores cycles (parenting under a descendant).
	void Reparent(Atom* a, Atom* newParent);
	// Like Reparent, but insert `a` directly BEFORE `sibling` in `sibling`'s parent (for reordering /
	// moving an atom up between siblings). nullptr sibling is ignored.
	void ReparentBefore(Atom* a, Atom* sibling);
	// Undo helpers: delete an atom subtree by id; insert one at a placement (parentId 0 = root).
	void RemoveAtomById(long id);
	void InsertAtom(Atom* a, long parentId, int index);

	// Live plugin (un)load support. ConvertPluginToUnknown turns every component owned by the
	// given plugin into an inert UnknownComponent placeholder (called when it's disabled);
	// RestorePluginComponents does the reverse — placeholders for that plugin's now-available
	// types become real components again (called when it's enabled).
	void ConvertPluginToUnknown(const std::string& moduleFile);
	void RestorePluginComponents(const std::string& moduleFile);
};

}  // namespace nuke

#endif // !NUKEE_SCENE_H
