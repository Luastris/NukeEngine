#include "API/Model/ID.h"

namespace nuke {

ID::ID()
{
	generate();
}

ID::ID(long id)
{
	this->id = id;
}

void ID::generate()
{
	id = reinterpret_cast<long>(this);
}
}  // namespace nuke