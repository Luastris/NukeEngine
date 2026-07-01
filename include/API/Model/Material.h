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
#include "reflect/Reflect.h"
#include <assimp/material.h>

namespace nuke {

using namespace std;

class NUKEENGINE_API Material
{
    // Reflected so the inspector auto-draws every field (asset pickers for maps, color swatch, sliders).
    // Base "Object" (not "Component") keeps it OUT of the Add-Component menu.
    NUKE_CLASS(Material, Object)
public:
    char* name = nullptr;          // legacy raw name (kept for old call sites)

    std::string guid;              // asset id (generated on import; "builtin:default" for the default)
    std::string matName;           // display name
    [[nuke::prop(label="Base Color")]] Color color = Color(1, 1, 1, 1);   // base color RGBA

    // Texture asset references (.nutex GUIDs). diffuse = base color.
    [[nuke::prop(asset="texture", label="Base Color Map")]] std::string diffuseGuid;
    [[nuke::prop(asset="texture", label="Normal Map")]]     std::string normalGuid;
    // Specular reflectance map (KHR_materials_specular): tints/scales the dielectric F0 (white = standard 0.04).
    [[nuke::prop(asset="texture", label="Specular Map")]]   std::string specularGuid;
    // PBR (metallic-roughness, UE/Unity-Lit) maps + scalar params.
    [[nuke::prop(asset="texture", label="Metallic-Roughness Map")]] std::string metalRoughGuid; // G=rough,B=metal
    [[nuke::prop(asset="texture", label="Occlusion Map")]]          std::string occlusionGuid;  // R = AO
    [[nuke::prop(asset="texture", label="Emissive Map")]]           std::string emissiveGuid;
    [[nuke::prop(label="Metallic", min=0, max=1)]]  float metallic  = 0.0f;
    [[nuke::prop(label="Roughness", min=0, max=1)]] float roughness = 0.6f;
    // KHR specular factor: scales dielectric specular reflectance (1 = default 0.04 F0; 0 = no dielectric specular).
    [[nuke::prop(label="Specular", min=0, max=1)]]  float specular = 1.0f;
    [[nuke::prop(label="Emissive")]]  Color emissive = Color(0, 0, 0, 1);
    [[nuke::prop(label="Emissive Intensity")]] float emissiveIntensity = 0.0f;
    // Whether this surface casts shadows. Default on; turn on for transparent surfaces too — the
    // shadow pass alpha-dithers by the material's alpha so see-through surfaces cast lighter shadows.
    [[nuke::prop(label="Cast Shadows")]] bool castShadows = true;
    // Blend mode: Opaque writes depth + no blend; Transparent/Additive blend and DON'T write depth (so the
    // engine sorts them back-to-front per camera). 0 = Opaque, 1 = Transparent (alpha), 2 = Additive.
    [[nuke::prop(label="Blend", enum="Opaque,Transparent,Additive")]] int blendMode = 0;

    Texture* diff = nullptr;       // runtime-resolved textures (via Resolve())
    Texture* norm = nullptr;
    Texture* spec = nullptr;
    Texture* mr   = nullptr;       // metallic-roughness
    Texture* ao   = nullptr;       // occlusion
    Texture* em   = nullptr;       // emissive

    Shader*      shader = nullptr;
    aiMaterial*  aiMat  = nullptr;

    // Shader asset ref; default = engine "world".
    [[nuke::prop(asset="shader", label="Shader")]] std::string shaderGuid = "world";

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
