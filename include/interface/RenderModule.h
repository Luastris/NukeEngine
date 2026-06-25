#pragma once
#ifndef NUKE_RENDER_MODULE_H
#define NUKE_RENDER_MODULE_H

// A NUKERenderModule is a loadable module that PROVIDES a renderer.
// It is fundamentally different from a NUKEModule (gameplay/editor extension):
//   * it is loaded FIRST, during engine bootstrap, before the main loop;
//   * exactly ONE is loaded, selected by `id` from config (e.g. "diligent");
//   * it acts as a FACTORY that creates an iRender implementation;
//   * the created renderer drives the main thread / window.
//
// A render-module DLL exports a single instance of its NUKERenderModule subclass
// under the symbol name "renderModule" (extension plugins use "plugin"); that is
// how the loader tells the two kinds of modules apart.
//
// Keeping this contract separate means the engine core (NukeEngine) does not link
// any concrete renderer (bgfx / Diligent): each backend lives in its own DLL and
// the engine talks only to the abstract iRender interface.

#include <string>
#include <boost/config.hpp>
#include "../render/irender.h"

namespace nuke {

class BOOST_SYMBOL_EXPORT NUKERenderModule
{
public:
	// Human-readable metadata (mirrors NUKEModule).
	char title[256]       = {};
	char description[4096] = {};
	char author[256]      = {};
	char site[1024]       = {};
	char version[30]      = {};

	// Stable backend identifier used to pick a renderer from config, e.g. "diligent", "bgfx".
	char id[64] = {};

	// Path to the loaded module, filled in by the runtime.
	std::string modulePath;

	// Create the renderer instance. Ownership is returned to the caller; the module
	// must stay loaded for the lifetime of the returned object (its vtable lives here).
	virtual iRender* CreateRenderer() = 0;

	// Destroy a renderer previously returned by CreateRenderer().
	virtual void DestroyRenderer(iRender* renderer) = 0;

	// Called before the module is unloaded (e.g. on app shutdown).
	virtual void Shutdown() {}

	virtual ~NUKERenderModule() {}
};

}  // namespace nuke

#endif // !NUKE_RENDER_MODULE_H
