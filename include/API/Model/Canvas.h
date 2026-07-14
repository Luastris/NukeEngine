#pragma once
#ifndef NUKEE_CANVAS_H
#define NUKEE_CANVAS_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"

namespace nuke {

// How a Canvas and its sprite/UI children are rendered. Reflected enum (typed in C#/Lua).
enum class CanvasMode : int { WorldSpace = 0, ScreenSpaceOverlay = 1, ScreenSpaceCamera = 2 };
template<> struct NukeEnumInfo<CanvasMode>
{
	static constexpr bool reflected = true;
	static const char* Name() { return "CanvasMode"; }
	static void Register() { Reflect_RegisterEnum("CanvasMode", { "WorldSpace", "ScreenSpaceOverlay", "ScreenSpaceCamera" }); }
};

// WHEN a screen-space canvas is drawn relative to post-processing (the dev's choice — a HUD can be
// crisp, or deliberately mangled by the game's post effects). WorldSpace canvases are always part of
// the scene (WithWorld) — their sprites are world objects and obey the world's rules.
enum class CanvasQueue : int { WithWorld = 0, AfterPost = 1 };
template<> struct NukeEnumInfo<CanvasQueue>
{
	static constexpr bool reflected = true;
	static const char* Name() { return "CanvasQueue"; }
	static void Register() { Reflect_RegisterEnum("CanvasQueue", { "WithWorld", "AfterPost" }); }
};

// A 2D layout container for Sprites (and future UI) — a scene-graph "canvas" (Unity-uGUI style),
// independent of the code-driven runtime GUI (NukeGUI). Sprite children (atoms parented under the
// canvas) anchor within its rectangle.
//   WorldSpace          — the canvas is a rectangle in the world; its sprites render as normal world
//                          geometry (2D games under an ortho camera, or diegetic UI).
//   ScreenSpaceOverlay  — children render in reference pixels scaled to the window, on top of the
//                          scene (a HUD). (renderer overlay pass)
//   ScreenSpaceCamera   — like Overlay but drawn over a specific camera's output.
class NUKEENGINE_API Canvas : public Component
{
	NUKE_CLASS(Canvas, Component)
public:
	[[nuke::prop(label="Mode", enum="WorldSpace,ScreenSpaceOverlay,ScreenSpaceCamera")]] CanvasMode mode = CanvasMode::WorldSpace;
	// Render queue for SCREEN-space canvases: WithWorld = drawn with the scene BEFORE post effects
	// (so post can distort the UI); AfterPost = drawn crisp on the final image. Ignored for WorldSpace.
	[[nuke::prop(label="Render Queue", enum="WithWorld,AfterPost")]] CanvasQueue renderQueue = CanvasQueue::AfterPost;
	// WorldSpace: rectangle size in WORLD units. ScreenSpace: reference resolution in PIXELS (the
	// canvas scales to fit the actual render target, so a layout authored at this size stays put).
	[[nuke::prop(label="Width")]]  float width  = 10.0f;
	[[nuke::prop(label="Height")]] float height = 10.0f;
	[[nuke::prop(label="Sort Order")]] int sortOrder = 0;   // draw order among canvases (higher = on top)

	Canvas();
	void Init(Atom* parent) override;
	void Update() override;
	void FixedUpdate() override;
	void Reset() override;
	void Pause() override;
	void Destroy() override;
};
}  // namespace nuke

#endif // !NUKEE_CANVAS_H
