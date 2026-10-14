#pragma once
#ifndef NUKEE_RECTANCHOR_H
#define NUKEE_RECTANCHOR_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"

namespace nuke {
class Canvas;

// PER-SIDE anchors for a CANVAS child: each enabled side pins the element's matching EDGE at a
// fixed distance (canvas units) from that canvas side. Two OPPOSITE sides make the element
// stretch. Applied every frame before rendering; does nothing without a canvas above it.
// The anchors FOLLOW the element: when something else moves or resizes it (the editor gizmo or
// inspector, a tween, a script) the distances are re-captured from the new pose, so the layout
// only ever moves an element when the CANVAS changes.
class NUKEENGINE_API RectAnchor : public Component
{
	NUKE_CLASS(RectAnchor, Component, "UI & 2D")
public:
	[[nuke::prop(label="Left")]]   bool left   = false;
	[[nuke::prop(label="Right")]]  bool right  = false;
	[[nuke::prop(label="Top")]]    bool top    = false;
	[[nuke::prop(label="Bottom")]] bool bottom = false;
	// Captured edge-to-side distances (canvas units).
	[[nuke::prop(label="Dist Left")]]   float distLeft   = 0.0f;
	[[nuke::prop(label="Dist Right")]]  float distRight  = 0.0f;
	[[nuke::prop(label="Dist Top")]]    float distTop    = 0.0f;
	[[nuke::prop(label="Dist Bottom")]] float distBottom = 0.0f;

	// The element measured against its nearest canvas, in canvas units with the origin at the
	// canvas centre: reference px on a screen canvas, world units on a world canvas.
	struct Frame { Canvas* canvas = nullptr; bool world = false; float ppu = 1.0f, hw = 0.0f, hh = 0.0f, cx = 0.0f, cy = 0.0f, w = 0.0f, h = 0.0f; };
	bool Measure(Frame& f) const;   // false without a canvas above
	// Distances of the pinned sides := from the element's CURRENT pose and size.
	void CaptureFromPose();
	// Layout cache (not serialized): the canvas-unit pose the layout pass last wrote. A
	// different pose next frame means something else moved the element -> re-capture.
	float lastCx = 0.0f, lastCy = 0.0f, lastW = 0.0f, lastH = 0.0f;
	bool  laidOut = false;

	RectAnchor();
	void Init(Atom* parent) override;
	void Update() override;
	void FixedUpdate() override;
	void Reset() override;
	void Pause() override;
	void Destroy() override;
};

}  // namespace nuke
#endif // !NUKEE_RECTANCHOR_H
