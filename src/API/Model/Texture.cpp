#include "API/Model/Texture.h"
#include <boost/filesystem/fstream.hpp>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <vector>
#include <initializer_list>

namespace nuke {

namespace bfs = boost::filesystem;

Texture::Texture(char* path) {
	strcpy(this->path, path);
}

Texture::Texture() {}

// Guess a texture's semantic usage from its filename suffix/tokens (bare-image import has no other signal).
// Tokenizes the stem on _ - . space and matches known role tokens; priority Normal > Emissive > Data > Color.
int Texture::GuessUsage(const std::string& filename)
{
	// stem: strip directory then extension, lowercase.
	size_t slash = filename.find_last_of("/\\");
	std::string s = (slash == std::string::npos) ? filename : filename.substr(slash + 1);
	size_t dot = s.find_last_of('.');
	if (dot != std::string::npos) s = s.substr(0, dot);
	for (char& c : s) c = (char)std::tolower((unsigned char)c);

	// tokenize
	std::vector<std::string> tok; std::string cur;
	for (char c : s) { if (c == '_' || c == '-' || c == '.' || c == ' ') { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } } else cur += c; }
	if (!cur.empty()) tok.push_back(cur);

	auto has = [&](std::initializer_list<const char*> set) {
		for (const std::string& t : tok) for (const char* k : set) if (t == k) return true;
		return false;
	};
	if (has({ "normal", "normalmap", "norm", "nor", "nrm", "nml", "n" }))                                   return UsageNormal;
	if (has({ "emissive", "emission", "emit", "emis", "ems", "glow" }))                                      return UsageEmissive;
	if (has({ "metallic", "metalness", "metal", "roughness", "rough", "mr", "orm", "rma", "arm", "ao",
	          "occlusion", "occ", "gloss", "glossiness", "mask", "height", "disp", "displacement", "bump" })) return UsageData;
	return UsageColor;
}

namespace {
	const char kMagic[8] = { 'N','U','T','E','X','\0','\0','\0' };
	const uint32_t kVersion = 5;   // v2: rt flag; v3: format+mipCount; v4: frameCount+delays (GIF); v5: usage
}

bool Texture::SaveToFile(const std::string& path) const
{
	bfs::path p(path);
	bfs::ofstream o(p, std::ios::binary);
	if (!o) return false;
	o.write(kMagic, 8);
	o.write((const char*)&kVersion, sizeof(kVersion));
	uint32_t glen = (uint32_t)guid.size(); o.write((const char*)&glen, 4); if (glen) o.write(guid.data(), glen);
	int32_t w = width, h = height; o.write((const char*)&w, 4); o.write((const char*)&h, 4);
	uint8_t rt = renderTexture ? 1 : 0; o.write((const char*)&rt, 1);   // v2
	int32_t fmt = format, mips = mipCount; o.write((const char*)&fmt, 4); o.write((const char*)&mips, 4);   // v3
	int32_t fc = frameCount < 1 ? 1 : frameCount; o.write((const char*)&fc, 4);                            // v4
	for (int k = 0; k < fc; ++k) { int32_t d = (k < (int)frameDelaysMs.size()) ? frameDelaysMs[k] : 100; o.write((const char*)&d, 4); }
	int32_t u = usage; o.write((const char*)&u, 4);                                                         // v5
	uint32_t bytes = (uint32_t)pixels.size(); o.write((const char*)&bytes, 4);
	if (bytes) o.write((const char*)pixels.data(), bytes);
	return (bool)o;
}

Texture* Texture::LoadFromFile(const std::string& path)
{
	bfs::path p(path);
	bfs::ifstream i(p, std::ios::binary);
	if (!i) return nullptr;
	char magic[8]; i.read(magic, 8);
	if (memcmp(magic, kMagic, 8) != 0) return nullptr;
	uint32_t version = 0; i.read((char*)&version, sizeof(version)); (void)version;
	Texture* t = new Texture();
	uint32_t glen = 0; i.read((char*)&glen, 4);
	if (glen) { std::string g(glen, '\0'); i.read(&g[0], glen); t->guid = g; }
	int32_t w = 0, h = 0; i.read((char*)&w, 4); i.read((char*)&h, 4);
	t->width = w; t->height = h;
	if (version >= 2) { uint8_t rt = 0; i.read((char*)&rt, 1); t->renderTexture = (rt != 0); }
	if (version >= 3) { int32_t fmt = 0, mips = 1; i.read((char*)&fmt, 4); i.read((char*)&mips, 4); t->format = fmt; t->mipCount = mips < 1 ? 1 : mips; }
	if (version >= 4)
	{
		int32_t fc = 1; i.read((char*)&fc, 4); t->frameCount = fc < 1 ? 1 : fc;
		t->frameDelaysMs.resize(t->frameCount);
		for (int k = 0; k < t->frameCount; ++k) { int32_t d = 100; i.read((char*)&d, 4); t->frameDelaysMs[k] = d; }
	}
	if (version >= 5) { int32_t u = 0; i.read((char*)&u, 4); t->usage = u; }   // v5: semantic usage
	uint32_t bytes = 0; i.read((char*)&bytes, 4);
	if (bytes) { t->pixels.resize(bytes); i.read((char*)t->pixels.data(), bytes); }
	if (!i && !i.eof()) { delete t; return nullptr; }
	return t;
}
}  // namespace nuke
