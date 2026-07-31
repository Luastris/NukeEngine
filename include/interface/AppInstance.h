#pragma once
#ifndef NUKEE_APPINSTANCE_H
#define NUKEE_APPINSTANCE_H
#include "NukeAPI.h"
#include <cstdint>
#include <atomic>                    // async world-load state flags
#include <memory>                    // staged world document handoff
#include <nlohmann/json_fwd.hpp>     // parsed world document (full include only in .cpp)
#include <boost/thread.hpp>
#include <boost/container/list.hpp>
#include <boost/container/map.hpp>
#include <map>
#include <config.h>
#include "../API/Model/Camera.h"
#include "../API/Model/World.h"
#include "../input/keyboard.h"
#include "../input/mouse.h"
#include "../render/irender.h"
#include "./EditorMenu/MenuStrip.h"

namespace nuke {

namespace bc = boost::container;

class NUKEENGINE_API AppInstance
{
protected:
	AppInstance();
	~AppInstance();
	bool _isEditor = false;
public:
	
	MenuStrip* menuStrip = nullptr;
	Atom* selectedInHieararchy = nullptr;
	int manipulationMode = 0;   // 0=Select 1=Move 2=Rotate 3=Scale
	int manipulationWorld = 0;
	int playState = 0;          // PIE: 0=Stopped(Edit) 1=Playing 2=Paused
	bool wireframe = false;     // viewport draw mode: false=Solid true=Wireframe
	//bc::list<btups::tuple<string, bst::function<void()>>> editorWindows;
	bc::map<string, bst::function<void()>>* editorWindows = nullptr;

	void PushWindow(const char* key, boost::function<void()> fWindow);

	//void PushWindow(string &key, boost::function<void()> fWindow);
	void PopWindow(string key);

	// Per-window open/closed flag, keyed by window id (created open on first access).
	bool* WindowOpen(const char* key);



	World* currentWorld = new World();
    KeyBoard* keyboard = nullptr;
    Mouse* mouse = nullptr;
	Config* config = nullptr;
    iRender* render = nullptr;
	std::map<std::string, bool> windowOpen;   // window id -> open flag (persisted by the editor)

	// Root for CONTENT relative paths — the project's content folder. Engine resources
	// (config/fonts/shaders/modules) stay relative to the EXE/cwd; only content resolves here.
	std::string contentRoot;
	// Resolve a content path: absolute -> as-is; else prefer <contentRoot>/path, falling back
	// to a cwd-relative path if that exists.
	std::string ResolveContent(const std::string& path) const;

	// --- World load/save. Paths are relative to the project content root (ResolveContent).
	std::string currentWorldPath;                     // content-relative path of the open world ("" = unsaved)
	// Script-initiated world switches arrive MID-TICK, while Update/FixedUpdate iterate the very
	// hierarchy a load would replace. The tick sets worldTickActive under the game lock; OpenWorld
	// then only QUEUES the path and World::Update applies it at the frame boundary.
	bool        worldTickActive = false;
	std::string pendingWorldLoad;
	// Absolute .nusave path applied by World::Update at the frame boundary; same mid-tick rule.
	std::string pendingSaveLoad;
	std::string WorldFullPath(const std::string& relPath) const;   // canonical content path for a world
	bool        ReadContent(const std::string& relPath, std::string& out) const; // bytes via all layers (pak = memory)
	bool        OpenWorld(const std::string& relPath); // load a world from content into currentWorld
	void        NameWorldFromPath(const std::string& relPath); // name an unnamed world from its file stem
	bool        SaveWorld(const std::string& relPath); // save currentWorld to content (creates dirs)
	void        NewWorld();                            // replace currentWorld with a fresh empty world

	bool isEditor();
	void setEditor(bool editor);

	static AppInstance* GetSingleton() 
	{
		static AppInstance instance;
		return &instance;
	}
	
	void UpdateThread();
	void StartUpdateThread();

	// Where the runtime GUI draws + the on-screen rect to map input into; set by the host each frame.
	// ABI: new members go at the END so existing offsets survive for non-rebuilt modules.
	uint64_t uiTarget = 0;            // render-target id (0 = backbuffer)
	int      uiX = 0, uiY = 0;        // target top-left in window pixels (input offset)
	int      uiW = 0, uiH = 0;        // target size in pixels

	// --- Fixed-frequency update thread ------------------------------------------------
	// World::FixedUpdate runs on its own thread at the world's fixedDt cadence, independent of
	// the render frame rate, and is gated to play mode. Hosts call Start once at boot, Stop at
	// shutdown. ABI: fixedThreadRun lives at the END of the class.
	void StartFixedThread();
	void StopFixedThread();
	void FixedThread();               // thread body (public for the thread bind, not for calling)
	volatile bool fixedThreadRun = false;

	// Pending screenshot request, captured at the END of World::Render — capturing mid-Update
	// would read an undefined backbuffer. ABI: appended at the END of the class.
	std::string pendingScreenshot;

	// --- ASYNC world load. A background job does the heavy part (content read, pak-layer merge,
	// JSON parse); the game thread only instantiates atoms, at the frame boundary, and only once
	// the script activates the staged world. ABI: appended at the END.
	std::atomic<int>      asyncLoadState{ 0 };       // 0 idle / 1 loading / 2 ready / 3 failed
	std::atomic<float>    asyncLoadProgress{ 0.f };  // coarse 0..1 while loading (1 = staged)
	std::atomic<unsigned> asyncLoadGen{ 0 };         // supersede/cancel: a stale job drops its result
	bool                  asyncLoadActivate = false; // script requested the swap (game thread only)
	std::string           asyncLoadPath;             // staged world's content path  (asyncLoadLock)
	std::shared_ptr<nlohmann::json> asyncLoadDoc;    // parsed (+merged) document    (asyncLoadLock)
	boost::mutex          asyncLoadLock;             // job -> game thread handoff guard

	// Compose a world's FINAL data string: the raw content file, or the mounted pak layers
	// merged (base + mods + overlay). Thread-safe; shared by OpenWorld and the async loader.
	bool   ComposeWorldData(const std::string& relPath, std::string& out);
	bool   StartWorldLoadAsync(const std::string& relPath); // begin/replace a background load
	double WorldLoadProgress();      // -1 idle/failed, else 0..1 (1 = staged, ready to activate)
	bool   WorldLoadReady();         // a staged world awaits ActivateLoadedWorld
	bool   ActivateLoadedWorld();    // queue the swap at the frame boundary; false if not ready
	void   CancelWorldLoadAsync();   // drop the loading/staged world
	void   ApplyAsyncWorldLoad();    // frame boundary (World::Update): perform the queued swap

	// --- INCREMENTAL (budgeted) activation: root atoms instantiate over frames within a per-frame
	// ms budget, optionally sorted outward from an origin. Emits "world.atomActivated"
	// {"id","name"} per root atom and "world.activationComplete" {"path"}. ABI: appended at the END.
	float activationBudgetMs  = 0.f;    // per-frame instantiation budget; 0 = whole world in one frame
	bool  activationOriginSet = false;  // sort root atoms by distance from activationOrigin
	float activationOrigin[3] = { 0.f, 0.f, 0.f };
	bool  activationActive = false;     // an incremental activation is in progress (game thread only)
	int   activationTotal = 0, activationDone = 0;
	std::shared_ptr<nlohmann::json> activationDoc;       // keeps the atom array alive while growing
	std::vector<const nlohmann::json*> activationQueue;  // sorted roots; next = activationQueue[activationDone]
	std::string activationPath;                          // world path (the completion event payload)

	void   SetWorldActivationBudget(double ms);
	double GetWorldActivationBudget();
	void   SetWorldActivationOrigin(float x, float y, float z);
	void   ClearWorldActivationOrigin();
	double WorldActivationProgress();                       // -1 idle, else 0..1 roots instantiated
	void   ContinueWorldActivation(bool ignoreBudget = false); // frame boundary: next budget slice
	void   FlushWorldActivation();                          // finish instantly
};

}  // namespace nuke

#endif // !NUKEE_APPINSTANCE_H
