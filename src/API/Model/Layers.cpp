#include "API/Model/Layers.h"
#include <array>
#include <sstream>

namespace nuke {

// Slot names live process-wide; the PROJECT is the source of truth (editor Project Settings ->
// game.nuproj "layers"), pushed here by the hosts at load. Index 0 = "Default" unless renamed.
static std::array<std::string, Layers::kCount>& Slots()
{
	static std::array<std::string, Layers::kCount> s = [] {
		std::array<std::string, Layers::kCount> a{};
		a[0] = "Default";
		a[31] = "Editor";   // convention: the editor's own objects (editor camera etc.)
		return a;
	}();
	return s;
}

std::string Layers::Name(double index)
{
	int i = (int)index;
	return (i >= 0 && i < kCount) ? Slots()[i] : std::string();
}

void Layers::SetName(double index, const std::string& name)
{
	int i = (int)index;
	if (i >= 0 && i < kCount) Slots()[i] = name;
}

double Layers::IndexOf(const std::string& name)
{
	if (name.empty()) return -1;
	for (int i = 0; i < kCount; ++i)
		if (Slots()[i] == name) return i;
	return -1;
}

double Layers::MaskOf(const std::string& names)
{
	if (names == "*") return (double)0xFFFFFFFFu;
	unsigned int mask = 0;
	std::stringstream ss(names);
	std::string part;
	while (std::getline(ss, part, ','))
	{
		size_t b = part.find_first_not_of(" \t"), e = part.find_last_not_of(" \t");
		if (b == std::string::npos) continue;
		int idx = (int)IndexOf(part.substr(b, e - b + 1));
		if (idx >= 0) mask |= (1u << idx);
	}
	return (double)mask;
}

void Layers::SetAll(const std::vector<std::string>& names)
{
	for (int i = 0; i < kCount; ++i)
		Slots()[i] = (i < (int)names.size()) ? names[i] : std::string();
	if (Slots()[0].empty()) Slots()[0] = "Default";   // slot 0 always usable
}

std::vector<std::string> Layers::All()
{
	return std::vector<std::string>(Slots().begin(), Slots().end());
}

}  // namespace nuke
