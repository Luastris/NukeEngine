#pragma once
#ifndef NUKEE_GAMEOBJECT_H
#define NUKEE_GAMEOBJECT_H
#include "NukeAPI.h"
#include <boost/container/list.hpp>
namespace bc = boost::container;

#include "Transform.h"
#include "Layers.h"
#include "ID.h"
#include "reflect/Reflect.h"

namespace nuke {
//
//template class bc::list<Component*>;
//template class bc::list<Atom*>;

#pragma pack(push, 1)
class NUKEENGINE_API Atom
{
	// Reflected for METHOD dispatch (name/tag/parenting/destroy work identically in every
	// scripting language). Atoms still travel as AtomRef stable ids, never object handles.
	NUKE_CLASS_NOCREATE(Atom, Object)
protected:


public:

	std::string name = "Atom";
	std::string tag = "Untagged";
	// Which MOD added this atom (world-merge provenance, RUNTIME only — never serialized):
	// "" = native to the base game. The editor badges non-native atoms with it.
	std::string modOrigin;
	Atom* parent = nullptr;
	Transform transform = Transform(this);
	
	ID id;
	std::string prefabGuid;   // if set, this atom is an INSTANCE of that prefab (manual apply/reset only)

	// Render layer INDEX (0..31, see nuke::Layers): cameras render an atom only when their
	// layerMask has this bit set. 0 = "Default"; 31 is conventionally the editor's own objects.
	int layer = 0;

	// PERSISTENT atom (the DontDestroyOnLoad pattern): a persistent ROOT atom survives a GAME
	// world switch (Game.LoadWorld / async activation) — its subtree, components and live
	// script state carry into the next world instead of being torn down. Only in PLAY (player
	// or PIE playing): editor edit-mode loads never carry atoms (opening world B must not
	// leak world A's content into its file), and a SAVEGAME load restores the full snapshot
	// (the save already contains the carried atoms — persisting would duplicate them).
	bool persistent = false;

    bc::list<Component*> components = bc::list<Component*>();
    bc::list<Atom*> children = bc::list<Atom*>();

	// Whole-atom switch (the SetActive pattern): a disabled atom — and its entire subtree —
	// neither updates, renders, receives events, nor keeps physics bodies alive. Distinct from
	// per-component `enabled`. APPENDED at the END of the data members (cross-DLL layout).
	bool enabled = true;

	Atom();
	Atom(const char* name);
	~Atom();


	[[nuke::func]] std::string GetName();
	[[nuke::func]] std::string GetTag();
	[[nuke::func]] void SetName(const std::string& name);
	[[nuke::func]] void SetTag(const std::string& tag);
	[[nuke::func]] void   SetLayer(double index);   // render layer index 0..31 (clamped; see nuke::Layers)
	[[nuke::func]] double GetLayer();
	[[nuke::func]] void SetPersistent(bool on);     // survive game world switches (root atoms; see `persistent`)
	[[nuke::func]] bool IsPersistent();
	[[nuke::func]] void SetEnabled(bool on);        // whole-atom switch incl. subtree (see `enabled`)
	[[nuke::func]] bool IsEnabled();                // this atom's own flag (parents may still disable it)
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

	[[nuke::func]] void SetParent(Atom* newparent);
	[[nuke::func]] Atom* GetParent();

	[[nuke::func]] void AddChild(Atom* newChild);

    template <class T>
	void Update() {
		for (auto c : children) {
			if (auto mr = c->GetComponent<T>())
				mr->Update();
		}
	}

	void Reset();
	void Pause();
	// DEFERRED destruction (delegates to World::QueueDestroy): the subtree is removed and
	// deleted at the end of the current Update — safe from scripts, callbacks, anywhere.
	[[nuke::func]] void Destroy();

private:

};
#pragma pack(pop)
}  // namespace nuke

#endif // !NUKEE_GAMEOBJECT_H
