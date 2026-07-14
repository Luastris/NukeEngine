#include "API/Model/Sprite.h"

namespace nuke {

Sprite::Sprite() : Component("Sprite") {}

void Sprite::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}
void Sprite::Update()      {}
void Sprite::FixedUpdate() {}
void Sprite::Reset()       {}
void Sprite::Pause()       {}
void Sprite::Destroy()     {}

void Sprite::SetTint(double r, double g, double b, double a)
{
	tint = Color((float)r, (float)g, (float)b, (float)a);
}
void Sprite::SetSize(double w, double h)   { width = (float)w; height = (float)h; }
void Sprite::SetPivot(double x, double y)  { pivotX = (float)x; pivotY = (float)y; }
void Sprite::SetFrame(double u0v, double v0v, double u1v, double v1v)
{
	u0 = (float)u0v; v0 = (float)v0v; u1 = (float)u1v; v1 = (float)v1v;
}

}  // namespace nuke
