#include "interface/RenderModular.h"

#define BOOST_FILESYSTEM_VERSION 3
#define BOOST_FILESYSTEM_NO_DEPRECATED
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/dll.hpp>
#include <memory>                 // boost.dll (1.91+) returns std::shared_ptr from import_symbol
#include <iostream>
#include <iterator>

namespace nuke {

namespace bfs = boost::filesystem;
using namespace std;

// The render module DLL exports its NUKERenderModule subclass instance under this
// (unmangled, extern "C") symbol name. Extension plugins use "plugin" instead.
static const char* kRenderSymbol = "renderModule";

static std::shared_ptr<NUKERenderModule> g_renderModule;
static iRender* g_renderer = nullptr;

iRender* LoadRenderModule(const std::string& preferredId)
{
	bfs::path modulesDir = bfs::current_path().concat("/modules");
	if (!bfs::exists(modulesDir))
	{
		bfs::create_directory(modulesDir);
		cout << "[RenderModular]\tcreated modules directory" << endl;
	}

	std::shared_ptr<NUKERenderModule> fallback; // first render module found, if no id match

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

void LoadBuiltinShaders(iRender* render, const std::string& dir)
{
	if (!render) return;
	boost::system::error_code ec;

	// Resolve relative to the EXE, not the cwd (the VS debugger's working dir may differ).
	bfs::path shaderDir = boost::dll::program_location(ec).parent_path() / dir;
	if (ec || !bfs::exists(shaderDir, ec)) shaderDir = bfs::path(dir);   // fallback: cwd-relative
	if (!bfs::exists(shaderDir, ec))
	{
		cout << "[RenderModular]\tbuilt-in shaders dir not found: " << shaderDir.string() << endl;
		return;
	}
	cout << "[RenderModular]\tloading shaders from " << shaderDir.string() << endl;
	for (bfs::directory_iterator it(shaderDir, ec), end; it != end; it.increment(ec))
	{
		if (ec) break;
		if (bfs::is_directory(it->path()) || it->path().extension() != ".hlsl") continue;
		bfs::ifstream f(it->path(), std::ios::binary);
		if (!f) continue;
		std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::string name = it->path().stem().string();   // "world.vs.hlsl" -> "world.vs"
		render->setShaderSource(name.c_str(), src.c_str());
		cout << "[RenderModular]\tshader '" << name << "' (" << src.size() << " bytes)" << endl;
	}
}

}  // namespace nuke