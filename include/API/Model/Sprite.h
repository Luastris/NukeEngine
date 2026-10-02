#pragma once
#ifndef NUKEE_SPRITE_H
#define NUKEE_SPRITE_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"
#include "API/Model/Color.h"
#include "API/Model/Widget.h"
#include <string>

namespace nuke {

class Texture;

// A textured quad: the image widget. The rect, Plane / Billboard mode and raycast flag come
// from Widget (width / height in world units); on a canvas it is the UI image (nine-slice via
// the texture's slices), outside one a world sprite. Drawn unlit, alpha-blended after the
// opaque geometry (depth-tested).
class NUKEENGINE_API Sprite : public Widget
{
	NUKE_CLASS(Sprite, Widget, "UI & 2D")
public:
	[[nuke::prop(asset="texture", label="Texture")]] std::string textureGuid;
	[[nuke::prop(label="Tint")]]   Color tint = Color(1.0f, 1.0f, 1.0f, 1.0f);
	[[nuke::prop(label="Pivot X", min=0, max=1)]] float pivotX = 0.5f;   // 0 = left edge, 1 = right edge
	[[nuke::prop(label="Pivot Y", min=0, max=1)]] float pivotY = 0.5f;   // 0 = bottom edge, 1 = top edge
	[[nuke::prop(label="Flip X")]] bool flipX = false;
	[[nuke::prop(label="Flip Y")]] bool flipY = false;
	// (nine-slice lives ON THE TEXTURE — Texture::nineSlice + slice borders, set in the slicer.)

	// Runtime UV region within the texture (full frame by default), driven by SpriteAnimator.
	// Not serialized.
	float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
	Texture* tex = nullptr;   // resolved from textureGuid by World::Render (via ResDB)

	Sprite();

	// Reflected API (C#/Lua).
	[[nuke::func]] void SetTint(double r, double g, double b, double a);
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
