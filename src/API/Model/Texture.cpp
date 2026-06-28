#include "API/Model/Texture.h"
#include <boost/filesystem/fstream.hpp>
#include <cstdint>
#include <cstring>

namespace nuke {

namespace bfs = boost::filesystem;

Texture::Texture(char* path) {
	strcpy(this->path, path);
}

Texture::Texture() {}

namespace {
	const char kMagic[8] = { 'N','U','T','E','X','\0','\0','\0' };
	const uint32_t kVersion = 4;   // v2: rt flag; v3: format+mipCount; v4: frameCount+delays (GIF animation)
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
	uint32_t bytes = 0; i.read((char*)&bytes, 4);
	if (bytes) { t->pixels.resize(bytes); i.read((char*)t->pixels.data(), bytes); }
	if (!i && !i.eof()) { delete t; return nullptr; }
	return t;
}
}  // namespace nuke
