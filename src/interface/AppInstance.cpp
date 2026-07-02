// Header-only boost.chrono, defined BEFORE any boost include (same mode as Time.cpp /
// Clock.cpp — the lib flavor double-defines steady_clock::now inside the engine DLL).
#define BOOST_CHRONO_HEADER_ONLY
#include "interface/AppInstance.h"
#include "API/Model/World.h"
#include <boost/chrono.hpp>
#include <boost/filesystem.hpp>
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

bool AppInstance::OpenWorld(const std::string& relPath)
{
	if (relPath.empty() || !currentScene) return false;
	boost::system::error_code ec;
	std::string full = WorldFullPath(relPath);
	if (!boost::filesystem::exists(boost::filesystem::path(full), ec))
		full = ResolveContent(relPath);   // legacy fallback (e.g. a world next to the exe)
	if (!boost::filesystem::exists(boost::filesystem::path(full), ec)) return false;
	selectedInHieararchy = nullptr;
	currentScene->LoadFromFile(full);
	currentWorldPath = relPath;
	return true;
}

bool AppInstance::SaveWorld(const std::string& relPath)
{
	if (relPath.empty() || !currentScene) return false;
	std::string full = WorldFullPath(relPath);   // forced into content (no cwd fallback on save)
	boost::system::error_code ec;
	boost::filesystem::path p(full);
	if (p.has_parent_path()) boost::filesystem::create_directories(p.parent_path(), ec);
	currentScene->SaveToFile(full);
	currentWorldPath = relPath;
	return true;
}

void AppInstance::NewWorld()
{
	if (currentScene) currentScene->Clear();   // empties the world but keeps the editor camera
	currentWorldPath.clear();
	selectedInHieararchy = nullptr;
}

AppInstance::AppInstance()
{
	//currentScene = new World();
	keyboard = KeyBoard::getSingleton();
	mouse = Mouse::getSingleton();
	//render = iRender::getSingleton();
	config = Config::getSingleton();
	
	if (!menuStrip)
		menuStrip = new MenuStrip();
	if (!editorWindows)
		editorWindows = new bc::map<string, bst::function<void()>>();
	cout << "[EditorInstance]\t" << "Current scene is: " << currentScene << "(" << currentScene->name << ")" << ", Hierarchy is: " << &currentScene->GetHierarchy() << endl;
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
			currentScene->Update();
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
		World* w = currentScene;
		const double dt = (w && w->settings.fixedDt > 0.0001f) ? w->settings.fixedDt : 1.0 / 60.0;
		if (w && playState == 1)
		{
			try { w->FixedUpdate(); }
			catch (const std::exception& e)
			{
				cout << "[FixedThread]\terror in FixedUpdate: " << e.what() << endl;
			}
		}
		next += bch::nanoseconds((long long)(dt * 1e9));
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