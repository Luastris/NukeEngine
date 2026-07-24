#pragma once
#ifndef NUKEE_ATOM_CREATORS_H
#define NUKEE_ATOM_CREATORS_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

// A registered ATOM TEMPLATE for the editor's create menu (the viewport "+" / hierarchy
// context). DATA ONLY — the registrant (an engine system or a MODULE) names the components;
// the editor instantiates them through reflection (Registry_All by type name), so module
// atom types appear in the menu with NO hard linkage, mirroring AssetCreators (0.6).
struct AtomCreator
{
	std::string category;                 // menu group, e.g. "Effects" ("" = top level)
	std::string label;                    // menu entry + default atom name, e.g. "Particle Emitter"
	std::string icon;                     // UTF-8 glyph for the menu ("" = generic)
	std::vector<std::string> components;  // reflected component TYPE NAMES to add, in order
};

NUKEENGINE_API void RegisterAtomCreator(const AtomCreator& desc);
NUKEENGINE_API const std::vector<AtomCreator>& AtomCreators();

}  // namespace nuke

#endif // !NUKEE_ATOM_CREATORS_H
