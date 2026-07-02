#include "API/Model/Game.h"
#include "API/Model/World.h"
#include "interface/AppInstance.h"
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

}  // namespace nuke
