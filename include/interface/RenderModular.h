#pragma once
#ifndef NUKE_RENDER_MODULAR_H
#define NUKE_RENDER_MODULAR_H
#include "RenderModule.h"
#include <string>

namespace nuke {

// Scans the modules/ directory for render modules (DLLs/SOs exporting the
// "renderModule" symbol) and instantiates the one whose id == preferredId.
// If preferredId is empty, the first render module found is used. If the
// preferred id is not found but other render modules exist, falls back to the
// first one. Returns the created iRender* (kept alive together with the loaded
// module) or nullptr if no render module is available.
iRender* LoadRenderModule(const std::string& preferredId = "");

// Destroys the renderer and releases the render module.
void UnloadRenderModule();

}  // namespace nuke

#endif // !NUKE_RENDER_MODULAR_H
