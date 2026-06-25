#include "API/Model/Atom.h"
#include <iostream>

namespace nuke {

using namespace std;

Atom::Atom() : transform(this)
{}

Atom::Atom(const char* name) : name(name), transform(this)
{
	cout << "[Atom]\t\t" << "New Atom(\"" << name << "\")" << endl;
}

Atom::~Atom() {}

std::string Atom::GetName()
{
	return this->name;
}

std::string Atom::GetTag()
{
	return this->tag;
}

void Atom::SetName(const char* name)
{
	this->name = name;
}

void Atom::SetTag(const char* tag)
{
	this->tag = tag;
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
void Atom::FixedUpdate() {}
void Atom::Update()
{
	for (auto child : children)
	{
		if (child)
			child->Update();
	}
	for (auto cmp : components)
	{
		if (cmp && cmp->enabled)
			cmp->Update();
	}
}

void Atom::SetParent(Atom* newparent) {
	newparent->children.push_back(this);
	parent = newparent;
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
	for (auto x : children)
	{
		x->Destroy();
	}
	parent->children.remove(this);
	free(this);
}
}  // namespace nuke