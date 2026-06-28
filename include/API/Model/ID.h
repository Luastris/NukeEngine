#pragma once
#ifndef NUKEE_ID_H
#define NUKEE_ID_H
#include "NukeAPI.h"

namespace nuke {

class NUKEENGINE_API ID {
public:
    unsigned long id;

	ID();
	ID(long id);

	void generate();   // assign a fresh process-unique id (monotonic counter)

	// Keep the global counter ahead of an id restored from disk, so newly generated ids never
	// collide with loaded ones. Call after deserializing an id.
	static void observe(unsigned long seen);
};
}  // namespace nuke

#endif // !NUKEE_ID_H
