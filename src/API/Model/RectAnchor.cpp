#include "API/Model/RectAnchor.h"
#include "API/Model/Atom.h"

namespace nuke {

RectAnchor::RectAnchor() : Component("RectAnchor") {}

void RectAnchor::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

// Layout is applied by the world before rendering (ApplyCanvasLayouts) — it needs the canvas rect.
void RectAnchor::Update()      {}
void RectAnchor::FixedUpdate() {}
void RectAnchor::Reset()       {}
void RectAnchor::Pause()       {}
void RectAnchor::Destroy()     {}

}  // namespace nuke
