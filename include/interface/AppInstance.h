#pragma once
#ifndef NUKEE_APPINSTANCE_H
#define NUKEE_APPINSTANCE_H
#include "NukeAPI.h"
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



	World* currentScene = new World();
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

	bool isEditor();
	void setEditor(bool editor);

	static AppInstance* GetSingleton() 
	{
		static AppInstance instance;
		return &instance;
	}
	
	void UpdateThread();
	void StartUpdateThread();
};

}  // namespace nuke

#endif // !NUKEE_APPINSTANCE_H
