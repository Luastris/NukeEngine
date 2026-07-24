#include "API/Model/Game.h"
#include "API/Model/World.h"
#include "API/Model/Time.h"
#include "API/Model/Package.h"       // packed vs raw decides the save dir (6.6)
#include "interface/AppInstance.h"
#include <boost/dll.hpp>             // program_location (the dist exe carries the game name)
#include <boost/filesystem/fstream.hpp>
#include "config.h"
#include "render/irender.h"
#include <boost/filesystem.hpp>   // project dir for the game's window.json (editor)
#include <algorithm>
#include <iostream>
#include <mutex>                  // screenshot request handoff (update thread -> render)
#define STB_IMAGE_WRITE_IMPLEMENTATION   // Screenshot encoder (the only engine TU with it)
#include <stb_image_write.h>

namespace nuke {

World* Game::GetWorld() { return AppInstance::GetSingleton()->currentWorld; }

bool Game::LoadWorld(const std::string& contentRelPath)
{
	return AppInstance::GetSingleton()->OpenWorld(contentRelPath);
}

// Async world loading (task #147) — thin facade over AppInstance's staged loader.
bool   Game::LoadWorldAsync(const std::string& contentRelPath) { return AppInstance::GetSingleton()->StartWorldLoadAsync(contentRelPath); }
double Game::LoadWorldProgress()   { return AppInstance::GetSingleton()->WorldLoadProgress(); }
bool   Game::LoadWorldReady()      { return AppInstance::GetSingleton()->WorldLoadReady(); }
bool   Game::ActivateLoadedWorld() { return AppInstance::GetSingleton()->ActivateLoadedWorld(); }
void   Game::CancelLoadWorld()     { AppInstance::GetSingleton()->CancelWorldLoadAsync(); }

// Incremental activation (task #148): budgeted growth, optionally outward from a point.
void   Game::SetWorldActivationBudget(double msPerFrame) { AppInstance::GetSingleton()->SetWorldActivationBudget(msPerFrame); }
double Game::GetWorldActivationBudget()                  { return AppInstance::GetSingleton()->GetWorldActivationBudget(); }
void   Game::SetWorldActivationOrigin(const Vector3& worldPos) { AppInstance::GetSingleton()->SetWorldActivationOrigin((float)worldPos.x, (float)worldPos.y, (float)worldPos.z); }
void   Game::ClearWorldActivationOrigin()                { AppInstance::GetSingleton()->ClearWorldActivationOrigin(); }
double Game::WorldActivationProgress()                   { return AppInstance::GetSingleton()->WorldActivationProgress(); }

bool Game::IsEditor()  { return AppInstance::GetSingleton()->isEditor(); }
bool Game::IsPlaying() { return AppInstance::GetSingleton()->playState == 1; }
bool Game::IsPaused()  { return AppInstance::GetSingleton()->playState == 2; }

void Game::SetPaused(bool paused)
{
	AppInstance* app = AppInstance::GetSingleton();
	if (app->playState == 0) return;   // edit mode: PIE start/stop is the editor's call
	app->playState = paused ? 2 : 1;
}

// The game's save directory (6.6). Editor + raw dev player: `<project>/saves` (beside the
// content — versionable, easy to wipe). Packaged player: `%APPDATA%/<exe stem>/saves` (the
// dist exe carries the game's name), never inside Program Files.
static boost::filesystem::path SaveDir()
{
	AppInstance* app = AppInstance::GetSingleton();
	namespace bfs = boost::filesystem;
	if (app->isEditor() || Package::MountedCount() == 0)
	{
		bfs::path content = app->contentRoot.empty() ? bfs::path("project/content") : bfs::path(app->contentRoot);
		return content.parent_path() / "saves";
	}
	const char* appdata = std::getenv("APPDATA");
	boost::system::error_code ec;
	bfs::path exe = boost::dll::program_location(ec);
	std::string game = ec ? std::string("NukeGame") : exe.stem().string();
	return bfs::path(appdata ? appdata : ".") / game / "saves";
}

bool Game::SaveGame(const std::string& slot)
{
	if (slot.empty()) return false;
	AppInstance* app = AppInstance::GetSingleton();
	if (!app->currentWorld) return false;
	namespace bfs = boost::filesystem;
	boost::system::error_code ec;
	bfs::path dir = SaveDir();
	bfs::create_directories(dir, ec);
	bfs::path file = dir / (slot + ".nusave");
	bfs::ofstream f(file, std::ios::binary);
	if (!f) { std::cout << "[Game]\t\tSaveGame: cannot write " << file.string() << std::endl; return false; }
	f << app->currentWorld->SaveToString();   // triggers OnBeforeSave across all components
	std::cout << "[Game]\t\tsaved '" << slot << "' -> " << file.string() << std::endl;
	return true;
}

bool Game::LoadGame(const std::string& slot)
{
	namespace bfs = boost::filesystem;
	boost::system::error_code ec;
	bfs::path file = SaveDir() / (slot + ".nusave");
	if (!bfs::exists(file, ec)) { std::cout << "[Game]\t\tLoadGame: no such save '" << slot << "'" << std::endl; return false; }
	// Queue to the frame boundary — a script calls this MID-TICK over the very hierarchy
	// the load replaces (same rule as Game.LoadWorld).
	AppInstance::GetSingleton()->pendingSaveLoad = bfs::absolute(file).string();
	return true;
}

std::string Game::ListSaves()
{
	namespace bfs = boost::filesystem;
	boost::system::error_code ec;
	bfs::path dir = SaveDir();
	if (!bfs::exists(dir, ec)) return "";
	std::vector<std::pair<std::time_t, std::string>> slots;
	for (bfs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec))
		if (it->path().extension() == ".nusave")
			slots.push_back({ bfs::last_write_time(it->path(), ec), it->path().stem().string() });
	std::sort(slots.begin(), slots.end(), [](auto& a, auto& b) { return a.first > b.first; });   // newest first
	std::string out;
	for (auto& s : slots) { if (!out.empty()) out += "\n"; out += s.second; }
	return out;
}

void Game::SetTimeScale(double scale)
{
	if (scale < 0.0) scale = 0.0;
	if (scale > 8.0) scale = 8.0;
	Time::getSingleton()->scale = scale;
}

double Game::GetTimeScale() { return Time::getSingleton()->scale; }

void Game::Quit()
{
	AppInstance* app = AppInstance::GetSingleton();
	if (app->isEditor())
	{
		// A game must not close the host application. Stopping PIE is an editor command.
		std::cout << "[Game]\t\tQuit() ignored in the editor (stop PIE from the toolbar)" << std::endl;
		return;
	}
	if (app->render) app->render->requestClose();
}

// --- window control ------------------------------------------------------------------------
namespace {
// The game's window settings are GAME DATA. In the editor they go to <project>/window.json
// (Package Project merges it into the shipped config) — NEVER into the editor's own
// config/main.json: a PIE script calling e.g. Game.SetTransparent(true) must not flip the
// EDITOR's window on its next launch (this exact hijack kept re-enabling transparency).
void SaveGameWindow()
{
	Config* c = Config::getSingleton();
	AppInstance* app = AppInstance::GetSingleton();
	if (!app->isEditor()) { c->saveWindow(); return; }   // the game owns its own config/main.json
	if (app->contentRoot.empty())
	{
		std::cout << "[Game]\t\twindow settings NOT saved (no project loaded)" << std::endl;
		return;
	}
	boost::filesystem::path proj = boost::filesystem::path(app->contentRoot).parent_path();
	c->saveWindowTo((proj / "window.json").string());
	std::cout << "[Game]\t\twindow settings saved to the project (editor config untouched)" << std::endl;
}

// Persist the window config + apply it to the live window. Live-apply is skipped in the
// editor (the window there is the EDITOR's, not the game's).
void ApplyAndSaveWindow()
{
	Config* c = Config::getSingleton();
	NukeWindow& win = c->window;
	win.fullscreen = win.mode != 0;   // keep the legacy mirror consistent
	SaveGameWindow();

	AppInstance* app = AppInstance::GetSingleton();
	if (app->isEditor()) return;
	if (!app->render) return;
	WindowDesc d;
	d.w = win.w; d.h = win.h; d.title = nullptr;   // title is not config; applyWindow leaves it alone
	d.decorated = win.decorated; d.resizable = win.resizable;
	d.floating  = win.floating;  d.maximized = win.maximized;
	d.mode = win.mode; d.fullscreen = win.fullscreen;
	d.transparent = win.transparent; d.opacity = win.opacity;
	d.backend = win.backend;
	app->render->applyWindow(d);
}
}  // namespace

void Game::SetResolution(int width, int height)
{
	if (width <= 0 || height <= 0) return;
	NukeWindow& win = Config::getSingleton()->window;
	win.w = width; win.h = height;
	ApplyAndSaveWindow();
}
void Game::SetWindowMode(WindowMode mode)
{
	int m = (int)mode;
	Config::getSingleton()->window.mode = std::max(0, std::min(2, m));
	ApplyAndSaveWindow();
}
void Game::SetBorderless(bool borderless)
{
	Config::getSingleton()->window.decorated = !borderless;
	ApplyAndSaveWindow();
}
void Game::SetTransparent(bool transparent)
{
	Config::getSingleton()->window.transparent = transparent;
	ApplyAndSaveWindow();
}
void Game::SetOpacity(double opacity)
{
	Config::getSingleton()->window.opacity = (float)std::max(0.0, std::min(1.0, opacity));
	ApplyAndSaveWindow();
}

void Game::SetVSync(bool on)
{
	Config* c = Config::getSingleton();
	c->window.vsync = on;
	SaveGameWindow();   // persist for next launch (project-side in the editor)
	// Unlike the geometry setters, vsync applies LIVE even in the editor — it's only the
	// present pacing of whatever window the renderer drives, not the game's window layout.
	AppInstance* app = AppInstance::GetSingleton();
	if (app->render) app->render->setVSync(on);
}

int        Game::WindowWidth()   { return Config::getSingleton()->window.w; }
int        Game::WindowHeight()  { return Config::getSingleton()->window.h; }
WindowMode Game::GetWindowMode() { return (WindowMode)Config::getSingleton()->window.mode; }
bool       Game::IsBorderless()  { return !Config::getSingleton()->window.decorated; }
bool   Game::IsTransparent() { return Config::getSingleton()->window.transparent; }
double Game::Opacity()       { return Config::getSingleton()->window.opacity; }
bool   Game::IsVSync()       { return Config::getSingleton()->window.vsync; }

// Queue the request: the capture happens at the END of World::Render (FlushScreenshot) —
// there the frame is fully drawn. Capturing here (mid-Update, before this frame renders)
// would read a stale/undefined rotated backbuffer. Mutex: the request may be set from a
// script on the update/fixed thread while the render thread consumes it.
static std::mutex gShotMx;

bool Game::Screenshot(const std::string& file)
{
	AppInstance* app = AppInstance::GetSingleton();
	if (!app->render || file.empty()) return false;
	std::lock_guard<std::mutex> lk(gShotMx);
	app->pendingScreenshot = file;
	return true;
}

// Called by World::Render after every camera finished (the image is complete). The PIE
// viewport RT in the editor (AppInstance::uiTarget), the backbuffer in the player.
void Game::FlushScreenshot()
{
	AppInstance* app = AppInstance::GetSingleton();
	std::string file;
	{
		std::lock_guard<std::mutex> lk(gShotMx);
		if (app->pendingScreenshot.empty()) return;
		file.swap(app->pendingScreenshot);
	}
	if (!app->render) return;
	int w = 0, h = 0;
	std::vector<uint8_t> rgba;
	if (!app->render->captureTarget((uint64_t)app->uiTarget, w, h, rgba) || w <= 0 || h <= 0)
	{
		std::cout << "[Game]\t\tScreenshot: capture failed" << std::endl;
		return;
	}
	std::string ext;
	{
		size_t dot = file.find_last_of('.');
		if (dot != std::string::npos) ext = file.substr(dot + 1);
		for (char& c : ext) c = (char)std::tolower((unsigned char)c);
	}
	int ok = 0;
	if (ext == "bmp")      ok = stbi_write_bmp(file.c_str(), w, h, 4, rgba.data());
	else if (ext == "tga") ok = stbi_write_tga(file.c_str(), w, h, 4, rgba.data());
	else                   ok = stbi_write_png(file.c_str(), w, h, 4, rgba.data(), w * 4);
	std::cout << "[Game]\t\tScreenshot " << (ok ? "saved: " : "FAILED: ") << file
	          << " (" << w << "x" << h << ")" << std::endl;
}

}  // namespace nuke
