#pragma once
#ifndef NUKEE_EDITOR_INSTANCE_H
#define NUKEE_EDITOR_INSTANCE_H
#include "NukeAPI.h"
#include "AppInstance.h"
#include "EditorMenu/MenuStrip.h"
#include <boost/container/list.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/container/map.hpp>

namespace nuke {

namespace bc = boost::container;
namespace bst = boost;
namespace btups = boost::tuples;

class NUKEENGINE_API EditorInstance
{
protected:
	EditorInstance();
	~EditorInstance();

public:
	bool isEditor();
	

	static EditorInstance* GetSingleton() 
	{
		static EditorInstance instance;
		/*if (!instance.currentWorld)
			instance.currentWorld = new World();
		if (!instance.currentWorld->hierarchy)
			instance.currentWorld->hierarchy = new bc::list<Atom*>();*/
		return &instance;
	}
};

}  // namespace nuke

#endif // !NUKEE_EDITOR_INSTANCE_H
