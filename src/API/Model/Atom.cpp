#include "API/Model/Atom.h"
#include "API/Model/Time.h"
#include "interface/AppInstance.h"
#include "reflect/ReflectBind.h"
#include <iostream>

namespace nuke {

using namespace std;

Atom::Atom() : transform(this)
{}

Atom::Atom(const char* name) : name(name), transform(this)
{
	cout << "[Atom]\t\t" << "New Atom(\"" << name << "\")" << endl;
}

Atom::~Atom()
{
	Reflect_DropObject(&transform);   // script handles to this transform go stale-safe
}

std::string Atom::GetName()
{
	return this->name;
}

std::string Atom::GetTag()
{
	return this->tag;
}

void Atom::SetName(const std::string& name)
{
	this->name = name;
}

void Atom::SetTag(const std::string& tag)
{
	this->tag = tag;
}

void Atom::SetLayer(double index)
{
	int i = (int)index;
	layer = i < 0 ? 0 : (i > 31 ? 31 : i);
}

double Atom::GetLayer()
{
	return (double)layer;
}

void Atom::SetPersistent(bool on) { persistent = on; }
bool Atom::IsPersistent()         { return persistent; }

// No eager side effects: every consumer gates on the flag per frame.
void Atom::SetEnabled(bool on) { enabled = on; }
bool Atom::IsEnabled()         { return enabled; }

Transform& Atom::GetTransform()
{
	return transform;
}

void Atom::AddComponent(Component* cmp) {
	cmp->Init(this);
}

void Atom::Init(Atom* parent)
{
	this->parent = parent;
}
// Fixed-step tick: children first, then this atom's enabled components.
void Atom::FixedUpdate()
{
	if (!enabled) return;   // whole subtree off
	for (auto child : children)
	{
		if (child)
			child->FixedUpdate();
	}
	for (auto cmp : components)
	{
		if (cmp && cmp->enabled)
			cmp->FixedUpdate();
	}
}
void Atom::Update()
{
	if (!enabled) return;   // whole subtree off
	for (auto child : children)
	{
		if (child)
			child->Update();
	}
	// tickEvery N = run every Nth frame, staggered by id so they don't all spike on one frame.
	const unsigned long long frame = Time::getSingleton()->frame;
	for (auto cmp : components)
	{
		if (!cmp || !cmp->enabled) continue;
		if (cmp->tickEvery > 1 && (frame + (unsigned long long)cmp->id.id) % (unsigned long long)cmp->tickEvery != 0)
			continue;
		cmp->Update();
	}
}

void Atom::SetParent(Atom* newparent) {
	// nullptr = world root; Reparent also guards against cycles.
	AppInstance::GetSingleton()->currentWorld->Reparent(this, newparent);
}

Atom* Atom::GetParent()
{
	return this->parent;
}

void Atom::AddChild(Atom* newChild) {
	children.push_back(newChild);
	newChild->parent = this;
}

void Atom::Reset() {}
void Atom::Pause() {}
void Atom::Destroy()
{
	// Deferred, whole-subtree: the world deletes it at the end of Update, never mid-traversal.
	if (World* w = AppInstance::GetSingleton()->currentWorld)
		w->QueueDestroy((long)id.id);
}
}  // namespace nuke