#include "interface/AtomCreators.h"
#include "interface/Modular.h"   // RegisteringModule: entries remember their module
#include <algorithm>
#include <iostream>

namespace nuke {

static std::vector<AtomCreator>& Pool() { static std::vector<AtomCreator> p; return p; }
static std::vector<std::string>& Owners() { static std::vector<std::string> p; return p; }   // parallel

void RegisterAtomCreator(const AtomCreator& desc)
{
	if (desc.label.empty() || desc.components.empty()) return;
	for (const AtomCreator& c : Pool())
		if (c.label == desc.label && c.category == desc.category) return;   // re-register (hot reload) = no dup
	Owners().push_back(RegisteringModule());
	Pool().push_back(desc);
	std::cout << "[AtomCreators]\tregistered '" << (desc.category.empty() ? "" : desc.category + "/")
	          << desc.label << "'" << std::endl;
}

const std::vector<AtomCreator>& AtomCreators() { return Pool(); }

void UnregisterAtomCreatorsOf(const std::string& moduleDll)
{
	if (moduleDll.empty()) return;
	for (size_t i = Pool().size(); i-- > 0;)
		if (Owners()[i] == moduleDll)
		{
			Pool().erase(Pool().begin() + i);
			Owners().erase(Owners().begin() + i);
		}
}

}  // namespace nuke
