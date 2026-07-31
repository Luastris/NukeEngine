#pragma once
#ifndef NUKEE_WORLD_HOOKS_H
#define NUKEE_WORLD_HOOKS_H
// Generic per-frame world render hooks for modules that need loop-level access rather than a
// component's OnRender. Register in OnLoad, unregister in Shutdown.
#include "NukeAPI.h"
#include <boost/function.hpp>
#include <vector>

namespace nuke {

class iRender;

struct WorldRenderHook
{
	virtual ~WorldRenderHook() {}
	// True = cameras this frame need the single-sample depth prepass (scene depth for the
	// module's shaders) even when no SSR/TAA/decals would run it.
	virtual bool wantsScenePrepass() { return false; }
	// Once per LIVE world render, before the shadow/probe/camera passes. `submitOpaques` draws
	// every opaque scene mesh with renderShadowObject into whatever depth target the hook has
	// bound — call it between the module's own begin/end seam calls, or not at all.
	virtual void preRender(iRender* /*r*/, const boost::function<void()>& /*submitOpaques*/) {}
};

NUKEENGINE_API void RegisterWorldRenderHook(WorldRenderHook* h);
NUKEENGINE_API void UnregisterWorldRenderHook(WorldRenderHook* h);
NUKEENGINE_API const std::vector<WorldRenderHook*>& WorldRenderHooks();

}  // namespace nuke

#endif // !NUKEE_WORLD_HOOKS_H
