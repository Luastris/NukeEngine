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
	const uint32_t kVersion = 1;
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
	uint32_t bytes = 0; i.read((char*)&bytes, 4);
	if (bytes) { t->pixels.resize(bytes); i.read((char*)t->pixels.data(), bytes); }
	if (!i && !i.eof()) { delete t; return nullptr; }
	return t;
}
}  // namespace nuke
