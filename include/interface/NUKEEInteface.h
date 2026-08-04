#pragma once
#ifndef NUKEE_INTERFACE_H
#define NUKEE_INTERFACE_H
#include <boost/config.hpp>   // BOOST_SYMBOL_EXPORT
#include <cstdint>

#include <string>
#include <utility>   // std::pair (shipExtras dist copies)
#include <vector>
#include "AppInstance.h"

// ---- Module ABI level ---------------------------------------------------------------------
// Stamps every module DLL with the NUKEModule vtable level it was compiled against; the loader
// reads it at discovery (ModuleAbi in Modular.h) and hosts MUST guard calls to appended virtuals
// with `ModuleAbi(m) >= <level of the virtual>` — an older DLL has a shorter vtable. Unstamped
// DLLs report level 1. Bump when appending a virtual, and tag the virtual with its level:
//   1 — provides/phase/queryService/cookContent/sharedService/shipExtras
//   2 — editorTool
#define NUKE_MODULE_ABI 3
extern "C" { __declspec(dllexport) __declspec(selectany) int nuke_module_abi = NUKE_MODULE_ABI; }

// ---- Engine BINARY-COMPATIBILITY generation -------------------------------------------------
// Tracks the whole engine ABI a module was compiled against (exported class layouts, signatures);
// the loader REFUSES a module whose stamp differs. Bump on any break. Unstamped = 1.
//   1 — pre-stamp builds
//   2 — Atom gained `enabled`; NukeWindow.vsync moved to the tail
//   3 — Mesh v4 layout (indexArray/streams/sections/LODs); MeshRenderer gained matGuids/mats
//   4 — Mesh gained skelGuid (v5); SkinnedMeshRenderer/Skeleton/SocketAttachment added
//   5 — Mesh gained morph targets (v6); iRender gained gpuSkin/setSkinPalette
//   6 — Anim runtime v2: AnimClip v3 (notifies/curves/prop tracks), Animator controller mode
//       (smGuid/rootMotion/graph), Camera shake, ResDB AnimSM/BlendSpace registries
//   7 — Animator drives every subtree SkinnedMeshRenderer (smrs vector; modular characters)
//   8 — AnimClip gained skelGuid (v4; chain retargeting across skeletons)
//   9 — iPhysics gained SwingTwist ragdoll joints (vtable append); Ragdoll/.nurag added
//  10 — AnimSM::State gained node coords (nx/ny); Animator gained the editor preview seam
//       (previewSm/previewBlend/muteNotifies)
//  11 — Mesh gained import-time materials (defaultMats, .numesh v7); reflection gained the
//       script-class registry (Reflect_RegisterScriptClass / Reflect_ScriptClasses)
#define NUKE_ENGINE_ABI 11
extern "C" { __declspec(dllexport) __declspec(selectany) int nuke_engine_abi = NUKE_ENGINE_ABI; }

namespace nuke {

// When a plugin must be brought up: PHASE_BOOT during engine bootstrap (before the window/UI
// exist), PHASE_RUNTIME after the host is up (toggleable live).
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

	//Plugin DLL file name (e.g. "NukeScript.dll"), filled by runtime. Stable id for the per-project load list.
	std::string moduleFile;

	//Main process instance. Used by plugin from code.
	AppInstance* instance;

	//When Shutdown() called turn this to true, please. Otherwise plugin will work incorrectly.
	bool stopped;

	//True while the plugin is activated (OnLoad + Run done).
	bool loaded = false;

	//Synchronous activation hook, called by the loader BEFORE Run(). Register component types
	//here, NOT in a static initializer, so a disabled plugin leaves its types unregistered.
	virtual void OnLoad() {}

	//Function for run plugin. Runs on a BACKGROUND thread, not the game/UI thread: main-thread
	//state (PushWindow/PopWindow, the world, ResDB) must be deferred through Jobs::RunOnMain.
	virtual void Run(AppInstance* instance) = 0;

	//Returns true if mod has settings
	virtual bool HasSettings() = 0;

	//Opens menu if plugin has settings
	virtual void Settings() = 0;

	//Function that calls before plugin unloading. E.g. when app closes.
	virtual void Shutdown() = 0;

	// ---- Service metadata ----------------------------------------------------------------
	// ABI: new virtuals live at the END of the class so the vtable prefix stays stable.

	//Search/filter labels shown in the plugin window (e.g. {"lua", "scripting"}).
	std::vector<std::string> tags;

	//Engine service this plugin provides ("render"/"physics"/"audio"/"scripting"/...), or "" for
	//a utility plugin. At most ONE provider per exclusive service is active at a time.
	virtual const char* provides() { return ""; }

	//When the plugin must come up. PHASE_BOOT providers cannot be hot-swapped — switching
	//persists the choice and applies after restart.
	virtual int phase() { return PHASE_RUNTIME; }

	//For service providers: the interface instance to register under provides(). Registered by
	//the loader AFTER OnLoad() and revoked BEFORE Shutdown(). Utility plugins keep nullptr.
	virtual void* queryService() { return nullptr; }

	// Packaging hook: when the dependency walk reaches a file, every loaded module is asked.
	// Return TRUE if this module OWNS the file type (the file then ships), and append every
	// content it uses — content-relative paths or ResDB asset GUIDs — to `outUses` (each is
	// resolved and walked recursively). A file no engine loader and no loaded module claims
	// never ships. PURE: may be called from a worker thread, must not touch live module state.
	// ABI: appended at the END of the vtable.
	virtual bool cookContent(const char* contentRel, const char* bytes, uint64_t size,
	                         std::vector<std::string>& outUses) { return false; }

	// Whether this module's service is SHARED — several providers may be live at once (e.g.
	// scripting). Exclusive services keep false and the loader displaces the previous provider.
	// ABI: appended at the END of the vtable.
	virtual bool sharedService() { return false; }

	// Extra files to ship with a packaged game beyond this module's own DLL:
	//   * `pakFiles`  — project-relative paths forced into the game pak;
	//   * `distFiles` — (source -> dist-relative destination) copies into the dist tree. A
	//                   relative source resolves against the runtime dir being shipped, an
	//                   absolute one against itself; a directory source copies recursively.
	// ABI: appended at the END of the vtable.
	virtual void shipExtras(const char* projectDir,
	                        std::vector<std::string>& pakFiles,
	                        std::vector<std::pair<std::string, std::string>>& distFiles) {}

	// Editor-only companion module supplying asset editors/panels for a runtime module's file
	// types. The editor offers it by default, and never ships it with a game.
	// ABI level 2: callers must guard with ModuleAbi(m) >= 2.
	virtual bool editorTool() { return false; }

	// The RUNTIME module this one is a companion to, by file name ("NukeTilemap.dll"); "" = none.
	// A companion has nothing to edit while its runtime module is off, so the editor keeps it
	// off too. ABI level 3: callers MUST guard with ModuleAbi(m) >= 3 — a module built against
	// an older header has no such slot, and the call would land on whatever follows the vtable.
	virtual const char* companionOf() { return ""; }
};

}  // namespace nuke

#endif
