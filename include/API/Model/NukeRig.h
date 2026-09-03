#pragma once
#ifndef NUKEE_NUKERIG_H
#define NUKEE_NUKERIG_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

class Skeleton;
class BoneMap;
struct MeshBone;

// C1: the engine's canonical humanoid rig ("NukeRig") + naming-scheme presets for the
// common sources. The canonical skeleton and the .nubonemap presets register as builtins
// (ResDB init), the importer recognizes a source's naming scheme, stamps canonical rig
// chains onto the imported .nuskel and pre-assigns the matching bone map — the existing
// auto-retargeter then plays any clip library on any character, both ways.
class NUKEENGINE_API NukeRig
{
public:
	enum Scheme { None = 0, Canonical, CC, Mixamo, UE };

	// The canonical humanoid skeleton asset: T-pose bind, rig chains, groups, hand/head
	// sockets. guid = "builtin:nukerig".
	static Skeleton* CreateCanonical();

	// Rename preset for a source scheme (CC/Mixamo/UE -> canonical names); null for others.
	// guids = "builtin:bonemap-cc" / "builtin:bonemap-mixamo" / "builtin:bonemap-ue".
	static BoneMap*    CreatePreset(Scheme s);
	static const char* PresetGuid(Scheme s);     // "" for None/Canonical
	static const char* SchemeName(Scheme s);     // display ("Mixamo")

	// Best-matching naming scheme of a bone palette (>= 6 recognized bones), else None.
	static Scheme Detect(const std::vector<MeshBone>& bones);

	// Add the canonical rig chains to `sk` using the scheme's own bone names (bones the
	// skeleton lacks are skipped; existing chains with the same name are kept).
	static void StampChains(Skeleton* sk, Scheme s);

	// C2: tune a recognized CC character's material by its conventional slot name
	// (Std_Skin_* / eyes / teeth / hair ...); false = no convention matched.
	static bool TuneCCMaterial(class Material* m);
	// A material name that reads as Reallusion CC content (Std_* / game-base Ga_*): CC5's UE5
	// export preset renames the bones, so the importer detects CC scenes by materials too.
	static bool LooksLikeCCMaterial(const std::string& name);

	// C2: CC ExPlus blendshape -> ARKit-52 morph name map ("builtin:morphmap-arkit-cc");
	// SkinnedMeshRenderer resolves morph names through it (Morph Map slot).
	static BoneMap*    CreateMorphPreset();
	static const char* MorphPresetGuid();
};

}  // namespace nuke

#endif // !NUKEE_NUKERIG_H
