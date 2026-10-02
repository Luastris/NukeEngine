#include "API/Model/Widget.h"
#include "API/Model/Atom.h"

namespace nuke {

Widget::Widget(const char* typeName) : Component(typeName) {}

void Widget::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}
void Widget::Update()      {}
void Widget::FixedUpdate() {}
void Widget::Reset()       {}
void Widget::Pause()       {}
void Widget::Destroy()     {}

void Widget::SetSize(double w, double h) { width = (float)w; height = (float)h; }

}  // namespace nuke
