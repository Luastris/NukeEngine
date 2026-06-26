#pragma once
#ifndef NUKE_RENDER_MODULAR_H
#define NUKE_RENDER_MODULAR_H
#include "NukeAPI.h"
#include "RenderModule.h"
#include <string>

namespace nuke {

// Scans the modules/ directory for render modules (DLLs/SOs exporting the
// "renderModule" symbol) and instantiates the one whose id == preferredId.
// If preferredId is empty, the first render module found is used. If the
// preferred id is not found but other render modules exist, falls back to the
// first one. Returns the created iRender* (kept alive together with the loaded
// module) or nullptr if no render module is available.
NUKEENGINE_API iRender* LoadRenderModule(const std::string& preferredId = "");

// Destroys the renderer and releases the render module.
NUKEENGINE_API void UnloadRenderModule();

// Load the engine's built-in shader files from `dir` (e.g. "shaders") and push their source
// into the renderer (render->setShaderSource) BEFORE render->init(). Each "<name>.hlsl" is
// registered under "<name>" (so "world.vs.hlsl" -> "world.vs"). The ENGINE does the file IO;
// the render module stays free of file/boost dependencies.
NUKEENGINE_API void LoadBuiltinShaders(iRender* render, const std::string& dir);

}  // namespace nuke

#endif // !NUKE_RENDER_MODULAR_H
