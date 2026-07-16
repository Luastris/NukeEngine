#include "API/Model/Atom.h"
#include "API/Model/Time.h"          // tick-interval gating reads the global frame counter (6.8)
#include "interface/AppInstance.h"   // SetParent delegates to the active world's Reparent
#include "reflect/ReflectBind.h"     // Reflect_DropObject: transform handles die with the atom
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
	Reflect_DropObject(&transform);   // script handles wrapping this transform atom stale-safe dead
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
// Fixed-step tick (driven by World::FixedUpdate) — same shape as Update: children first,
// then this atom's enabled components.
void Atom::FixedUpdate()
{
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
	for (auto child : children)
	{
		if (child)
			child->Update();
	}
	// Tick intervals (6.8): a component with tickEvery N runs every Nth frame, STAGGERED by
	// its id — 500 pawns at N=5 tick ~100 per frame instead of all spiking on the same one.
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
	// Detach from the old location + attach to the new one (nullptr = scene root), via the active
	// world's Reparent — which also guards against cycles.
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
	// DEFERRED, whole-subtree: the world removes + deletes it at the end of Update — never
	// mid-traversal (the old immediate `free(this)` corrupted the running iteration and
	// crashed on root atoms). Safe to call from scripts, contacts, even on `self`.
	if (World* w = AppInstance::GetSingleton()->currentWorld)
		w->QueueDestroy((long)id.id);
}
}  // namespace nuke