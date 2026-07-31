#pragma once
#ifndef NUKEE_GAME_H
#define NUKEE_GAME_H
#include "NukeAPI.h"
#include "World.h"   // reflected: GetWorld() returns the World OBJECT to scripts
#include "config.h"  // nuke::WindowMode (the typed window API)
#include <string>

namespace nuke {

// Reflected WindowMode enum. Must be specialized here so it is visible where MakeMethod for Game
// is instantiated (Reflect.gen.cpp includes Game.h); labels must match the enumerators in config.h.
template<> struct NukeEnumInfo<WindowMode>
{
	static constexpr bool reflected = true;
	static const char* Name() { return "WindowMode"; }
	static void Register() { Reflect_RegisterEnum("WindowMode", { "Windowed", "BorderlessFullscreen", "ExclusiveFullscreen" }); }
};

// Game-side runtime facade over the host: current world, world switching, play state, window,
// savegames, quitting. Behaves sensibly in both hosts (editor PIE and Player).
class NUKEENGINE_API Game
{
	NUKE_CLASS_NOCREATE(Game, Object)
public:
	[[nuke::func]] static World* GetWorld();   // the currently loaded world

	// Switch to another world from project content (content-relative path, e.g. "Worlds/level2.nuworld").
	[[nuke::func]] static bool LoadWorld(const std::string& contentRelPath);

	// --- ASYNC world loading ----
	// Background load on the job pool while the current world keeps running; poll
	// LoadWorldProgress/LoadWorldReady, then ActivateLoadedWorld to swap at the frame boundary.
	[[nuke::func]] static bool   LoadWorldAsync(const std::string& contentRelPath);
	[[nuke::func]] static double LoadWorldProgress();    // -1 = none/failed, else 0..1 (1 = staged)
	[[nuke::func]] static bool   LoadWorldReady();       // staged world awaits activation
	[[nuke::func]] static bool   ActivateLoadedWorld();  // swap at the frame boundary; false if not ready
	[[nuke::func]] static void   CancelLoadWorld();      // drop the loading/staged world

	// --- INCREMENTAL activation ----
	// With a budget (ms of instantiation per frame) ActivateLoadedWorld streams root atoms in over
	// several frames, optionally ordered outward from the activation origin. Budget 0 = all at once.
	// Emits "world.atomActivated" {"id","name"} per root atom and "world.activationComplete" {"path"}.
	[[nuke::func]] static void   SetWorldActivationBudget(double msPerFrame);
	[[nuke::func]] static double GetWorldActivationBudget();
	[[nuke::func]] static void   SetWorldActivationOrigin(const Vector3& worldPos);
	[[nuke::func]] static void   ClearWorldActivationOrigin();
	[[nuke::func]] static double WorldActivationProgress();   // -1 = not growing, else 0..1 instantiated

	[[nuke::func]] static bool IsEditor();     // running inside the editor host (plugins/game may branch)
	[[nuke::func]] static bool IsPlaying();    // play mode active (PIE playing / Player)
	[[nuke::func]] static bool IsPaused();     // play mode paused (PIE pause)
	[[nuke::func]] static void SetPaused(bool paused);   // no-op in edit mode

	// Game speed: scales Time.Delta(), the game calendar and the fixed physics cadence.
	// 0 = frozen but Update still runs (unlike SetPaused), 1 = normal. Clamped to [0..8];
	// edit mode ignores the scale.
	[[nuke::func]] static void   SetTimeScale(double scale);
	[[nuke::func]] static double GetTimeScale();

	[[nuke::func]] static void Quit();   // closes the Player window; ignored in the editor

	// --- SAVEGAMES: runtime snapshots, distinct from world assets ----
	// SaveGame writes the running world (atoms, script state, tilemaps, calendar, event schedule)
	// to `<slot>.nusave` in the save dir. LoadGame applies at the frame boundary. ListSaves returns
	// newline-separated slot names, newest first.
	[[nuke::func]] static bool        SaveGame(const std::string& slot);
	[[nuke::func]] static bool        LoadGame(const std::string& slot);
	[[nuke::func]] static std::string ListSaves();

	// --- WINDOW control ----
	// Every setter updates + persists the window config and applies it live through the renderer;
	// in the editor the live change is skipped but the config is still written for the game.
	[[nuke::func]] static void SetResolution(int width, int height);
	[[nuke::func]] static void SetWindowMode(WindowMode mode);
	[[nuke::func]] static void SetBorderless(bool borderless);   // windowed decoration on/off
	// Per-pixel desktop transparency. The swap chain alpha mode is fixed at creation, so this
	// only takes effect on the NEXT launch.
	[[nuke::func]] static void SetTransparent(bool transparent);
	[[nuke::func]] static void SetOpacity(double opacity);        // whole-window 0..1 (live)
	[[nuke::func]] static void SetVSync(bool on);                 // cap FPS to display refresh

	[[nuke::func]] static int        WindowWidth();
	[[nuke::func]] static int        WindowHeight();
	[[nuke::func]] static WindowMode GetWindowMode();
	[[nuke::func]] static bool       IsBorderless();
	[[nuke::func]] static bool       IsTransparent();
	[[nuke::func]] static double     Opacity();
	[[nuke::func]] static bool       IsVSync();

	// Queue a capture of the current game image; it happens at the end of this frame's render.
	// Format by extension (.png/.bmp/.tga, default png). Slow — GPU flush + readback.
	[[nuke::func]] static bool Screenshot(const std::string& file);
	static void FlushScreenshot();   // host-side: World::Render calls it once per frame
};

}  // namespace nuke

#endif // !NUKEE_GAME_H
