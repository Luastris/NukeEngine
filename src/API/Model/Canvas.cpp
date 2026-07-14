#include "API/Model/Canvas.h"

namespace nuke {

Canvas::Canvas() : Component("Canvas") {}

void Canvas::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void Canvas::Update()      {}
void Canvas::FixedUpdate() {}
void Canvas::Reset()       {}
void Canvas::Pause()       {}
void Canvas::Destroy()     {}

}  // namespace nuke
