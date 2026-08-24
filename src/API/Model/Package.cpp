// On-disk layout (little-endian): magic[6]="NUPAK1", uint16 flags, uint64 tocOffset, payloads, then
// TOC = uint32 count + per entry uint16 pathLen, path (utf8 '/'), uint8 method,
// uint64 offset/rawSize/packSize, uint32 crc32(raw).
// flags bit 0 (format v2): each entry continues with uint8 layout, uint32 blockCount and per
// block uint32 rawSize/packSize/meta[3] + uint8 method — the payload is those blocks back to
// back, each compressed on its own (GDeflate streams are DirectStorage requests as they lie).
#include "API/Model/Package.h"
#include "API/Model/Jobs.h"      // block compression fans out on the pool
#include "interface/Modular.h"   // module dependency check on mount
#include "config.h"              // writableDir: mod cache + user mods.json live there
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/thread/mutex.hpp>
#include <zlib.h>
#include <zstd.h>
#include <gdeflate/GDeflate.h>
#include <nlohmann/json.hpp>   // config/mods.json + per-mod "mod.json" manifests (deps)
#include <algorithm>
#include <atomic>
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
static const uint16_t kFlagBlocks = 1;   // format v2: block tables in the TOC

static std::map<uint8_t, Package::UncookFn>& Layouts() { static std::map<uint8_t, Package::UncookFn> m; return m; }
static std::map<std::string, Package::CookFn>& Cooks() { static std::map<std::string, Package::CookFn> m; return m; }
void Package::RegisterLayout(uint8_t layout, const UncookFn& uncook) { Layouts()[layout] = uncook; }
void Package::RegisterCook(const std::string& extension, const CookFn& cook) { Cooks()[extension] = cook; }

int Package::MaxLevel(int method)
{
	if (method == M_Zlib) return 9;
	if (method == M_Zstd) return ZSTD_maxCLevel();
	if (method == M_GDeflate) return (int)GDeflate::MaximumCompressionLevel;
	return 1;
}

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

bool Package::Inflate(uint8_t method, const char* packed, size_t packSize, uint64_t rawSize, std::string& out)
{
	if (method == M_Store) { out.assign(packed, packSize); return true; }
	out.resize((size_t)rawSize);
	if (rawSize == 0) return true;
	if (method == M_Zlib)
	{
		uLongf dst = (uLongf)rawSize;
		if (::uncompress((Bytef*)&out[0], &dst, (const Bytef*)packed, (uLong)packSize) != Z_OK) return false;
		return dst == rawSize;
	}
	if (method == M_Zstd)
	{
		size_t r = ZSTD_decompress(&out[0], (size_t)rawSize, packed, packSize);
		return !ZSTD_isError(r) && r == rawSize;
	}
	if (method == M_GDeflate)
		return GDeflate::Decompress((uint8_t*)&out[0], (size_t)rawSize, (const uint8_t*)packed, packSize, 1);
	return false;
}

// Compress one block with `method`; empty result = keep it stored.
static void Deflate(int method, int level, const char* raw, size_t rawSize, std::string& packed)
{
	packed.clear();
	if (rawSize == 0) return;
	if (method == Package::M_Zlib)
	{
		uLongf cap = compressBound((uLong)rawSize);
		packed.resize(cap);
		int lv = level < 1 ? 1 : (level > 9 ? 9 : level);
		if (::compress2((Bytef*)&packed[0], &cap, (const Bytef*)raw, (uLong)rawSize, lv) == Z_OK) packed.resize(cap);
		else packed.clear();
	}
	else if (method == Package::M_Zstd)
	{
		size_t cap = ZSTD_compressBound(rawSize);
		packed.resize(cap);
		int lv = level < 1 ? 1 : (level > ZSTD_maxCLevel() ? ZSTD_maxCLevel() : level);
		size_t r = ZSTD_compress(&packed[0], cap, raw, rawSize, lv);
		if (!ZSTD_isError(r)) packed.resize(r); else packed.clear();
	}
	else if (method == Package::M_GDeflate)
	{
		size_t cap = GDeflate::CompressBound(rawSize);
		packed.resize(cap);
		uint32_t lv = (uint32_t)(level < 1 ? 1 : (level > (int)GDeflate::MaximumCompressionLevel ? (int)GDeflate::MaximumCompressionLevel : level));
		// Single-threaded per block: the blocks themselves fan out on the Jobs pool.
		if (GDeflate::Compress((uint8_t*)&packed[0], &cap, (const uint8_t*)raw, rawSize, lv, GDeflate::COMPRESS_SINGLE_THREAD))
			packed.resize(cap);
		else packed.clear();
	}
	if (packed.size() >= rawSize) packed.clear();   // didn't shrink: store
}

bool Package::Compress(uint8_t method, int level, const char* raw, size_t rawSize, std::string& out)
{
	Deflate(method, level, raw, rawSize, out);
	return !out.empty();
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
	const bool blocked = (flags & kFlagBlocks) != 0;
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
		if (blocked)
		{
			uint32_t nb = 0;
			f.read((char*)&e.layout, 1);
			f.read((char*)&nb, 4);
			if (!f || nb > 16u * 1024 * 1024) return false;
			e.blocks.resize(nb);
			for (uint32_t b = 0; b < nb; ++b)
			{
				Package::Block& blk = e.blocks[b];
				f.read((char*)&blk.rawSize, 4); f.read((char*)&blk.packSize, 4);
				f.read((char*)blk.meta, 12);
				f.read((char*)&blk.method, 1);
			}
		}
		if (!f) return false;
		out.push_back(std::move(e));
	}
	return true;
}

// The inflated blocks of an entry, in order (a v1 entry = its single payload).
static bool ReadEntryBlocks(const std::string& pakPath, const Package::Entry& e, std::vector<std::string>& blocks)
{
	bfs::ifstream f(bfs::path(pakPath), std::ios::binary);
	if (!f) return false;
	f.seekg((std::streamoff)e.offset, std::ios::beg);
	std::string packed;
	packed.resize((size_t)e.packSize);
	if (e.packSize) f.read(&packed[0], (std::streamsize)e.packSize);
	if (!f) return false;
	if (e.blocks.empty())
	{
		blocks.resize(1);
		return Package::Inflate(e.method, packed.data(), packed.size(), e.rawSize, blocks[0]);
	}
	blocks.resize(e.blocks.size());
	std::vector<uint64_t> at(e.blocks.size());
	uint64_t off = 0;
	for (size_t b = 0; b < e.blocks.size(); ++b) { at[b] = off; off += e.blocks[b].packSize; }
	if (off != e.packSize) return false;
	std::atomic<bool> ok(true);
	// Serial below a handful of blocks; the calling worker would otherwise just wait on itself.
	Jobs::ParallelFor(0, (int)e.blocks.size(), e.blocks.size() < 4 ? (int)e.blocks.size() : 1, [&](int b)
	{
		const Package::Block& blk = e.blocks[b];
		if (!Package::Inflate(blk.method, packed.data() + at[b], blk.packSize, blk.rawSize, blocks[b])) ok = false;
	});
	return ok;
}

bool Package::Uncook(const Entry& e, std::vector<std::string>& blocks, std::string& out)
{
	if (e.layout != 0)
	{
		auto it = Layouts().find(e.layout);
		if (it == Layouts().end())
		{
			cout << "[Package]\t'" << e.path << "': unknown layout " << (int)e.layout << " (module missing?)" << endl;
			return false;
		}
		return it->second(e.blocks, blocks, out);
	}
	if (blocks.size() == 1) { out.swap(blocks[0]); return true; }
	out.clear(); out.reserve((size_t)e.rawSize);
	for (std::string& b : blocks) out += b;
	return true;
}

static bool ReadEntry(const std::string& pakPath, const Package::Entry& e, std::string& out)
{
	std::vector<std::string> blocks;
	if (!ReadEntryBlocks(pakPath, e, blocks)) return false;
	if (!Package::Uncook(e, blocks, out)) return false;
	return Package::Crc32(out.data(), out.size()) == e.crc;
}

// ---- writer ------------------------------------------------------------------------------

bool Package::Create(const std::vector<std::pair<std::string, std::string>>& files,
                     const std::string& outPak, int method, int level,
                     const std::function<void(int, int)>& progress, const CreateOptions* options)
{
	boost::system::error_code ec;
	if (bfs::path(outPak).has_parent_path()) bfs::create_directories(bfs::path(outPak).parent_path(), ec);
	bfs::ofstream o(bfs::path(outPak), std::ios::binary | std::ios::trunc);
	if (!o) { cout << "[Package]\tcan't write " << outPak << endl; return false; }

	const CreateOptions defaults;
	const CreateOptions& opt = options ? *options : defaults;
	const uint32_t blockBytes = opt.blockBytes < 65536 ? 65536 : opt.blockBytes;

	uint16_t flags = kFlagBlocks; uint64_t tocOff = 0;
	o.write(kMagic, 6); o.write((const char*)&flags, 2); o.write((const char*)&tocOff, 8);

	std::vector<Entry> toc;
	toc.reserve(files.size());
	const int total = (int)files.size();
	int done = 0;

	// One file, cooked and compressed: the TOC entry + its packed payload.
	struct Packed { Entry e; std::string payload; bool ok = false; };
	auto packOne = [&](const std::pair<std::string, std::string>& fp, Packed& out)
	{
		std::string raw;
		if (!ReadWhole(fp.second, raw)) { cout << "[Package]\tcan't read " << fp.second << endl; return; }
		Entry& e = out.e;
		e.path = fp.first;
		for (char& c : e.path) if (c == '\\') c = '/';
		e.rawSize = raw.size();
		e.crc = Crc32(raw.data(), raw.size());

		// Cook (layout > 0) or split the bytes at blockBytes (layout 0).
		Cooked cooked;
		bool cookedOk = false;
		if (opt.cook)
		{
			std::string ext = bfs::path(e.path).extension().string();
			for (char& c : ext) c = (char)std::tolower((unsigned char)c);
			auto ck = Cooks().find(ext);
			if (ck != Cooks().end()) cookedOk = ck->second(raw, blockBytes, cooked) && !cooked.blocks.empty();
		}
		if (!cookedOk)
		{
			cooked = Cooked();
			cooked.bytes.swap(raw);
			uint64_t off = 0;
			do
			{
				CookedBlock cb; cb.offset = off;
				cb.size = std::min<uint64_t>(blockBytes, cooked.bytes.size() - off);
				cooked.blocks.push_back(cb);
				off += cb.size;
			} while (off < cooked.bytes.size());
		}
		e.layout = cooked.layout;
		for (const CookedBlock& cb : cooked.blocks)
			if (cb.size > 0xFFFFFFFFull || cb.offset + cb.size > cooked.bytes.size())
			{ cout << "[Package]\tcook of '" << e.path << "' produced an invalid block" << endl; return; }

		// Every block compresses on its own (the pool works the blocks of one file; the
		// caller already fans files out).
		std::vector<std::string> packed(cooked.blocks.size());
		std::vector<uint8_t>     methods(cooked.blocks.size(), (uint8_t)method);
		Jobs::ParallelFor(0, (int)cooked.blocks.size(), 1, [&](int b)
		{
			const CookedBlock& cb = cooked.blocks[b];
			if (method != M_Store) Deflate(method, level, cooked.bytes.data() + cb.offset, (size_t)cb.size, packed[b]);
			if (packed[b].empty()) { packed[b].assign(cooked.bytes.data() + cb.offset, (size_t)cb.size); methods[b] = M_Store; }
		});
		e.method = (uint8_t)method;
		e.blocks.resize(cooked.blocks.size());
		uint64_t packTotal = 0;
		for (size_t b = 0; b < packed.size(); ++b)
		{
			Block& blk = e.blocks[b];
			blk.rawSize = (uint32_t)cooked.blocks[b].size; blk.packSize = (uint32_t)packed[b].size();
			blk.method = methods[b];
			memcpy(blk.meta, cooked.blocks[b].meta, sizeof(blk.meta));
			packTotal += packed[b].size();
		}
		out.payload.reserve((size_t)packTotal);
		for (std::string& p : packed) out.payload += p;
		e.packSize = packTotal;
		out.ok = true;
	};

	// Files go through in windows: a window packs in parallel, then streams out in order
	// (bounded memory — a window holds a few files' worth of raw + packed bytes at most).
	const int window = std::max(2, Jobs::WorkerCount() * 2);
	for (int base = 0; base < total; base += window)
	{
		const int n = std::min(window, total - base);
		std::vector<Packed> batch(n);
		Jobs::ParallelFor(0, n, 1, [&](int i) { packOne(files[base + i], batch[i]); });
		for (int i = 0; i < n; ++i)
		{
			Packed& p = batch[i];
			if (!p.ok) { o.close(); bfs::remove(bfs::path(outPak), ec); return false; }
			p.e.offset = (uint64_t)o.tellp();
			if (!p.payload.empty()) o.write(p.payload.data(), (std::streamsize)p.payload.size());
			toc.push_back(std::move(p.e));
			if (progress) progress(++done, total);
		}
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
		uint32_t nb = (uint32_t)e.blocks.size();
		o.write((const char*)&e.layout, 1);
		o.write((const char*)&nb, 4);
		for (const Block& blk : e.blocks)
		{
			o.write((const char*)&blk.rawSize, 4); o.write((const char*)&blk.packSize, 4);
			o.write((const char*)blk.meta, 12);
			o.write((const char*)&blk.method, 1);
		}
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
static std::vector<std::string> gDlcNames;            // DLCs mounted by MountDlcs
static std::vector<std::string> gModuleCacheDirs;     // native mod/DLC plugins unpacked for the loader
static std::map<std::string, std::string> gTypeReplaces;   // merged mod "replaces" (mount order wins)
static void ExtractPakModules(const std::string& gameRoot, const std::string& pakPath,
                              const std::string& name);   // defined with MountModList below

bool Package::Mount(const std::string& pakPath, int priority)
{
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

void Package::UnmountAll()
{
	boost::mutex::scoped_lock l(gPakLock);
	gMounts.clear(); gDlcNames.clear(); gModuleCacheDirs.clear(); gTypeReplaces.clear();
}

std::string Package::TypeReplacement(const std::string& type)
{
	boost::mutex::scoped_lock l(gPakLock);
	auto it = gTypeReplaces.find(type);
	return it != gTypeReplaces.end() ? it->second : std::string();
}
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
	// The RAW layer wins over mounts: it is the dev/modder overlay (a packed runtime has no raw root).
	if (!gRawRoot.empty())
	{
		boost::system::error_code ec;
		bfs::path p = bfs::path(gRawRoot) / rel;
		if (bfs::exists(p, ec)) return ReadWhole(p.string(), out);
	}
	std::string k = LowerKey(rel);
	{
		boost::mutex::scoped_lock l(gPakLock);
		for (size_t i = 0; i < gMounts.size(); ++i)
		{
			const MountLayer& m = gMounts[i];
			auto it = m.byKey.find(k);
			if (it == m.byKey.end()) continue;
			// Warn ONCE per path when a layer above the base shadows a path a lower layer also carries.
			if (m.priority > 0)
				for (size_t j = i + 1; j < gMounts.size(); ++j)
					if (gMounts[j].byKey.count(k))
					{
						static std::set<std::string> warned;
						if (warned.insert(k).second)
							cout << "[Package]\t'" << rel << "' OVERRIDDEN by: "
							     << bfs::path(m.pakPath).stem().string() << endl;
						break;
					}
			const Entry e = it->second;             // copy: read outside the lock
			const std::string pak = m.pakPath;
			l.unlock();
			return ReadEntry(pak, e, out);
		}
	}
	return false;
}

bool Package::Locate(const std::string& rel, Location& out)
{
	if (!gRawRoot.empty())
	{
		boost::system::error_code ec;
		if (bfs::exists(bfs::path(gRawRoot) / rel, ec)) return false;   // raw overlay wins: a disk file
	}
	const std::string k = LowerKey(rel);
	boost::mutex::scoped_lock l(gPakLock);
	for (const MountLayer& m : gMounts)
	{
		auto it = m.byKey.find(k);
		if (it == m.byKey.end()) continue;
		out.pakPath = m.pakPath;
		out.entry   = it->second;
		return true;
	}
	return false;
}

bool Package::ReadBlocks(const Location& loc, std::vector<std::string>& blocks)
{
	return ReadEntryBlocks(loc.pakPath, loc.entry, blocks);
}

bool Package::ReadBlock(const Location& loc, size_t index, std::string& out)
{
	const Entry& e = loc.entry;
	if (e.blocks.empty())
	{
		if (index != 0) return false;
		std::vector<std::string> one;
		if (!ReadEntryBlocks(loc.pakPath, e, one)) return false;
		out.swap(one[0]);
		return true;
	}
	if (index >= e.blocks.size()) return false;
	uint64_t off = e.offset;
	for (size_t b = 0; b < index; ++b) off += e.blocks[b].packSize;
	const Block& blk = e.blocks[index];
	bfs::ifstream f(bfs::path(loc.pakPath), std::ios::binary);
	if (!f) return false;
	f.seekg((std::streamoff)off, std::ios::beg);
	std::string packed((size_t)blk.packSize, '\0');
	if (blk.packSize) f.read(&packed[0], (std::streamsize)blk.packSize);
	if (!f) return false;
	return Inflate(blk.method, packed.data(), packed.size(), blk.rawSize, out);
}

// ---- mods with dependencies (mods-on-mods) ----------------------------------------------------
static std::vector<Package::ModInfo> gMods;   // mount order, bottom-up (guarded by gPakLock)

const std::vector<Package::ModInfo>& Package::Mods() { return gMods; }

bool Package::Unmount(const std::string& pakPath)
{
	boost::mutex::scoped_lock l(gPakLock);
	boost::system::error_code ec;
	auto same = [&](const std::string& p) {
		return p == pakPath || bfs::equivalent(bfs::path(p), bfs::path(pakPath), ec);
	};
	for (auto it = gMounts.begin(); it != gMounts.end(); ++it)
		if (same(it->pakPath))
		{
			gMounts.erase(it);
			for (auto m = gMods.begin(); m != gMods.end(); ++m)
				if (same(m->pakPath)) { gMods.erase(m); break; }
			return true;
		}
	return false;
}

const char* Package::CurrentPlatform()
{
#if defined(_WIN64)
	return "win64";
#elif defined(_WIN32)
	return "win32";
#elif defined(__APPLE__)
	return "mac64";
#else
	return "linux64";
#endif
}

// Empty list = runs anywhere; otherwise the current platform must be listed.
static bool PlatformListOk(const std::vector<std::string>& platforms)
{
	if (platforms.empty()) return true;
	for (const std::string& p : platforms)
		if (p == "any" || p == Package::CurrentPlatform()) return true;
	return false;
}

bool Package::ReadPakInfo(const std::string& pakPath, PakInfo& out)
{
	out = PakInfo();
	Package::File pf;
	std::string man;
	if (!pf.Open(pakPath) || !pf.Read("pak.json", man)) return false;
	nlohmann::json j = nlohmann::json::parse(man, nullptr, false);
	if (j.is_discarded() || !j.is_object()) return false;
	out.kind = j.value("kind", std::string());
	out.name = j.value("name", std::string());
	out.base = j.value("base", std::string());
	if (j.contains("platforms") && j["platforms"].is_array())
		for (auto& p : j["platforms"])
			if (p.is_string()) out.platforms.push_back(p.get<std::string>());
	if (j.contains("parts") && j["parts"].is_array())
		for (auto& p : j["parts"])
			if (p.is_string()) out.parts.push_back(p.get<std::string>());
	if (j.contains("modules") && j["modules"].is_array())
		for (auto& p : j["modules"])
			if (p.is_string()) out.modules.push_back(p.get<std::string>());
	out.partOf = j.value("part_of", std::string());
	return true;
}

int Package::MountPakParts(const std::string& mainPak, int priority)
{
	PakInfo pi;
	if (!ReadPakInfo(mainPak, pi) || pi.parts.empty()) return 0;
	boost::system::error_code ec;
	const bfs::path dir = bfs::path(mainPak).parent_path();
	int mounted = 0;
	for (const std::string& part : pi.parts)
	{
		bfs::path p = dir / bfs::path(part).filename();
		if (!bfs::exists(p, ec)) { cout << "[Package]\tpart MISSING: " << part << endl; continue; }
		if (Package::Mount(p.string(), priority)) ++mounted;
	}
	return mounted;
}

const std::vector<std::string>& Package::MountedDlcs() { return gDlcNames; }

int Package::MountDlcs(const std::string& gameRoot, const std::string& baseName)
{
	gDlcNames.clear();
	bfs::path dir = bfs::path(gameRoot) / "content" / "dlc";
	boost::system::error_code ec;
	if (!bfs::exists(dir, ec) || !bfs::is_directory(dir, ec)) return 0;
	std::vector<std::string> paks;
	for (bfs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
		if (it->path().extension() == ".nupak") paks.push_back(it->path().string());
	std::sort(paks.begin(), paks.end());   // deterministic load order
	auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	int prio = 1, mounted = 0;   // above the base (0), below the mods (1000+)
	for (const std::string& p : paks)
	{
		const std::string fn = bfs::path(p).filename().string();
		PakInfo pi;
		if (!ReadPakInfo(p, pi)) { cout << "[Package]\tdlc skipped (no dlc manifest): " << fn << endl; continue; }
		if (!pi.partOf.empty()) continue;   // a split part: its main pak mounts it
		if (pi.kind != "dlc")
		{
			cout << "[Package]\tdlc skipped (no dlc manifest): " << fn << endl;
			continue;
		}
		// A DLC belongs to ONE game; legacy bases carry no name, so folder placement is the only binding.
		if (!pi.base.empty() && !baseName.empty() && lower(pi.base) != lower(baseName))
		{
			cout << "[Package]\tdlc '" << pi.name << "' is for '" << pi.base
			     << "', not '" << baseName << "' — skipped" << endl;
			continue;
		}
		if (!PlatformListOk(pi.platforms))
		{
			cout << "[Package]\tdlc '" << pi.name << "' excludes platform "
			     << CurrentPlatform() << " — skipped" << endl;
			continue;
		}
		// Engine plugins its content is built on — same rule as a mod, including the exemption:
		// a plugin the DLC itself ships (modules/ inside its pak) satisfies its own requirement.
		{
			Package::File dpf;
			const bool dOpen = dpf.Open(p);
			auto shipsIt = [&](const std::string& name)
			{
				if (!dOpen) return false;
				std::string want = bfs::path(name).stem().string();
				for (char& c : want) c = (char)tolower((unsigned char)c);
				for (const Entry& e : dpf.Entries())
				{
					if (LowerKey(e.path).compare(0, 8, "modules/") != 0) continue;
					std::string st = bfs::path(e.path).stem().string();
					for (char& c : st) c = (char)tolower((unsigned char)c);
					if (st == want) return true;
				}
				return false;
			};
			std::string missing;
			for (const std::string& mod : pi.modules)
				if (!shipsIt(mod) && !ModuleInstalled(mod)) missing += (missing.empty() ? "" : ", ") + mod;
			if (!missing.empty())
			{
				cout << "[Package]\tdlc '" << pi.name << "' needs module(s) [" << missing
				     << "] which are not installed — skipped" << endl;
				continue;
			}
		}
		if (Package::Mount(p, prio))
		{
			MountPakParts(p, prio);   // the DLC's own split parts ride at its priority
			gDlcNames.push_back(pi.name.empty() ? bfs::path(p).stem().string() : pi.name);
			ExtractPakModules(gameRoot, p, gDlcNames.back());
			++prio; ++mounted;
		}
	}
	return mounted;
}

int Package::MountMods(const std::string& gameRoot)
{
	bfs::path root(gameRoot);
	// The USER'S mod list (writable root) wins; the shipped one beside the pak is the
	// fallback — an installed bundle's config is read-only defaults.
	bfs::path listPath = Config::writableDir() / "config" / "mods.json";
	boost::system::error_code lec;
	if (!bfs::exists(listPath, lec))
		listPath = root / "config" / "mods.json";
	bfs::ifstream mf(listPath);
	if (!mf) return 0;
	nlohmann::json mj = nlohmann::json::parse(mf, nullptr, false);
	if (mj.is_discarded() || !mj.contains("mods") || !mj["mods"].is_array()) return 0;
	std::vector<std::string> entries;
	for (auto& m : mj["mods"])
		if (m.is_string()) entries.push_back(m.get<std::string>());
	return MountModList(gameRoot, entries);
}

const std::vector<std::string>& Package::ModuleCacheDirs() { return gModuleCacheDirs; }

// Native plugins a mounted pak (mod or DLC) ships ride in it under "modules/". Machine code
// cannot run out of an archive — relocations, imports, TLS blocks, x64 unwind tables and
// symbols are all the OS loader's job and it needs a real file — so they land in
// <gameRoot>/config/modcache/<name>/, where the module system picks them up with the same ABI
// gate as any other plugin. A file whose bytes already match is left alone, so this costs
// nothing after the first run.
static void ExtractPakModules(const std::string& gameRoot, const std::string& pakPath,
                              const std::string& name)
{
	Package::File pf;
	if (!pf.Open(pakPath)) return;
	std::string safe = name.empty() ? bfs::path(pakPath).stem().string() : name;
	for (char& c : safe) if (strchr("\\/:*?\"<>|", c)) c = '_';
	// Cached module binaries must land in the WRITABLE root: an installed game bundle is
	// read-only territory (and signed) — see Config::writableDir().
	const bfs::path dir = Config::writableDir() / "config" / "modcache" / safe;
	(void)gameRoot;
	int wrote = 0, kept = 0;
	boost::system::error_code ec;
	for (const Package::Entry& e : pf.Entries())
	{
		const std::string key = LowerKey(e.path);
		if (key.compare(0, 8, "modules/") != 0) continue;
		std::string raw;
		if (!pf.Read(e.path, raw) || raw.empty()) continue;
		const bfs::path dst = dir / bfs::path(e.path).filename();
		// Same bytes already there: leave the file alone so a running session keeps its lock.
		bool same = false;
		if (bfs::exists(dst, ec) && (uintmax_t)bfs::file_size(dst, ec) == (uintmax_t)raw.size())
		{
			bfs::ifstream in(dst, std::ios::binary);
			std::string cur((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			same = (cur == raw);
		}
		if (same) { ++kept; continue; }
		bfs::create_directories(dir, ec);
		bfs::ofstream o(dst, std::ios::binary | std::ios::trunc);
		if (!o) { cout << "[Package]\t'" << safe << "': cannot cache module " << e.path << endl; continue; }
		o.write(raw.data(), (std::streamsize)raw.size());
		++wrote;
	}
	if (wrote + kept > 0)
	{
		// DLCs extract before the mod pass, and the editor remounts stacks repeatedly: the list
		// survives both, so entries de-dup here and only UnmountAll clears it.
		if (std::find(gModuleCacheDirs.begin(), gModuleCacheDirs.end(), dir.string()) == gModuleCacheDirs.end())
			gModuleCacheDirs.push_back(dir.string());
		cout << "[Package]\t'" << safe << "': " << (wrote + kept) << " native module(s) ready ("
		     << wrote << " written, " << kept << " unchanged) in " << dir.string() << endl;
	}
}

int Package::MountModList(const std::string& gameRoot, const std::vector<std::string>& entries)
{
	// Substitutions reset with the mod metadata: only mods contribute them, so unlike the
	// module cache dirs (DLCs extract before this) there is nothing here to preserve.
	{ boost::mutex::scoped_lock l(gPakLock); gMods.clear(); gTypeReplaces.clear(); }
	bfs::path root(gameRoot);

	// 1) Resolve every entry to a file + read its manifest (name + requires).
	std::vector<ModInfo> mods;
	for (const std::string& mp : entries)
	{
		boost::system::error_code ec;
		// Tolerant resolution: as written, game-root-relative, mods/, then mods/<filename>.
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
				mi.name     = j.value("name", mi.name);
				mi.platform = j.value("platform", std::string());
				mi.partOf   = j.value("part_of", std::string());
				if (j.contains("requires") && j["requires"].is_array())
					for (auto& r : j["requires"])
						if (r.is_string()) mi.requires_.push_back(r.get<std::string>());
				if (j.contains("dlc") && j["dlc"].is_array())
					for (auto& r : j["dlc"])
						if (r.is_string()) mi.dlc.push_back(r.get<std::string>());
				if (j.contains("modules") && j["modules"].is_array())
					for (auto& r : j["modules"])
						if (r.is_string()) mi.modules.push_back(r.get<std::string>());
				if (j.contains("replaces") && j["replaces"].is_object())
					for (auto it = j["replaces"].begin(); it != j["replaces"].end(); ++it)
						if (it.value().is_string()) mi.replaces.push_back({ it.key(), it.value().get<std::string>() });
				if (j.contains("parts") && j["parts"].is_array())
					for (auto& r : j["parts"])
						if (r.is_string()) mi.parts.push_back(r.get<std::string>());
			}
		}
		// A part pak is mounted by its main mod — never as a mod of its own.
		if (!mi.partOf.empty())
		{
			cout << "[Package]\t'" << bfs::path(resolved).filename().string()
			     << "' is a part of mod '" << mi.partOf << "' — mounted with it, not listed" << endl;
			continue;
		}
		// A mod authored on top of a DLC patches content that only that DLC brings.
		{
			auto eqCI = [](const std::string& a, const std::string& b)
			{
				if (a.size() != b.size()) return false;
				for (size_t k = 0; k < a.size(); ++k)
					if (tolower((unsigned char)a[k]) != tolower((unsigned char)b[k])) return false;
				return true;
			};
			std::string missing;
			for (const std::string& d : mi.dlc)
			{
				bool have = false;
				for (const std::string& h : gDlcNames) if (eqCI(h, d)) { have = true; break; }
				if (!have) missing += (missing.empty() ? "" : ", ") + d;
			}
			if (!missing.empty())
			{
				cout << "[Package]\tmod '" << mi.name << "' needs DLC [" << missing
				     << "] which is not installed — skipped" << endl;
				continue;
			}
		}
		// Engine plugins the content is built on. Without them its components would load as
		// inert placeholders — a half-applied mod is worse than an absent one. A plugin the mod
		// ITSELF ships (modules/ inside its pak) satisfies its own requirement: it is extracted
		// and discovered right after mounting, so demanding it be pre-installed would deadlock
		// the exact mod that brings it.
		{
			auto shipsIt = [&](const std::string& name)
			{
				std::string want = bfs::path(name).stem().string();
				for (char& c : want) c = (char)tolower((unsigned char)c);
				for (const Entry& e : pf.Entries())
				{
					if (LowerKey(e.path).compare(0, 8, "modules/") != 0) continue;
					std::string st = bfs::path(e.path).stem().string();
					for (char& c : st) c = (char)tolower((unsigned char)c);
					if (st == want) return true;
				}
				return false;
			};
			std::string missing, off;
			for (const std::string& mod : mi.modules)
			{
				if (shipsIt(mod)) continue;
				bool loaded = false;
				if (!ModuleInstalled(mod, &loaded)) missing += (missing.empty() ? "" : ", ") + mod;
				else if (!loaded && !GetModules().empty()) off += (off.empty() ? "" : ", ") + mod;
			}
			if (!missing.empty())
			{
				cout << "[Package]\tmod '" << mi.name << "' needs module(s) [" << missing
				     << "] which are not installed — skipped" << endl;
				continue;
			}
			if (!off.empty())
				cout << "[Package]\tmod '" << mi.name << "' needs module(s) [" << off
				     << "] which are installed but disabled — its content stays inert" << endl;
		}
		// Content-only mods are cross-platform; native code was tagged at packaging time.
		if (!mi.platform.empty() && mi.platform != "any" && mi.platform != CurrentPlatform())
		{
			cout << "[Package]\tmod '" << mi.name << "' is " << mi.platform << "-only (this is "
			     << CurrentPlatform() << ") — skipped" << endl;
			continue;
		}
		mods.push_back(std::move(mi));
	}

	// 2) Dependency-aware order: repeatedly take mods whose requirements are already placed (config
	// order kept among independents). A missing/cyclic dependency never becomes ready -> skipped.
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

	// 3) Mount above the base AND the DLC layer: DLCs sit at 1..K, mods start at 1000.
	int prio = 1000, mounted = 0;
	for (ModInfo& mi : order)
	{
		if (!Package::Mount(mi.pakPath, prio)) { cout << "[Package]\tmod skipped (bad pak): " << mi.pakPath << endl; continue; }
		// Parts ride at the same priority as their main pak (a split never duplicates a path).
		for (const std::string& part : mi.parts)
		{
			boost::system::error_code pec;
			bfs::path pcand[] = { bfs::path(part), root / part, root / "mods" / part,
			                      root / "mods" / bfs::path(part).filename(),
			                      bfs::path(mi.pakPath).parent_path() / bfs::path(part).filename() };
			std::string presolved;
			for (bfs::path& c : pcand) if (bfs::exists(c, pec) && !bfs::is_directory(c, pec)) { presolved = c.string(); break; }
			if (presolved.empty() || !Package::Mount(presolved, prio))
				cout << "[Package]\tmod '" << mi.name << "': part '" << part << "' missing/bad — SKIPPED (content of that part won't load)" << endl;
		}
		if (!mi.requires_.empty())
		{
			std::string deps;
			for (const std::string& r : mi.requires_) deps += (deps.empty() ? "" : ", ") + r;
			cout << "[Package]\tmod '" << mi.name << "' depends on [" << deps << "]" << endl;
		}
		ExtractPakModules(root.string(), mi.pakPath, mi.name);
		++prio; ++mounted;
		{
			boost::mutex::scoped_lock l(gPakLock);
			gMods.push_back(mi);
			// Mount order is authority: a later mod's substitution overwrites an earlier one's.
			for (const auto& rp : mi.replaces)
			{
				gTypeReplaces[rp.first] = rp.second;
				cout << "[Package]\tmod '" << mi.name << "' replaces component '" << rp.first
				     << "' with '" << rp.second << "'" << endl;
			}
		}
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
	// RAW layers only: pak entries deliberately resolve to "" — packed content is bytes via Read().
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
