#pragma once
#ifndef NUKEE_ASSET_CREATORS_H
#define NUKEE_ASSET_CREATORS_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

// A plugin-registered "New ..." command for the editor's browser. The plugin supplies only DATA
// (no boost / no file IO): a menu label, the file extension, a default base name, and the initial
// file content (text). The EDITOR creates the file itself (UniquePath, write, rename).
struct AssetCreator
{
	std::string label;     // menu entry, e.g. "Lua Script"
	std::string ext;       // file extension incl. dot, e.g. ".lua"
	std::string baseName;  // default file name (no ext), e.g. "New Script"
	std::string content;   // initial file text
};

NUKEENGINE_API void RegisterAssetCreator(const std::string& label, const std::string& ext,
                                         const std::string& baseName, const std::string& content);
NUKEENGINE_API const std::vector<AssetCreator>& AssetCreators();

}  // namespace nuke

#endif // !NUKEE_ASSET_CREATORS_H
