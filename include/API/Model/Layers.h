#pragma once
#ifndef NUKEE_LAYERS_H
#define NUKEE_LAYERS_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>
#include <vector>

namespace nuke {

// RENDER LAYERS (channels): 32 fixed slots with project-editable NAMES (Unity-style). Every Atom
// carries a layer INDEX (Atom::layer, 0..31); every Camera carries a 32-bit MASK (Camera::layerMask,
// bit i = "this camera renders atoms on layer i", -1 = everything). World::Render filters the camera
// passes (meshes, sprites, decals, module render hooks, the G-buffer prepass) by that mask; global
// passes (shadow maps, reflection probes, the RT scene) intentionally see the whole world.
//
// Slot names are PROJECT data: the editor edits them in Project Settings and persists them in
// game.nuproj ("layers"); both hosts push them here at boot. Index 0 defaults to "Default";
// index 31 is conventionally the editor's own objects (the editor camera lives there).
class NUKEENGINE_API Layers
{
	NUKE_CLASS_NOCREATE(Layers, Object)
public:
	static const int kCount = 32;

	// ---- reflected (scripts: nuke.Layers.* / Layers.*) ----------------------------------------------
	[[nuke::func]] static std::string Name(double index);                          // "" for unnamed slots
	[[nuke::func]] static double      IndexOf(const std::string& name);            // -1 if no slot has it
	[[nuke::func]] static double      MaskOf(const std::string& names);            // "UI,FX" -> bitmask (-1 on "*")
	[[nuke::func]] static void        SetName(double index, const std::string& name);

	// ---- native (hosts/editor) -----------------------------------------------------------------------
	static void        SetAll(const std::vector<std::string>& names);   // project load (missing -> defaults)
	static std::vector<std::string> All();                              // 32 entries (project save / UI)
};

}  // namespace nuke
#endif // !NUKEE_LAYERS_H
