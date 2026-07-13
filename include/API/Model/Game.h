#pragma once
#ifndef NUKEE_GAME_H
#define NUKEE_GAME_H
#include "NukeAPI.h"
#include "World.h"   // reflected: GetWorld() returns the World OBJECT to scripts
#include "config.h"  // nuke::WindowMode (the typed window API)
#include <string>

namespace nuke {

// WindowMode is a REFLECTED enum: SetWindowMode/WindowMode below take/return it, and the
// bindings emit a real `enum WindowMode` (C#) / `nuke.WindowMode` table (Lua). Labels match
// the enumerators (see config.h). Specialize here so it's visible where MakeMethod for Game
// is instantiated (Reflect.gen.cpp includes Game.h).
template<> struct NukeEnumInfo<WindowMode>
{
	static constexpr bool reflected = true;
	static const char* Name() { return "WindowMode"; }
	static void Register() { Reflect_RegisterEnum("WindowMode", { "Windowed", "BorderlessFullscreen", "ExclusiveFullscreen" }); }
};

// The game-side runtime facade: what gameplay code (C++ components, scripts via the
// bindings) uses to talk to the HOST — current world, world switching, play state,
// quitting — without touching editor internals. Thin static wrapper over
// AppInstance/iRender; behaves sensibly in BOTH hosts (editor PIE and Player).
// [[nuke::func]]-reflected: every backend binds it automatically (Lua nuke.Game.*, C# Game.*).
class NUKEENGINE_API Game
{
	NUKE_CLASS_NOCREATE(Game, Object)
public:
	[[nuke::func]] static World* GetWorld();   // the currently loaded world

	// Switch to another world from the project content (content-relative path, e.g.
	// "Worlds/level2.nuworld"). Same path the Player uses at boot (AppInstance::OpenWorld).
	[[nuke::func]] static bool LoadWorld(const std::string& contentRelPath);

	[[nuke::func]] static bool IsEditor();     // running inside the editor host (plugins/game may branch)
	[[nuke::func]] static bool IsPlaying();    // play mode active (PIE playing / Player)
	[[nuke::func]] static bool IsPaused();     // play mode paused (PIE pause)
	// Pause/resume play mode. In edit mode (editor, not playing) this is a no-op —
	// starting/stopping PIE is the EDITOR's action, not the game's.
	[[nuke::func]] static void SetPaused(bool paused);

	// End the game: in the Player the window closes and the main loop returns (clean
	// shutdown). In the editor a game cannot close the host — logged and ignored
	// (stopping PIE is an editor command).
	[[nuke::func]] static void Quit();

	// --- WINDOW control (a game's video-settings menu) -------------------------------------
	// Every setter: updates the window CONFIG, PERSISTS it (config/main.json's window block,
	// leaving the rest intact) so the NEXT launch honours it, and applies it LIVE through the
	// renderer. In the editor the live change is skipped (that window is the editor's) but the
	// config is still written for the game. `transparent` is a per-pixel creation property —
	// it can't toggle live, so it takes effect on the next launch.
	[[nuke::func]] static void SetResolution(int width, int height);
	// Windowed / borderless-fullscreen / exclusive-fullscreen (see WindowMode in config.h).
	[[nuke::func]] static void SetWindowMode(WindowMode mode);
	[[nuke::func]] static void SetBorderless(bool borderless);   // windowed decoration on/off
	// Per-pixel transparency to the desktop (a DirectComposition swap chain with premultiplied
	// alpha). The swap chain's alpha mode is fixed at creation, so this persists the choice and
	// takes effect on the NEXT launch (the renderer then composes the window transparent).
	[[nuke::func]] static void SetTransparent(bool transparent);
	[[nuke::func]] static void SetOpacity(double opacity);        // whole-window 0..1 (live)

	[[nuke::func]] static int        WindowWidth();
	[[nuke::func]] static int        WindowHeight();
	[[nuke::func]] static WindowMode GetWindowMode();
	[[nuke::func]] static bool       IsBorderless();
	[[nuke::func]] static bool       IsTransparent();
	[[nuke::func]] static double     Opacity();
};

}  // namespace nuke

#endif // !NUKEE_GAME_H
