#include "interface/ComponentIcons.h"
#include <iostream>

namespace nuke {

static std::vector<ComponentIcon>& Pool() { static std::vector<ComponentIcon> p; return p; }

void RegisterComponentIcon(const ComponentIcon& desc)
{
	if (desc.component.empty() || desc.glyph.empty()) return;
	for (ComponentIcon& c : Pool())
		if (c.component == desc.component) { c = desc; return; }   // re-register (hot reload) = replace
	Pool().push_back(desc);
	std::cout << "[ComponentIcons]\tregistered '" << desc.component << "'" << std::endl;
}

const std::vector<ComponentIcon>& ComponentIcons() { return Pool(); }

}  // namespace nuke
