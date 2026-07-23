#include "interface/AssetCreators.h"
#include <algorithm>
#include <cctype>

namespace nuke {

static std::vector<AssetCreator>& reg() { static std::vector<AssetCreator> v; return v; }

static std::string LowerExt(const std::string& e)
{
	std::string s = e;
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
	return s;
}

void RegisterAssetCreator(const AssetCreator& desc)
{
	for (const AssetCreator& c : reg())            // dedup (re-enabling a plugin re-registers)
		if (c.label == desc.label && c.ext == desc.ext) return;
	reg().push_back(desc);
}

void RegisterAssetCreator(const std::string& label, const std::string& ext,
                          const std::string& baseName, const std::string& content)
{
	AssetCreator d;
	d.label = label; d.ext = ext; d.baseName = baseName; d.content = content;
	RegisterAssetCreator(d);
}

const std::vector<AssetCreator>& AssetCreators() { return reg(); }

const AssetCreator* AssetCreatorForExt(const std::string& ext)
{
	const std::string want = LowerExt(ext);
	for (const AssetCreator& c : reg())
		if (LowerExt(c.ext) == want) return &c;
	return nullptr;
}

// ---- module-supplied asset editors (the type's owner brings the tooling) ---------------

static std::vector<std::pair<std::string, std::function<void(const std::string&)>>>& edReg()
{
	static std::vector<std::pair<std::string, std::function<void(const std::string&)>>> v;
	return v;
}

void RegisterAssetEditor(const std::string& ext, std::function<void(const std::string&)> open)
{
	if (ext.empty() || !open) return;
	const std::string key = LowerExt(ext);
	for (auto& e : edReg())
		if (e.first == key) { e.second = std::move(open); return; }   // re-enable: refresh the hook
	edReg().push_back({ key, std::move(open) });
}

const std::function<void(const std::string&)>* AssetEditorForExt(const std::string& ext)
{
	const std::string want = LowerExt(ext);
	for (auto& e : edReg())
		if (e.first == want) return &e.second;
	return nullptr;
}

}  // namespace nuke
