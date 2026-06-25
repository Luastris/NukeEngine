#pragma once
#ifndef NUKEE_APPINSTANCE_H
#define NUKEE_APPINSTANCE_H
#include "NukeAPI.h"
#include <boost/thread.hpp>
#include <boost/container/list.hpp>
#include <boost/container/map.hpp>
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



	World* currentScene = new World();
    KeyBoard* keyboard = nullptr;
    Mouse* mouse = nullptr;
	Config* config = nullptr;
    iRender* render = nullptr;

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
