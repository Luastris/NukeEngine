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
	// Reflected: scripts hold the CURRENT world as an object handle (Game.GetWorld()) and
	// use the [[nuke::func]] API below 1:1 — find/create/destroy/reparent atoms, (de)serialize.
	NUKE_CLASS_NOCREATE(World, Object)
protected:
	bc::list<Atom*> *hierarchy = nullptr;
public:
	[[nuke::prop]] std::string name = "Default world";

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
	~World();   // drops every script handle wrapping this world

	[[nuke::func]] Atom* Get(const std::string& name);
	[[nuke::func]] Atom* GetById(long id);     // recursive lookup by stable atom id
	bc::list<Atom*>& GetHierarchy();
	[[nuke::func]] void Add(Atom* atom);
	// Create an empty atom at the world root (fresh stable id) — THE script-side factory
	// (scripts can't `new`). Parent afterwards via Reparent/atom:SetParent.
	[[nuke::func]] Atom* CreateAtom(const std::string& name);
	// Deferred destruction: queue an atom subtree by id; it is removed and deleted at a
	// SAFE point (end of World::Update, game lock held) — never mid-iteration. This is what
	// Atom::Destroy delegates to, so scripts may destroy anything (their own atom included)
	// from Update/collision callbacks without invalidating the running traversal.
	[[nuke::func]] void QueueDestroy(long atomId);

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
	std::vector<long> destroyQueue;   // QueueDestroy ids; flushed at the end of Update (under gameLock)
public:

	// The camera the GAME is viewed through (UE "possess"): the one whose Main Camera flag is
	// set, else the world's FIRST camera in hierarchy order (a single camera = itself). Editor
	// infrastructure cameras (Camera::editorCamera) never count. Used by PIE's game-camera view,
	// the player, the audio listener — and available to scripts (World.GetMainCamera).
	[[nuke::func]] Camera* GetMainCamera();

	// Ray-pick the nearest Atom (with a MeshRenderer) hit by a world-space ray.
	// Returns nullptr on miss. Used by the editor viewport for click-to-select.
	[[nuke::func]] Atom* Pick(const Vector3& origin, const Vector3& dir);
	// Same, but also reports the hit distance along `dir` (normalized) — used by spawn-on-surface.
	// NOT named Pick(): an overload would make &World::Pick ambiguous for the reflection codegen.
	Atom* PickDist(const Vector3& origin, const Vector3& dir, float& outDist);

	// Text (.nuworld JSON) scene serialization via reflection. The editor camera is
	// excluded from save and preserved across load (it is editor infrastructure).
	[[nuke::func]] std::string SaveToString();   // serialize to JSON text (also used for PIE snapshots)
	[[nuke::func]] void        LoadFromString(const std::string& data);
	// Load from an ALREADY-PARSED document (C++ only, not reflected). The async world loader
	// parses (+merges) on a background job and hands the document over — the game thread then
	// skips the parse (the heavy part) and only instantiates atoms.
	void LoadFromJson(const nlohmann::json& j);
	// Merge every mounted layer's copy of ONE world (Package::ReadAll order: base first,
	// mods above). Each layer is diffed against the BASE (atoms by id, components by cid)
	// and the diffs apply bottom-up — two mods editing the same world MERGE instead of the
	// top file replacing the other; a true conflict resolves to the higher layer. Returns
	// the merged world JSON (layers.back() when there is nothing to merge).
	static std::string MergeWorldLayers(const std::vector<std::string>& layers);
	// Mods-on-mods: deps[i] = indices (into `layers`) of layer i's dependencies. A layer's
	// diff baseline = base + its dependency closure — a patch-mod authored on top of mods
	// A+B carries their content in its world copy, and diffing it against base would
	// re-impose A's and B's changes as the patch's own; diffing against base+A+B leaves
	// only the patch's actual fixes. deps[0] is ignored (the base has no dependencies).
	static std::string MergeWorldLayers(const std::vector<std::string>& layers,
	                                    const std::vector<std::vector<int>>& deps);
	// Recorded BASELINES: basis[i] = the world EXACTLY as layer i's author saw it when the
	// mod was packed ("" = none; Package Mod stores it as "basis/<rel>" inside the .numod).
	// A layer diffs against ITS OWN basis, so a mod carries only what its author actually
	// touched — a stale mod can never read as "delete everything the base gained since".
	// Missing basis falls back to the deps closure (legacy mods).
	static std::string MergeWorldLayers(const std::vector<std::string>& layers,
	                                    const std::vector<std::vector<int>>& deps,
	                                    const std::vector<std::string>& basis);
	// PROVENANCE: names[i] = the mod that layer i is ("" = base/raw). Atoms/components a
	// mod ADDS get tagged "__mod" in the merged JSON — LoadAtom lifts it into the runtime
	// modOrigin fields (never serialized back), so the editor can badge non-native content.
	static std::string MergeWorldLayers(const std::vector<std::string>& layers,
	                                    const std::vector<std::vector<int>>& deps,
	                                    const std::vector<std::string>& basis,
	                                    const std::vector<std::string>& names);
	[[nuke::func]] void SaveToFile(const std::string& path);
	[[nuke::func]] void LoadFromFile(const std::string& path);
	[[nuke::func]] void Clear();   // drop all atoms except the Editor Camera (for "New World")
	// Move an atom under a new parent (nullptr = scene root). Detaches from its current location
	// (old parent's children or the root list) first; ignores cycles (parenting under a descendant).
	[[nuke::func]] void Reparent(Atom* a, Atom* newParent);
	// Like Reparent, but insert `a` directly BEFORE `sibling` in `sibling`'s parent (for reordering /
	// moving an atom up between siblings). nullptr sibling is ignored.
	[[nuke::func]] void ReparentBefore(Atom* a, Atom* sibling);
	// Undo helpers: delete an atom subtree by id; insert one at a placement (parentId 0 = root).
	// (Scripts prefer QueueDestroy — RemoveAtomById deletes IMMEDIATELY, editor-only safe.)
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
