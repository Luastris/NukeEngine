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

	// --- ASYNC world loading: hide the wait behind a transition level -----------------------
	// LoadWorldAsync starts a BACKGROUND load (content read + mod-layer merge + JSON parse —
	// the heavy part) on the engine job pool; the current world (your transition level with a
	// loading screen) keeps running. Poll LoadWorldProgress/LoadWorldReady, then call
	// ActivateLoadedWorld — the pre-parsed world swaps in at the frame boundary, so the
	// switch itself is short. A second LoadWorldAsync supersedes the first; CancelLoadWorld
	// drops the loading/staged world. Typical flow (transition-level script):
	//   start:  Game.LoadWorldAsync("Worlds/big.nuworld")
	//   update: draw Game.LoadWorldProgress(); if Game.LoadWorldReady() then Game.ActivateLoadedWorld()
	[[nuke::func]] static bool   LoadWorldAsync(const std::string& contentRelPath);
	[[nuke::func]] static double LoadWorldProgress();    // -1 = none/failed, else 0..1 (1 = staged)
	[[nuke::func]] static bool   LoadWorldReady();       // staged world awaits activation
	[[nuke::func]] static bool   ActivateLoadedWorld();  // swap at the frame boundary; false if not ready
	[[nuke::func]] static void   CancelLoadWorld();      // drop the loading/staged world

	[[nuke::func]] static bool IsEditor();     // running inside the editor host (plugins/game may branch)
	[[nuke::func]] static bool IsPlaying();    // play mode active (PIE playing / Player)
	[[nuke::func]] static bool IsPaused();     // play mode paused (PIE pause)
	// Pause/resume play mode. In edit mode (editor, not playing) this is a no-op —
	// starting/stopping PIE is the EDITOR's action, not the game's.
	[[nuke::func]] static void SetPaused(bool paused);

	// Game SPEED (colony-sim fast-forward): scales Time.Delta(), the game calendar and the
	// fixed physics cadence. 0 = frozen world that still takes orders (Update keeps running —
	// unlike SetPaused, which halts Update entirely), 1 = normal, 2/3 = fast-forward.
	// Clamped to [0..8]. Edit mode ignores the scale (previews run real-time).
	[[nuke::func]] static void   SetTimeScale(double scale);
	[[nuke::func]] static double GetTimeScale();

	// End the game: in the Player the window closes and the main loop returns (clean
	// shutdown). In the editor a game cannot close the host — logged and ignored
	// (stopping PIE is an editor command).
	[[nuke::func]] static void Quit();

	// --- SAVEGAMES (6.6): full runtime snapshots, distinct from world ASSETS ----------------
	// SaveGame serializes the RUNNING world — atoms, live script state (per each class's
	// SaveMode), tilemap layers, the game calendar and the pending event schedule — into
	// `<slot>.nusave` in the game's SAVE DIR, never into content: the editor/dev player uses
	// `<project>/saves/`, the packaged player `%APPDATA%/<game>/saves/`. LoadGame applies at
	// the frame boundary (safe from a running script). ListSaves returns newline-separated
	// slot names, newest first.
	[[nuke::func]] static bool        SaveGame(const std::string& slot);
	[[nuke::func]] static bool        LoadGame(const std::string& slot);
	[[nuke::func]] static std::string ListSaves();

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
	// Vertical sync: true = cap FPS to the display refresh (no tearing), false = uncapped.
	// Applies LIVE in both hosts (it's just present pacing) and persists for the next launch.
	[[nuke::func]] static void SetVSync(bool on);

	[[nuke::func]] static int        WindowWidth();
	[[nuke::func]] static int        WindowHeight();
	[[nuke::func]] static WindowMode GetWindowMode();
	[[nuke::func]] static bool       IsBorderless();
	[[nuke::func]] static bool       IsTransparent();
	[[nuke::func]] static double     Opacity();
	[[nuke::func]] static bool       IsVSync();

	// Save the CURRENT game image (the PIE viewport in the editor, the backbuffer in the
	// player) to an image file. QUEUED: the capture happens at the end of this frame's
	// render, when the image is complete. Format by extension: .png / .bmp / .tga (anything
	// else = png). Relative paths resolve against the working directory. SLOW (GPU flush +
	// readback) — a screenshot/photo-mode/verification facility.
	[[nuke::func]] static bool Screenshot(const std::string& file);
	static void FlushScreenshot();   // host-side: World::Render calls it once per frame
};

}  // namespace nuke

#endif // !NUKEE_GAME_H
