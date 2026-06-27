#pragma once
#ifndef NUKEE_GAMEOBJECT_H
#define NUKEE_GAMEOBJECT_H
#include "NukeAPI.h"
#include <boost/container/list.hpp>
namespace bc = boost::container;

#include "Transform.h"
#include "Layers.h"
#include "ID.h"

namespace nuke {
//
//template class bc::list<Component*>;
//template class bc::list<Atom*>;

#pragma pack(push, 1)
class NUKEENGINE_API Atom
{
protected:
	

public:
	
	std::string name = "Atom";
	std::string tag = "Untagged";
	Atom* parent = nullptr;
	Transform transform = Transform(this);
	
	ID id;
	std::string prefabGuid;   // if set, this atom is an INSTANCE of that prefab (manual apply/reset only)

	int layer = Layer::L_DEFAULT;

    bc::list<Component*> components = bc::list<Component*>();
    bc::list<Atom*> children = bc::list<Atom*>();

	Atom();
	Atom(const char* name);
	~Atom();


	std::string GetName();
	std::string GetTag();
	void SetName(const char* name);
	void SetTag(const char* tag);
	Transform& GetTransform();
	
	template<class T>
	T* GetComponent(){
		for (Component* cmp : this->components)
		{
			if (auto res = dynamic_cast<T*>(cmp))
				return res;
		}
		return nullptr;
	}
	
	
	template<class T>
	bc::list<T*> GetComponents() {
		bc::list<T*> lst;
		for (auto cmp : components)
			if (auto res = dynamic_cast<T*>(cmp))
				lst.push_back(res);
		return lst;
	}

	void AddComponent(Component* cmp);

	void Init(Atom* parent);
	void FixedUpdate();
	void Update();

	void SetParent(Atom* newparent);
	Atom* GetParent();

	void AddChild(Atom* newChild);

    template <class T>
	void Update() {
		for (auto c : children) {
			if (auto mr = c->GetComponent<T>())
				mr->Update();
		}
	}

	void Reset();
	void Pause();
	void Destroy();

private:

};
#pragma pack(pop)
}  // namespace nuke

#endif // !NUKEE_GAMEOBJECT_H
