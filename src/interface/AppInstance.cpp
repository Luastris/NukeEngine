// Must precede any boost include: the lib flavor double-defines steady_clock::now in the engine DLL.
#define BOOST_CHRONO_HEADER_ONLY
#include "interface/AppInstance.h"
#include "interface/AssetCreators.h"
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
#elif defined(__linux__)
#include <pthread.h>   // pthread_setaffinity_np (fixed-thread core pinning)
#include <sched.h>     // cpu_set_t
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

std::string AppInstance::WorldFullPath(const std::string& relPath) const
{
	boost::filesystem::path rp(relPath);
	if (rp.is_absolute() || contentRoot.empty()) return relPath;
	return (boost::filesystem::path(contentRoot) / rp).string();
}

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
	// The world lives in a mounted pak. SEVERAL layers may carry it, so they MERGE semantically
	// (World::MergeWorldLayers) instead of the top file winning, each against ITS OWN baseline.
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
			// The layer's own recorded baseline ("basis/<rel>"): diffing against it applies the
			// author's ACTUAL point changes, so a stale mod can't wipe what the base gained since.
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
	// Mid-tick: loading now would replace the hierarchy the tick is iterating, so queue it
	// for the frame boundary. `true` = accepted (a missing world logs on apply).
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

// --- async world load ------------------------------------------------------------------
// The background job reads + merges + parses; the staged document is applied at the frame
// boundary by ApplyAsyncWorldLoad. A new Start supersedes a pending one (generation counter).

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
		if (asyncLoadGen != my || Jobs::Stopping()) return;   // superseded, cancelled or exiting
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
	// A still-growing world is dropped as-is, partial.
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
	// Drop any leftover growth queue so it can't keep instantiating into the new world.
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
	// INCREMENTAL activation: swap to the world header now, then let the world GROW — root
	// atoms instantiate over the next frames within the ms budget, ordered from the origin.
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
	ContinueWorldActivation();   // first slice runs THIS frame
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
			// The engine only announces the atom; the game drives any appearance effect.
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
	
	RegisterBuiltinFileIcons();   // the engine declares the icons of its own file types
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

// Fixed-frequency loop: ticks World::FixedUpdate at the world's fixedDt off an ABSOLUTE
// deadline, so the cadence depends on neither the frame rate nor how long a step took.
void AppInstance::FixedThread()
{
	namespace bch = boost::chrono;

#ifdef _WIN32
	// Pin the sim to its own core (config physicsCore: -1 = auto/last core, -2 = don't pin)
	// and raise priority so a busy render loop can't starve the cadence.
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
#elif defined(__linux__)
	// Same contract with POSIX plumbing. Priority: plain nice() — SCHED_FIFO/RR needs
	// CAP_SYS_NICE and games shouldn't ask for privileges.
	{
		int core = config ? config->physicsCore : -1;
		if (core == -1)
			core = (int)boost::thread::hardware_concurrency() - 1;
		if (core >= 0 && core < CPU_SETSIZE)
		{
			cpu_set_t set;
			CPU_ZERO(&set);
			CPU_SET(core, &set);
			if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0)
				cout << "[AppInstance]\tfixed-update thread pinned to core " << core << endl;
		}
	}
#endif

	bch::steady_clock::time_point next = bch::steady_clock::now();
	while (fixedThreadRun)
	{
		World* w = currentWorld;
		const double dt = (w && w->settings.fixedDt > 0.0001f) ? w->settings.fixedDt : 1.0 / 60.0;
		// One FixedUpdate is always ONE fixedDt of GAME time, so at time scale s the cadence
		// runs s× faster in real time. Scale 0 = frozen: no steps, idle at the base cadence.
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