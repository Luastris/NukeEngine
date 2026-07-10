// NUPAK container + layered content resolver (roadmap 3.2). See Package.h for the design.
//
// On-disk layout (little-endian):
//   header: char magic[6] = "NUPAK1"; uint16 flags = 0; uint64 tocOffset;
//   ...entry payloads (raw or compressed)...
//   TOC at tocOffset: uint32 count; per entry:
//     uint16 pathLen; char path[pathLen] (utf8, '/');
//     uint8 method; uint64 offset; uint64 rawSize; uint64 packSize; uint32 crc32(raw);
#include "API/Model/Package.h"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/thread/mutex.hpp>
#include <zlib.h>
#include <zstd.h>
#include <nlohmann/json.hpp>   // config/mods.json + per-mod "mod.json" manifests (deps)
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <map>
#include <set>

namespace bfs = boost::filesystem;
using std::cout;
using std::endl;

namespace nuke {

static const char kMagic[6] = { 'N', 'U', 'P', 'A', 'K', '1' };

// ---- helpers ---------------------------------------------------------------------------

static std::string LowerKey(const std::string& s)
{
	std::string k; k.reserve(s.size());
	for (char c : s) k += (c == '\\') ? '/' : (char)std::tolower((unsigned char)c);
	return k;
}

uint32_t Package::Crc32(const void* data, size_t size, uint32_t seed)
{
	return (uint32_t)::crc32(seed, (const Bytef*)data, (uInt)size);
}

static bool ReadWhole(const std::string& path, std::string& out)
{
	bfs::ifstream f(bfs::path(path), std::ios::binary);
	if (!f) return false;
	f.seekg(0, std::ios::end);
	std::streamoff n = f.tellg();
	f.seekg(0, std::ios::beg);
	out.resize((size_t)(n < 0 ? 0 : n));
	if (n > 0) f.read(&out[0], n);
	return (bool)f;
}

static bool Decompress(uint8_t method, const std::string& packed, uint64_t rawSize, std::string& out)
{
	if (method == Package::M_Store) { out = packed; return true; }
	out.resize((size_t)rawSize);
	if (method == Package::M_Zlib)
	{
		uLongf dst = (uLongf)rawSize;
		if (::uncompress((Bytef*)&out[0], &dst, (const Bytef*)packed.data(), (uLong)packed.size()) != Z_OK) return false;
		return dst == rawSize;
	}
	if (method == Package::M_Zstd)
	{
		size_t r = ZSTD_decompress(rawSize ? &out[0] : (char*)nullptr, (size_t)rawSize,
		                           packed.data(), packed.size());
		return !ZSTD_isError(r) && r == rawSize;
	}
	return false;
}

// ---- TOC parse (shared by File and the mount layer) --------------------------------------

static bool ParseToc(const std::string& pakPath, std::vector<Package::Entry>& out)
{
	out.clear();
	bfs::ifstream f(bfs::path(pakPath), std::ios::binary);
	if (!f) return false;
	char magic[6]; uint16_t flags = 0; uint64_t tocOff = 0;
	f.read(magic, 6); f.read((char*)&flags, 2); f.read((char*)&tocOff, 8);
	if (!f || memcmp(magic, kMagic, 6) != 0) return false;
	f.seekg((std::streamoff)tocOff, std::ios::beg);
	uint32_t count = 0;
	f.read((char*)&count, 4);
	if (!f || count > 10 * 1000 * 1000) return false;
	out.reserve(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		uint16_t plen = 0;
		f.read((char*)&plen, 2);
		if (!f || plen == 0 || plen > 4096) return false;
		Package::Entry e;
		e.path.resize(plen);
		f.read(&e.path[0], plen);
		f.read((char*)&e.method, 1);
		f.read((char*)&e.offset, 8);
		f.read((char*)&e.rawSize, 8);
		f.read((char*)&e.packSize, 8);
		f.read((char*)&e.crc, 4);
		if (!f) return false;
		out.push_back(std::move(e));
	}
	return true;
}

static bool ReadEntry(const std::string& pakPath, const Package::Entry& e, std::string& out)
{
	bfs::ifstream f(bfs::path(pakPath), std::ios::binary);
	if (!f) return false;
	f.seekg((std::streamoff)e.offset, std::ios::beg);
	std::string packed;
	packed.resize((size_t)e.packSize);
	if (e.packSize) f.read(&packed[0], (std::streamsize)e.packSize);
	if (!f) return false;
	if (!Decompress(e.method, packed, e.rawSize, out)) return false;
	return Package::Crc32(out.data(), out.size()) == e.crc;
}

// ---- writer ------------------------------------------------------------------------------

bool Package::Create(const std::vector<std::pair<std::string, std::string>>& files,
                     const std::string& outPak, int method, int level,
                     const std::function<void(int, int)>& progress)
{
	boost::system::error_code ec;
	if (bfs::path(outPak).has_parent_path()) bfs::create_directories(bfs::path(outPak).parent_path(), ec);
	bfs::ofstream o(bfs::path(outPak), std::ios::binary | std::ios::trunc);
	if (!o) { cout << "[Package]\tcan't write " << outPak << endl; return false; }

	uint16_t flags = 0; uint64_t tocOff = 0;
	o.write(kMagic, 6); o.write((const char*)&flags, 2); o.write((const char*)&tocOff, 8);

	std::vector<Entry> toc;
	toc.reserve(files.size());
	const int total = (int)files.size();
	int done = 0;
	for (const auto& fp : files)
	{
		std::string raw;
		if (!ReadWhole(fp.second, raw))
		{
			cout << "[Package]\tcan't read " << fp.second << endl;
			o.close(); bfs::remove(bfs::path(outPak), ec);
			return false;
		}
		Entry e;
		e.path = fp.first;
		for (char& c : e.path) if (c == '\\') c = '/';
		e.rawSize = raw.size();
		e.crc = Crc32(raw.data(), raw.size());
		e.method = (uint8_t)method;

		std::string packed;
		if (method == M_Zlib && !raw.empty())
		{
			uLongf cap = compressBound((uLong)raw.size());
			packed.resize(cap);
			int lv = level < 1 ? 1 : (level > 9 ? 9 : level);
			if (::compress2((Bytef*)&packed[0], &cap, (const Bytef*)raw.data(), (uLong)raw.size(), lv) == Z_OK)
				packed.resize(cap);
			else packed.clear();
		}
		else if (method == M_Zstd && !raw.empty())
		{
			size_t cap = ZSTD_compressBound(raw.size());
			packed.resize(cap);
			int lv = level < 1 ? 1 : (level > ZSTD_maxCLevel() ? ZSTD_maxCLevel() : level);
			size_t r = ZSTD_compress(&packed[0], cap, raw.data(), raw.size(), lv);
			if (!ZSTD_isError(r)) packed.resize(r); else packed.clear();
		}
		// Store when requested, when compression failed, or when it didn't shrink.
		if (packed.empty() || packed.size() >= raw.size()) { packed = raw; e.method = M_Store; }

		e.offset = (uint64_t)o.tellp();
		e.packSize = packed.size();
		if (!packed.empty()) o.write(packed.data(), (std::streamsize)packed.size());
		toc.push_back(std::move(e));
		if (progress) progress(++done, total);
	}

	tocOff = (uint64_t)o.tellp();
	uint32_t count = (uint32_t)toc.size();
	o.write((const char*)&count, 4);
	for (const Entry& e : toc)
	{
		uint16_t plen = (uint16_t)e.path.size();
		o.write((const char*)&plen, 2);
		o.write(e.path.data(), plen);
		o.write((const char*)&e.method, 1);
		o.write((const char*)&e.offset, 8);
		o.write((const char*)&e.rawSize, 8);
		o.write((const char*)&e.packSize, 8);
		o.write((const char*)&e.crc, 4);
	}
	o.seekp(8, std::ios::beg);           // patch the header's tocOffset
	o.write((const char*)&tocOff, 8);
	if (!o) { o.close(); bfs::remove(bfs::path(outPak), ec); return false; }
	return true;
}

// ---- standalone handle --------------------------------------------------------------------

bool Package::File::Open(const std::string& pakPath)
{
	path = pakPath;
	return ParseToc(pakPath, entries);
}

const Package::Entry* Package::File::Find(const std::string& rel) const
{
	std::string k = LowerKey(rel);
	for (const Entry& e : entries)
		if (LowerKey(e.path) == k) return &e;
	return nullptr;
}

bool Package::File::Read(const std::string& rel, std::string& out) const
{
	const Entry* e = Find(rel);
	return e && ReadEntry(path, *e, out);
}

// ---- mounted layer stack --------------------------------------------------------------------

struct MountLayer
{
	std::string pakPath;
	int priority = 0;
	std::time_t pakTime = 0;                          // cache validity vs re-packed paks
	std::map<std::string, Package::Entry> byKey;      // LowerKey(path) -> entry
};

static std::vector<MountLayer> gMounts;               // sorted by priority DESC
static std::string gRawRoot;
static boost::mutex gPakLock;

bool Package::Mount(const std::string& pakPath, int priority)
{
	// Distinct diagnostics: a wrong PATH (the common config typo) must not read as a
	// corrupt package.
	boost::system::error_code fec;
	if (!bfs::exists(bfs::path(pakPath), fec))
	{
		cout << "[Package]\tmount FAILED (file not found): " << pakPath
		     << "  (cwd: " << bfs::current_path(fec).string() << ")" << endl;
		return false;
	}
	std::vector<Entry> toc;
	if (!ParseToc(pakPath, toc))
	{
		cout << "[Package]\tmount FAILED (not a NUPAK / corrupt): " << pakPath << endl;
		return false;
	}
	boost::mutex::scoped_lock l(gPakLock);
	MountLayer m;
	m.pakPath = pakPath;
	m.priority = priority;
	boost::system::error_code ec;
	m.pakTime = bfs::last_write_time(bfs::path(pakPath), ec);
	for (Entry& e : toc) m.byKey[LowerKey(e.path)] = std::move(e);
	const size_t count = m.byKey.size();
	gMounts.push_back(std::move(m));
	std::stable_sort(gMounts.begin(), gMounts.end(),
	                 [](const MountLayer& a, const MountLayer& b) { return a.priority > b.priority; });
	cout << "[Package]\tmounted " << bfs::path(pakPath).filename().string()
	     << " (" << count << " entries, priority " << priority << ")" << endl;
	return true;
}

void Package::UnmountAll() { boost::mutex::scoped_lock l(gPakLock); gMounts.clear(); }
int  Package::MountedCount() { boost::mutex::scoped_lock l(gPakLock); return (int)gMounts.size(); }

std::vector<std::string> Package::MountedPaks()
{
	boost::mutex::scoped_lock l(gPakLock);
	std::vector<std::string> out;
	out.reserve(gMounts.size());
	for (const MountLayer& m : gMounts) out.push_back(m.pakPath);
	return out;
}

void Package::SetRawRoot(const std::string& projectDir) { boost::mutex::scoped_lock l(gPakLock); gRawRoot = projectDir; }
const std::string& Package::RawRoot() { return gRawRoot; }

bool Package::Read(const std::string& rel, std::string& out)
{
	// The RAW layer wins over mounts: it is the developer/modder OVERLAY (their edits sit
	// on top of the read-only base pak). A packed runtime simply has no raw root.
	if (!gRawRoot.empty())
	{
		boost::system::error_code ec;
		bfs::path p = bfs::path(gRawRoot) / rel;
		if (bfs::exists(p, ec)) return ReadWhole(p.string(), out);
	}
	std::string k = LowerKey(rel);
	{
		boost::mutex::scoped_lock l(gPakLock);
		for (const MountLayer& m : gMounts)
		{
			auto it = m.byKey.find(k);
			if (it != m.byKey.end())
			{
				const Entry e = it->second;         // copy: read outside the lock
				const std::string pak = m.pakPath;
				l.unlock();
				return ReadEntry(pak, e, out);
			}
		}
	}
	return false;
}

// ---- mods with dependencies (mods-on-mods) ----------------------------------------------------
static std::vector<Package::ModInfo> gMods;   // mount order, bottom-up (guarded by gPakLock)

const std::vector<Package::ModInfo>& Package::Mods() { return gMods; }

int Package::MountMods(const std::string& gameRoot)
{
	{ boost::mutex::scoped_lock l(gPakLock); gMods.clear(); }
	bfs::path root(gameRoot);
	bfs::ifstream mf(root / "config" / "mods.json");
	if (!mf) return 0;
	nlohmann::json mj = nlohmann::json::parse(mf, nullptr, false);
	if (mj.is_discarded() || !mj.contains("mods") || !mj["mods"].is_array()) return 0;

	// 1) Resolve every enabled entry to a file + read its manifest (name + requires).
	std::vector<ModInfo> mods;
	for (auto& m : mj["mods"])
	{
		if (!m.is_string()) continue;
		std::string mp = m.get<std::string>();
		boost::system::error_code ec;
		// Tolerant resolution: as written (absolute or game-root-relative), then under
		// mods/, then mods/<filename> — a bare name in the config is the common way.
		bfs::path cand[] = { bfs::path(mp), root / mp, root / "mods" / mp,
		                     root / "mods" / bfs::path(mp).filename() };
		std::string resolved;
		for (bfs::path& c : cand) if (bfs::exists(c, ec) && !bfs::is_directory(c, ec)) { resolved = c.string(); break; }
		if (resolved.empty())
		{
			cout << "[Package]\tmod not found (skipped): " << mp << endl;
			continue;
		}
		ModInfo mi;
		mi.pakPath = resolved;
		mi.name = bfs::path(resolved).stem().string();
		Package::File pf;
		std::string man;
		if (pf.Open(resolved) && pf.Read("mod.json", man))
		{
			nlohmann::json j = nlohmann::json::parse(man, nullptr, false);
			if (!j.is_discarded() && j.is_object())
			{
				mi.name = j.value("name", mi.name);
				if (j.contains("requires") && j["requires"].is_array())
					for (auto& r : j["requires"])
						if (r.is_string()) mi.requires_.push_back(r.get<std::string>());
			}
		}
		mods.push_back(std::move(mi));
	}

	// 2) Dependency-aware order: repeatedly take the first mod whose requirements are all
	// already placed (keeps the config order among independents, pulls dependencies below
	// dependents). A mod with a missing/cyclic dependency never becomes ready -> skipped.
	auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	std::vector<ModInfo> order;
	std::set<std::string> placed;
	std::vector<bool> done(mods.size(), false);
	for (bool progress = true; progress; )
	{
		progress = false;
		for (size_t i = 0; i < mods.size(); ++i)
		{
			if (done[i]) continue;
			bool ready = true;
			for (const std::string& r : mods[i].requires_) ready &= placed.count(lower(r)) != 0;
			if (!ready) continue;
			placed.insert(lower(mods[i].name));
			order.push_back(mods[i]);
			done[i] = true;
			progress = true;
		}
	}
	for (size_t i = 0; i < mods.size(); ++i)
		if (!done[i])
		{
			std::string missing;
			for (const std::string& r : mods[i].requires_)
				if (!placed.count(lower(r))) missing += (missing.empty() ? "" : ", ") + r;
			cout << "[Package]\tmod '" << mods[i].name << "' requires [" << missing
			     << "] which is not enabled — skipped" << endl;
		}

	// 3) Mount in that order above the base pak (priority 1..N).
	int prio = 1, mounted = 0;
	for (ModInfo& mi : order)
	{
		if (!Package::Mount(mi.pakPath, prio)) { cout << "[Package]\tmod skipped (bad pak): " << mi.pakPath << endl; continue; }
		if (!mi.requires_.empty())
		{
			std::string deps;
			for (const std::string& r : mi.requires_) deps += (deps.empty() ? "" : ", ") + r;
			cout << "[Package]\tmod '" << mi.name << "' depends on [" << deps << "]" << endl;
		}
		++prio; ++mounted;
		boost::mutex::scoped_lock l(gPakLock);
		gMods.push_back(mi);
	}
	return mounted;
}

int Package::ReadAll(const std::string& rel, std::vector<std::string>& out)
{
	out.clear();
	std::string k = LowerKey(rel);
	// Collect (pak, entry) hits under the lock, decompress outside it. gMounts is sorted by
	// priority DESC — walk it in reverse for bottom-up (base game first, mods above).
	std::vector<std::pair<std::string, Entry>> hits;
	{
		boost::mutex::scoped_lock l(gPakLock);
		for (auto it = gMounts.rbegin(); it != gMounts.rend(); ++it)
		{
			auto e = it->byKey.find(k);
			if (e != it->byKey.end()) hits.push_back({ it->pakPath, e->second });
		}
	}
	for (auto& h : hits)
	{
		std::string data;
		if (ReadEntry(h.first, h.second, data)) out.push_back(std::move(data));
	}
	if (!gRawRoot.empty())   // the dev/modder overlay is the TOP layer
	{
		boost::system::error_code ec;
		bfs::path p = bfs::path(gRawRoot) / rel;
		std::string data;
		if (bfs::exists(p, ec) && ReadWhole(p.string(), data)) out.push_back(std::move(data));
	}
	return (int)out.size();
}

int Package::ReadAllInfo(const std::string& rel, std::vector<std::pair<std::string, std::string>>& out)
{
	out.clear();
	std::string k = LowerKey(rel);
	std::vector<std::pair<std::string, Entry>> hits;
	{
		boost::mutex::scoped_lock l(gPakLock);
		for (auto it = gMounts.rbegin(); it != gMounts.rend(); ++it)
		{
			auto e = it->byKey.find(k);
			if (e != it->byKey.end()) hits.push_back({ it->pakPath, e->second });
		}
	}
	for (auto& h : hits)
	{
		std::string data;
		if (ReadEntry(h.first, h.second, data)) out.push_back({ std::move(data), h.first });
	}
	if (!gRawRoot.empty())
	{
		boost::system::error_code ec;
		bfs::path p = bfs::path(gRawRoot) / rel;
		std::string data;
		if (bfs::exists(p, ec) && ReadWhole(p.string(), data)) out.push_back({ std::move(data), std::string() });
	}
	return (int)out.size();
}

bool Package::ReadMounted(const std::string& rel, std::string& out)
{
	std::string k = LowerKey(rel);
	boost::mutex::scoped_lock l(gPakLock);
	for (const MountLayer& m : gMounts)   // priority DESC: first hit = top mounted layer
	{
		auto it = m.byKey.find(k);
		if (it != m.byKey.end())
		{
			const Entry e = it->second;
			const std::string pak = m.pakPath;
			l.unlock();
			return ReadEntry(pak, e, out);
		}
	}
	return false;
}

bool Package::Exists(const std::string& rel)
{
	if (!gRawRoot.empty())
	{
		boost::system::error_code ec;
		if (bfs::exists(bfs::path(gRawRoot) / rel, ec)) return true;
	}
	std::string k = LowerKey(rel);
	boost::mutex::scoped_lock l(gPakLock);
	for (const MountLayer& m : gMounts)
		if (m.byKey.count(k)) return true;
	return false;
}

std::string Package::ResolveRead(const std::string& rel)
{
	// RAW layers only. Pak entries deliberately resolve to NOTHING here: decompressing
	// them onto disk would lay the packed project out in the open (the user was explicit
	// that the pak is the protection) — packed content is served as BYTES via Read().
	if (gRawRoot.empty()) return std::string();
	boost::system::error_code ec;
	bfs::path p = bfs::path(gRawRoot) / rel;
	return bfs::exists(p, ec) ? p.string() : std::string();
}

std::vector<std::string> Package::List(const std::string& prefix)
{
	std::string pk = LowerKey(prefix);
	std::map<std::string, std::string> found;   // key -> original path (top layer wins; raw overlay is the top)
	if (!gRawRoot.empty())
	{
		boost::system::error_code ec;
		bfs::path root(gRawRoot);
		if (bfs::exists(root, ec))
			for (bfs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
			{
				if (ec) break;
				if (bfs::is_directory(it->path())) continue;
				std::string rel = bfs::relative(it->path(), root, ec).generic_string();
				if (ec) { ec.clear(); continue; }
				std::string key = LowerKey(rel);
				// Dev noise never counts as content.
				if (key == "editor_state.json") continue;
				if (key.size() > 4 && key.compare(key.size() - 4, 4, ".log") == 0) continue;
				if (key.compare(0, pk.size(), pk) == 0 && !found.count(key))
					found[key] = rel;
			}
	}
	{
		boost::mutex::scoped_lock l(gPakLock);
		for (const MountLayer& m : gMounts)
			for (const auto& kv : m.byKey)
				if (kv.first.compare(0, pk.size(), pk) == 0 && !found.count(kv.first))
					found[kv.first] = kv.second.path;
	}
	std::vector<std::string> out;
	out.reserve(found.size());
	for (auto& kv : found) out.push_back(kv.second);
	return out;
}

}  // namespace nuke
