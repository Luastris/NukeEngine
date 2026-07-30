#include "interface/Modular.h"
#include "interface/Services.h"
#include "reflect/Reflect.h"
#include "API/Model/World.h"
#include "API/Model/Package.h"   // packed built-in shaders (3.2)
#include <boost/filesystem/fstream.hpp>
#include <iterator>
#include <map>
#include <set>
#ifdef _WIN32
#include <windows.h>   // SetDllDirectoryW: module deps resolve from the exe dir, not the CWD
#endif

namespace nuke {

// Single instance, owned by the engine DLL.
static bc::vector<std::shared_ptr<NUKEModule>> g_modules;
static AppInstance* g_instance = nullptr;   // host, captured at discovery (for EnablePlugin)
static std::map<std::string, std::string> g_typePlugin;   // component type -> owning dll name
static std::map<const NUKEModule*, int> g_moduleAbi;      // per-DLL ABI stamp (see NUKEEInteface.h)

bc::vector<std::shared_ptr<NUKEModule>>& GetModules() { return g_modules; }

// The ABI level a module's DLL was built against (its exported nuke_module_abi, read at
// discovery; 1 when the DLL predates the stamp). Callers of vtable-appended virtuals guard
// with this so a stale module DEGRADES (new virtual = its default) instead of crashing the
// host on a vtable slot the DLL doesn't have.
int ModuleAbi(const NUKEModule* m)
{
	auto it = g_moduleAbi.find(m);
	return (it != g_moduleAbi.end()) ? it->second : 1;
}

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

#ifdef _WIN32
// Read two exports from a DLL's PE export table by parsing the FILE — no LoadLibrary of any
// kind (see the gate below for why). Sets `hasPlugin` when an exported "plugin" symbol exists
// and `engineAbi` from the exported `nuke_engine_abi` int (1 = pre-stamp build). Returns false
// when the file isn't a parseable x64 PE — callers then fall back to the post-load gate.
static bool PreflightPeExports(const std::string& path, bool& hasPlugin, int& engineAbi)
{
	hasPlugin = false; engineAbi = 1;
	bfs::ifstream f(bfs::path(path), std::ios::binary);
	if (!f) return false;
	auto rd = [&](long long off, void* dst, size_t n) -> bool
	{
		f.clear(); f.seekg((std::streamoff)off);
		f.read((char*)dst, (std::streamsize)n);
		return (bool)f;
	};
	IMAGE_DOS_HEADER dos;
	if (!rd(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
	DWORD sig = 0; IMAGE_FILE_HEADER fh;
	if (!rd(dos.e_lfanew, &sig, 4) || sig != IMAGE_NT_SIGNATURE) return false;
	if (!rd(dos.e_lfanew + 4, &fh, sizeof(fh))) return false;
	if (fh.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) return false;   // not PE32+
	IMAGE_OPTIONAL_HEADER64 oh;
	if (!rd(dos.e_lfanew + 4 + sizeof(fh), &oh, sizeof(oh)) || oh.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
	const IMAGE_DATA_DIRECTORY& expDir = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	if (!expDir.VirtualAddress) return true;   // parsed fine — just exports nothing
	std::vector<IMAGE_SECTION_HEADER> secs(fh.NumberOfSections);
	if (secs.empty() || !rd(dos.e_lfanew + 4 + sizeof(fh) + fh.SizeOfOptionalHeader,
	                        secs.data(), secs.size() * sizeof(IMAGE_SECTION_HEADER))) return false;
	auto off = [&](DWORD rva) -> long long
	{
		for (const IMAGE_SECTION_HEADER& s : secs)
		{
			const DWORD size = s.Misc.VirtualSize > s.SizeOfRawData ? s.Misc.VirtualSize : s.SizeOfRawData;
			if (rva >= s.VirtualAddress && rva < s.VirtualAddress + size)
				return (long long)rva - s.VirtualAddress + s.PointerToRawData;
		}
		return -1;
	};
	IMAGE_EXPORT_DIRECTORY ed;
	const long long edOff = off(expDir.VirtualAddress);
	if (edOff < 0 || !rd(edOff, &ed, sizeof(ed))) return false;
	if (!ed.NumberOfNames || !ed.NumberOfFunctions) return true;
	std::vector<DWORD> nameRvas(ed.NumberOfNames);
	std::vector<WORD>  ordinals(ed.NumberOfNames);
	std::vector<DWORD> funcRvas(ed.NumberOfFunctions);
	const long long na = off(ed.AddressOfNames), no = off(ed.AddressOfNameOrdinals), fa = off(ed.AddressOfFunctions);
	if (na < 0 || no < 0 || fa < 0) return false;
	if (!rd(na, nameRvas.data(), nameRvas.size() * sizeof(DWORD))) return false;
	if (!rd(no, ordinals.data(), ordinals.size() * sizeof(WORD))) return false;
	if (!rd(fa, funcRvas.data(), funcRvas.size() * sizeof(DWORD))) return false;
	for (DWORD i = 0; i < ed.NumberOfNames; ++i)
	{
		const long long so = off(nameRvas[i]);
		if (so < 0) continue;
		char nm[64] = {};
		f.clear(); f.seekg((std::streamoff)so);
		f.read(nm, sizeof(nm) - 1);              // partial read at EOF is fine — gcount bytes
		nm[f.gcount() > 0 ? f.gcount() : 0] = 0;
		if (strcmp(nm, "plugin") == 0) hasPlugin = true;
		else if (strcmp(nm, "nuke_engine_abi") == 0 && ordinals[i] < ed.NumberOfFunctions)
		{
			const long long vo = off(funcRvas[ordinals[i]]);
			int v = 0;
			if (vo >= 0 && rd(vo, &v, sizeof(v))) engineAbi = v;
		}
	}
	return true;
}
#endif

// ---- SEH shields around FOREIGN module code -------------------------------------------------
// The loader's contract: NO broken, stale or config-mismatched DLL may ever take the host
// down. Stamps (above) catch the KNOWN mismatches cheaply; these shields catch everything
// else — an access violation (or any exception) inside module code refuses that module and
// the host keeps running. MSVC C++ exceptions ride on SEH too, so both kinds are caught.
// C++ unwinding can't cross __try, hence the tiny raw wrapper functions (no C++ locals).
static NUKEModule* DiscoverModuleFileBody(const std::string& absPath);
NUKEModule* DiscoverModuleFile(const std::string& absPath)
{
	__try { return DiscoverModuleFileBody(absPath); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		cout << "[Modular]\tREFUSED (crashed during discovery): " << absPath
		     << " — broken or stale binary; rebuild or remove it" << endl;
		return nullptr;
	}
}

static bool SehOnLoad(NUKEModule* m)
{
	__try { m->OnLoad(); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void* SehQueryService(NUKEModule* m)
{
	__try { return m->queryService(); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool SehShutdown(NUKEModule* m)
{
	__try { m->Shutdown(); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The plugin's Run() executes on its own background thread — an unshielded crash there kills
// the process no matter how careful the loader was. Same rule: log, stop, host lives.
static void SehRunGuard(NUKEModule* m, AppInstance* inst)
{
	__try { m->Run(inst); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		cout << "[Modular]\tplugin Run() crashed (module survives disabled): " << m->moduleFile << endl;
	}
}

// Import ONE plugin DLL into the pool. Skips non-plugins (no "plugin" export) and file
// names already discovered (the host's own modules/ wins over a same-named project module).
static NUKEModule* DiscoverModuleFileBody(const std::string& absPath)
{
	bfs::path p(absPath);
	auto ext = p.extension().string();
	if (ext != ".dll" && ext != ".so") return nullptr;
	const std::string file = p.filename().string();
	for (auto& m : g_modules)
		if (m && m->moduleFile == file) return nullptr;   // already in the pool
	try
	{
#ifdef _WIN32
		// Engine BINARY-compat gate WITHOUT running the DLL's code, and WITHOUT the OS loader:
		// LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES) is a trap — it plants an uninitialized
		// image in the loader's cache, and the later REAL LoadLibrary can be handed that same
		// image (imports unresolved, statics never run; seen live: NukeCSharp discovered as ''
		// with a garbage moduleFile, then a null service provider crash). So the stamps are
		// read by parsing the PE export table straight from the FILE BYTES (PreflightPeExports
		// below) — zero loader-state side effects, statics of a stale module never execute.
		{
			bool hasPlugin = false; int engineAbi = 1;
			if (PreflightPeExports(p.string(), hasPlugin, engineAbi))
			{
				if (!hasPlugin)
					return nullptr;   // not a NUKE module — skip without ever loading it
				if (engineAbi != NUKE_ENGINE_ABI)
				{
					cout << "[Modular]\t" << file << " REFUSED: built against engine ABI " << engineAbi
					     << ", this engine is " << NUKE_ENGINE_ABI
					     << " — rebuild the module (game modules: File -> Build & Reload Game Modules)" << endl;
					return nullptr;
				}
			}
			// Unparseable/foreign binary: fall through — the post-load gate below still refuses
			// a stale ABI (worst case its statics ran, same as before the pre-flight existed).
		}
#endif
		// Extension plugins export an unmangled "plugin" symbol — skip any other DLL.
		// ALTERED SEARCH PATH: a module's DLL dependencies resolve from the MODULE'S OWN
		// directory first (a game module linking NukeTilemap.dll finds it beside itself in
		// modules/ — the default search only looks next to the EXE).
		boost::dll::shared_library lib(p.string(), boost::dll::load_mode::load_with_altered_search_path);
		if (!lib.has("plugin"))
			return nullptr;

		// Second line of the ABI gate (non-Windows path: no pre-flight probe above).
		{
			int engineAbi = 1;
			if (lib.has("nuke_engine_abi")) engineAbi = lib.get<const int>("nuke_engine_abi");
			if (engineAbi != NUKE_ENGINE_ABI)
			{
				cout << "[Modular]\t" << file << " REFUSED: built against engine ABI " << engineAbi
				     << ", this engine is " << NUKE_ENGINE_ABI
				     << " — rebuild the module (game modules: File -> Build & Reload Game Modules)" << endl;
				return nullptr;
			}
		}

		auto plugin = boost::dll::import_symbol<NUKEModule>(p.string(), "plugin",
			boost::dll::load_mode::load_with_altered_search_path);
		plugin->modulePath = p.generic_string();
		plugin->moduleFile = file;
		plugin->loaded     = false;
		// ABI stamp: how new a NUKEModule vtable this DLL carries. No stamp = level 1
		// (built before it existed) — appended virtuals must not be called on it.
		int abi = 1;
		if (lib.has("nuke_module_abi")) abi = lib.get<const int>("nuke_module_abi");
		g_moduleAbi[plugin.get()] = abi;
		if (abi < NUKE_MODULE_ABI)
			cout << "[Modular]\tplugin '" << plugin->title << "' has ABI " << abi << " (engine "
			     << NUKE_MODULE_ABI << ") — rebuild it to use the newer module hooks" << endl;
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

	// modules/ sits NEXT TO THE EXECUTABLE (editor and dist layouts alike) — resolve against
	// the exe dir, never the CWD: a shortcut-launched packaged game (arbitrary working dir)
	// otherwise scans a random folder and finds no modules at all.
	boost::system::error_code ec;
	bfs::path exeDir = boost::dll::program_location(ec).parent_path();
	if (ec || exeDir.empty()) exeDir = bfs::current_path();
#ifdef _WIN32
	// Module DLLs load with LOAD_WITH_ALTERED_SEARCH_PATH, which REPLACES the application
	// directory with the module's own dir in the dependency search — their runtime deps
	// (glfw3, boost, ...) live in the EXE dir and were only ever found via the CWD slot.
	// Pin the exe dir into the standard search order so a launch from any working
	// directory (shortcut, terminal elsewhere) resolves them.
	SetDllDirectoryW(exeDir.wstring().c_str());
#endif
	const bfs::path modulesDir = exeDir / "modules";
	if (!bfs::exists(modulesDir, ec))
	{
		bfs::create_directory(modulesDir, ec);
		cout << "directory created!" << endl;
	}

	DiscoverModulesIn(modulesDir.string());
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
		g_moduleAbi.erase(m.get());   // the rebuilt DLL re-registers its stamp at discovery
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
	// SEH-shielded: OnLoad runs FOREIGN code. A crash refuses the plugin instead of killing
	// the host (classic victim: a project Game.dll built against another engine build or the
	// other Debug/Release configuration — the ABI stamp can't see a config mismatch).
	if (!SehOnLoad(m))
	{
		cout << "[Modular]\t" << m->moduleFile << " REFUSED: crashed in OnLoad — stale or built "
		     << "for another engine build/configuration; rebuild it "
		     << "(game modules: File -> Build & Reload Game Modules)" << endl;
		m->loaded = false;
		m->stopped = true;
		return;
	}
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
		if (void* iface = SehQueryService(m))
			Services_Provide(service.c_str(), iface);

	cout << "[Modular]\tenabled '" << m->title << "'" << endl;
	boost::thread(boost::bind(&SehRunGuard, m, g_instance));   // shielded: a crashing Run() must not kill the host
}

void DisablePlugin(NUKEModule* m)
{
	if (!m || !m->loaded) return;

	// Revoke THIS module's interface FIRST so no consumer can grab it while it's dying —
	// by instance, not by name: a shared service's other providers must stay registered.
	if (*m->provides())
		Services_RevokeIface(m->provides(), SehQueryService(m));

	// Live downgrade FIRST (while the type's reflection + vtable are still valid): convert this
	// plugin's live components into inert placeholders so nothing dangles after it goes away.
	if (g_instance && g_instance->currentWorld)
		g_instance->currentWorld->ConvertPluginToUnknown(m->moduleFile);

	if (!SehShutdown(m))   // shielded like OnLoad — teardown of a broken module must not kill the host
		cout << "[Modular]\t" << m->moduleFile << ": Shutdown crashed (module dropped anyway)" << endl;
	m->loaded = false;
	cout << "[Modular]\tdisabled '" << m->title << "'" << endl;
}

void UnloadModules()
{
	// Two passes: runtime plugins first, boot providers (the renderer) LAST — a scripting
	// or GUI plugin's Shutdown may still touch the renderer; the reverse can't happen.
	// FULL DisablePlugin per module (not a bare Shutdown): the live-component downgrade must
	// run so module-owned components DIE (and e.g. unsubscribe their Events lambdas) before
	// g_modules.clear() frees the DLLs — else engine statics hold std::functions whose code
	// is gone and the CRT teardown segfaults (hit by NukeNativeRim's RimGame subscriptions).
	// Teardown breadcrumbs go to STDERR raw: cout is tee'd into the log ring, and at this
	// point a wedged stream/thread can hang or assert inside the tee — stderr can't.
	for (int phase : { PHASE_RUNTIME, PHASE_BOOT })
		for (auto i : g_modules)
		{
			if (!i || !i->loaded || i->phase() != phase) continue;
			fprintf(stderr, "[Unload]\tdisabling %s\n", i->moduleFile.c_str()); fflush(stderr);
			DisablePlugin(i.get());
		}
	fprintf(stderr, "[Unload]\tfreeing module DLLs\n"); fflush(stderr);
	g_modules.clear();
	fprintf(stderr, "[Unload]\tdone\n"); fflush(stderr);
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