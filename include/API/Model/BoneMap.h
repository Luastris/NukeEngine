#pragma once
#ifndef NUKEE_BONEMAP_H
#define NUKEE_BONEMAP_H
#include "NukeAPI.h"
#include <map>
#include <string>

namespace nuke {

// Retargeting asset (.nubonemap, plain JSON): renames a clip's channel bone names onto a
// skeleton's (e.g. "mixamorig:Hips" -> "Hips"). The Animator applies it when binding clip
// channels; runtime MapBone() entries override it.
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
