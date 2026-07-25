#pragma once
#ifndef NUKEE_COMPONENT_ICONS_H
#define NUKEE_COMPONENT_ICONS_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

// A registered VIEWPORT ICON for a component type (the editor's clickable entity-icon
// overlay for atoms with no visible mesh). DATA ONLY — the registrant (an engine system or
// a MODULE) names its component TYPE; the editor matches by name at draw time, so module
// components get icons with NO editor linkage and NO editor hardcode. Mirrors
// AtomCreators/AssetCreators (0.6).
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
