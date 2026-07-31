#pragma once
#ifndef NUKEE_ATOM_CREATORS_H
#define NUKEE_ATOM_CREATORS_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

// A registered atom template for the editor's create menu. DATA ONLY — the registrant names the
// components and the editor instantiates them through reflection, so no hard linkage is needed.
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
