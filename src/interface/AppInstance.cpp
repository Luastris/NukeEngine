#include "interface/AppInstance.h"
#include <boost/filesystem.hpp>

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