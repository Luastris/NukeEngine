#include "interface/Modular.h"
#include "reflect/Reflect.h"
#include "API/Model/World.h"
#include <map>
#include <set>

namespace nuke {

// Single instance, owned by the engine DLL.
static bc::vector<std::shared_ptr<NUKEModule>> g_modules;
static AppInstance* g_instance = nullptr;   // host, captured at discovery (for EnablePlugin)
static std::map<std::string, std::string> g_typePlugin;   // component type -> owning dll name

bc::vector<std::shared_ptr<NUKEModule>>& GetModules() { return g_modules; }

// Which plugin (dll name) provides a component type, or "" for engine built-ins. Learned by
// diffing the reflection registry around each plugin's OnLoad().
const char* PluginForType(const std::string& type)
{
	auto it = g_typePlugin.find(type);
	return (it != g_typePlugin.end()) ? it->second.c_str() : "";
}

static bool IsPluginLoaded(const std::string& dll)
{
	for (auto& m : g_modules)
		if (m && m->moduleFile == dll) return m->loaded;
	return false;
}

// A type is "active" (its components are live, not inert placeholders) when it's a built-in
// or when its providing plugin is currently loaded.
bool IsTypeActive(const std::string& type)
{
	auto it = g_typePlugin.find(type);
	if (it == g_typePlugin.end()) return true;   // built-in / not plugin-owned
	return IsPluginLoaded(it->second);
}

// Discovery only: import every plugin DLL (so the manager has its metadata) into the shared
// pool, but do NOT activate any. The caller activates the project's chosen plugins.
void InitModules(AppInstance* instance)
{
	g_instance = instance;

	if (!bfs::exists(bfs::path(bfs::current_path().concat("/modules"))))
	{
		bfs::create_directory(bfs::current_path().concat("/modules"));
		cout << "directory created!" << endl;
	}

	for (auto& p : boost::filesystem::directory_iterator(bfs::current_path().concat("/modules")))
	{
		if (bfs::is_directory(p.path()))
			continue;
		auto ext = p.path().extension().string();
		if (ext != ".dll" && ext != ".so")
			continue;
		try
		{
			// Extension plugins export an unmangled "plugin" symbol; render modules export
			// "renderModule" instead — skip those (and any other non-plugin DLL).
			boost::dll::shared_library lib(p.path().string());
			if (!lib.has("plugin"))
				continue;

			auto plugin = boost::dll::import_symbol<NUKEModule>(p.path().string(), "plugin");
			plugin->modulePath = p.path().generic_string();
			plugin->moduleFile = p.path().filename().string();
			plugin->loaded     = false;
			g_modules.push_back(plugin);
			cout << "[Modular]\tdiscovered plugin '" << plugin->title << "' from "
			     << plugin->moduleFile << endl;
		}
		catch (const std::exception& e)
		{
			cout << "[Modular]\tfailed to load " << p.path().filename().string()
			     << ": " << e.what() << endl;
		}
	}
}

void EnablePlugin(NUKEModule* m)
{
	if (!m || m->loaded) return;
	m->loaded  = true;
	m->stopped = false;

	// Diff the registry around OnLoad() to learn which component types this plugin provides.
	std::set<std::string> before;
	for (TypeInfo* t : Registry_All()) before.insert(t->name);
	m->OnLoad();   // synchronous: registers the plugin's component types
	for (TypeInfo* t : Registry_All())
		if (!before.count(t->name)) g_typePlugin[t->name] = m->moduleFile;

	// Live upgrade: turn any inert placeholders of this plugin's types back into real
	// components now that the type is available again.
	if (g_instance && g_instance->currentScene)
		g_instance->currentScene->RestorePluginComponents(m->moduleFile);

	cout << "[Modular]\tenabled '" << m->title << "'" << endl;
	boost::thread(boost::bind(&NUKEModule::Run, m, g_instance));
}

void DisablePlugin(NUKEModule* m)
{
	if (!m || !m->loaded) return;

	// Live downgrade FIRST (while the type's reflection + vtable are still valid): convert this
	// plugin's live components into inert placeholders so nothing dangles after it goes away.
	if (g_instance && g_instance->currentScene)
		g_instance->currentScene->ConvertPluginToUnknown(m->moduleFile);

	m->Shutdown();   // tears down windows/menus etc.; sets stopped = true
	m->loaded = false;
	cout << "[Modular]\tdisabled '" << m->title << "'" << endl;
}

void UnloadModules()
{
	for (auto i : g_modules)
	{
		if (i && i->loaded)
			i->Shutdown();
	}
	g_modules.clear();
}
}  // namespace nuke