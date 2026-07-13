#include "API/Model/Game.h"
#include "API/Model/World.h"
#include "interface/AppInstance.h"
#include "config.h"
#include "render/irender.h"
#include <algorithm>
#include <iostream>

namespace nuke {

World* Game::GetWorld() { return AppInstance::GetSingleton()->currentScene; }

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
// Persist the window config + apply it to the live window. Live-apply is skipped in the
// editor (the window there is the EDITOR's, not the game's), but the config IS saved so the
// packaged game / next launch picks it up.
void ApplyAndSaveWindow()
{
	Config* c = Config::getSingleton();
	NukeWindow& win = c->window;
	win.fullscreen = win.mode != 0;   // keep the legacy mirror consistent
	c->saveWindow();

	AppInstance* app = AppInstance::GetSingleton();
	if (app->isEditor())
	{
		std::cout << "[Game]\t\twindow config saved (live change skipped in the editor; applies to the game)" << std::endl;
		return;
	}
	if (!app->render) return;
	WindowDesc d;
	d.w = win.w; d.h = win.h; d.title = win.title.c_str();
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

int        Game::WindowWidth()   { return Config::getSingleton()->window.w; }
int        Game::WindowHeight()  { return Config::getSingleton()->window.h; }
WindowMode Game::GetWindowMode() { return (WindowMode)Config::getSingleton()->window.mode; }
bool       Game::IsBorderless()  { return !Config::getSingleton()->window.decorated; }
bool   Game::IsTransparent() { return Config::getSingleton()->window.transparent; }
double Game::Opacity()       { return Config::getSingleton()->window.opacity; }

}  // namespace nuke
