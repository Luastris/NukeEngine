#include "interface/AssetCreators.h"

namespace nuke {

static std::vector<AssetCreator>& reg() { static std::vector<AssetCreator> v; return v; }

void RegisterAssetCreator(const std::string& label, const std::string& ext,
                          const std::string& baseName, const std::string& content)
{
	for (const AssetCreator& c : reg())            // dedup (re-enabling a plugin re-registers)
		if (c.label == label && c.ext == ext) return;
	reg().push_back({ label, ext, baseName, content });
}

const std::vector<AssetCreator>& AssetCreators() { return reg(); }

}  // namespace nuke
