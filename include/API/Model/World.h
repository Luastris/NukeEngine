#pragma once
#ifndef NUKEE_SCENE_H
#define NUKEE_SCENE_H
#include "NukeAPI.h"
#include "Atom.h"
#include "reflect/Reflect.h"
#include <boost/thread/recursive_mutex.hpp>
#include <memory>
#include <vector>
#include <nlohmann/json_fwd.hpp>   // LoadFromJson (async load hands over a pre-parsed document)

namespace nuke {

class iRender;
class Camera;

class NUKEENGINE_API World
{
	// Reflected: scripts hold the CURRENT world as an object handle (Game.GetWorld()).
	NUKE_CLASS_NOCREATE(World, Object)
protected:
	bc::list<Atom*> *hierarchy = nullptr;
public:
	[[nuke::prop]] std::string name = "Default world";

	// World-level render settings (saved in .nuworld -> "settings"), pushed to the renderer each
	// frame in Render(). Shadow GLOBALS only — which lights cast is per-Light.
	struct Settings
	{
		int   shadowRes        = 2048;    // shadow map resolution (1024/2048/4096)
		float shadowDistance   = 60.0f;   // directional ortho extent / range
		float shadowDepthBias  = 0.0015f;
		float shadowNormalBias = 0.0f;
		float shadowSoftness   = 1.0f;    // PCF kernel scale
		bool  frustumCull      = true;    // skip drawing objects outside the camera frustum (toggle)
		// Physics (drives the fixed-step loop; pushed to the physics service).
		float gravity[3] = { 0.0f, -9.81f, 0.0f };
		float fixedDt    = 1.0f / 60.0f;  // fixed simulation timestep (seconds)
	};
	Settings settings;

	// Auxiliary world (editor-owned asset previews): rendered IN ADDITION to the current world,
	// so it skips the global heavy passes (RT build) — the live world must stay the last writer.
	bool auxiliary = false;

	World();
	~World();   // drops every script handle wrapping this world

	[[nuke::func]] Atom* Get(const std::string& name);
	[[nuke::func]] Atom* GetById(long id);     // recursive lookup by stable atom id
	bc::list<Atom*>& GetHierarchy();
	[[nuke::func]] void Add(Atom* atom);
	// Create an empty atom at the world root (fresh stable id) — the script-side factory.
	[[nuke::func]] Atom* CreateAtom(const std::string& name);
	// Deferred destruction: queue an atom subtree by id; it is removed and deleted at a SAFE
	// point (end of Update, game lock held) — never mid-iteration, so scripts may destroy
	// anything from Update/collision callbacks without invalidating the running traversal.
	[[nuke::func]] void QueueDestroy(long atomId);

	void Start();

	void Update();              // game logic, once per frame (Play mode); takes the game lock
	// ONE fixed step (settings.fixedDt): sync bodies -> iPhysics::step -> pull dynamic poses into
	// Transforms -> Atom::FixedUpdate; runs even without a physics provider. Driven by
	// AppInstance's FIXED-FREQUENCY THREAD, never the render loop — cadence is frame-rate independent.
	void FixedUpdate();
	void Render(iRender* r);    // draw pass: one render per camera (Edit + Play)

	// THE GAME LOCK — thread-safety contract between the game thread and the fixed-update thread:
	// every entry that touches the hierarchy or enters the script VM holds it (Update,
	// FixedUpdate's world phases — the physics solve itself runs OUTSIDE the lock —
	// LoadFromString/Clear, the OnGUI sweep). RECURSIVE so a script may re-enter.
	void LockGame();
	void UnlockGame();

private:
	boost::recursive_mutex gameLock;
	std::vector<long> destroyQueue;   // QueueDestroy ids; flushed at the end of Update (under gameLock)
public:

	// The camera the GAME is viewed through: the one whose Main Camera flag is set, else the
	// world's FIRST camera in hierarchy order. Editor cameras (Camera::editorCamera) never count.
	[[nuke::func]] Camera* GetMainCamera();

	// Ray-pick the nearest Atom (with a MeshRenderer) hit by a world-space ray; nullptr on miss.
	[[nuke::func]] Atom* Pick(const Vector3& origin, const Vector3& dir);
	// Same, but also reports the hit distance along `dir` (normalized). NOT an overload of Pick():
	// that would make &World::Pick ambiguous for the reflection codegen.
	Atom* PickDist(const Vector3& origin, const Vector3& dir, float& outDist);

	// Text (.nuworld JSON) serialization via reflection. The editor camera is excluded from save
	// and preserved across load.
	[[nuke::func]] std::string SaveToString();   // serialize to JSON text (also used for PIE snapshots)
	[[nuke::func]] void        LoadFromString(const std::string& data);
	// Load from an ALREADY-PARSED document (C++ only, not reflected): the async loader parses and
	// merges on a background job, so the game thread only instantiates atoms.
	void LoadFromJson(const nlohmann::json& j);
	// The same load split into PHASES for incremental (budgeted) activation: header + old-world
	// teardown, then root atoms one by one, then the finalize pass. LoadFromJson = all three.
	void  LoadHeaderFromJson(const nlohmann::json& j);    // name/settings/calendar + teardown
	Atom* AddAtomFromJson(const nlohmann::json& atomJ);   // instantiate ONE root atom (its subtree)
	void  FinalizeIncrementalLoad();                      // duplicate-id heal + AtomRef resolve
	// One-shot: the NEXT load must NOT carry persistent atoms (the savegame snapshot already
	// contains them). Consumed by the teardown.
	bool suppressPersistOnce = false;
	// Merge every mounted layer's copy of ONE world (Package::ReadAll order: base first, mods
	// above). Each layer is diffed against the BASE (atoms by id, components by cid) and the
	// diffs apply bottom-up; a true conflict resolves to the higher layer. Returns the merged
	// world JSON (layers.back() when there is nothing to merge).
	static std::string MergeWorldLayers(const std::vector<std::string>& layers);
	// Mods-on-mods: deps[i] = indices (into `layers`) of layer i's dependencies; a layer's diff
	// baseline is base + its dependency closure. deps[0] is ignored (the base has none).
	static std::string MergeWorldLayers(const std::vector<std::string>& layers,
	                                    const std::vector<std::vector<int>>& deps);
	// Recorded BASELINES: basis[i] = the world exactly as layer i's author saw it when the mod
	// was packed ("" = none). A layer diffs against ITS OWN basis; missing basis falls back to
	// the deps closure (legacy mods).
	static std::string MergeWorldLayers(const std::vector<std::string>& layers,
	                                    const std::vector<std::vector<int>>& deps,
	                                    const std::vector<std::string>& basis);
	// PROVENANCE: names[i] = the mod that layer i is ("" = base/raw). Atoms/components a mod ADDS
	// get tagged "__mod" in the merged JSON; LoadAtom lifts it into the runtime modOrigin fields
	// (never serialized back).
	static std::string MergeWorldLayers(const std::vector<std::string>& layers,
	                                    const std::vector<std::vector<int>>& deps,
	                                    const std::vector<std::string>& basis,
	                                    const std::vector<std::string>& names);
	[[nuke::func]] void SaveToFile(const std::string& path);
	[[nuke::func]] void LoadFromFile(const std::string& path);
	[[nuke::func]] void Clear();   // drop all atoms except the Editor Camera (for "New World")
	// Move an atom under a new parent (nullptr = world root); detaches from its current location
	// first and ignores cycles (parenting under a descendant).
	[[nuke::func]] void Reparent(Atom* a, Atom* newParent);
	// Like Reparent, but insert `a` directly BEFORE `sibling` in `sibling`'s parent. nullptr
	// sibling is ignored.
	[[nuke::func]] void ReparentBefore(Atom* a, Atom* sibling);
	// Undo helpers: delete an atom subtree by id; insert one at a placement (parentId 0 = root).
	// RemoveAtomById deletes IMMEDIATELY (editor-only safe) — scripts use QueueDestroy.
	void RemoveAtomById(long id);
	void InsertAtom(Atom* a, long parentId, int index);

	// Live plugin (un)load support: Convert turns every component owned by the plugin into an
	// inert UnknownComponent placeholder (on disable); Restore does the reverse (on enable).
	void ConvertPluginToUnknown(const std::string& moduleFile);
	void RestorePluginComponents(const std::string& moduleFile);
};

}  // namespace nuke

#endif // !NUKEE_SCENE_H
