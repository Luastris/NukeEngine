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

// The layout itself is applied by the WORLD before rendering (ApplyCanvasLayouts in World.cpp) —
// it needs the owning canvas's rectangle, which the component alone doesn't know.
void RectAnchor::Update()      {}
void RectAnchor::FixedUpdate() {}
void RectAnchor::Reset()       {}
void RectAnchor::Pause()       {}
void RectAnchor::Destroy()     {}

}  // namespace nuke
