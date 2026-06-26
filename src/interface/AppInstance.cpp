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