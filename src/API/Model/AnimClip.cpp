#include "API/Model/AnimClip.h"
#include <cmath>
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
// v3 appends notifies (u32 count + per notify f32 t | i32 type | 3 str | 3 f32), curves
// (u32 count + per curve str name + keys) and prop tracks (u32 count + per track 3 str |
// i32 dim | keys). v4 appends the source-skeleton guid (str).
namespace {
	const char     kMagic[8] = { 'N','U','A','N','I','M','\0','\0' };
	const uint32_t kVersion  = 4;
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

void AnimClip::Sample(const std::vector<Key>& keys, double t, float out[4])
{
	out[0] = out[1] = out[2] = out[3] = 0;
	if (keys.empty()) return;
	if (keys.size() == 1 || t <= keys.front().t) { memcpy(out, keys.front().v, sizeof(float) * 4); return; }
	if (t >= keys.back().t) { memcpy(out, keys.back().v, sizeof(float) * 4); return; }
	size_t hi = 1;
	while (hi < keys.size() && keys[hi].t < (float)t) ++hi;
	const Key& a = keys[hi - 1];
	const Key& b = keys[hi];
	const float f = (b.t > a.t) ? ((float)t - a.t) / (b.t - a.t) : 0.0f;
	for (int k = 0; k < 4; ++k) out[k] = a.v[k] + (b.v[k] - a.v[k]) * f;
}

void AnimClip::SampleQuat(const std::vector<Key>& keys, double t, float out[4])
{
	// nlerp on the shorter arc — keys are dense enough that it matches slerp visually
	Sample(keys, t, out);
	if (keys.size() < 2) return;
	size_t hi = 1;
	while (hi < keys.size() && keys[hi].t < (float)t) ++hi;
	if (hi >= keys.size()) return;
	const Key& a = keys[hi - 1];
	const Key& b = keys[hi];
	float dot = 0;
	for (int k = 0; k < 4; ++k) dot += a.v[k] * b.v[k];
	const float f = (b.t > a.t) ? ((float)t - a.t) / (b.t - a.t) : 0.0f;
	const float sb = dot < 0 ? -1.0f : 1.0f;
	float len2 = 0;
	for (int k = 0; k < 4; ++k)
	{
		out[k] = a.v[k] + (sb * b.v[k] - a.v[k]) * f;
		len2 += out[k] * out[k];
	}
	if (len2 > 1e-12f)
	{
		const float inv = 1.0f / sqrtf(len2);
		for (int k = 0; k < 4; ++k) out[k] *= inv;
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
	uint32_t nc = (uint32_t)notifies.size(); wr(o, nc);
	for (const Notify& n : notifies)
	{
		wr(o, n.t); wr(o, n.type);
		wrStr(o, n.name); wrStr(o, n.asset); wrStr(o, n.socket);
		wr(o, n.a); wr(o, n.b); wr(o, n.c);
	}
	uint32_t cvc = (uint32_t)curves.size(); wr(o, cvc);
	for (const Curve& c : curves) { wrStr(o, c.name); wrKeys(o, c.keys); }
	uint32_t pc = (uint32_t)propTracks.size(); wr(o, pc);
	for (const PropTrack& p : propTracks)
	{
		wrStr(o, p.path); wrStr(o, p.comp); wrStr(o, p.prop);
		wr(o, p.dim);
		wrKeys(o, p.keys);
	}
	wrStr(o, skelGuid);
	return (bool)o;
}

void AnimClip::AddEvent(float t, const std::string& name)
{
	auto it = events.begin();
	while (it != events.end() && it->t <= t) ++it;
	events.insert(it, { t, name });
}

void AnimClip::AddNotify(double t, double type, const std::string& name,
                         const std::string& asset, const std::string& socket,
                         double a, double b, double c)
{
	Notify n;
	n.t = (float)t; n.type = (int)type;
	n.name = name; n.asset = asset; n.socket = socket;
	n.a = (float)a; n.b = (float)b; n.c = (float)c;
	auto it = notifies.begin();
	while (it != notifies.end() && it->t <= n.t) ++it;
	notifies.insert(it, n);
}

void AnimClip::AddCurveKey(const std::string& curve, double t, double v)
{
	Curve* c = nullptr;
	for (Curve& x : curves) if (x.name == curve) { c = &x; break; }
	if (!c) { curves.push_back({ curve, {} }); c = &curves.back(); }
	Key k; k.t = (float)t; k.v[0] = (float)v; k.v[1] = k.v[2] = k.v[3] = 0;
	auto it = c->keys.begin();
	while (it != c->keys.end() && it->t <= k.t) ++it;
	c->keys.insert(it, k);
}

void AnimClip::AddPropKey(const std::string& path, const std::string& comp,
                          const std::string& prop, double t, double v)
{
	PropTrack* tr = nullptr;
	for (PropTrack& x : propTracks)
		if (x.path == path && x.comp == comp && x.prop == prop) { tr = &x; break; }
	if (!tr) { propTracks.push_back({ path, comp, prop, 1, {} }); tr = &propTracks.back(); }
	Key k; k.t = (float)t; k.v[0] = (float)v; k.v[1] = k.v[2] = k.v[3] = 0;
	auto it = tr->keys.begin();
	while (it != tr->keys.end() && it->t <= k.t) ++it;
	tr->keys.insert(it, k);
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
	if (version >= 3)
	{
		uint32_t nc = 0; rd(i, nc);
		c->notifies.resize(nc);
		for (uint32_t k = 0; k < nc; ++k)
		{
			Notify& n = c->notifies[k];
			rd(i, n.t); rd(i, n.type);
			n.name = rdStr(i); n.asset = rdStr(i); n.socket = rdStr(i);
			rd(i, n.a); rd(i, n.b); rd(i, n.c);
		}
		uint32_t cvc = 0; rd(i, cvc);
		c->curves.resize(cvc);
		for (uint32_t k = 0; k < cvc; ++k) { c->curves[k].name = rdStr(i); rdKeys(i, c->curves[k].keys); }
		uint32_t pc = 0; rd(i, pc);
		c->propTracks.resize(pc);
		for (uint32_t k = 0; k < pc; ++k)
		{
			PropTrack& p = c->propTracks[k];
			p.path = rdStr(i); p.comp = rdStr(i); p.prop = rdStr(i);
			rd(i, p.dim);
			rdKeys(i, p.keys);
		}
	}
	if (version >= 4)
		c->skelGuid = rdStr(i);
	if (!i && !i.eof()) { delete c; return nullptr; }
	return c;
}

}  // namespace nuke
