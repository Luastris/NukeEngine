#include "API/Model/SpriteAnimator.h"
#include "API/Model/Sprite.h"
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

int SpriteAnimator::Count() const
{
	int cols = columns > 0 ? columns : 1, rws = rows > 0 ? rows : 1;
	int avail = cols * rws - (firstFrame > 0 ? firstFrame : 0);
	int c = (frameCount > 0) ? frameCount : avail;
	if (c > avail) c = avail;
	return c > 0 ? c : 1;
}

void SpriteAnimator::Apply()
{
	Sprite* sp = atom ? atom->GetComponent<Sprite>() : nullptr;
	if (!sp) return;
	const int cols = columns > 0 ? columns : 1;
	const int rws  = rows    > 0 ? rows    : 1;
	int cell = firstFrame + (current % Count());
	if (cell >= cols * rws) cell = cols * rws - 1;
	if (cell < 0) cell = 0;
	int cx = cell % cols, cy = cell / cols;
	float w = 1.0f / cols, h = 1.0f / rws;
	float u0 = cx * w, u1 = u0 + w;
	float v0 = cy * h, v1 = v0 + h;
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
