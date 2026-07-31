#pragma once
#ifndef NUKEE_LAYERS_H
#define NUKEE_LAYERS_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>
#include <vector>

namespace nuke {

// Render layers: 32 slots with project-editable names. Atom::layer is an index, Camera::layerMask a
// 32-bit mask (-1 = everything) filtering the camera passes; global passes see the whole world.
// Names live in game.nuproj ("layers"); index 0 = "Default", index 31 = editor objects by convention.
class NUKEENGINE_API Layers
{
	NUKE_CLASS_NOCREATE(Layers, Object)
public:
	static const int kCount = 32;

	// ---- reflected ----
	[[nuke::func]] static std::string Name(double index);                          // "" for unnamed slots
	[[nuke::func]] static double      IndexOf(const std::string& name);            // -1 if no slot has it
	[[nuke::func]] static double      MaskOf(const std::string& names);            // "UI,FX" -> bitmask (-1 on "*")
	[[nuke::func]] static void        SetName(double index, const std::string& name);

	// ---- native (hosts/editor) ----
	static void        SetAll(const std::vector<std::string>& names);   // project load (missing -> defaults)
	static std::vector<std::string> All();                              // 32 entries (project save / UI)
};

}  // namespace nuke
#endif // !NUKEE_LAYERS_H
