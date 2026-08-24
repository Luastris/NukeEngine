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
	// Frame-rate cap, live. 0 = uncapped; vsync applies on top. Config: window.fpsLimit.
	[[nuke::func]] static void   SetFpsLimit(double fps);
	[[nuke::func]] static double GetFpsLimit();
	// Custom cursors (.nucursor flipbook assets bound to named states).
	[[nuke::func]] static bool SetCursor(const std::string& contentRel);   // bind + select "default"
	[[nuke::func]] static bool BindCursor(const std::string& state, const std::string& contentRel);
	[[nuke::func]] static void SetCursorState(const std::string& state);
	[[nuke::func]] static void SetCursorSoftware(bool on);
	[[nuke::func]] static void ResetCursor();

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
	// Overlay flags (desktop-companion class windows). All live-settable.
	[[nuke::func]] static void SetAlwaysOnTop(bool onTop);        // window stays above all others
	[[nuke::func]] static void SetClickThrough(bool through);     // clicks pass to windows beneath
	// Invisible to screenshots/recorders — the user still sees the window, capture sees what is
	// behind it. Windows + macOS; X11/Wayland have no such protocol (ignored with a log).
	[[nuke::func]] static void SetHideFromCapture(bool hide);

	// T3 texture streaming: mip-pool VRAM budget in MB (0 = off). Live; persisted like the other
	// window/config settings. Streamed textures keep a low-mip tail and stream detail by distance.
	[[nuke::func]] static void SetTextureStreaming(double budgetMB);
	// One stats line: "streamed=N resident=X.XMB full=Y.YMB saved=Z.ZMB" (probes/console).
	[[nuke::func]] static std::string TextureStreamInfo();

	// Last completed frame's render counters (probes/console).
	[[nuke::func]] static double DrawCalls();
	[[nuke::func]] static double Triangles();
	// Streaming boot: true once the atoms around the activation origin (the main camera) are in —
	// the moment a loading screen may drop while the rest of the world keeps growing.
	[[nuke::func]] static bool WorldStartZoneReady();
	// Cook a JSON document (world / cell / prefab) into the binary form Package Project ships
	// ("NCBR" + CBOR); every loader reads both. dst may equal src.
	[[nuke::func]] static bool CookDocument(const std::string& src, const std::string& dst);
	// Pack a directory into a NUPAK (entries project-relative to `root`, the registered asset
	// cooks applied): method 0 store / 1 zlib / 2 zstd / 3 gdeflate. Mod tools and probes.
	[[nuke::func]] static bool PackDirectory(const std::string& root, const std::string& outPak, int method, int level, int blockMB);
	// Direct IO (Fast loading 4): the provider in use and what it served so far, one line.
	[[nuke::func]] static std::string StorageInfo();
	// Hi-Z occlusion (R4): draws tagged / held back by the last camera; live on/off toggle.
	[[nuke::func]] static double OcclusionTracked();
	[[nuke::func]] static double OcclusionCulled();
	[[nuke::func]] static void   SetOcclusionCulling(bool on);

	[[nuke::func]] static int        WindowWidth();
	[[nuke::func]] static int        WindowHeight();
	[[nuke::func]] static WindowMode GetWindowMode();
	[[nuke::func]] static bool       IsBorderless();
	[[nuke::func]] static bool       IsTransparent();
	[[nuke::func]] static double     Opacity();
	[[nuke::func]] static bool       IsVSync();
	[[nuke::func]] static bool       IsAlwaysOnTop();
	[[nuke::func]] static bool       IsClickThrough();
	[[nuke::func]] static bool       IsHideFromCapture();

	// Queue a capture of the current game image; it happens at the end of this frame's render.
	// Format by extension (.png/.bmp/.tga, default png). Slow — GPU flush + readback.
	[[nuke::func]] static bool Screenshot(const std::string& file);
	static void FlushScreenshot();   // host-side: World::Render calls it once per frame
};

}  // namespace nuke

#endif // !NUKEE_GAME_H
