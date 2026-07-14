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

// What a decal projects. Reflected enum (typed in C#/Lua).
//   Albedo         — a texture stamped onto surfaces (blood, bullet holes, posters): alpha-blended.
//   LightProjector — a light/gobo pattern (a "slide projector" / flashlight cookie): added to the image.
enum class DecalMode : int { Albedo = 0, LightProjector = 1 };
template<> struct NukeEnumInfo<DecalMode>
{
	static constexpr bool reflected = true;
	static const char* Name() { return "DecalMode"; }
	static void Register() { Reflect_RegisterEnum("DecalMode", { "Albedo", "LightProjector" }); }
};

// A screen-space (deferred-style) decal: a BOX volume in the world (the atom's transform — position,
// rotation, scale = box size). Every surface inside the box (reconstructed from the depth prepass)
// gets the decal texture projected along the box's local Z. Works on ANY geometry, touches no material.
// Sibling of a Transform. Like Unreal decal actors / Unity HDRP-URP decal projectors.
class NUKEENGINE_API Decal : public Component
{
	NUKE_CLASS(Decal, Component)
public:
	[[nuke::prop(asset="texture", label="Texture")]] std::string textureGuid;
	[[nuke::prop(label="Mode", enum="Albedo,Light Projector")]] DecalMode mode = DecalMode::Albedo;
	[[nuke::prop(label="Tint")]]      Color tint = Color(1.0f, 1.0f, 1.0f, 1.0f);
	[[nuke::prop(label="Intensity", min=0, max=8)]]  float intensity = 1.0f;
	// Fade where the receiving surface turns away from the projection axis (0 = no fade / project onto
	// everything, 1 = only faces pointing straight at the projector). Kills stretching on side walls.
	[[nuke::prop(label="Angle Fade", min=0, max=1)]] float angleFade = 0.4f;

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
