// Header-only boost.chrono, defined BEFORE any boost include (same mode as Time.cpp /
// Clock.cpp — the lib flavor double-defines steady_clock::now inside the engine DLL).
#define BOOST_CHRONO_HEADER_ONLY
#include "interface/AppInstance.h"
#include "API/Model/World.h"
#include "API/Model/Time.h"      // fixed-thread cadence scales with Game.SetTimeScale
#include "API/Model/Package.h"   // packed-content resolve (3.2)
#include "API/Model/Jobs.h"      // async world load runs on the engine pool
#include "API/Model/Events.h"    // incremental activation emits world.atomActivated events
#include "reflect/Reflect.h"     // per-slice AtomRef resolve while the world grows
#include <nlohmann/json.hpp>     // background parse of the staged world document
#include <algorithm>             // stable_sort (activation origin ordering)
#include <map>                   // mod-name -> layer index (world-merge baselines)
#include <boost/chrono.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#ifdef _WIN32
#include <Windows.h>   // SetThreadAffinityMask / SetThreadPriority (fixed-thread core pinning)
#endif

namespace nuke {

std::string AppInstance::ResolveContent(const std::string& path) const
{
	if (path.empty()) return path;
	boost::filesystem::path p(path);
	if (p.is_absolute()) return path;
	boost::system::error_code ec;
	if (!contentRoot.empty())
	{
		boost::filesystem::path cand = boost::filesystem::path(contentRoot) / p;
		if (boost::filesystem::exists(cand, ec)) return cand.string();   // prefer the project
	}
	if (boost::filesystem::exists(p, ec)) return path;                   // cwd/root fallback
	if (!contentRoot.empty()) return (boost::filesystem::path(contentRoot) / p).string();
	return path;
}

// The canonical on-disk path for a world: ALWAYS under the project content root (worlds live in
// the project content, never "wherever"). Absolute paths pass through unchanged.
std::string AppInstance::WorldFullPath(const std::string& relPath) const
{
	boost::filesystem::path rp(relPath);
	if (rp.is_absolute() || contentRoot.empty()) return relPath;
	return (boost::filesystem::path(contentRoot) / rp).string();
}

// Read a content-relative file through every layer: the raw project/overlay from disk,
// mounted paks from MEMORY (packed content is bytes-only — it never touches the disk).
bool AppInstance::ReadContent(const std::string& relPath, std::string& out) const
{
	std::string full = ResolveContent(relPath);
	boost::system::error_code ec;
	if (!full.empty() && boost::filesystem::exists(boost::filesystem::path(full), ec))
	{
		boost::filesystem::ifstream f(boost::filesystem::path(full), std::ios::binary);
		if (f) { out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); return true; }
	}
	if (Package::MountedCount() > 0)
		return Package::Read("content/" + boost::filesystem::path(relPath).generic_string(), out);
	return false;
}

// A world's FINAL data string, resolved through every source: the raw content file from
// disk, or the mounted pak layers MERGED (base game + mods + overlay). THREAD-SAFE —
// filesystem reads + mutex-guarded Package reads, no engine-object mutation — so both the
// synchronous OpenWorld and the async loader's background job run through here.
bool AppInstance::ComposeWorldData(const std::string& relPath, std::string& out)
{
	boost::system::error_code ec;
	std::string full = WorldFullPath(relPath);
	if (!boost::filesystem::exists(boost::filesystem::path(full), ec))
		full = ResolveContent(relPath);   // legacy fallback (e.g. a world next to the exe)
	if (boost::filesystem::exists(boost::filesystem::path(full), ec))
	{
		boost::filesystem::ifstream f(boost::filesystem::path(full), std::ios::binary);
		if (!f) return false;
		out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		return true;
	}
	// Packed runtime (3.2): the world lives in a mounted pak — load it from bytes.
	// SEVERAL layers may carry this world (base game + mods editing it): they MERGE
	// semantically (World::MergeWorldLayers) instead of the top file winning outright.
	// Each layer diffs against ITS OWN baseline: an ordinary mod against the base game,
	// a patch-mod against base + the mods it requires (mod.json), the modder's raw
	// overlay against everything mounted below it (the session it was authored in).
	std::vector<std::pair<std::string, std::string>> hits;   // (data, source pak; "" = raw)
	if (Package::MountedCount() > 0 && Package::ReadAllInfo("content/" + relPath, hits) > 0)
	{
		std::vector<std::string> layers;
		std::vector<std::vector<int>> deps(hits.size());
		std::vector<std::string> basis(hits.size());   // per-layer recorded baseline ("" = none)
		std::vector<std::string> names(hits.size());   // provenance: the mod each layer is
		// Which layer index a mod NAME resolves to (only mods carrying THIS world count).
		std::map<std::string, int> nameToLayer;
		auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
		for (size_t i = 0; i < hits.size(); ++i)
		{
			layers.push_back(hits[i].first);
			if (hits[i].second.empty())
			{
				// The raw overlay: authored on top of the FULL mounted stack.
				for (size_t j = 1; j < i; ++j) deps[i].push_back((int)j);
				continue;
			}
			// The layer's own recorded BASELINE (Package Mod stores the world exactly as
			// the mod's author saw it under "basis/<rel>"): the diff applies the author's
			// ACTUAL point changes — a stale mod can't wipe what the base gained since.
			if (i > 0)
			{
				Package::File pf;
				std::string b;
				if (pf.Open(hits[i].second) && pf.Read("basis/content/" + relPath, b)) basis[i] = b;
			}
			for (const Package::ModInfo& mi : Package::Mods())
				if (mi.pakPath == hits[i].second)
				{
					names[i] = mi.name;
					nameToLayer[lower(mi.name)] = (int)i;
					for (const std::string& r : mi.requires_)
					{
						auto it = nameToLayer.find(lower(r));   // deps mount below -> already seen
						if (it != nameToLayer.end()) deps[i].push_back(it->second);
					}
					break;
				}
		}
		out = layers.size() > 1 ? World::MergeWorldLayers(layers, deps, basis, names) : layers[0];
		return true;
	}
	return false;
}

bool AppInstance::OpenWorld(const std::string& relPath)
{
	if (relPath.empty() || !currentWorld) return false;
	// Mid-tick call (a script's Game.LoadWorld inside Update/FixedUpdate): loading NOW
	// would replace the hierarchy the tick is iterating. Queue it — World::Update applies
	// it at the frame boundary. `true` = accepted (a missing world logs on apply).
	if (worldTickActive)
	{
		pendingWorldLoad = relPath;
		return true;
	}
	FlushWorldActivation();   // a still-growing world completes before a synchronous load replaces it
	std::string data;
	if (!ComposeWorldData(relPath, data)) return false;
	selectedInHieararchy = nullptr;
	currentWorld->LoadFromString(data);
	currentWorldPath = relPath;
	NameWorldFromPath(relPath);
	std::cout << "[World]\t\t\t" << "Loaded " << relPath << std::endl;
	return true;
}

// --- async world load (Game.LoadWorldAsync; task #147) --------------------------------
// The background job reads + merges + PARSES (the heavy part for big worlds). The staged
// document then waits for the script's ActivateLoadedWorld — applied by World::Update at
// the frame boundary via ApplyAsyncWorldLoad. A new Start supersedes a pending one (the
// generation counter makes the stale job drop its result silently).

bool AppInstance::StartWorldLoadAsync(const std::string& relPath)
{
	if (relPath.empty() || !currentWorld) return false;
	const unsigned my = ++asyncLoadGen;
	{
		boost::mutex::scoped_lock l(asyncLoadLock);
		asyncLoadPath = relPath;
		asyncLoadDoc.reset();
	}
	asyncLoadActivate = false;
	asyncLoadState = 1;
	asyncLoadProgress = 0.05f;
	std::cout << "[World]\t\t\t" << "async load started: '" << relPath << "'" << std::endl;
	Jobs::Schedule([this, relPath, my]()
	{
		std::string data;
		const bool ok = ComposeWorldData(relPath, data);
		if (asyncLoadGen != my) return;   // superseded/cancelled — drop silently
		if (!ok)
		{
			asyncLoadState = 3;
			std::cout << "[World]\t\t\t" << "async load: world '" << relPath << "' not found" << std::endl;
			return;
		}
		asyncLoadProgress = 0.35f;
		auto doc = std::make_shared<nlohmann::json>(nlohmann::json::parse(data, nullptr, false));
		if (asyncLoadGen != my) return;
		if (doc->is_discarded())
		{
			asyncLoadState = 3;
			std::cout << "[World]\t\t\t" << "async load: '" << relPath << "' is not valid JSON" << std::endl;
			return;
		}
		asyncLoadProgress = 0.95f;
		{
			boost::mutex::scoped_lock l(asyncLoadLock);
			if (asyncLoadGen != my) return;
			asyncLoadDoc = doc;
		}
		asyncLoadState = 2;
		asyncLoadProgress = 1.0f;
		std::cout << "[World]\t\t\t" << "async load staged: '" << relPath << "' (ActivateLoadedWorld to switch)" << std::endl;
	});
	return true;
}

double AppInstance::WorldLoadProgress()
{
	const int s = asyncLoadState;
	if (s == 0 || s == 3) return -1.0;
	return (double)asyncLoadProgress.load();
}

bool AppInstance::WorldLoadReady() { return asyncLoadState == 2; }

bool AppInstance::ActivateLoadedWorld()
{
	if (asyncLoadState != 2) return false;
	asyncLoadActivate = true;   // World::Update performs the swap at the frame boundary
	return true;
}

void AppInstance::CancelWorldLoadAsync()
{
	++asyncLoadGen;   // any in-flight job drops its result
	{
		boost::mutex::scoped_lock l(asyncLoadLock);
		asyncLoadDoc.reset();
		asyncLoadPath.clear();
	}
	asyncLoadActivate = false;
	asyncLoadState = 0;
	asyncLoadProgress = 0.f;
	// A GROWING world is dropped as-is (partial) — the only cancel site outside gameplay is
	// PIE stop, which restores the edit scene wholesale right after.
	activationActive = false;
	activationQueue.clear();
	activationDoc.reset();
}

void AppInstance::ApplyAsyncWorldLoad()
{
	if (!asyncLoadActivate || asyncLoadState != 2 || !currentWorld) return;
	std::shared_ptr<nlohmann::json> doc;
	std::string path;
	{
		boost::mutex::scoped_lock l(asyncLoadLock);
		doc.swap(asyncLoadDoc);
		path.swap(asyncLoadPath);
	}
	asyncLoadActivate = false;
	asyncLoadState = 0;
	asyncLoadProgress = 0.f;
	++asyncLoadGen;
	if (!doc) return;
	// A new world superseding one still GROWING: the header teardown below wipes its atoms
	// anyway — just drop the leftover queue so it can't keep instantiating into the new world.
	activationActive = false;
	activationQueue.clear();
	activationDoc.reset();

	selectedInHieararchy = nullptr;
	if (activationBudgetMs <= 0.f)
	{
		currentWorld->LoadFromJson(*doc);   // pre-parsed: the game thread only instantiates
		currentWorldPath = path;
		NameWorldFromPath(path);
		std::cout << "[World]\t\t\t" << "async world activated: '" << path << "'" << std::endl;
		return;
	}
	// INCREMENTAL activation: swap to the world header now (old world torn down, calendar/
	// settings applied), then let the world GROW — root atoms instantiate over the next
	// frames within the ms budget, optionally ordered outward from the activation origin.
	currentWorld->LoadHeaderFromJson(*doc);
	currentWorldPath = path;
	NameWorldFromPath(path);
	activationDoc = doc;
	activationPath = path;
	activationQueue.clear();
	if (doc->contains("atoms"))
		for (const nlohmann::json& gj : (*doc)["atoms"])
			activationQueue.push_back(&gj);
	if (activationOriginSet)
	{
		const float ox = activationOrigin[0], oy = activationOrigin[1], oz = activationOrigin[2];
		auto dist2 = [&](const nlohmann::json* aj) -> double
		{
			if (!aj->contains("transform")) return 1e30;
			const nlohmann::json& tr = (*aj)["transform"];
			if (!tr.contains("position") || !tr["position"].is_array() || tr["position"].size() < 3) return 1e30;
			const double dx = tr["position"][0].get<double>() - ox;
			const double dy = tr["position"][1].get<double>() - oy;
			const double dz = tr["position"][2].get<double>() - oz;
			return dx * dx + dy * dy + dz * dz;
		};
		std::stable_sort(activationQueue.begin(), activationQueue.end(),
		                 [&](const nlohmann::json* a, const nlohmann::json* b) { return dist2(a) < dist2(b); });
	}
	activationTotal = (int)activationQueue.size();
	activationDone = 0;
	activationActive = true;
	std::cout << "[World]\t\t\t" << "async world activating incrementally: '" << path << "' ("
	          << activationTotal << " root atoms, " << activationBudgetMs << " ms/frame)" << std::endl;
	ContinueWorldActivation();   // first slice THIS frame — the origin-nearest atoms pop in at once
}

// --- incremental activation (the "world grows around the player" pattern) --------------

void   AppInstance::SetWorldActivationBudget(double ms) { activationBudgetMs = ms < 0.0 ? 0.f : (float)ms; }
double AppInstance::GetWorldActivationBudget()          { return activationBudgetMs; }

void AppInstance::SetWorldActivationOrigin(float x, float y, float z)
{
	activationOrigin[0] = x; activationOrigin[1] = y; activationOrigin[2] = z;
	activationOriginSet = true;
}

void AppInstance::ClearWorldActivationOrigin() { activationOriginSet = false; }

double AppInstance::WorldActivationProgress()
{
	if (!activationActive) return -1.0;
	return activationTotal > 0 ? (double)activationDone / (double)activationTotal : 1.0;
}

void AppInstance::ContinueWorldActivation(bool ignoreBudget)
{
	if (!activationActive || !currentWorld) return;
	const auto t0 = boost::chrono::steady_clock::now();
	while (activationDone < activationTotal)
	{
		const nlohmann::json* aj = activationQueue[activationDone];
		Atom* a = currentWorld->AddAtomFromJson(*aj);
		++activationDone;
		if (a)
		{
			// The GAME drives the appearance effect (wireframe fade / particles / goo) —
			// the engine only announces the atom. Payload keys: id (stable), name.
			nlohmann::json p{ { "id", (long long)a->id.id }, { "name", a->GetName() } };
			Events::Emit("world.atomActivated", p.dump());
		}
		if (!ignoreBudget && activationDone < activationTotal)
		{
			const double ms = boost::chrono::duration_cast<boost::chrono::duration<double, boost::milli>>(
				boost::chrono::steady_clock::now() - t0).count();
			if (ms >= activationBudgetMs) break;
		}
	}
	// Refs to atoms that exist RESOLVE progressively; the rest hook up as their targets appear.
	Reflect_ResolveAtomRefs();
	if (activationDone >= activationTotal)
	{
		activationActive = false;
		activationQueue.clear();
		activationDoc.reset();
		currentWorld->FinalizeIncrementalLoad();
		nlohmann::json p{ { "path", activationPath } };
		Events::Emit("world.activationComplete", p.dump());
		std::cout << "[World]\t\t\t" << "async world activated: '" << activationPath << "' (incremental)" << std::endl;
	}
}

void AppInstance::FlushWorldActivation() { ContinueWorldActivation(true); }

// A world with no authored name (older files / never named) reads its name from the file
// stem, so Game.GetWorld().Name is meaningful ("MusicVisTest") instead of the constructor
// default. An explicitly named world (LoadFromString set it) is left untouched.
void AppInstance::NameWorldFromPath(const std::string& relPath)
{
	if (currentWorld && currentWorld->name.empty())
		currentWorld->name = boost::filesystem::path(relPath).stem().string();
}

bool AppInstance::SaveWorld(const std::string& relPath)
{
	if (relPath.empty() || !currentWorld) return false;
	std::string full = WorldFullPath(relPath);   // forced into content (no cwd fallback on save)
	boost::system::error_code ec;
	boost::filesystem::path p(full);
	if (p.has_parent_path()) boost::filesystem::create_directories(p.parent_path(), ec);
	currentWorld->SaveToFile(full);
	currentWorldPath = relPath;
	return true;
}

void AppInstance::NewWorld()
{
	if (currentWorld) currentWorld->Clear();   // empties the world but keeps the editor camera
	currentWorldPath.clear();
	selectedInHieararchy = nullptr;
}

AppInstance::AppInstance()
{
	//currentWorld = new World();
	keyboard = KeyBoard::getSingleton();
	mouse = Mouse::getSingleton();
	//render = iRender::getSingleton();
	config = Config::getSingleton();
	
	if (!menuStrip)
		menuStrip = new MenuStrip();
	if (!editorWindows)
		editorWindows = new bc::map<string, bst::function<void()>>();
	cout << "[EditorInstance]\t" << "Current scene is: " << currentWorld << "(" << currentWorld->name << ")" << ", Hierarchy is: " << &currentWorld->GetHierarchy() << endl;
}
AppInstance::~AppInstance() {}

bool* AppInstance::WindowOpen(const char* key)
{
	auto it = windowOpen.find(key);
	if (it == windowOpen.end())
		it = windowOpen.emplace(key, true).first;   // default: open
	return &it->second;
}

bool AppInstance::isEditor() {
	return _isEditor;
}

void AppInstance::setEditor(bool editor) {
	_isEditor = editor;
}

void AppInstance::UpdateThread()
{
	while (true)
	{
		try
		{
			currentWorld->Update();
			boost::this_thread::sleep(boost::posix_time::milliseconds(40));
		}
		catch (const std::exception&)
		{

		}
	}

}

void AppInstance::StartUpdateThread()
{
	boost::thread updt(boost::bind(&AppInstance::UpdateThread, this));
}

// Fixed-frequency loop: ticks World::FixedUpdate at the world's fixedDt using an absolute
// deadline (sleep_until), so the cadence never depends on frames or on how long a step
// took — a fast render loop doesn't speed it up, a slow one doesn't starve it.
void AppInstance::FixedThread()
{
	namespace bch = boost::chrono;

#ifdef _WIN32
	// Pin the simulation to its own core (config "physicsCore": -1 = auto -> the LAST
	// core, keeping it off core 0 where the OS scheduler and the main thread live;
	// -2 = don't pin) and bump priority so a busy render loop can't starve the cadence.
	{
		int core = config ? config->physicsCore : -1;
		if (core == -1)
			core = (int)boost::thread::hardware_concurrency() - 1;
		if (core >= 0 && core < 64)
			SetThreadAffinityMask(GetCurrentThread(), 1ull << core);
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		if (core >= 0)
			cout << "[AppInstance]\tfixed-update thread pinned to core " << core << endl;
	}
#endif

	bch::steady_clock::time_point next = bch::steady_clock::now();
	while (fixedThreadRun)
	{
		World* w = currentWorld;
		const double dt = (w && w->settings.fixedDt > 0.0001f) ? w->settings.fixedDt : 1.0 / 60.0;
		// Game speed (Game.SetTimeScale): one FixedUpdate is always ONE fixedDt of GAME time,
		// so at scale s the cadence runs s× faster in real time (2x = twice the steps per
		// real second — the sim fast-forwards without changing the step size). Scale 0 =
		// frozen: no steps at all, idle at the base cadence waiting for unpause.
		const double s = Time::getSingleton()->scale;
		const bool   frozen = s <= 0.0001;
		if (w && playState == 1 && !frozen)
		{
			const bch::steady_clock::time_point t0 = bch::steady_clock::now();
			try { w->FixedUpdate(); }
			catch (const std::exception& e)
			{
				cout << "[FixedThread]\terror in FixedUpdate: " << e.what() << endl;
			}
			const double stepMs = bch::duration_cast<bch::duration<double, boost::milli>>(bch::steady_clock::now() - t0).count();
			if (stepMs > 50.0) cout << "[FixedThread]	SLOW step: " << stepMs << " ms" << endl;
		}
		const double realDt = (playState == 1 && !frozen) ? dt / s : dt;
		next += bch::nanoseconds((long long)((realDt < 0.001 ? 0.001 : realDt) * 1e9));
		const bch::steady_clock::time_point now = bch::steady_clock::now();
		if (next < now) next = now;   // fell behind (hitch/debugger): resume cadence, no burst catch-up
		boost::this_thread::sleep_until(next);
	}
	cout << "[AppInstance]\tfixed-update thread stopped" << endl;
}

void AppInstance::StartFixedThread()
{
	if (fixedThreadRun) return;
	fixedThreadRun = true;
	boost::thread fxt(boost::bind(&AppInstance::FixedThread, this));
	cout << "[AppInstance]\tfixed-update thread started" << endl;
}

void AppInstance::StopFixedThread()
{
	fixedThreadRun = false;
}

void AppInstance::PushWindow(const char* key, boost::function<void()> fWindow) {
	for (auto tup : *editorWindows) {
		if (tup.first.compare(key) == 0)
			throw std::runtime_error("Window key must be unique!");
	}
	cout << "[EditorInstance]\t" << "Pushing window \"" << key << "\"" << endl;
	editorWindows->insert(make_pair(string(key), fWindow));
}
void AppInstance::PopWindow(string key) {
	editorWindows->erase(key);
}

}  // namespace nuke