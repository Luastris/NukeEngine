#pragma once
#ifndef NUKEE_MATERIAL_H
#define NUKEE_MATERIAL_H
#include "NukeAPI.h"
#include <iostream>
#include <string>
#include <map>
#include <array>
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

    // Appended at the END to keep the existing member layout stable (so the renderer, which
    // reads color/diff, doesn't need a rebuild). Shader asset ref; default = engine "world".
    std::string  shaderGuid = "world";

    // Custom shader-parameter VALUES, keyed by the param name from the shader's MatCB (Shader::props).
    // Only meaningful on a material INSTANCE (mr->mat); the shared asset leaves this empty.
    std::map<std::string, std::array<float, 4>> props;

    Material();

    void ImportAiMaterial(aiMaterial* m);   // name + color only (textures handled by the importer)
    void Resolve();                         // bind diff/norm/spec from ResDB by GUID
    // Deep copy for instancing: a MeshRenderer owns an INSTANCE (clone) of the referenced asset, so
    // scene edits (color/shader/props) live on the instance + save with the world, never touching
    // the original .numat. Re-resolves its texture/shader pointers from ResDB.
    Material* Clone() const;

    // Native asset format (.numat, JSON): guid + name + color + texture GUIDs.
    bool             SaveToFile(const std::string& path) const;
    static Material* LoadFromFile(const std::string& path);
};
}  // namespace nuke

#endif // !NUKEE_MATERIAL_H
