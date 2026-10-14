#include "API/Model/RectAnchor.h"
#include "API/Model/Atom.h"
#include "API/Model/Canvas.h"
#include "API/Model/Widget.h"

namespace nuke {

RectAnchor::RectAnchor() : Component("RectAnchor") {}

void RectAnchor::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

bool RectAnchor::Measure(Frame& f) const
{
	if (!atom) return false;
	Canvas* cv = nullptr;
	for (Atom* p = atom->parent; p && !cv; p = p->parent) cv = p->GetComponent<Canvas>();
	if (!cv || !cv->transform) return false;
	f.canvas = cv;
	f.world  = (cv->mode == CanvasMode::WorldSpace);
	f.ppu    = f.world ? 1.0f : (cv->pixelsPerUnit > 0.01f ? cv->pixelsPerUnit : 100.0f);
	f.hw = cv->width * 0.5f; f.hh = cv->height * 0.5f;
	Transform& t = atom->GetTransform();
	Vector3 cp = cv->transform->globalPosition(), gp = t.globalPosition(), gs = t.globalScale();
	if (f.world)
	{
		Vector3 R = cv->transform->right(), U = cv->transform->up();
		Vector3 d(gp.x - cp.x, gp.y - cp.y, gp.z - cp.z);
		f.cx = (float)(d.x*R.x + d.y*R.y + d.z*R.z);
		f.cy = (float)(d.x*U.x + d.y*U.y + d.z*U.z);
	}
	else { f.cx = (float)(gp.x - cp.x) * f.ppu; f.cy = (float)(gp.y - cp.y) * f.ppu; }
	Widget* wg = atom->GetComponent<Widget>();   // any widget (sprites, Prism controls) has the rect
	f.w = wg ? wg->width  * (float)gs.x * f.ppu : 0.0f;
	f.h = wg ? wg->height * (float)gs.y * f.ppu : 0.0f;
	return true;
}

void RectAnchor::CaptureFromPose()
{
	Frame f;
	if (!Measure(f)) return;
	if (left)   distLeft   = (f.cx - f.w * 0.5f) + f.hw;
	if (right)  distRight  = f.hw - (f.cx + f.w * 0.5f);
	if (bottom) distBottom = (f.cy - f.h * 0.5f) + f.hh;
	if (top)    distTop    = f.hh - (f.cy + f.h * 0.5f);
	lastCx = f.cx; lastCy = f.cy; lastW = f.w; lastH = f.h; laidOut = true;   // this pose IS the layout
}

// Layout is applied by the world before rendering (ApplyCanvasLayouts) — it needs the canvas rect.
void RectAnchor::Update()      {}
void RectAnchor::FixedUpdate() {}
void RectAnchor::Reset()       {}
void RectAnchor::Pause()       {}
void RectAnchor::Destroy()     {}

}  // namespace nuke
