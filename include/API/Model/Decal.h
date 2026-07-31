#pragma once
#ifndef NUKEE_DECAL_H
#define NUKEE_DECAL_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"
#include "API/Model/Color.h"
#include <string>

namespace nuke {

class Texture;

// What a decal projects: Albedo = alpha-blended texture, LightProjector = additive gobo/cookie.
enum class DecalMode : int { Albedo = 0, LightProjector = 1 };
template<> struct NukeEnumInfo<DecalMode>
{
	static constexpr bool reflected = true;
	static const char* Name() { return "DecalMode"; }
	static void Register() { Reflect_RegisterEnum("DecalMode", { "Albedo", "LightProjector" }); }
};

// Screen-space decal: a box volume (the atom's transform) projecting a texture along its local Z
// onto every surface inside it. Works on any geometry, touches no material.
class NUKEENGINE_API Decal : public Component
{
	NUKE_CLASS(Decal, Component, "Rendering")
public:
	[[nuke::prop(asset="texture", label="Texture")]] std::string textureGuid;
	[[nuke::prop(label="Mode", enum="Albedo,Light Projector")]] DecalMode mode = DecalMode::Albedo;
	[[nuke::prop(label="Tint")]]      Color tint = Color(1.0f, 1.0f, 1.0f, 1.0f);
	[[nuke::prop(label="Intensity", min=0, max=8)]]  float intensity = 1.0f;
	[[nuke::prop(label="Angle Fade", min=0, max=1)]] float angleFade = 0.4f;   // 0 = project on everything, 1 = only faces aimed at the projector

	Texture* tex = nullptr;   // resolved from textureGuid by World::Render

	Decal();
	void Init(Atom* parent) override;
	void Update() override;
	void FixedUpdate() override;
	void Reset() override;
	void Pause() override;
	void Destroy() override;
};
}  // namespace nuke

#endif // !NUKEE_DECAL_H
