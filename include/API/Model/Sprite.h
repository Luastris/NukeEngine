#pragma once
#ifndef NUKEE_SPRITE_H
#define NUKEE_SPRITE_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"
#include "API/Model/Color.h"
#include <string>

namespace nuke {

class Texture;

// How a sprite quad is oriented. Reflected enum (typed in C#/Lua, combo in the inspector).
enum class SpriteMode : int { Plane = 0, Billboard = 1 };
template<> struct NukeEnumInfo<SpriteMode>
{
	static constexpr bool reflected = true;
	static const char* Name() { return "SpriteMode"; }
	static void Register() { Reflect_RegisterEnum("SpriteMode", { "Plane", "Billboard" }); }
};

// A textured quad in the world — a 2D sprite. Plane mode lies in the atom's transform (for 2D
// games and a Canvas); Billboard mode always faces the camera (effects, particles). Drawn unlit
// and alpha-blended after the opaque geometry (transparent, depth-tested). Sibling of a Transform.
class NUKEENGINE_API Sprite : public Component
{
	NUKE_CLASS(Sprite, Component)
public:
	[[nuke::prop(asset="texture", label="Texture")]] std::string textureGuid;
	[[nuke::prop(label="Tint")]]   Color tint = Color(1.0f, 1.0f, 1.0f, 1.0f);
	[[nuke::prop(label="Width")]]  float width  = 1.0f;   // quad size in world units
	[[nuke::prop(label="Height")]] float height = 1.0f;
	[[nuke::prop(label="Pivot X", min=0, max=1)]] float pivotX = 0.5f;   // 0 = left edge, 1 = right edge
	[[nuke::prop(label="Pivot Y", min=0, max=1)]] float pivotY = 0.5f;   // 0 = bottom edge, 1 = top edge
	[[nuke::prop(label="Mode", enum="Plane,Billboard")]] SpriteMode mode = SpriteMode::Plane;
	[[nuke::prop(label="Flip X")]] bool flipX = false;
	[[nuke::prop(label="Flip Y")]] bool flipY = false;
	// (nine-slice lives ON THE TEXTURE — Texture::nineSlice + slice borders, set in the slicer.)

	// Runtime UV region [u0,v0,u1,v1] within the texture — full frame by default. Sprite animation
	// (SpriteAnimator) drives this to show atlas cells. Not serialized (comes from the frame/anim).
	float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
	Texture* tex = nullptr;   // resolved from textureGuid by World::Render (via ResDB)

	Sprite();

	// Reflected API (C#/Lua).
	[[nuke::func]] void SetTint(double r, double g, double b, double a);
	[[nuke::func]] void SetSize(double w, double h);
	[[nuke::func]] void SetPivot(double x, double y);
	[[nuke::func]] void SetFrame(double u0v, double v0v, double u1v, double v1v);   // UV region (atlas cell)

	void Init(Atom* parent) override;
	void Update() override;
	void FixedUpdate() override;
	void Reset() override;
	void Pause() override;
	void Destroy() override;
};
}  // namespace nuke

#endif // !NUKEE_SPRITE_H
