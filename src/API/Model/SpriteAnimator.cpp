#include "API/Model/SpriteAnimator.h"
#include "API/Model/Sprite.h"
#include "API/Model/Texture.h"   // tex->width/height + sprite grid (Usage=Sprite)
#include "API/Model/resdb.h"     // resolve the sprite's texture (grid lives on it)
#include "API/Model/Time.h"

namespace nuke {

SpriteAnimator::SpriteAnimator() : Component("SpriteAnimator") {}

void SpriteAnimator::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	playing = playOnStart;
	current = 0; timeAcc = 0.0f;
	Apply();
}

// The sprite's texture (only when flagged Usage=Sprite) owns the grid. Resolves the texture on demand so
// the grid is available even before the first render (editor preview).
Texture* SpriteAnimator::SheetTex() const
{
	Sprite* sp = atom ? atom->GetComponent<Sprite>() : nullptr;
	if (!sp) return nullptr;
	if (!sp->tex && !sp->textureGuid.empty()) sp->tex = ResDB::getSingleton()->GetTexture(sp->textureGuid);
	return (sp->tex && sp->tex->usage == Texture::UsageSprite) ? sp->tex : nullptr;
}

int SpriteAnimator::Count() const
{
	Texture* t = SheetTex();
	int total = t ? t->SpriteCount() : 1;
	int avail = total - (firstFrame > 0 ? firstFrame : 0);
	int c = (frameCount > 0) ? frameCount : avail;
	if (c > avail) c = avail;
	return c > 0 ? c : 1;
}

void SpriteAnimator::Apply()
{
	Sprite* sp = atom ? atom->GetComponent<Sprite>() : nullptr;
	if (!sp) return;
	Texture* t = SheetTex();
	int cell = firstFrame + (current % Count());
	int x0 = 0, y0 = 0, cw = 0, ch = 0;
	if (!t || !t->SpriteCellRect(cell, x0, y0, cw, ch)) { sp->SetFrame(0, 0, 1, 1); return; }
	// Inset the cell by HALF a texel on every side: linear filtering otherwise samples the neighbouring
	// cell at the edge (the classic sprite-sheet "bleed" — bits of the next frame poking in).
	float iu = 0.5f, iv = 0.5f;
	float u0 = (x0 + iu) / (float)t->width,  u1 = (x0 + cw - iu) / (float)t->width;
	float v0 = (y0 + iv) / (float)t->height, v1 = (y0 + ch - iv) / (float)t->height;
	sp->SetFrame(u0, v0, u1, v1);
}

void SpriteAnimator::Update()
{
	int n = Count();
	if (playing && fps > 0.0f && n > 1)
	{
		timeAcc += (float)Time::getSingleton()->delta;
		float dur = 1.0f / fps;
		while (timeAcc >= dur)
		{
			timeAcc -= dur;
			if (++current >= n)
			{
				if (loop) current = 0;
				else { current = n - 1; playing = false; break; }
			}
		}
	}
	Apply();
}

void SpriteAnimator::FixedUpdate() {}
void SpriteAnimator::Reset()       {}
void SpriteAnimator::Pause()       {}
void SpriteAnimator::Destroy()     {}

void SpriteAnimator::Play()       { playing = true; }
void SpriteAnimator::Stop()       { playing = false; current = 0; timeAcc = 0.0f; Apply(); }
bool SpriteAnimator::IsPlaying()  { return playing; }
int  SpriteAnimator::CurrentFrame() { return current; }
void SpriteAnimator::SetFrame(int index)
{
	int n = Count(); if (n < 1) n = 1;
	current = ((index % n) + n) % n;   // wrap negatives
	timeAcc = 0.0f;
	Apply();
}

}  // namespace nuke
