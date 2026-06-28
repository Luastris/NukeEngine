#include "API/Model/ID.h"

namespace nuke {

// Process-wide monotonic id source. (The old generate() used reinterpret_cast<long>(this) — the object's
// address — which truncates a 64-bit pointer to 32-bit `long` AND reuses freed addresses, so duplicated /
// prefab-round-tripped atoms collided. A counter is unique within the session; observe() keeps it ahead of
// ids loaded from disk so new atoms never reuse a saved id.)
static unsigned long g_idCounter = 1;   // 0 stays reserved for "none" / root

ID::ID()
{
	generate();
}

ID::ID(long id)
{
	this->id = (unsigned long)id;
	observe(this->id);
}

void ID::generate()
{
	id = g_idCounter++;
}

void ID::observe(unsigned long seen)
{
	if (seen >= g_idCounter) g_idCounter = seen + 1;
}

}  // namespace nuke
