#pragma once
#ifndef NUKEE_INTERFACE_H
#define NUKEE_INTERFACE_H
#include <boost/dll.hpp>
using namespace boost;

#include "AppInstance.h"

namespace nuke {


class BOOST_SYMBOL_EXPORT NUKEModule {
public:
	//Title of the plugin
	char title[256];
	
	//Description of the plugin
	char description[4096];
	
	//Author name
	char author[256];

	//Author or plugin site
	char site[1024];

	//Plugin version
	char version[30];

	//Path to the plugin, filled by runtime
	std::string modulePath;

	//Plugin DLL file name (e.g. "NukeScript.dll"), filled by runtime. Stable id for the
	//per-project load list — plugins live in a shared pool, projects pick which to load.
	std::string moduleFile;

	//Main process instance. Used by plugin from code.
	AppInstance* instance;

	//When Shutdown() called turn this to true, please. Otherwise plugin will work incorrectly.
	bool stopped;

	//True while the plugin is activated (OnLoad + Run done). Discovered-but-not-loaded
	//plugins still appear in the manager so they can be toggled on.
	bool loaded = false;

	//Synchronous activation hook, called by the loader BEFORE Run() when the plugin is
	//enabled. Register component types here (NOT in a static initializer) so a disabled
	//plugin leaves its types unregistered and its components stay inert placeholders.
	virtual void OnLoad() {}

	//Function for run plugin. Runs in the background thread.
	virtual void Run(AppInstance* instance) = 0;

	//Returns true if mod has settings
	virtual bool HasSettings() = 0;

	//Opens menu if plugin has settings
	virtual void Settings() = 0;

	//Function that calls before plugin unloading. E.g. when app closes.
	virtual void Shutdown() = 0;
};

}  // namespace nuke

#endif
