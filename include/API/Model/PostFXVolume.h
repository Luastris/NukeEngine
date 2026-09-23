#pragma once
#ifndef NUKEE_POSTFXVOLUME_H
#define NUKEE_POSTFXVOLUME_H
#include "API/Model/Include.h"
#include "API/Model/Vector.h"
#include "API/Model/PostProcess.h"
#include "reflect/Reflect.h"
#include <vector>

namespace nuke {

// Post-effect volume (E2): a camera inside the shape has this volume's settings BLENDED over its
// own — the SDR tonemap's exposure / white point and the parameters of the effects in the camera's
// post chain (matched by shader; an effect the camera does not run is not added). Outside the
// shape the influence fades over the blend distance; several volumes stack by priority.
class NUKEENGINE_API PostFXVolume : public Component
{
	NUKE_CLASS(PostFXVolume, Component, "Rendering")
public:
	[[nuke::prop(label="Shape", enum="Sphere,Box")]]   int     shape = 1;
	[[nuke::prop(label="Radius", min=0)]]              float   radius = 10.0f;                 // sphere
	[[nuke::prop(label="Half Extents")]]               Vector3 halfExtents = Vector3(5, 5, 5);   // box (local)
	[[nuke::prop(label="Priority", tip="Higher wins where volumes overlap (blended in ascending order).")]] int priority = 0;
	[[nuke::prop(label="Blend Distance", min=0, tip="Metres outside the shape over which the influence fades to nothing.")]] float blendDistance = 2.0f;
	[[nuke::prop(label="Weight", min=0, max=1, tip="Full influence inside the shape (1 = the volume's values replace the camera's).")]] float weight = 1.0f;
	[[nuke::prop(label="Override Exposure", tip="Blend the SDR tonemap exposure toward the value below (HDR projects).")]] bool overrideExposure = false;
	[[nuke::prop(label="Exposure", min=0, max=8)]]     float   exposure = 1.0f;
	[[nuke::prop(label="Override White Point", tip="Blend the SDR tonemap white point toward the value below.")]] bool overrideWhitePoint = false;
	[[nuke::prop(label="White Point", min=0.1, max=8)]] float  whitePoint = 1.0f;
	[[nuke::prop(hidden)]] std::string effectsData;   // the effect overrides (JSON, the PostProcess chain format); edited via the custom inspector

	std::vector<PostEffect> effects;   // runtime overrides (parsed from effectsData): shader + the params to blend toward

	PostFXVolume();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	void EnsureParsed();
	void Commit();

	// Influence 0..1 of this volume at a world point: 1 inside, fading over the blend distance.
	float WeightAt(const Vector3& p) const;
	// Every live volume (registered by Init, dropped by Destroy).
	static const std::vector<PostFXVolume*>& All();

private:
	std::string parsedFrom;
};
}  // namespace nuke

#endif // !NUKEE_POSTFXVOLUME_H
