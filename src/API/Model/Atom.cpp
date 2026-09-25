#include "API/Model/Atom.h"
#include "API/Model/Time.h"
#include "API/Model/TimeVolume.h"   // local time: per-domain multipliers around each component tick
#include "API/Model/World.h"   // BumpHierarchy: views cache rows on the hierarchy version
#include "interface/AppInstance.h"
#include "reflect/ReflectBind.h"
#include <iostream>
#include <set>

namespace nuke {

using namespace std;

Atom::Atom() : transform(this)
{}

Atom::Atom(const char* name) : name(name), transform(this)
{
	cout << "[Atom]\t\t" << "New Atom(\"" << name << "\")" << endl;
}

// Hidden atoms by pointer; keys are compared, never dereferenced, and an atom drops its
// own key when it dies — a recycled address can never inherit the flag.
static std::set<const Atom*>& HiddenAtoms()
{
	static std::set<const Atom*> v;
	return v;
}

void Atom::SetRuntimeHidden(Atom* a, bool on)
{
	if (!a) return;
	if (on) HiddenAtoms().insert(a); else HiddenAtoms().erase(a);
	World::BumpHierarchy();
}

bool Atom::RuntimeHidden(const Atom* a)
{
	if (HiddenAtoms().empty()) return false;
	for (const Atom* p = a; p; p = p->parent)
		if (HiddenAtoms().count(p)) return true;
	return false;
}

Atom::~Atom()
{
	HiddenAtoms().erase(this);
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
	World::BumpHierarchy();
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
void Atom::SetEnabled(bool on) { enabled = on; World::BumpHierarchy(); }
bool Atom::IsEnabled()         { return enabled; }
bool Atom::IsFolder()          { return folder; }
void Atom::SetAlwaysLoaded(bool on) { alwaysLoaded = on; }
bool Atom::IsAlwaysLoaded()         { return alwaysLoaded; }

Transform& Atom::GetTransform()
{
	return transform;
}

void Atom::AddComponent(Component* cmp) {
	cmp->Init(this);
	World::BumpHierarchy();
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
	const float* ts = TimeVolume::Any() ? LocalTimeScales() : nullptr;
	for (auto cmp : components)
	{
		if (!cmp || !cmp->enabled) continue;
		if (ts) { Time::LocalScope sc(ts[(int)cmp->timeDomain()]); cmp->FixedUpdate(); }
		else cmp->FixedUpdate();
	}
}

// Local time (TimeVolume): the multipliers this frame, computed once per atom, then cached.
const float* Atom::LocalTimeScales()
{
	const unsigned long long frame = Time::getSingleton()->frame;
	if (localTimeFrame != frame) { TimeVolume::ScalesFor(this, localTimeScale); localTimeFrame = frame; }
	return localTimeScale;
}
double Atom::GetTimeScale() { return TimeVolume::Any() ? (double)LocalTimeScales()[(int)TimeDomain::Logic] : 1.0; }

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
	const float* ts = TimeVolume::Any() ? LocalTimeScales() : nullptr;   // local time: per-domain multipliers
	for (auto cmp : components)
	{
		if (!cmp || !cmp->enabled) continue;
		if (cmp->tickEvery > 1 && (frame + (unsigned long long)cmp->id.id) % (unsigned long long)cmp->tickEvery != 0)
			continue;
		if (ts) { Time::LocalScope sc(ts[(int)cmp->timeDomain()]); cmp->Update(); }
		else cmp->Update();
	}
}

void Atom::LateUpdate()
{
	if (!enabled) return;   // whole subtree off
	for (auto child : children)
	{
		if (child)
			child->LateUpdate();
	}
	const float* ts = TimeVolume::Any() ? LocalTimeScales() : nullptr;
	for (auto cmp : components)
	{
		if (!cmp || !cmp->enabled) continue;
		if (ts) { Time::LocalScope sc(ts[(int)cmp->timeDomain()]); cmp->LateUpdate(); }
		else cmp->LateUpdate();
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
	World::BumpHierarchy();
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