#pragma once
#ifndef NUKEE_RECTANCHOR_H
#define NUKEE_RECTANCHOR_H
#include "NukeAPI.h"
#include "Include.h"
#include "reflect/Reflect.h"

namespace nuke {

// PER-SIDE anchors for a CANVAS child: each enabled side pins the element's matching EDGE at a
// fixed distance (in canvas units: reference px on a screen canvas, world units on a world one)
// from that canvas side. The distance is captured when the anchor is enabled (the inspector shows
// the Anchors block in the Transform section for canvas children).
//
//   One side enabled (e.g. Left)          — the element keeps its size and its distance to that
//                                            side whatever the canvas/screen does.
//   Two OPPOSITE sides (Left+Right)       — both edges pinned: the element STRETCHES with the
//                                            canvas (its Sprite width/height follow the rect).
//
// Applied every frame before rendering (edit mode included), pinning the element to the canvas
// surface (its Z never drifts). Without a canvas above it the component does nothing.
class NUKEENGINE_API RectAnchor : public Component
{
	NUKE_CLASS(RectAnchor, Component)
public:
	[[nuke::prop(label="Left")]]   bool left   = false;
	[[nuke::prop(label="Right")]]  bool right  = false;
	[[nuke::prop(label="Top")]]    bool top    = false;
	[[nuke::prop(label="Bottom")]] bool bottom = false;
	// Captured edge-to-side distances (canvas units). The inspector fills them when a side is
	// toggled on; editing them nudges the element.
	[[nuke::prop(label="Dist Left")]]   float distLeft   = 0.0f;
	[[nuke::prop(label="Dist Right")]]  float distRight  = 0.0f;
	[[nuke::prop(label="Dist Top")]]    float distTop    = 0.0f;
	[[nuke::prop(label="Dist Bottom")]] float distBottom = 0.0f;

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
