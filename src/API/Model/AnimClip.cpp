#include "API/Model/AnimClip.h"
#include <sstream>
#include <cstdint>
#include <cstring>
#include <boost/filesystem/fstream.hpp>

namespace nuke {

namespace bfs = boost::filesystem;

// ---- native .nuanim (binary) ---------------------------------------------------------
// Layout: magic "NUANIM\0\0" | u32 version | str guid | str name | f64 duration |
//         u32 channelCount, per channel: str bone | 3 x (u32 keyCount + Key[]).
// v2 appends events: u32 count + per event (f32 t | str name).
namespace {
	const char     kMagic[8] = { 'N','U','A','N','I','M','\0','\0' };
	const uint32_t kVersion  = 2;
	template <class T> void wr(bfs::ofstream& o, const T& v) { o.write((const char*)&v, sizeof(T)); }
	template <class T> void rd(std::istream& i, T& v)       { i.read((char*)&v, sizeof(T)); }
	void wrStr(bfs::ofstream& o, const std::string& s) { uint32_t n = (uint32_t)s.size(); wr(o, n); if (n) o.write(s.data(), n); }
	std::string rdStr(std::istream& i) { uint32_t n = 0; rd(i, n); std::string s(n, '\0'); if (n) i.read(&s[0], n); return s; }
	void wrKeys(bfs::ofstream& o, const std::vector<AnimClip::Key>& k)
	{
		uint32_t n = (uint32_t)k.size(); wr(o, n);
		if (n) o.write((const char*)k.data(), sizeof(AnimClip::Key) * n);
	}
	void rdKeys(std::istream& i, std::vector<AnimClip::Key>& k)
	{
		uint32_t n = 0; rd(i, n);
		k.resize(n);
		if (n) i.read((char*)k.data(), sizeof(AnimClip::Key) * n);
	}
}

bool AnimClip::SaveToFile(const std::string& path) const
{
	bfs::ofstream o(bfs::path(path), std::ios::binary);
	if (!o) return false;
	o.write(kMagic, 8);
	wr(o, kVersion);
	wrStr(o, guid);
	wrStr(o, name);
	wr(o, duration);
	uint32_t cc = (uint32_t)channels.size(); wr(o, cc);
	for (const Channel& c : channels)
	{
		wrStr(o, c.bone);
		wrKeys(o, c.pos);
		wrKeys(o, c.rot);
		wrKeys(o, c.scl);
	}
	uint32_t ec = (uint32_t)events.size(); wr(o, ec);
	for (const Event& e : events) { wr(o, e.t); wrStr(o, e.name); }
	return (bool)o;
}

void AnimClip::AddEvent(float t, const std::string& name)
{
	auto it = events.begin();
	while (it != events.end() && it->t <= t) ++it;
	events.insert(it, { t, name });
}

AnimClip* AnimClip::LoadFromFile(const std::string& path)
{
	bfs::ifstream i(bfs::path(path), std::ios::binary);
	if (!i) return nullptr;
	return LoadFromStream(i);
}

AnimClip* AnimClip::LoadFromMemory(const std::string& data)
{
	std::istringstream i(data, std::ios::binary);
	return LoadFromStream(i);
}

AnimClip* AnimClip::LoadFromStream(std::istream& i)
{
	char magic[8]; i.read(magic, 8);
	if (memcmp(magic, kMagic, 8) != 0) return nullptr;
	uint32_t version = 0; rd(i, version);
	(void)version;
	AnimClip* c = new AnimClip();
	c->guid = rdStr(i);
	c->name = rdStr(i);
	rd(i, c->duration);
	uint32_t cc = 0; rd(i, cc);
	c->channels.resize(cc);
	for (uint32_t k = 0; k < cc; ++k)
	{
		Channel& ch = c->channels[k];
		ch.bone = rdStr(i);
		rdKeys(i, ch.pos);
		rdKeys(i, ch.rot);
		rdKeys(i, ch.scl);
	}
	if (version >= 2)
	{
		uint32_t ec = 0; rd(i, ec);
		c->events.resize(ec);
		for (uint32_t k = 0; k < ec; ++k) { rd(i, c->events[k].t); c->events[k].name = rdStr(i); }
	}
	if (!i && !i.eof()) { delete c; return nullptr; }
	return c;
}

}  // namespace nuke
