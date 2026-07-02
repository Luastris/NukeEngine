#pragma once
#ifndef NUKEE_GAME_H
#define NUKEE_GAME_H
#include "NukeAPI.h"
#include <string>

namespace nuke {

class World;

// The game-side runtime facade: what gameplay code (C++ components, scripts via the
// bindings) uses to talk to the HOST — current world, world switching, play state,
// quitting — without touching editor internals. Thin static wrapper over
// AppInstance/iRender; behaves sensibly in BOTH hosts (editor PIE and Player).
class NUKEENGINE_API Game
{
public:
	static World* GetWorld();      // the currently loaded world

	// Switch to another world from the project content (content-relative path, e.g.
	// "Worlds/level2.nuworld"). Same path the Player uses at boot (AppInstance::OpenWorld).
	static bool LoadWorld(const std::string& contentRelPath);

	static bool IsEditor();        // running inside the editor host (plugins/game may branch)
	static bool IsPlaying();       // play mode active (PIE playing / Player)
	static bool IsPaused();        // play mode paused (PIE pause)
	// Pause/resume play mode. In edit mode (editor, not playing) this is a no-op —
	// starting/stopping PIE is the EDITOR's action, not the game's.
	static void SetPaused(bool paused);

	// End the game: in the Player the window closes and the main loop returns (clean
	// shutdown). In the editor a game cannot close the host — logged and ignored
	// (stopping PIE is an editor command).
	static void Quit();
};

}  // namespace nuke

#endif // !NUKEE_GAME_H
