#pragma once
#ifndef NUKEE_WIDGET_H
#define NUKEE_WIDGET_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"
#include "API/Model/Vector.h"

namespace nuke {

class iRender;
class Canvas;
class Camera;
class Texture;
struct NukeSpriteParams;

// The engine side of one canvas DRAW, handed to Widget::OnCanvasDraw by World::Render in every
// canvas mode (screen, the editor camera's world-plane preview, world space). The widget emits
// textured quads in its own rect space — canvas units (reference px on a screen canvas, world
// units on a world canvas), origin at the widget centre, +y up — and the engine maps them.
class NUKEENGINE_API CanvasDrawCtx
{
public:
	iRender* r = nullptr;
	Atom*    atom = nullptr;
	Canvas*  canvas = nullptr;     // the canvas the widget lives under (never null)
	Camera*  cam = nullptr;
	float    w = 0.0f, h = 0.0f;   // the widget rect in canvas units (transform scale applied)
	float    pxToUnits = 1.0f;     // one texture pixel in canvas units (glyphs, nine-slice)
	bool     editorPlane = false;  // the editor camera previewing a screen canvas on its plane
	bool     hasClip = false;      // an ancestor clips (screen canvases): Params() merges it in
	float    clip[4] = { 0, 0, 0, 0 };   // {x0, y0, x1, y1} canvas reference px, centre origin, +y up

	// A textured quad: centre offset (x, y) from the widget centre, size (qw, qh), canvas units.
	void Quad(Texture* tex, float x, float y, float qw, float qh, const float uv[4], const float tint[4]);
	// Sticky sprite parameters for the quads that follow (SDF text, clip); nullptr = defaults.
	// Always reset before returning from OnCanvasDraw.
	void Params(const NukeSpriteParams* p);

	// Mapping state, filled by the engine. mode 0 = screen (rect = widget centre in reference
	// px, refSize / queue / scaleMode per the canvas), 1 = world quad (centre + unit axes).
	int     mode = 0;
	float   rect[2] = { 0.0f, 0.0f };
	float   refSize[2] = { 0.0f, 0.0f };
	int     queue = 0;
	int     scaleMode = 0;
	int     overlay = 0;   // world quad: after post, no depth (Widget.overlay / a world canvas's AfterPost queue)
	Vector3 center, R, U;
};

// How a widget quad is oriented OUTSIDE a canvas (a canvas pins its children to its plane).
// Reflected enum (typed in C#/Lua, combo in the inspector).
enum class SpriteMode : int { Plane = 0, Billboard = 1 };
template<> struct NukeEnumInfo<SpriteMode>
{
	static constexpr bool reflected = true;
	static const char* Name() { return "SpriteMode"; }
	static void Register() { Reflect_RegisterEnum("SpriteMode", { "Plane", "Billboard" }); }
};

// Base of every canvas widget — Sprite included — and of the NukePrism widgets: a rectangle the
// engine lays out (RectAnchor), picks and asks to draw. Under a Canvas it lies in the canvas
// plane at its world offset (a screen canvas shows width/height times Pixels Per Unit); without
// one it is a world quad like a sprite: Plane in the atom's transform or a Billboard, 100 px of
// UI per world unit. Never created by name — a widget is always a concrete subclass.
class NUKEENGINE_API Widget : public Component
{
	NUKE_CLASS_NOCREATE(Widget, Component)
public:
	[[nuke::prop(label="Width")]]  float width  = 1.0f;   // world units
	[[nuke::prop(label="Height")]] float height = 1.0f;
	[[nuke::prop(label="Mode", enum="Plane,Billboard", tip="Outside a canvas: lie in the transform, or face the camera")]] SpriteMode mode = SpriteMode::Plane;
	[[nuke::prop(label="Raycast Target", tip="The pointer can hit this widget (clicks stop here)")]] bool raycastTarget = true;
	[[nuke::prop(label="Clip Children", tip="Descendants draw only inside this rect (screen canvases): masks, scroll views")]] bool clipChildren = false;
	[[nuke::prop(label="Overlay", tip="Outside a canvas: draw after post over everything (no depth test) and take the pointer through walls")]] bool overlay = false;

	explicit Widget(const char* typeName = "Widget");

	// Draw: the engine resolved the rect (ctx.w/h) and the mapping, the widget emits quads.
	virtual void OnCanvasDraw(CanvasDrawCtx& ctx) { (void)ctx; }

	[[nuke::func]] void SetSize(double w, double h);

	void Init(Atom* parent) override;
	void Update() override;
	void FixedUpdate() override;
	void Reset() override;
	void Pause() override;
	void Destroy() override;
};

}  // namespace nuke
#endif // !NUKEE_WIDGET_H
