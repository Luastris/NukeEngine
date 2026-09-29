// Versioned data upgrades: the format / component / savegame registries and the content batch.
#include "API/Model/Migrations.h"
#include "API/Model/AnimClip.h"
#include "API/Model/JsonDoc.h"
#include "API/Model/Log.h"
#include "API/Model/Mesh.h"
#include "API/Model/Texture.h"
#include "interface/Modular.h"
#include "interface/AppInstance.h"
#include <ctime>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/thread/mutex.hpp>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>

namespace bfs = boost::filesystem;
using json = nlohmann::json;

namespace nuke {

namespace {
struct StepRec { int from; std::string desc; Migrations::Step fn; };
struct Format  { int current = 1; std::vector<StepRec> steps; };
struct Binary  { std::string ext; char magic[8]; int current; int reimportBelow; std::function<bool(const std::string&)> resave; };

boost::mutex g_mx;
std::map<std::string, Format>& Formats()    { static std::map<std::string, Format> m; return m; }
std::map<std::string, Format>& Components() { static std::map<std::string, Format> m; return m; }
std::map<std::string, std::string>& JsonExts() { static std::map<std::string, std::string> m; return m; }
std::vector<Binary>& Binaries() { static std::vector<Binary> v; return v; }
int g_gameVersion = 0;
std::vector<Migrations::TextUpgrader> g_textUpgraders;
Migrations::Report g_last;
bool g_builtins = false;

// A step's note: collected for the batch / savegame report and logged there. The loaders'
// in-memory upgrades pass no log - a world full of unstamped components would flood the log
// on every load until the files are re-saved.
// A refusal is never silent: it reaches the log whether or not a report collects it.
void Warn(std::vector<std::string>* log, const std::string& s)
{
	if (log) log->push_back(s);
	std::cout << "[Migrations]\t" << s << std::endl;
	Log::Write(LOG_WARN, "Migrations", s);
}
void Say(std::vector<std::string>* log, const std::string& s)
{
	if (log) log->push_back(s);   // the batch prints its report once; the save load prints its log
}

// The engine's own formats (declared once; modules and games add theirs at static init).
void DeclareBuiltins()
{
	if (g_builtins) return;
	g_builtins = true;
	std::map<std::string, std::string>& e = JsonExts();
	e[".nuworld"] = "world";      e[".nuprefab"] = "prefab";   e[".numat"] = "material";
	e[".nuskel"] = "skeleton";    e[".nubonemap"] = "bonemap"; e[".nusm"] = "animsm";
	e[".nublend"] = "blendspace"; e[".nuseq"] = "sequence";    e[".nurag"] = "ragdoll";
	e[".nuinput"] = "input";      e[".nucursor"] = "cursor";   e[".nupair"] = "pair";
	for (auto& kv : e) Formats()[kv.second];   // current 1
	Formats()["save"];
	auto bin = [](const char* ext, const char* magic, int cur, std::function<bool(const std::string&)> resave)
	{
		Binary b; b.ext = ext; memcpy(b.magic, magic, 8); b.current = cur; b.reimportBelow = 0; b.resave = resave;
		Binaries().push_back(b);
	};
	bin(".numesh", "NUMESH\0\0", Mesh::FormatVersion(), [](const std::string& p) { Mesh* m = Mesh::LoadFromFile(p); if (!m) return false; const bool ok = m->SaveToFile(p); delete m; return ok; });
	bin(".nutex",  "NUTEX\0\0\0", Texture::FormatVersion(), [](const std::string& p) { Texture* t = Texture::LoadFromFile(p); if (!t) return false; const bool ok = t->SaveToFile(p); delete t; return ok; });
	bin(".nuanim", "NUANIM\0\0", AnimClip::FormatVersion(), [](const std::string& p) { AnimClip* c = AnimClip::LoadFromFile(p); if (!c) return false; const bool ok = c->SaveToFile(p); delete c; return ok; });
}

// Steps from `from` up to `current` on `doc`; false = the stamp is newer than `current`.
bool RunSteps(const char* what, const std::string& name, Format& f, int from, json& doc, std::vector<std::string>* log, bool* changed)
{
	if (from > f.current)
	{
		Warn(log, std::string(what) + " '" + name + "' is version " + std::to_string(from) + ", this engine knows " + std::to_string(f.current) + " - needs a newer engine");
		return false;
	}
	for (int v = from; v < f.current; ++v)
	{
		bool any = false;
		for (StepRec& s : f.steps)
			if (s.from == v) { s.fn(doc); any = true; Say(log, std::string(what) + " '" + name + "' v" + std::to_string(v) + " -> v" + std::to_string(v + 1) + ": " + s.desc); }
		if (!any) Say(log, std::string(what) + " '" + name + "' v" + std::to_string(v) + " -> v" + std::to_string(v + 1) + " (no step: defaults)");
		*changed = true;
	}
	return true;
}
}  // namespace

// ---- formats ----------------------------------------------------------------------------------

void Migrations::DeclareFormat(const std::string& kind, int current)
{
	boost::mutex::scoped_lock l(g_mx); DeclareBuiltins();
	Formats()[kind].current = std::max(1, current);
}

int Migrations::FormatVersion(const std::string& kind)
{
	boost::mutex::scoped_lock l(g_mx); DeclareBuiltins();
	auto it = Formats().find(kind);
	return it == Formats().end() ? 1 : it->second.current;
}

void Migrations::Register(const std::string& kind, int from, const std::string& description, Step step)
{
	if (!step || from < 1) return;
	boost::mutex::scoped_lock l(g_mx); DeclareBuiltins();
	Format& f = Formats()[kind];
	f.steps.push_back({ from, description, step });
	f.current = std::max(f.current, from + 1);
}

int Migrations::StampOf(const json& doc)
{
	if (!doc.is_object()) return 1;
	auto it = doc.find("version");
	return (it != doc.end() && it->is_number_integer()) ? it->get<int>() : 1;
}

bool Migrations::Pending(const std::string& kind, const json& doc)
{
	return StampOf(doc) != FormatVersion(kind);
}

bool Migrations::Upgrade(const std::string& kind, json& doc, std::vector<std::string>* log)
{
	if (!doc.is_object()) return true;
	Format f;
	{ boost::mutex::scoped_lock l(g_mx); DeclareBuiltins(); f = Formats()[kind]; }
	const int from = StampOf(doc);
	if (from == f.current) return true;
	bool changed = false;
	if (!RunSteps("document", kind, f, from, doc, log, &changed)) return false;
	doc["version"] = f.current;
	return true;
}

// ---- components ---------------------------------------------------------------------------------

void Migrations::DeclareComponentVersion(const std::string& type, int current)
{
	boost::mutex::scoped_lock l(g_mx);
	Format& f = Components()[type];
	f.current = std::max(f.current, current);
}

int Migrations::ComponentVersion(const std::string& type)
{
	boost::mutex::scoped_lock l(g_mx);
	auto it = Components().find(type);
	return it == Components().end() ? 0 : it->second.current;
}

void Migrations::RegisterComponent(const std::string& type, int from, const std::string& description, Step step)
{
	if (!step || from < 0) return;
	boost::mutex::scoped_lock l(g_mx);
	Format& f = Components()[type];
	f.steps.push_back({ from, description, step });
	f.current = std::max(f.current, from + 1);
}

bool Migrations::UpgradeComponent(json& c, std::vector<std::string>* log)
{
	if (!c.is_object()) return false;
	auto tit = c.find("type");
	if (tit == c.end() || !tit->is_string()) return false;
	Format f;
	{
		boost::mutex::scoped_lock l(g_mx);
		auto it = Components().find(tit->get<std::string>());
		if (it == Components().end()) return false;
		f = it->second;
	}
	auto vit = c.find("v");
	const int from = (vit != c.end() && vit->is_number_integer()) ? vit->get<int>() : 0;   // unstamped = the pre-versioning data
	if (from == f.current) return false;
	if (from > f.current) { Warn(log, "component '" + tit->get<std::string>() + "' is data version " + std::to_string(from) + ", this build knows " + std::to_string(f.current) + " - loading as is"); return false; }
	bool changed = false;
	for (int v = from; v < f.current; ++v)
		for (StepRec& s : f.steps)
			if (s.from == v) { s.fn(c); changed = true; Say(log, "component '" + tit->get<std::string>() + "' v" + std::to_string(v) + " -> v" + std::to_string(v + 1) + ": " + s.desc); }
	c["v"] = f.current;
	return changed || from != f.current;
}

bool Migrations::UpgradeAtomTree(json& atom, std::vector<std::string>* log)
{
	if (!atom.is_object()) return false;
	bool changed = false;
	{
		boost::mutex::scoped_lock l(g_mx);
		if (Components().empty()) return false;   // nothing declared: the common fast path
	}
	auto cs = atom.find("components");
	if (cs != atom.end() && cs->is_array())
		for (json& c : *cs) changed |= UpgradeComponent(c, log);
	auto ch = atom.find("children");
	if (ch != atom.end() && ch->is_array())
		for (json& k : *ch) changed |= UpgradeAtomTree(k, log);
	return changed;
}

// ---- savegames ------------------------------------------------------------------------------------

void Migrations::SetGameVersion(int version) { boost::mutex::scoped_lock l(g_mx); g_gameVersion = std::max(0, version); }
int  Migrations::GameVersion()               { boost::mutex::scoped_lock l(g_mx); return g_gameVersion; }
void Migrations::AddSaveTextUpgrader(TextUpgrader fn) { if (!fn) return; boost::mutex::scoped_lock l(g_mx); g_textUpgraders.push_back(fn); }

json Migrations::SaveHeader()
{
	json h;
	h["engine"] = EngineVersion();
	h["format"] = FormatVersion("world");
	h["game"]   = GameVersion();
	return h;
}

bool Migrations::UpgradeSave(json& doc, std::vector<std::string>* log)
{
	if (!doc.is_object()) return false;
	// 1. the world format (the document IS a world document)
	if (!Upgrade("world", doc, log)) return false;
	// 2. the game's own save-data version
	const json h = doc.value("save", json::object());
	const int from = h.is_object() ? h.value("game", 0) : 0;
	const int to = GameVersion();
	if (from > to) { Warn(log, "save is game version " + std::to_string(from) + ", this build is " + std::to_string(to) + " - refused"); return false; }
	if (from < to)
	{
		Format f; std::vector<TextUpgrader> tus;
		{ boost::mutex::scoped_lock l(g_mx); DeclareBuiltins(); f = Formats()["save"]; tus = g_textUpgraders; }
		bool changed = false;
		bool anyStep = false;
		for (const StepRec& s : f.steps) if (s.from >= from && s.from < to) { anyStep = true; break; }
		if (anyStep)
		{
			for (int v = from; v < to; ++v)
				for (StepRec& s : f.steps)
					if (s.from == v) { s.fn(doc); changed = true; Say(log, "save game v" + std::to_string(v) + " -> v" + std::to_string(v + 1) + ": " + s.desc); }
		}
		else
		{
			// The script hosts: the first one whose game defines an upgrader takes it (1 = handled,
			// 0 = no handler there, -1 = the handler failed).
			int handled = 0;
			std::string text = doc.dump();
			for (TextUpgrader& tu : tus)
			{
				const int rc = tu(from, to, text);
				if (rc == 0) continue;
				handled = rc; break;
			}
			if (handled < 0) { Warn(log, "the game's save upgrader failed (" + std::to_string(from) + " -> " + std::to_string(to) + ") - refused"); return false; }
			if (handled > 0)
			{
				json u = json::parse(text, nullptr, false);
				if (u.is_discarded() || !u.is_object()) { Warn(log, "the game's save upgrader returned invalid JSON - refused"); return false; }
				doc = std::move(u);
				changed = true;
				Say(log, "save game v" + std::to_string(from) + " -> v" + std::to_string(to) + " (script upgrader)");
			}
			else Say(log, "save game v" + std::to_string(from) + " -> v" + std::to_string(to) + " (no upgrader registered: loading as is)");
		}
		(void)changed;
	}
	doc["save"] = SaveHeader();
	// 3. component data
	auto atoms = doc.find("atoms");
	if (atoms != doc.end() && atoms->is_array())
		for (json& a : *atoms) UpgradeAtomTree(a, log);
	return true;
}

// ---- the batch ------------------------------------------------------------------------------------

void Migrations::DeclareBinary(const std::string& ext, const char* magic8, int current, int reimportBelow, std::function<bool(const std::string&)> resave)
{
	boost::mutex::scoped_lock l(g_mx); DeclareBuiltins();
	for (Binary& b : Binaries())
		if (b.ext == ext) { memcpy(b.magic, magic8, 8); b.current = current; b.reimportBelow = reimportBelow; if (resave) b.resave = resave; return; }
	Binary b; b.ext = ext; memcpy(b.magic, magic8, 8); b.current = current; b.reimportBelow = reimportBelow; b.resave = resave;
	Binaries().push_back(b);
}

void Migrations::DeclareJsonExt(const std::string& ext, const std::string& kind)
{
	boost::mutex::scoped_lock l(g_mx); DeclareBuiltins();
	JsonExts()[ext] = kind;
	Formats()[kind];
}

std::string Migrations::Report::Text() const
{
	std::ostringstream o;
	o << (applied ? "Upgraded" : "Would upgrade") << ": " << upgraded << " document(s), " << resaved << " binary asset(s) re-saved";
	if (reimport) o << ", " << reimport << " need a reimport";
	if (newer)    o << ", " << newer << " need a NEWER engine";
	if (failed)   o << ", " << failed << " failed";
	o << "\n";
	for (const Item& i : items) o << "  " << i.action << "  " << i.path << (i.note.empty() ? "" : "  - " + i.note) << "\n";
	if (items.empty()) o << "  everything is current\n";
	return o.str();
}

Migrations::Report Migrations::UpgradeProjectContent(const std::string& dir, bool apply)
{
	Report r; r.applied = apply;
	boost::system::error_code ec;
	std::map<std::string, std::string> exts; std::vector<Binary> bins;
	{ boost::mutex::scoped_lock l(g_mx); DeclareBuiltins(); exts = JsonExts(); bins = Binaries(); }
	const bfs::path root(dir);
	if (!bfs::is_directory(root, ec)) { r.items.push_back({ dir, "missing", "not a directory" }); ++r.failed; g_last = r; return r; }
	for (bfs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
	{
		if (ec) break;
		if (bfs::is_directory(it->path(), ec)) continue;
		const bfs::path p = it->path();
		std::string ext = p.extension().string();
		for (char& c : ext) c = (char)tolower((unsigned char)c);
		const std::string rel = bfs::relative(p, root, ec).generic_string();
		// binary
		bool isBin = false;
		for (const Binary& b : bins)
		{
			if (b.ext != ext) continue;
			isBin = true;
			bfs::ifstream f(p, std::ios::binary);
			char magic[8] = {}; uint32_t ver = 0;
			if (!f || !f.read(magic, 8) || memcmp(magic, b.magic, 8) != 0 || !f.read((char*)&ver, 4)) { r.items.push_back({ rel, "skip", "unreadable header" }); ++r.failed; break; }
			f.close();
			if ((int)ver > b.current) { r.items.push_back({ rel, "NEWER", "v" + std::to_string(ver) + " > v" + std::to_string(b.current) }); ++r.newer; break; }
			if (b.reimportBelow && (int)ver < b.reimportBelow) { r.items.push_back({ rel, "reimport", "v" + std::to_string(ver) + ": the importer changed since - drop the source in again" }); ++r.reimport; }
			if ((int)ver < b.current)
			{
				if (apply)
				{
					if (b.resave && b.resave(p.string())) { r.items.push_back({ rel, "re-saved", "v" + std::to_string(ver) + " -> v" + std::to_string(b.current) }); ++r.resaved; }
					else { r.items.push_back({ rel, "FAILED", "re-save v" + std::to_string(ver) }); ++r.failed; }
				}
				else { r.items.push_back({ rel, "re-save", "v" + std::to_string(ver) + " -> v" + std::to_string(b.current) }); ++r.resaved; }
			}
			break;
		}
		if (isBin) continue;
		auto ke = exts.find(ext);
		if (ke == exts.end()) continue;
		std::string text;
		{
			bfs::ifstream f(p, std::ios::binary);
			if (!f) continue;
			text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		}
		if (text.compare(0, 4, "NCBR") == 0) continue;   // a cooked copy: the batch edits sources only
		json doc = ParseDoc(text);
		if (doc.is_discarded() || !doc.is_object()) continue;
		std::string kind = ke->second;
		if (kind == "world" && doc.value("type", std::string()) == "WorldCells") kind = "world";   // cells share the world's steps
		std::vector<std::string> log;
		json before = doc;
		bool ok = Upgrade(kind, doc, &log);
		if (!ok) { r.items.push_back({ rel, "NEWER", log.empty() ? "" : log.back() }); ++r.newer; continue; }
		if (kind == "world" || kind == "prefab")
		{
			if (kind == "prefab") UpgradeAtomTree(doc, &log);
			else if (doc.contains("atoms")) for (json& a : doc["atoms"]) UpgradeAtomTree(a, &log);
		}
		if (doc == before) continue;
		if (kind == "world" || kind == "prefab") doc["version"] = FormatVersion(kind);   // a rewritten document carries its stamp
		std::string note;
		for (const std::string& s : log) { if (!note.empty()) note += "; "; note += s; }
		if (apply)
		{
			bfs::ofstream o(p, std::ios::binary);
			if (!o) { r.items.push_back({ rel, "FAILED", "cannot write" }); ++r.failed; continue; }
			o << doc.dump(2) << "\n";
			r.items.push_back({ rel, "upgraded", note });
		}
		else r.items.push_back({ rel, "upgrade", note });
		++r.upgraded;
	}
	g_last = r;
	std::cout << "[Migrations]\t" << r.Text();
	Log::Write(LOG_INFO, "Migrations", r.Text());
	return r;
}

const Migrations::Report& Migrations::LastReport() { return g_last; }

std::string Migrations::UpgradeContent(const std::string& dir, bool apply) { return UpgradeProjectContent(dir, apply).Text(); }
std::string Migrations::LastReportText() { return g_last.Text(); }

// ---- backups ------------------------------------------------------------------------------------------

namespace {
std::string NowStamp()
{
	char b[32] = {};
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
#ifdef _WIN32
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	std::strftime(b, sizeof(b), "%Y%m%d-%H%M", &tmv);
	return b;
}
bool CopyFileTo(const bfs::path& from, const bfs::path& to, std::string* error)
{
	boost::system::error_code ec;
	bfs::create_directories(to.parent_path(), ec);
	bfs::remove(to, ec); ec.clear();
	bfs::copy_file(from, to, ec);
	if (ec) { if (error) *error = "cannot copy " + from.string() + " -> " + to.string() + ": " + ec.message(); return false; }
	return true;
}
// The running project's roots for the reflected pair: the content root + the .nuproj beside it.
bool ProjectRoots(std::string& contentDir, std::string& projectFile, std::string* error)
{
	AppInstance* app = AppInstance::GetSingleton();
	if (!app || app->contentRoot.empty()) { if (error) *error = "no project open"; return false; }
	contentDir = app->contentRoot;
	boost::system::error_code ec;
	const bfs::path pd = bfs::path(contentDir).parent_path();
	for (bfs::directory_iterator it(pd, ec), end; it != end && !ec; it.increment(ec))
		if (it->path().extension() == ".nuproj") { projectFile = it->path().string(); break; }
	return true;
}
}  // namespace

bool Migrations::BackupProject(const std::string& contentDir, const std::string& projectFile, const std::string& backupDir, bool whole, const Report* touched, std::string* error)
{
	boost::system::error_code ec;
	const bfs::path root(contentDir), out(backupDir);
	if (!bfs::is_directory(root, ec)) { if (error) *error = "no content root: " + contentDir; return false; }
	if (bfs::exists(out / "nubackup.json", ec)) { if (error) *error = "a backup already lives in " + backupDir; return false; }
	bfs::create_directories(out / "content", ec);
	if (ec) { if (error) *error = "cannot create " + backupDir + ": " + ec.message(); return false; }
	std::vector<std::string> files;
	if (whole)
	{
		for (bfs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec))
			if (!bfs::is_directory(it->path(), ec)) files.push_back(bfs::relative(it->path(), root, ec).generic_string());
	}
	else if (touched)
	{
		for (const Report::Item& i : touched->items)
			if (i.action != "NEWER" && i.action != "FAILED" && i.action != "skip" && i.action != "missing") files.push_back(i.path);
	}
	int n = 0;
	for (const std::string& rel : files)
	{
		if (!bfs::exists(root / rel, ec)) continue;
		if (!CopyFileTo(root / rel, out / "content" / rel, error)) return false;
		++n;
	}
	std::string projName;
	if (!projectFile.empty() && bfs::exists(bfs::path(projectFile), ec))
	{
		if (!CopyFileTo(bfs::path(projectFile), out / bfs::path(projectFile).filename(), error)) return false;
		projName = bfs::path(projectFile).filename().string();
	}
	json m;
	m["engine"] = EngineVersion(); m["date"] = NowStamp(); m["project"] = projName; m["whole"] = whole; m["files"] = files;
	bfs::ofstream f(out / "nubackup.json", std::ios::binary);
	if (!f) { if (error) *error = "cannot write the backup manifest"; return false; }
	f << m.dump(2) << "\n";
	std::cout << "[Migrations]\tbacked up " << n << " file(s)" << (projName.empty() ? "" : " + " + projName) << " -> " << backupDir << std::endl;
	return true;
}

bool Migrations::ReadBackupInfo(const std::string& backupDir, BackupInfo& out)
{
	bfs::ifstream f(bfs::path(backupDir) / "nubackup.json", std::ios::binary);
	if (!f) return false;
	json m = json::parse(f, nullptr, false);
	if (m.is_discarded() || !m.is_object()) return false;
	out.dir = backupDir; out.engine = m.value("engine", std::string()); out.date = m.value("date", std::string());
	out.project = m.value("project", std::string()); out.whole = m.value("whole", false);
	out.files = m.contains("files") && m["files"].is_array() ? (int)m["files"].size() : 0;
	return true;
}

bool Migrations::RestoreProject(const std::string& backupDir, const std::string& contentDir, const std::string& projectFile, std::string* error)
{
	boost::system::error_code ec;
	const bfs::path in(backupDir), root(contentDir);
	bfs::ifstream f(in / "nubackup.json", std::ios::binary);
	if (!f) { if (error) *error = "no nubackup.json in " + backupDir; return false; }
	json m = json::parse(f, nullptr, false);
	if (m.is_discarded() || !m.contains("files") || !m["files"].is_array()) { if (error) *error = "bad backup manifest"; return false; }
	int n = 0;
	for (const json& j : m["files"])
	{
		if (!j.is_string()) continue;
		const std::string rel = j.get<std::string>();
		if (!bfs::exists(in / "content" / rel, ec)) continue;
		if (!CopyFileTo(in / "content" / rel, root / rel, error)) return false;
		++n;
	}
	const std::string proj = m.value("project", std::string());
	if (!proj.empty() && !projectFile.empty() && bfs::exists(in / proj, ec))
		if (!CopyFileTo(in / proj, bfs::path(projectFile), error)) return false;
	std::cout << "[Migrations]\trestored " << n << " file(s)" << (proj.empty() ? "" : " + " + proj) << " <- " << backupDir << std::endl;
	return true;
}

std::vector<Migrations::BackupInfo> Migrations::FindBackups(const std::string& projectDir)
{
	std::vector<BackupInfo> v;
	boost::system::error_code ec;
	const bfs::path pd = bfs::absolute(bfs::path(projectDir));
	const std::string prefix = pd.filename().string() + "-backup-";
	for (bfs::directory_iterator it(pd.parent_path(), ec), end; it != end && !ec; it.increment(ec))
	{
		if (!bfs::is_directory(it->path(), ec)) continue;
		const std::string name = it->path().filename().string();
		if (name.compare(0, prefix.size(), prefix) != 0) continue;
		BackupInfo bi;
		if (ReadBackupInfo(it->path().string(), bi)) v.push_back(bi);
	}
	std::sort(v.begin(), v.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.date > b.date; });
	return v;
}

std::string Migrations::DefaultBackupDir()
{
	std::string contentDir, projectFile;
	if (!ProjectRoots(contentDir, projectFile, nullptr)) return "";
	const bfs::path pd = bfs::absolute(bfs::path(contentDir)).parent_path();
	std::string ver = EngineVersion();
	for (char& c : ver) if (c == ' ' || c == '/') c = '_';
	return (pd.parent_path() / (pd.filename().string() + "-backup-" + ver + "-" + NowStamp())).string();
}

std::string Migrations::Backup(const std::string& backupDir, bool wholeProject)
{
	std::string contentDir, projectFile, err;
	if (!ProjectRoots(contentDir, projectFile, &err)) return err;
	const std::string dir = backupDir.empty() ? DefaultBackupDir() : backupDir;
	if (!BackupProject(contentDir, projectFile, dir, wholeProject, &g_last, &err)) return err;
	return "";
}

std::string Migrations::Restore(const std::string& backupDir)
{
	std::string contentDir, projectFile, err;
	if (!ProjectRoots(contentDir, projectFile, &err)) return err;
	if (!RestoreProject(backupDir, contentDir, projectFile, &err)) return err;
	return "";
}

}  // namespace nuke
