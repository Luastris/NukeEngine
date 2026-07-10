#pragma once
#ifndef NUKEE_BONEMAP_H
#define NUKEE_BONEMAP_H
#include "NukeAPI.h"
#include <map>
#include <string>

namespace nuke {

// Retargeting asset (.nubonemap, roadmap 3.1): renames a clip's channel bone names onto
// a skeleton's bone names (e.g. "mixamorig:Hips" -> "Hips"). PLAIN JSON — text-editable
// in the editor's file editor; the Animator references it by guid (asset picker) and
// applies it when binding clip channels. Runtime MapBone() entries override it.
class NUKEENGINE_API BoneMap
{
public:
	std::string guid;
	std::string name;                            // display name (defaults to the file stem)
	std::map<std::string, std::string> map;      // clip channel name -> skeleton bone name

	bool            SaveToFile(const std::string& path) const;
	static BoneMap* LoadFromFile(const std::string& path);
	static BoneMap* LoadFromString(const std::string& text, const std::string& name = std::string());   // packed content (3.2)

	// Fresh file template for the browser's "New" menu.
	static std::string Template();
};

}  // namespace nuke

#endif // !NUKEE_BONEMAP_H
