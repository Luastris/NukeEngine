#include "interface/Modular.h"

namespace nuke {

// Single instance, owned by the engine DLL.
static bc::vector<boost::shared_ptr<NUKEModule>> g_modules;

bc::vector<boost::shared_ptr<NUKEModule>>& GetModules() { return g_modules; }

void InitModules(AppInstance* instance)
{
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
			g_modules.push_back(plugin);
			cout << "[Modular]\tloaded plugin '" << plugin->title << "' from "
			     << p.path().filename().string() << endl;
			boost::thread(boost::bind(&NUKEModule::Run, plugin.get(), instance));
		}
		catch (const std::exception& e)
		{
			cout << "[Modular]\tfailed to load " << p.path().filename().string()
			     << ": " << e.what() << endl;
		}
	}
}
void UnloadModules()
{
	for (auto i : g_modules)
	{
		if (i) {
			i.get()->Shutdown();
		}
	}
	g_modules.clear();
}
}  // namespace nuke