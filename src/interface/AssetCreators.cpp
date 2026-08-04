#include "interface/AssetCreators.h"
#include "interface/IconsFileTypes.h"
#include <map>
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
	if (!desc.icon.empty()) RegisterFileIcon(desc.ext, desc.icon);   // the descriptor carries it
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

// ---- file-type icons (the type's owner declares its glyph) -----------------------------

static std::map<std::string, std::string>& iconReg() { static std::map<std::string, std::string> m; return m; }

void RegisterFileIcon(const std::string& ext, const std::string& glyph)
{
	if (ext.empty() || glyph.empty()) return;
	iconReg()[LowerExt(ext)] = glyph;
}

const char* FileIconForExt(const std::string& ext)
{
	auto it = iconReg().find(LowerExt(ext));
	return it == iconReg().end() ? "" : it->second.c_str();
}

// The engine's OWN formats. Everything else — scripts, tile sets, effect graphs — is declared
// by the module that owns it, so an unloaded module simply leaves its files generic.
void RegisterBuiltinFileIcons()
{
	static const struct { const char* ext; const char* icon; } kBuiltin[] = {
		{ ".numesh",    ICON_FT_MESH },        { ".numat",     ICON_FT_MATERIAL },
		{ ".nutex",     ICON_FT_IMAGE },       { ".nuprefab",  ICON_FT_PREFAB },
		{ ".nuanim",    ICON_FT_ANIM },        { ".nuskel",    ICON_FT_SKELETON },
		{ ".nubonemap", ICON_FT_BONEMAP },     { ".nusm",      ICON_FT_STATEMACHINE },
		{ ".nublend",   ICON_FT_BLENDSPACE },  { ".nuseq",     ICON_FT_SEQUENCE },
		{ ".nurag",     ICON_FT_RAGDOLL },     { ".nuworld",   ICON_FT_WORLD },
		{ ".nushader",  ICON_FT_SHADER },      { ".hlsl",      ICON_FT_SHADER },
		{ ".nuinput",   ICON_FT_INPUT },       { ".nupak",     ICON_FT_PACKAGE },
		{ ".numod",     ICON_FT_MOD },         { ".nusave",    ICON_FT_SAVE },
		{ ".nuproj",    ICON_FT_PROJECT },
		// media the engine imports natively
		{ ".png", ICON_FT_IMAGE }, { ".jpg", ICON_FT_IMAGE }, { ".jpeg", ICON_FT_IMAGE },
		{ ".tga", ICON_FT_IMAGE }, { ".bmp", ICON_FT_IMAGE },  { ".ico",  ICON_FT_IMAGE },
		{ ".ogg", ICON_FT_AUDIO }, { ".wav", ICON_FT_AUDIO },  { ".mp3",  ICON_FT_AUDIO },
		{ ".flac", ICON_FT_AUDIO },
	};
	for (const auto& b : kBuiltin) RegisterFileIcon(b.ext, b.icon);
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
