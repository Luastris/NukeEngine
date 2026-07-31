#pragma once
#ifndef NUKEE_COMPONENT_ICONS_H
#define NUKEE_COMPONENT_ICONS_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

// A registered viewport icon for a component type (the editor's overlay for atoms with no
// visible mesh). DATA ONLY — the editor matches by type name at draw time, so no linkage is needed.
struct ComponentIcon
{
	std::string component;            // reflected component TYPE NAME, e.g. "ParticleEmitter"
	std::string glyph;                // UTF-8 icon glyph (write \xee.. escapes in source!)
	float color[4] = { 1, 1, 1, 0.92f };   // icon tint, rgba 0..1
};

NUKEENGINE_API void RegisterComponentIcon(const ComponentIcon& desc);
NUKEENGINE_API const std::vector<ComponentIcon>& ComponentIcons();

}  // namespace nuke

#endif // !NUKEE_COMPONENT_ICONS_H
