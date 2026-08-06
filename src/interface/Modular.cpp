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
#include <tlhelp32.h>  // thread enumeration at unload (see KillModuleThreads)
#endif

namespace nuke {

// Modules the ABI gate turned away, by absolute path. A refused module is INVISIBLE — its
// components never reach the registry — so the editor needs to know it happened to rebuild it
// instead of leaving the user with silently missing classes.
static std::set<std::string> g_refused;

std::vector<std::string> RefusedModules()
{
	return std::vector<std::string>(g_refused.begin(), g_refused.end());
}

// Single instance, owned by the engine DLL.
static bc::vector<std::shared_ptr<NUKEModule>> g_modules;
static AppInstance* g_instance = nullptr;   // host, captured at discovery (for EnablePlugin)
static std::map<std::string, std::string> g_typePlugin;   // component type -> owning dll name
static std::map<const NUKEModule*, int> g_moduleAbi;      // per-DLL ABI stamp (see NUKEEInteface.h)
// Worker threads the loader started for modules, tagged with the owning DLL and the OS thread
// id so a stuck one can be named and killed before its code is unmapped.
struct ModThread
{
	std::string   dll;
	unsigned long id = 0;
	boost::thread th;
};
static std::map<const NUKEModule*, ModThread> g_runThreads;

#ifdef _WIN32
static void KillThreadsOfModule(const std::string& dllFile);
#endif

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

// Is a plugin INSTALLED? `name` matches the DLL file name (with or without the extension) or
// the module title, case-insensitively. The pool answers once discovery has run; before that —
// mods mount before InitModules — the modules directory on disk does. `outLoaded`, when given,
// reports whether that plugin is currently ACTIVE, which is a separate question: a mod whose
// module is installed but switched off still mounts, its components just load inert.
bool ModuleInstalled(const std::string& name, bool* outLoaded)
{
	if (outLoaded) *outLoaded = false;
	if (name.empty()) return true;
	auto low = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
	const std::string want = low(bfs::path(name).stem().string());
	for (auto& m : g_modules)
	{
		if (!m) continue;
		if (low(bfs::path(m->moduleFile).stem().string()) == want || low(m->title) == want)
		{
			if (outLoaded) *outLoaded = m->loaded;
			return true;
		}
	}
	boost::system::error_code ec;
	const bfs::path dir = boost::dll::program_location(ec).parent_path() / "modules";
	if (ec) return false;
	for (bfs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
		if (low(it->path().stem().string()) == want) return true;
	return false;
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

// The libraries a native image links against. This is FILE-FORMAT work, not OS work: the layout
// is the same bytes whatever machine reads them, so it is parsed by offset — a Windows build can
// answer about a .so and the other way round, and nothing here is compiled out per platform.
// Only the headers are touched, never the whole file.
static std::vector<std::string> ImageImportNames(const std::string& path)
{
	std::vector<std::string> out;
	bfs::ifstream f(bfs::path(path), std::ios::binary);
	if (!f) return out;
	// Header window: everything the import walk needs to locate lives in the first pages.
	std::vector<char> h(0x1000);
	f.read(h.data(), (std::streamsize)h.size());
	const size_t got = (size_t)(f.gcount() > 0 ? f.gcount() : 0);
	h.resize(got);
	auto u16 = [&](size_t o) -> uint32_t
	{ return o + 2 <= h.size() ? (uint32_t)((uint8_t)h[o] | ((uint8_t)h[o + 1] << 8)) : 0u; };
	auto u32 = [&](size_t o) -> uint32_t
	{
		if (o + 4 > h.size()) return 0u;
		return (uint32_t)((uint8_t)h[o] | ((uint8_t)h[o + 1] << 8) | ((uint8_t)h[o + 2] << 16)
		                | ((uint32_t)(uint8_t)h[o + 3] << 24));
	};
	if (h.size() < 0x40 || u16(0) != 0x5A4D) return out;                 // "MZ"
	const uint32_t pe = u32(0x3C);
	if (pe + 24 > h.size() || u32(pe) != 0x00004550) return out;         // "PE\0\0"
	const uint32_t nSec = u16(pe + 6), optSize = u16(pe + 20);
	const size_t opt = pe + 24;
	const uint32_t magic = u16(opt);
	if (magic != 0x10B && magic != 0x20B) return out;                    // PE32 / PE32+
	const size_t dirs = opt + (magic == 0x20B ? 112 : 96);
	const uint32_t impRva = u32(dirs + 8);                               // import directory
	if (!impRva) return out;
	const size_t secs = opt + optSize;
	auto toOff = [&](uint32_t rva) -> long long
	{
		for (uint32_t i = 0; i < nSec; ++i)
		{
			const size_t s = secs + (size_t)i * 40;
			const uint32_t va = u32(s + 12);
			uint32_t size = u32(s + 8);                                  // VirtualSize
			const uint32_t rawSz = u32(s + 16), raw = u32(s + 20);
			if (size < rawSz) size = rawSz;
			if (rva >= va && rva < va + size) return (long long)raw + (rva - va);
		}
		return -1LL;
	};
	long long cur = toOff(impRva);
	if (cur < 0) return out;
	for (int guard = 0; guard < 4096; ++guard, cur += 20)               // IMAGE_IMPORT_DESCRIPTOR
	{
		char rec[20] = {};
		f.clear(); f.seekg((std::streamoff)cur);
		f.read(rec, sizeof(rec));
		if (f.gcount() < (std::streamsize)sizeof(rec)) break;
		const uint32_t nameRva = (uint32_t)((uint8_t)rec[12] | ((uint8_t)rec[13] << 8)
		                                  | ((uint8_t)rec[14] << 16) | ((uint32_t)(uint8_t)rec[15] << 24));
		if (!nameRva) break;                                             // null terminator record
		const long long no = toOff(nameRva);
		if (no < 0) continue;
		char nm[256] = {};
		f.clear(); f.seekg((std::streamoff)no);
		f.read(nm, sizeof(nm) - 1);
		nm[f.gcount() > 0 ? (size_t)f.gcount() : 0] = 0;
		std::string s = bfs::path(nm).stem().string();
		for (char& c : s) c = (char)tolower((unsigned char)c);
		if (!s.empty()) out.push_back(s);
	}
	return out;
}

// The engine plugins a native binary links against, filtered to plugins this installation has.
std::vector<std::string> ModuleImportsOf(const std::string& binaryPath)
{
	std::vector<std::string> out;
	for (const std::string& imp : ImageImportNames(binaryPath))
		if (ModuleInstalled(imp)) out.push_back(imp);
	return out;
}

// Every reflected type a plugin owns, as name -> plugin file stem. The packager matches these
// names inside script sources and managed assemblies: a class a module brought is the only
// honest trace a script leaves of needing that module.
std::vector<std::pair<std::string, std::string>> PluginOwnedTypes()
{
	std::vector<std::pair<std::string, std::string>> out;
	for (const auto& kv : g_typePlugin)
		if (!kv.second.empty())
			out.push_back({ kv.first, bfs::path(kv.second).stem().string() });
	return out;
}

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

// ---- shielded vtable queries (see Modular.h) ------------------------------------------------
// Separate one-liners: a __try frame cannot hold objects with destructors, so each wrapper does
// exactly one call and nothing else.
static bool SehEditorTool(NUKEModule* m, bool& out)
{
	__try { out = m->editorTool(); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SehCompanionOf(NUKEModule* m, const char*& out)
{
	__try { out = m->companionOf(); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SehHasSettings(NUKEModule* m, bool& out)
{
	__try { out = m->HasSettings(); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SehSettings(NUKEModule* m)
{
	__try { m->Settings(); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void ModuleFaulted(NUKEModule* m, const char* what)
{
	cout << "[Modular]	" << (m && m->moduleFile[0] ? m->moduleFile : "<module>")
	     << ": " << what << " faulted — the module is stale or built for another engine build; "
	     << "the call was ignored" << endl;
}

bool ModuleIsEditorTool(NUKEModule* m)
{
	if (!m || ModuleAbi(m) < 2) return false;   // slot does not exist in this DLL
	bool v = false;
	if (!SehEditorTool(m, v)) { ModuleFaulted(m, "editorTool()"); return false; }
	return v;
}

std::string ModuleCompanionOf(NUKEModule* m)
{
	if (!m || ModuleAbi(m) < 3) return std::string();
	const char* v = nullptr;
	if (!SehCompanionOf(m, v)) { ModuleFaulted(m, "companionOf()"); return std::string(); }
	return v ? std::string(v) : std::string();
}

bool ModuleHasSettings(NUKEModule* m)
{
	if (!m) return false;
	bool v = false;
	if (!SehHasSettings(m, v)) { ModuleFaulted(m, "HasSettings()"); return false; }
	return v;
}

bool ModuleDrawSettings(NUKEModule* m)
{
	if (!m) return false;
	if (!SehSettings(m)) { ModuleFaulted(m, "Settings()"); return false; }
	return true;
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
	g_refused.erase(p.generic_string());   // re-discovery re-judges this file
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
	// Native plugins the mounted mods brought. The host's own modules/ was walked first, so a
	// mod cannot shadow an engine plugin by reusing its file name.
	for (const std::string& d : Package::ModuleCacheDirs()) DiscoverModulesIn(d);
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
#ifdef _WIN32
		KillThreadsOfModule(moduleFile);   // this path really unmaps: nothing of its may still run
#endif
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
	// Kept, not detached: DisablePlugin stops it before the DLL can be freed.
	g_runThreads.erase(m);
	ModThread mt;
	mt.dll = m->moduleFile;
	mt.th  = boost::thread(boost::bind(&SehRunGuard, m, g_instance));
#ifdef _WIN32
	mt.id = ::GetThreadId(mt.th.native_handle());
#endif
	g_runThreads[m] = std::move(mt);
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
	// The module's worker must be GONE before anything frees this DLL — a live thread in
	// unmapped code faults. Cooperative stop first (Shutdown set `stopped`), then a hard kill.
	{
		auto rt = g_runThreads.find(m);
		if (rt != g_runThreads.end())
		{
			ModThread& t = rt->second;
			if (!t.th.try_join_for(boost::chrono::seconds(2)))
			{
#ifdef _WIN32
				cout << "[Modular]\t" << t.dll << ": worker thread " << t.id
				     << " ignored stop for 2 s — terminating it" << endl;
				::TerminateThread(t.th.native_handle(), 0);
#else
				cout << "[Modular]\t" << t.dll << ": worker thread " << t.id
				     << " ignored stop for 2 s — detached (no portable kill)" << endl;
#endif
				t.th.detach();   // the boost::thread must not outlive this scope joinable
			}
			g_runThreads.erase(rt);
		}
	}
	m->loaded = false;
	cout << "[Modular]\tdisabled '" << m->title << "'" << endl;
}

#ifdef _WIN32
// Kill every live thread whose Win32 start address lies inside `dllFile`, by (dll, thread id).
// Called before that DLL is actually unmapped (hot-reload) — a thread left running there wakes
// up in unmapped code. Threads owned by the exe, the engine or system DLLs are never touched.
static void KillThreadsOfModule(const std::string& dllFile)
{
	typedef LONG(WINAPI * PFN_NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
	static PFN_NtQIT NtQIT = (PFN_NtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE) return;
	THREADENTRY32 te = { sizeof(te) };
	const DWORD selfPid = GetCurrentProcessId(), selfTid = GetCurrentThreadId();
	for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
	{
		if (te.th32OwnerProcessID != selfPid || te.th32ThreadID == selfTid) continue;
		HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_TERMINATE, FALSE, te.th32ThreadID);
		if (!h) continue;
		void* start = nullptr;
		char file[MAX_PATH] = "?";
		if (NtQIT && NtQIT(h, 9 /*ThreadQuerySetWin32StartAddress*/, &start, sizeof(start), nullptr) == 0 && start)
		{
			HMODULE hm = nullptr;
			char path[MAX_PATH] = "";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)start, &hm) && hm
			    && GetModuleFileNameA(hm, path, MAX_PATH))
			{
				const char* slash = strrchr(path, '\\');
				strncpy(file, slash ? slash + 1 : path, MAX_PATH - 1);
				file[MAX_PATH - 1] = 0;
			}
		}
		if (_stricmp(file, dllFile.c_str()) == 0)
		{
			cout << "[Modular]\t" << dllFile << ": killing leftover thread " << te.th32ThreadID
			     << " before the unmap" << endl;
			TerminateThread(h, 0);
		}
		CloseHandle(h);
	}
	CloseHandle(snap);
}
#endif

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
	// PROCESS EXIT: everything is shut down, but the DLLs stay MAPPED on purpose. Foreign worker
	// threads (the OS pool, GPU drivers, the CLR, injected overlays) can still call back into a
	// module after this point, and that call would land in unmapped code. The OS reclaims the
	// mappings at exit anyway. Live hot-reload is unaffected — UnloadModuleDll still unmaps.
	while (!g_modules.empty())
	{
		auto m = g_modules.back();
		g_modules.pop_back();
		fprintf(stderr, "[Unload]\tkeeping %s mapped\n", m ? m->moduleFile.c_str() : "?"); fflush(stderr);
		if (m) new std::shared_ptr<NUKEModule>(m);   // deliberate: outlives us by design
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