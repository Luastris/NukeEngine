// The asset import facade: images + audio in the engine, scene files through the "exchange"
// service, plugin importers first; the worker driver with the status-bar progress.
#include "import/Importer.h"
#include "interface/Importers.h"   // plugin importer registry (dispatched from ImportAny)
#include "interface/Modular.h"     // RegisteringModule: importer entries remember their module
#include "interface/Services.h"
#include "service/iExchange.h"
#include "API/Model/Texture.h"
#include "API/Model/resdb.h"
#include "API/Model/Jobs.h"        // async import
#include "API/Model/StatusBar.h"   // live import status
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/atomic.hpp>
#include <boost/thread/mutex.hpp>
#include <stb_image.h>             // GIF frames (the implementation lives in stb_impl.cpp)
#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>

namespace bfs = boost::filesystem;
using std::cout; using std::endl;

namespace nuke {

namespace {
// Live import progress (worker -> status bar). Installed only by an ASYNC import;
// a synchronous game-thread import leaves it null.
struct ImportProgress
{
	std::string key;    // StatusBar entry id (unique per queued import)
	std::string name;   // source filename (label prefix)
	int done  = 0;      // completed units
	int total = 0;      // 0 until the source was counted -> indeterminate bar
};
thread_local ImportProgress* tlProg = nullptr;
// Thread-local defer sink: non-null only inside a WORKER import; a synchronous import on the
// game thread applies mutations immediately.
thread_local std::vector<boost::function<void()>>* tlDeferSink = nullptr;

std::string SafeStem(const char* in)
{
	std::string s = (in && in[0]) ? in : "asset";
	for (char& c : s)
		if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
	return s;
}
std::string LowerExt(const char* path)
{
	std::string ext = bfs::path(path).extension().string();
	for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	return ext;
}
std::vector<std::string> SplitExts(const std::string& joined)
{
	std::vector<std::string> out;
	std::string cur;
	for (char c : joined) { if (c == ';') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += c; }
	if (!cur.empty()) out.push_back(cur);
	return out;
}
std::string ServiceExts(bool exportSide)
{
	iExchange* x = GetService<iExchange>();
	if (!x) return "";
	const int n = exportSide ? x->ExportExtensions(nullptr, 0) : x->ImportExtensions(nullptr, 0);
	if (n <= 0) return "";
	std::string s((size_t)n, '\0');
	if (exportSide) x->ExportExtensions(&s[0], n); else x->ImportExtensions(&s[0], n);
	return s;
}
}  // namespace

Importer* Importer::getSingleton() { static Importer s; return &s; }

void Importer::Stage(const std::string& label, float sub)
{
	if (!tlProg) return;
	if (tlProg->total <= 0)
	{
		StatusBar::Set(tlProg->key, tlProg->name + " — " + label, StatusBar::kIndeterminate);
		return;
	}
	if (sub < 0.0f) sub = 0.0f;
	if (sub > 1.0f) sub = 1.0f;
	StatusBar::Set(tlProg->key, tlProg->name + " — " + label, ((float)tlProg->done + sub) / (float)tlProg->total);
}
void Importer::UnitDone()        { if (tlProg) ++tlProg->done; }
void Importer::SetTotal(int total) { if (tlProg) { tlProg->done = 0; tlProg->total = total; } }

void Importer::Reg(const boost::function<void()>& f)
{
	if (tlDeferSink) tlDeferSink->push_back(f);
	else f();
}

bool Importer::ModelImportAvailable() { return GetService<iExchange>() != nullptr; }
std::vector<std::string> Importer::ModelExtensions()  { return SplitExts(ServiceExts(false)); }
std::vector<std::string> Importer::ExportExtensions() { return SplitExts(ServiceExts(true)); }
const std::vector<std::string>& Importer::ImageExtensions()
{
	static const std::vector<std::string> v = { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".pic", ".ppm", ".pgm" };
	return v;
}
const std::vector<std::string>& Importer::AudioExtensions()
{
	static const std::vector<std::string> v = { ".ogg", ".wav", ".mp3", ".flac" };
	return v;
}

int Importer::ImportModel(const char* srcPath, const char* destDir)
{
	iExchange* x = GetService<iExchange>();
	if (!x)
	{
		cout << "[Import]\tno model importer: enable the NukeExchange module (the 'exchange' service) to import "
		     << bfs::path(srcPath).filename().string() << endl;
		return 0;
	}
	boost::system::error_code ec;
	bfs::create_directories(destDir, ec);
	return x->ImportModel(srcPath, destDir);
}

std::string Importer::ImportImage(const char* srcPath, const char* destDir)
{
	const std::string ext = LowerExt(srcPath);
	SetTotal(1);   // one unit: this image
	Stage("reading");

	Texture* tex = new Texture();
	tex->guid = ResDB::NewGuid();
	tex->usage = (Texture::Usage)Texture::GuessUsage(srcPath);   // bare image: role guessed from the filename suffix
	int w = 0, h = 0;

	if (ext == ".gif")
	{
		// Animated GIF: load ALL frames + per-frame delays (frames stacked w*h*frames*4, RGBA8, BC per frame, no mips).
		bfs::ifstream gf(bfs::path(srcPath), std::ios::binary);
		std::vector<unsigned char> buf((std::istreambuf_iterator<char>(gf)), std::istreambuf_iterator<char>());
		if (buf.empty()) { delete tex; cout << "[Import]\tgif read failed: " << srcPath << endl; return std::string(); }
		int frames = 0, comp = 0; int* delays = nullptr;
		unsigned char* px = stbi_load_gif_from_memory(buf.data(), (int)buf.size(), &delays, &w, &h, &frames, &comp, 4);
		if (!px || frames < 1) { delete tex; cout << "[Import]\tgif decode failed: " << srcPath << endl; return std::string(); }
		// stb returns DELTA frames (transparent where unchanged) — composite forward into full frames.
		const size_t fb = (size_t)w * h * 4;
		for (int k = 1; k < frames; ++k)
		{
			unsigned char* prev = px + (size_t)(k - 1) * fb;
			unsigned char* cur  = px + (size_t)k * fb;
			for (size_t p = 0; p < (size_t)w * h; ++p)
				if (cur[p * 4 + 3] == 0) { cur[p*4] = prev[p*4]; cur[p*4+1] = prev[p*4+1]; cur[p*4+2] = prev[p*4+2]; cur[p*4+3] = prev[p*4+3]; }
		}
		std::vector<int> delaysMs(frames);
		for (int k = 0; k < frames; ++k) delaysMs[k] = (delays && delays[k] > 0) ? delays[k] : 100;
		std::vector<unsigned char> all(px, px + fb * frames);
		stbi_image_free(px);
		if (delays) stbi_image_free(delays);
		tex->BuildFramesFromRGBA(all, w, h, frames, delaysMs, [](float f) { Importer::Stage("compressing frames", f); });
	}
	else
	{
		std::vector<unsigned char> rgba;
		if (!Texture::DecodeImageFile(srcPath, rgba, w, h)) { delete tex; cout << "[Import]\timage load failed: " << srcPath << endl; return std::string(); }
		tex->BuildFromRGBA(rgba, w, h, tex->usage, [](float f) { Importer::Stage("compressing", f); });   // BC1/BC3/BC5 + mip chain
	}

	const std::string stem = SafeStem(bfs::path(srcPath).stem().string().c_str());
	boost::system::error_code ec;
	bfs::path out = bfs::path(destDir) / (stem + ".nutex");
	for (int k = 1; bfs::exists(out, ec); ++k)
		out = bfs::path(destDir) / (stem + "_" + std::to_string(k) + ".nutex");
	if (!tex->SaveToFile(out.string())) { delete tex; return std::string(); }
	const std::string outPath = out.string();
	Reg([tex, outPath]
	{
		ResDB::getSingleton()->RegisterTexture(tex);
		ResDB::getSingleton()->SetAssetPath(tex->guid, outPath);
	});   // main-thread when async
	cout << "[Import]\twrote " << out.filename().string() << " (" << w << "x" << h << ")" << endl;
	return tex->guid;
}

// Import an audio file as a collision-safe COPY into content. No custom asset format and
// nothing to register — components reference the file by its content-relative path.
bool Importer::ImportAudio(const char* srcPath, const char* destDir)
{
	SetTotal(1);
	Stage("copying");
	const std::string stem = SafeStem(bfs::path(srcPath).stem().string().c_str());
	const std::string ext  = LowerExt(srcPath);
	boost::system::error_code ec;
	bfs::path out = bfs::path(destDir) / (stem + ext);
	for (int k = 1; bfs::exists(out, ec); ++k)
		out = bfs::path(destDir) / (stem + "_" + std::to_string(k) + ext);
	bfs::copy_file(bfs::path(srcPath), out, ec);
	if (ec) { cout << "[Import]\taudio copy failed: " << srcPath << " (" << ec.message() << ")" << endl; return false; }
	cout << "[Import]\twrote " << out.filename().string() << " (audio)" << endl;
	return true;
}

// --- plugin importer registry (interface/Importers.h) --------------------------------------
namespace {
std::vector<AssetImporter>& importerReg() { static std::vector<AssetImporter> v; return v; }
std::vector<std::string>& importerOwners() { static std::vector<std::string> v; return v; }   // parallel
}

void RegisterImporter(const AssetImporter& imp)
{
	for (const AssetImporter& e : importerReg())   // dedup by label (re-enabling a plugin re-registers)
		if (e.label == imp.label) return;
	importerOwners().push_back(RegisteringModule());
	importerReg().push_back(imp);
}
const std::vector<AssetImporter>& AssetImporters() { return importerReg(); }

void UnregisterImportersOf(const std::string& moduleDll)
{
	if (moduleDll.empty()) return;
	for (size_t i = importerReg().size(); i-- > 0;)
		if (importerOwners()[i] == moduleDll)
		{
			importerReg().erase(importerReg().begin() + i);
			importerOwners().erase(importerOwners().begin() + i);
		}
}
void ImporterDefer(const std::function<void()>& fn) { Importer::Reg(fn); }   // -> main thread (see Importer::Reg)
const AssetImporter* ImporterForExt(const std::string& ext)
{
	std::string want = ext;
	for (char& c : want) c = (char)std::tolower((unsigned char)c);
	for (const AssetImporter& e : importerReg())
		for (const std::string& x : e.exts)
		{
			std::string xl = x; for (char& c : xl) c = (char)std::tolower((unsigned char)c);
			if (xl == want) return &e;
		}
	return nullptr;
}

bool Importer::ImportAny(const char* srcPath, const char* destDir)
{
	if (!srcPath || !*srcPath) return false;
	const std::string ext = LowerExt(srcPath);
	// Plugin importers win over built-ins (a plugin may deliberately override one).
	if (const AssetImporter* imp = ImporterForExt(ext))
		return imp->import ? imp->import(srcPath, destDir) : false;
	for (const std::string& e : ImageExtensions())
		if (ext == e) return !ImportImage(srcPath, destDir).empty();
	for (const std::string& e : AudioExtensions())
		if (ext == e) return ImportAudio(srcPath, destDir);
	return ImportModel(srcPath, destDir) > 0;
}

void Importer::ImportAnyAsync(const std::string& srcPath, const std::string& destDir, boost::function<void(bool)> onDone)
{
	// Unique status-bar entry per queued import.
	static boost::atomic<int> seq(0);
	const std::string key  = "import#" + std::to_string(++seq);
	const std::string name = bfs::path(srcPath).filename().string();
	StatusBar::Set(key, name + " — queued", StatusBar::kIndeterminate);

	Importer* self = getSingleton();
	Jobs::Schedule([self, srcPath, destDir, onDone, key, name]()
	{
		// Serialize imports: two racing over the same destination names would collide.
		static boost::mutex importLock;
		boost::mutex::scoped_lock l(importLock);

		auto defers = std::make_shared<std::vector<boost::function<void()>>>();
		ImportProgress prog;
		prog.key = key; prog.name = name;
		tlProg = &prog;
		tlDeferSink = defers.get();
		bool ok = false;
		try { ok = self->ImportAny(srcPath.c_str(), destDir.c_str()); }
		catch (const std::exception& e) { cout << "[Import]\tasync import threw: " << e.what() << endl; }
		tlDeferSink = nullptr;
		tlProg = nullptr;

		// Registrations + completion land on the GAME thread (ResDB is not thread-safe).
		StatusBar::Set(key, name + " — registering", 1.0f);
		Jobs::RunOnMain([defers, onDone, ok, key]()
		{
			for (auto& f : *defers) f();
			StatusBar::Remove(key);
			if (onDone) onDone(ok);
		});
	});
}

}  // namespace nuke
