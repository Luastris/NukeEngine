#include "interface/RenderModular.h"

#define BOOST_FILESYSTEM_VERSION 3
#define BOOST_FILESYSTEM_NO_DEPRECATED
#include <boost/filesystem.hpp>
#include <boost/dll.hpp>
#include <iostream>

namespace nuke {

namespace bfs = boost::filesystem;
using namespace std;

// The render module DLL exports its NUKERenderModule subclass instance under this
// (unmangled, extern "C") symbol name. Extension plugins use "plugin" instead.
static const char* kRenderSymbol = "renderModule";

static boost::shared_ptr<NUKERenderModule> g_renderModule;
static iRender* g_renderer = nullptr;

iRender* LoadRenderModule(const std::string& preferredId)
{
	bfs::path modulesDir = bfs::current_path().concat("/modules");
	if (!bfs::exists(modulesDir))
	{
		bfs::create_directory(modulesDir);
		cout << "[RenderModular]\tcreated modules directory" << endl;
	}

	boost::shared_ptr<NUKERenderModule> fallback; // first render module found, if no id match

	for (auto& p : bfs::directory_iterator(modulesDir))
	{
		if (bfs::is_directory(p.path()))
			continue;
		auto ext = p.path().extension().string();
		if (ext != ".dll" && ext != ".so")
			continue;

		try
		{
			boost::dll::shared_library lib(p.path().string());
			if (!lib.has(kRenderSymbol))
				continue; // not a render module (maybe an extension plugin)

			auto mod = boost::dll::import_symbol<NUKERenderModule>(p.path().string(), kRenderSymbol);
			mod->modulePath = p.path().generic_string();
			cout << "[RenderModular]\tfound render module '" << mod->id
				 << "' in " << p.path().filename().string() << endl;

			if (preferredId.empty() || preferredId == mod->id)
			{
				g_renderModule = mod;
				g_renderer = mod->CreateRenderer();
				cout << "[RenderModular]\tloaded renderer '" << mod->id << "'" << endl;
				return g_renderer;
			}
			if (!fallback)
				fallback = mod; // remember the first available as a fallback
		}
		catch (const std::exception& e)
		{
			cout << "[RenderModular]\tskip " << p.path().filename().string()
				 << " (" << e.what() << ")" << endl;
		}
	}

	if (fallback)
	{
		cout << "[RenderModular]\tid '" << preferredId
			 << "' not found, falling back to '" << fallback->id << "'" << endl;
		g_renderModule = fallback;
		g_renderer = g_renderModule->CreateRenderer();
		return g_renderer;
	}

	cout << "[RenderModular]\tno render module found in modules/" << endl;
	return nullptr;
}

void UnloadRenderModule()
{
	if (g_renderModule)
	{
		if (g_renderer)
		{
			g_renderModule->DestroyRenderer(g_renderer);
			g_renderer = nullptr;
		}
		g_renderModule->Shutdown();
		g_renderModule.reset();
	}
}

}  // namespace nuke