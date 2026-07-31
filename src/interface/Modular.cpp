// Must precede any boost header: the lib flavor of chrono double-defines steady_clock::now (LNK2005).
#define BOOST_CHRONO_HEADER_ONLY
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
static std::map<const NUKEModule*, boost::thread> g_runThreads;   // joined before the DLL is freed
static std::set<const NUKEModule*> g_runawayModules;      // Run() ignored stop: DLL must stay mapped

bc::vector<std::shared_ptr<NUKEModule>>& GetModules() { return g_modules; }

// The ABI level a module's DLL was built against (exported nuke_module_abi; 1 when absent).
// Guard calls to vtable-appended virtuals with this — a stale DLL lacks the slot.
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
// Read two exports out of a DLL's PE export table by parsing the file — no LoadLibrary.
// `hasPlugin` = an exported "plugin" symbol exists; `engineAbi` = the exported
// nuke_engine_abi int (1 when absent). Returns false when the file isn't a parseable PE32+.
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
	if (!expDir.VirtualAddress) return true;   // valid PE that exports nothing
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
		f.read(nm, sizeof(nm) - 1);              // a short read at EOF is fine
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

// Shields around foreign module code: a fault inside a module refuses that module instead of
// taking the host down. Separate wrappers because __try frames cannot hold C++ destructors.
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

#ifdef _WIN32
// MSVC: __try/__except catches access violations AND C++ exceptions (C++ EH rides on SEH).
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
#else
// Portable fallback: C++ exceptions only — a hard fault is the crash reporter's business.
static bool SehOnLoad(NUKEModule* m)       { try { m->OnLoad(); return true; } catch (...) { return false; } }
static void* SehQueryService(NUKEModule* m){ try { return m->queryService(); } catch (...) { return nullptr; } }
static bool SehShutdown(NUKEModule* m)     { try { m->Shutdown(); return true; } catch (...) { return false; } }
#endif

// Run() executes on its own thread: an unshielded crash there kills the whole process.
static void SehRunGuard(NUKEModule* m, AppInstance* inst)
{
#ifdef _WIN32
	__try { m->Run(inst); }
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		cout << "[Modular]\tplugin Run() crashed (module survives disabled): " << m->moduleFile << endl;
	}
#else
	try { m->Run(inst); }
	catch (...)
	{
		cout << "[Modular]\tplugin Run() threw (module survives disabled): " << m->moduleFile << endl;
	}
#endif
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
		// ABI gate BEFORE the DLL's code can run. Not LoadLibraryEx(DONT_RESOLVE_DLL_REFERENCES):
		// that plants an uninitialized image the later real LoadLibrary can be handed back.
		{
			bool hasPlugin = false; int engineAbi = 1;
			if (PreflightPeExports(p.string(), hasPlugin, engineAbi))
			{
				if (!hasPlugin)
					return nullptr;   // not a NUKE module
				if (engineAbi != NUKE_ENGINE_ABI)
				{
					cout << "[Modular]\t" << file << " REFUSED: built against engine ABI " << engineAbi
					     << ", this engine is " << NUKE_ENGINE_ABI
					     << " — rebuild the module (game modules: File -> Build & Reload Game Modules)" << endl;
					return nullptr;
				}
			}
		}
#endif
		// Plugins export an unmangled "plugin" symbol. ALTERED SEARCH PATH: a module's DLL
		// dependencies resolve from the MODULE'S OWN dir first, not only next to the EXE.
		boost::dll::shared_library lib(p.string(), boost::dll::load_mode::load_with_altered_search_path);
		if (!lib.has("plugin"))
			return nullptr;

		// Second line of the ABI gate (and the only one where there is no pre-flight above).
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
		// ABI stamp: how new a NUKEModule vtable this DLL carries; no stamp = level 1.
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

	// modules/ sits NEXT TO THE EXECUTABLE — resolve against the exe dir, never the CWD
	// (a shortcut-launched game has an arbitrary working dir).
	boost::system::error_code ec;
	bfs::path exeDir = boost::dll::program_location(ec).parent_path();
	if (ec || exeDir.empty()) exeDir = bfs::current_path();
#ifdef _WIN32
	// LOAD_WITH_ALTERED_SEARCH_PATH replaces the application dir in the dependency search, so
	// pin the exe dir: module deps (glfw3, boost, ...) live there and must resolve from any CWD.
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

// Unload a module's DLL so a rebuild can overwrite the file; the pool's shared_ptr is the
// lock. PHASE_BOOT providers (the renderer) can never be torn down mid-run.
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

	// One active provider per EXCLUSIVE service. A PHASE_BOOT provider cannot be swapped live
	// (the choice applies at next start); SHARED services load their providers side by side.
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
	// Shielded: OnLoad runs foreign code, and the ABI stamp cannot see a Debug/Release mismatch.
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

	// Live upgrade: inert placeholders of this plugin's types become real components again.
	if (g_instance && g_instance->currentWorld)
		g_instance->currentWorld->RestorePluginComponents(m->moduleFile);

	// Registration is loader-bound, not left to the plugin, so provide/revoke cannot get out
	// of sync with the plugin lifecycle.
	if (!service.empty())
		if (void* iface = SehQueryService(m))
			Services_Provide(service.c_str(), iface);

	cout << "[Modular]\tenabled '" << m->title << "'" << endl;
	// Kept, not detached: DisablePlugin joins it before the DLL can be freed.
	g_runThreads[m] = boost::thread(boost::bind(&SehRunGuard, m, g_instance));
}

void DisablePlugin(NUKEModule* m)
{
	if (!m || !m->loaded) return;

	// Revoke FIRST, and by instance rather than by name — a shared service's other providers
	// must stay registered.
	if (*m->provides())
		Services_RevokeIface(m->provides(), SehQueryService(m));

	// Downgrade live components to inert placeholders FIRST, while the type's reflection and
	// vtable are still valid.
	if (g_instance && g_instance->currentWorld)
		g_instance->currentWorld->ConvertPluginToUnknown(m->moduleFile);

	if (!SehShutdown(m))   // shielded like OnLoad
		cout << "[Modular]\t" << m->moduleFile << ": Shutdown crashed (module dropped anyway)" << endl;
	// Reap Run() BEFORE anyone can free this DLL — a live thread in unmapped code is an AV.
	{
		auto rt = g_runThreads.find(m);
		if (rt != g_runThreads.end())
		{
			if (!rt->second.try_join_for(boost::chrono::seconds(2)))
			{
				rt->second.detach();
				g_runawayModules.insert(m);
				cout << "[Modular]\t" << m->moduleFile << ": Run() ignored stop for 2 s — its DLL "
				     << "stays mapped until process exit (unloading under a live thread = crash)" << endl;
			}
			g_runThreads.erase(rt);
		}
	}
	m->loaded = false;
	cout << "[Modular]\tdisabled '" << m->title << "'" << endl;
}

void UnloadModules()
{
	// Two passes: runtime plugins first, boot providers (the renderer) LAST. FULL DisablePlugin,
	// not a bare Shutdown: module-owned components must die (unsubscribing their lambdas) before
	// the DLLs are freed. Breadcrumbs go to raw stderr — cout can assert this late in teardown.
	for (int phase : { PHASE_RUNTIME, PHASE_BOOT })
		for (auto i : g_modules)
		{
			if (!i || !i->loaded || i->phase() != phase) continue;
			fprintf(stderr, "[Unload]\tdisabling %s\n", i->moduleFile.c_str()); fflush(stderr);
			DisablePlugin(i.get());
		}
	while (!g_modules.empty())
	{
		auto m = g_modules.back();
		g_modules.pop_back();
		if (m && g_runawayModules.count(m.get()))
		{
			// Deliberate leak: a runaway Run() thread still executes this DLL's code, so the
			// mapping must outlive it (the OS reclaims it at process exit).
			fprintf(stderr, "[Unload]\tkeeping %s mapped (runaway Run thread)\n", m->moduleFile.c_str()); fflush(stderr);
			new std::shared_ptr<NUKEModule>(m);
			continue;
		}
		fprintf(stderr, "[Unload]\tfreeing %s\n", m ? m->moduleFile.c_str() : "?"); fflush(stderr);
		m.reset();   // last ref -> boost::dll unmaps the DLL here
	}
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

// Feed the renderer the built-in shaders packed under "shaders/" in the Package layers
// (so mods can override them like any other entry).
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