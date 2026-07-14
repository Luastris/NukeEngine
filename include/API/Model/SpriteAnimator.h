#pragma once
#ifndef NUKEE_SPRITEANIMATOR_H
#define NUKEE_SPRITEANIMATOR_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"

namespace nuke {

// Plays a sprite-sheet animation by driving the sibling Sprite's UV frame each update. The sprite's
// texture is treated as a grid of columns×rows cells; the animation cycles cells
// [firstFrame .. firstFrame+frameCount) at `fps`. Purely engine-side — it just calls Sprite::SetFrame,
// so it needs no renderer support. Sibling of a Sprite on the same atom.
class NUKEENGINE_API SpriteAnimator : public Component
{
	NUKE_CLASS(SpriteAnimator, Component)
public:
	// The atlas grid (columns×rows) lives on the sprite's TEXTURE (Usage=Sprite), not here — configure it
	// once on the texture and every animator using it inherits the slicing.
	[[nuke::prop(label="First Frame", min=0)]]     int   firstFrame  = 0;   // starting cell (row-major, 0 = top-left)
	[[nuke::prop(label="Frame Count", min=0)]]     int   frameCount  = 0;   // number of cells to play (0 = to the end)
	[[nuke::prop(label="FPS", min=0, max=120)]]    float fps         = 12.0f;
	[[nuke::prop(label="Loop")]]                   bool  loop        = true;
	[[nuke::prop(label="Play On Start")]]          bool  playOnStart = true;

	// Runtime (not serialized).
	float timeAcc = 0.0f;
	int   current = 0;       // 0-based index within [0, Count())
	bool  playing = false;

	SpriteAnimator();

	[[nuke::func]] void Play();
	[[nuke::func]] void Stop();               // stop and rewind to the first frame
	[[nuke::func]] bool IsPlaying();
	[[nuke::func]] void SetFrame(int index);  // jump to a frame (0-based within the animation range)
	[[nuke::func]] int  CurrentFrame();

	void Init(Atom* parent) override;
	void Update() override;
	void FixedUpdate() override;
	void Reset() override;
	void Pause() override;
	void Destroy() override;

private:
	class Texture* SheetTex() const;   // sibling Sprite's texture IFF flagged Usage=Sprite (owns the grid), else null
	int  Count() const;   // number of frames actually playable given the grid
	void Apply();         // push the current cell's UV region to the sibling Sprite
};
}  // namespace nuke

#endif // !NUKEE_SPRITEANIMATOR_H
