#pragma once
#ifndef NUKEE_RESOURCE_H
#define NUKEE_RESOURCE_H
#include "NukeAPI.h"
#include <string>
#include <vector>

namespace nuke {

class Mesh;
class Material;
class Texture;
class Shader;

// Public asset-database facade: find/enumerate assets by GUID (the id worlds and
// components reference) without reaching into ResDB internals. A Resource is a light
// DESCRIPTOR; live objects come from the typed getters (null when the guid is unknown
// or belongs to another type).
class NUKEENGINE_API Resource
{
public:
	std::string guid;
	std::string type;   // "mesh" | "material" | "texture" | "shader" | "" (unknown)
	std::string name;   // display name where the asset carries one (else "")
	std::string path;   // source file path when known (imported assets; "" for builtins)

	bool Valid() const { return !type.empty(); }

	static Resource Find(const std::string& guid);          // descriptor lookup
	static Resource FindByPath(const std::string& path);    // by imported source path
	static std::vector<Resource> All(const std::string& type = "");   // "" = every known asset

	static Mesh*     GetMesh(const std::string& guid);
	static Material* GetMaterial(const std::string& guid);
	static Texture*  GetTexture(const std::string& guid);
	static Shader*   GetShader(const std::string& guid);
};

}  // namespace nuke

#endif // !NUKEE_RESOURCE_H
