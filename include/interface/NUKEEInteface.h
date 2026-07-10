#pragma once
#ifndef NUKEE_INTERFACE_H
#define NUKEE_INTERFACE_H
#include <boost/config.hpp>   // BOOST_SYMBOL_EXPORT (the loader side pulls boost/dll itself)
#include <cstdint>

#include <string>
#include <vector>
#include "AppInstance.h"

namespace nuke {

// When a plugin must be brought up. PHASE_BOOT providers (e.g. the renderer) are enabled
// during engine bootstrap, BEFORE the window/UI exist; PHASE_RUNTIME plugins are enabled
// after the host is up and can be toggled live.
enum PluginPhase { PHASE_BOOT = 0, PHASE_RUNTIME = 1 };

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

	// ---- Service metadata (unified plugin model) ----------------------------------------
	// New virtuals live at the END of the class so the vtable prefix stays stable for
	// modules that haven't been rebuilt yet. (All in-tree plugins rebuild together anyway.)

	//Search/filter labels shown in the plugin window (e.g. {"lua", "scripting"}).
	std::vector<std::string> tags;

	//Which engine service this plugin provides: "render" | "physics" | "audio" | "scripting"
	//| ... — or "" for an ordinary utility plugin. At most ONE provider per service is active;
	//enabling a provider disables the current sibling (radio behavior in the plugin window).
	virtual const char* provides() { return ""; }

	//When the plugin must come up. Service providers the host cannot run without (the
	//renderer: window/device) return PHASE_BOOT and are enabled during bootstrap; they can
	//NOT be hot-swapped — switching persists the choice and applies after restart.
	virtual int phase() { return PHASE_RUNTIME; }

	//For service providers: the interface instance to register under provides() — e.g. a
	//render plugin returns its iRender*. Called by the loader AFTER OnLoad() (registered via
	//Services_Provide) and revoked BEFORE Shutdown(), so the lifecycle is loader-bound and a
	//provider can't forget to revoke. Utility plugins keep the nullptr default.
	virtual void* queryService() { return nullptr; }

	// ---- Shipping cooker hook (3.2) ------------------------------------------------------
	// The editor's Package Project walks the dependency closure of the shipped worlds; the
	// EDITOR itself only understands the engine's serialized data (worlds/prefabs/materials
	// — reflected props, GUIDs, content paths). Everything else is a MODULE's domain: when
	// the walk reaches a file, every loaded module is asked. Return TRUE if this module OWNS
	// the file type (its scripts/data — the file ships), and append every content it uses —
	// content-relative paths, hardcoded paths, or ResDB asset GUIDs — to `outUses` (each is
	// resolved and walked recursively). A file no engine loader and no LOADED module claims
	// never ships (e.g. .lua without the scripting module). PURE function: may be called
	// from a worker thread, must not touch live module state.
	// ABI: new virtuals append at the END of the class, never mid-vtable.
	virtual bool cookContent(const char* contentRel, const char* bytes, uint64_t size,
	                         std::vector<std::string>& outUses) { return false; }
};

}  // namespace nuke

#endif
