#include "interface/AtomCreators.h"
#include <iostream>

namespace nuke {

static std::vector<AtomCreator>& Pool() { static std::vector<AtomCreator> p; return p; }

void RegisterAtomCreator(const AtomCreator& desc)
{
	if (desc.label.empty() || desc.components.empty()) return;
	for (const AtomCreator& c : Pool())
		if (c.label == desc.label && c.category == desc.category) return;   // re-register (hot reload) = no dup
	Pool().push_back(desc);
	std::cout << "[AtomCreators]\tregistered '" << (desc.category.empty() ? "" : desc.category + "/")
	          << desc.label << "'" << std::endl;
}

const std::vector<AtomCreator>& AtomCreators() { return Pool(); }

}  // namespace nuke
