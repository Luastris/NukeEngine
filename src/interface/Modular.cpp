#include "interface/Modular.h"
#include "interface/Services.h"
#include "reflect/Reflect.h"
#include "API/Model/World.h"
#include "API/Model/Package.h"   // packed built-in shaders (3.2)
#include <boost/filesystem/fstream.hpp>
#include <iterator>
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

// Import ONE plugin DLL into the pool. Skips non-plugins (no "plugin" export) and file
// names already discovered (the host's own modules/ wins over a same-named project module).
NUKEModule* DiscoverModuleFile(const std::string& absPath)
{
	bfs::path p(absPath);
	auto ext = p.extension().string();
	if (ext != ".dll" && ext != ".so") return nullptr;
	const std::string file = p.filename().string();
	for (auto& m : g_modules)
		if (m && m->moduleFile == file) return nullptr;   // already in the pool
	try
	{
		// Extension plugins export an unmangled "plugin" symbol — skip any other DLL
		// (module dependencies like glfw3.dll live in the same directory).
		boost::dll::shared_library lib(p.string());
		if (!lib.has("plugin"))
			return nullptr;

		auto plugin = boost::dll::import_symbol<NUKEModule>(p.string(), "plugin");
		plugin->modulePath = p.generic_string();
		plugin->moduleFile = file;
		plugin->loaded     = false;
		g_modules.push_back(plugin);
		cout << "[Modular]\tdiscovered plugin '" << plugin->title << "' from "
		     << plugin->moduleFile << endl;
		return plugin.get();
	}
	catch (const std::exception& e)
	{
		cout << "[Modular]\tfailed to load " << file << ": " << e.what() << endl;
		return nullptr;
	}
}

void DiscoverModulesIn(const std::string& dir)
{
	boost::system::error_code ec;
	if (dir.empty() || !bfs::exists(bfs::path(dir), ec)) return;
	for (bfs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
	{
		if (bfs::is_directory(it->path()))
			continue;
		DiscoverModuleFile(it->path().string());
	}
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

	DiscoverModulesIn(bfs::path(bfs::current_path().concat("/modules")).string());
}

// Phase 6.0: unload a module's DLL so a rebuild can overwrite the file. The pool's
// shared_ptr is the lock — dropping it frees the library (boost::dll). PHASE_BOOT
// providers (the renderer) can never be torn down mid-run.
bool UnloadModuleDll(const std::string& moduleFile)
{
	for (size_t i = 0; i < g_modules.size(); ++i)
	{
		auto& m = g_modules[i];
		if (!m || m->moduleFile != moduleFile) continue;
		if (m->phase() == PHASE_BOOT)
		{
			cout << "[Modular]\t'" << moduleFile << "' is a boot-phase provider — restart to swap it" << endl;
			return false;
		}
		if (m->loaded) DisablePlugin(m.get());   // live components -> UnknownComponent placeholders
		const long uses = m.use_count();
		if (uses > 1)
			cout << "[Modular]\tWARNING: '" << moduleFile << "' has " << (uses - 1)
			     << " extra reference(s) — the DLL stays locked until they drop" << endl;
		g_modules.erase(g_modules.begin() + i);
		cout << "[Modular]\tunloaded '" << moduleFile << "' (file unlocked for rebuild)" << endl;
		return true;
	}
	return false;
}

NUKEModule* ActiveServiceProvider(const char* service)
{
	if (!service || !*service) return nullptr;
	for (auto& m : g_modules)
		if (m && m->loaded && std::string(m->provides()) == service) return m.get();
	return nullptr;
}

NUKEModule* FindServiceProvider(const char* service, const std::string& preferredFile)
{
	if (!service || !*service) return nullptr;
	NUKEModule* first = nullptr;
	for (auto& m : g_modules)
	{
		if (!m || std::string(m->provides()) != service) continue;
		if (m->moduleFile == preferredFile) return m.get();
		if (!first) first = m.get();
	}
	if (first && !preferredFile.empty())
		cout << "[Modular]\t'" << service << "' provider '" << preferredFile
		     << "' not found, falling back to '" << first->moduleFile << "'" << endl;
	return first;
}

void EnablePlugin(NUKEModule* m)
{
	if (!m || m->loaded) return;

	// One active provider per EXCLUSIVE service. A PHASE_BOOT provider (the renderer: it
	// owns the window/device) cannot be swapped live in either direction — the UI persists
	// the new choice, which takes effect on the next start. SHARED services (scripting)
	// skip the displacement entirely: Lua and C# providers load side by side.
	const std::string service = m->provides();
	if (!service.empty() && !m->sharedService())
	{
		if (NUKEModule* cur = ActiveServiceProvider(service.c_str()))
		{
			if (m->phase() == PHASE_BOOT || cur->phase() == PHASE_BOOT)
			{
				cout << "[Modular]\t'" << m->title << "' provides '" << service
				     << "' (boot phase) — current provider '" << cur->title
				     << "' stays; the change applies after restart" << endl;
				return;
			}
			DisablePlugin(cur);
		}
	}

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
	if (g_instance && g_instance->currentWorld)
		g_instance->currentWorld->RestorePluginComponents(m->moduleFile);

	// Service providers register their interface instance under the service name. Loader-
	// bound (not done by the plugin itself) so provide/revoke can never get out of sync
	// with the plugin lifecycle.
	if (!service.empty())
		if (void* iface = m->queryService())
			Services_Provide(service.c_str(), iface);

	cout << "[Modular]\tenabled '" << m->title << "'" << endl;
	boost::thread(boost::bind(&NUKEModule::Run, m, g_instance));
}

void DisablePlugin(NUKEModule* m)
{
	if (!m || !m->loaded) return;

	// Revoke THIS module's interface FIRST so no consumer can grab it while it's dying —
	// by instance, not by name: a shared service's other providers must stay registered.
	if (*m->provides())
		Services_RevokeIface(m->provides(), m->queryService());

	// Live downgrade FIRST (while the type's reflection + vtable are still valid): convert this
	// plugin's live components into inert placeholders so nothing dangles after it goes away.
	if (g_instance && g_instance->currentWorld)
		g_instance->currentWorld->ConvertPluginToUnknown(m->moduleFile);

	m->Shutdown();   // tears down windows/menus etc.; sets stopped = true
	m->loaded = false;
	cout << "[Modular]\tdisabled '" << m->title << "'" << endl;
}

void UnloadModules()
{
	// Two passes: runtime plugins first, boot providers (the renderer) LAST — a scripting
	// or GUI plugin's Shutdown may still touch the renderer; the reverse can't happen.
	for (int phase : { PHASE_RUNTIME, PHASE_BOOT })
		for (auto i : g_modules)
		{
			if (!i || !i->loaded || i->phase() != phase) continue;
			if (*i->provides())
				Services_RevokeIface(i->provides(), i->queryService());   // this instance only
			i->Shutdown();
			i->loaded = false;
		}
	g_modules.clear();
}

void LoadBuiltinShaders(iRender* render, const std::string& dir)
{
	if (!render) return;
	boost::system::error_code ec;

	// Resolve relative to the EXE, not the cwd (the VS debugger's working dir may differ).
	bfs::path shaderDir = boost::dll::program_location(ec).parent_path() / dir;
	if (ec || !bfs::exists(shaderDir, ec)) shaderDir = bfs::path(dir);   // fallback: cwd-relative
	if (!bfs::exists(shaderDir, ec))
	{
		cout << "[Modular]\tbuilt-in shaders dir not found: " << shaderDir.string() << endl;
		return;
	}
	cout << "[Modular]\tloading shaders from " << shaderDir.string() << endl;
	for (bfs::directory_iterator it(shaderDir, ec), end; it != end; it.increment(ec))
	{
		if (ec) break;
		if (bfs::is_directory(it->path()) || it->path().extension() != ".hlsl") continue;
		bfs::ifstream f(it->path(), std::ios::binary);
		if (!f) continue;
		std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::string name = it->path().stem().string();   // "world.vs.hlsl" -> "world.vs"
		render->setShaderSource(name.c_str(), src.c_str());
		cout << "[Modular]\tshader '" << name << "' (" << src.size() << " bytes)" << endl;
	}
}

// Packed runtime (3.2): the engine built-ins ride INSIDE game.nupak under "shaders/" — feed
// them to the renderer from the Package layers (so mods can override them like any entry).
void LoadBuiltinShadersPackaged(iRender* render)
{
	if (!render) return;
	int n = 0;
	for (const std::string& rel : Package::List("shaders/"))
	{
		const std::string suf = ".hlsl";
		if (rel.size() <= suf.size() || rel.compare(rel.size() - suf.size(), suf.size(), suf) != 0) continue;
		std::string src;
		if (!Package::Read(rel, src)) continue;
		std::string name = bfs::path(rel).stem().string();   // "shaders/world.vs.hlsl" -> "world.vs"
		render->setShaderSource(name.c_str(), src.c_str());
		++n;
	}
	cout << "[Modular]\t" << n << " built-in shaders loaded from the package" << endl;
}
}  // namespace nuke