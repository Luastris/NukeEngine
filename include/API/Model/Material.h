#pragma once
#ifndef NUKEE_MATERIAL_H
#define NUKEE_MATERIAL_H
#include "NukeAPI.h"
#include <iostream>
#include <string>
#include "Texture.h"
#include "Shader.h"
#include <assimp/material.h>

namespace nuke {

using namespace std;

class NUKEENGINE_API Material
{
public:
    char* name = nullptr;          // legacy raw name (kept for old call sites)

    std::string guid;              // asset id (generated on import; "builtin:default" for the default)
    std::string matName;           // display name
    float       color[4] = { 1, 1, 1, 1 };   // base/diffuse color RGBA

    // Texture asset references (.nutex GUIDs).
    std::string diffuseGuid, normalGuid, specularGuid;

    Texture* diff = nullptr;       // runtime-resolved textures (via Resolve())
    Texture* norm = nullptr;
    Texture* spec = nullptr;

    Shader*      shader = nullptr;
    aiMaterial*  aiMat  = nullptr;

    Material();

    void ImportAiMaterial(aiMaterial* m);   // name + color only (textures handled by the importer)
    void Resolve();                         // bind diff/norm/spec from ResDB by GUID

    // Native asset format (.numat, JSON): guid + name + color + texture GUIDs.
    bool             SaveToFile(const std::string& path) const;
    static Material* LoadFromFile(const std::string& path);
};
}  // namespace nuke

#endif // !NUKEE_MATERIAL_H
