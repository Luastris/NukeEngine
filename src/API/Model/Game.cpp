#include "API/Model/Game.h"
#include "API/Model/World.h"
#include "API/Model/Time.h"
#include "interface/AppInstance.h"
#include "config.h"
#include "render/irender.h"
#include <boost/filesystem.hpp>   // project dir for the game's window.json (editor)
#include <algorithm>
#include <iostream>

namespace nuke {

World* Game::GetWorld() { return AppInstance::GetSingleton()->currentWorld; }

bool Game::LoadWorld(const std::string& contentRelPath)
{
	return AppInstance::GetSingleton()->OpenWorld(contentRelPath);
}

bool Game::IsEditor()  { return AppInstance::GetSingleton()->isEditor(); }
bool Game::IsPlaying() { return AppInstance::GetSingleton()->playState == 1; }
bool Game::IsPaused()  { return AppInstance::GetSingleton()->playState == 2; }

void Game::SetPaused(bool paused)
{
	AppInstance* app = AppInstance::GetSingleton();
	if (app->playState == 0) return;   // edit mode: PIE start/stop is the editor's call
	app->playState = paused ? 2 : 1;
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

}  // namespace nuke
