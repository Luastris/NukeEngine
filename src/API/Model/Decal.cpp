#include "API/Model/Decal.h"
#include "API/Model/Time.h"

namespace nuke {

Decal::Decal() : Component("Decal") {}

void Decal::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	spawnTime = Time::getSingleton()->elapsed;
}

void Decal::Update()      {}
void Decal::FixedUpdate() {}
void Decal::Reset()       { spawnTime = Time::getSingleton()->elapsed; }   // PIE start replays the appear
void Decal::Pause()       {}
void Decal::Destroy()     {}

}  // namespace nuke
