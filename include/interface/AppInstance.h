#pragma once
#ifndef NUKEE_APPINSTANCE_H
#define NUKEE_APPINSTANCE_H
#include "NukeAPI.h"
#include <cstdint>
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

	// Per-window open/closed flag, owned by the host and keyed by window id (created open on
	// first access). Plugins/panels use it for ImGui::Begin p_open so the editor can persist it.
	bool* WindowOpen(const char* key);



	World* currentWorld = new World();
    KeyBoard* keyboard = nullptr;
    Mouse* mouse = nullptr;
	Config* config = nullptr;
    iRender* render = nullptr;
	std::map<std::string, bool> windowOpen;   // window id -> open flag (persisted by the editor)

	// Root for CONTENT relative paths (scripts, assets referenced by path) — the project's
	// content folder. Engine resources (config/fonts/shaders/modules) stay relative to the
	// EXE/cwd; only content resolves here, so a script "scripts/spin.lua" is found in the
	// project, not the exe root. Set by the host (editor/player) at startup.
	std::string contentRoot;
	// Resolve a content path: absolute -> as-is; else prefer <contentRoot>/path, falling back
	// to a cwd-relative path if that exists (so nothing root-relative breaks).
	std::string ResolveContent(const std::string& path) const;

	// --- World (a.k.a. "level") load/save API. Paths are relative to the project content root
	// (resolved via ResolveContent), so worlds always live IN the project content, not "wherever".
	// Shared by the editor (New/Open/Save) and the game (loads the project's default world).
	std::string currentWorldPath;                     // content-relative path of the open world ("" = unsaved)
	// GAME-INITIATED world switches (Game.LoadWorld from a script) arrive MID-TICK — while
	// World::Update/FixedUpdate iterate the very hierarchy a load would replace. The tick
	// sets worldTickActive (under the game lock); OpenWorld then only QUEUES the path and
	// World::Update applies it at the frame boundary, after its traversal.
	bool        worldTickActive = false;
	std::string pendingWorldLoad;
	// SAVEGAME load (Game.LoadGame, 6.6): an ABSOLUTE .nusave path, applied by World::Update
	// at the frame boundary via LoadFromFile — same mid-tick safety rule as pendingWorldLoad.
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

	// Where the runtime GUI (NukeGUI) draws + the on-screen rect to map input into. The host sets this
	// each frame: editor -> the viewport RT + its screen rect; Player -> 0 (backbuffer) + full window.
	// (ABI: new members at the END to keep existing offsets for non-rebuilt modules.)
	uint64_t uiTarget = 0;            // render-target id (0 = backbuffer)
	int      uiX = 0, uiY = 0;        // target top-left in window pixels (input offset)
	int      uiW = 0, uiH = 0;        // target size in pixels

	// --- Fixed-frequency update thread ------------------------------------------------
	// World::FixedUpdate (physics + Component::FixedUpdate) runs on ITS OWN THREAD at the
	// world's fixedDt cadence — fully independent of the render frame rate. Gated to play
	// mode (playState == 1): in the editor it idles until PIE, in the Player it always
	// ticks. Hosts call StartFixedThread() once at boot and StopFixedThread() at shutdown.
	// (ABI: fixedThreadRun at the END of the class.)
	void StartFixedThread();
	void StopFixedThread();
	void FixedThread();               // thread body (public for the thread bind, not for calling)
	volatile bool fixedThreadRun = false;

	// Pending screenshot request (Game.Screenshot): captured at the END of World::Render -
	// the frame is fully drawn there; capturing from a script (mid-Update, pre-render) would
	// read a stale/undefined rotated backbuffer. (ABI: appended at the END of the class.)
	std::string pendingScreenshot;
};

}  // namespace nuke

#endif // !NUKEE_APPINSTANCE_H
