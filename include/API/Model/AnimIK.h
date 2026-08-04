#pragma once
#ifndef NUKEE_ANIMIK_H
#define NUKEE_ANIMIK_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

class Animator;
class Atom;

// Foot placement on top of the Animator's pose: raycasts under each foot chain (from the
// skeleton's IK rig), lowers the PELVIS so the deepest foot can reach, pulls each foot to
// its ground height and rolls it onto the surface normal. Sits NEXT TO the Animator; the
// Animator calls ApplyTo() every posed frame between sampling and the IK solve, so the
// raycasts see the CLEAN animation pose (no feedback). Needs a physics provider.
class NUKEENGINE_API FootIK : public Component
{
	NUKE_CLASS(FootIK, Component, "Animation")
public:
	// IK-rig chain names; empty = auto-pick the first two chains whose names contain
	// left/right (l_/r_, .l/.r ... markers).
	[[nuke::prop(label="Left Chain")]]  std::string leftChain;
	[[nuke::prop(label="Right Chain")]] std::string rightChain;
	[[nuke::prop(min=0, max=2,  label="Ray Up",   tip="Raycast start height above the animated foot.")]]   float rayUp = 0.5f;
	[[nuke::prop(min=0, max=5,  label="Ray Down", tip="Raycast length below the animated foot.")]]         float rayDown = 1.5f;
	[[nuke::prop(min=0, max=0.5, label="Foot Height", tip="Sole thickness: the foot rests this far above the hit surface.")]] float footHeight = 0.05f;
	[[nuke::prop(min=0, max=2,  label="Max Pelvis Drop", tip="How far the hips may sink so a low foot can reach.")]] float maxDrop = 0.6f;
	[[nuke::prop(min=0, max=1,  label="Step Ignore", tip="A foot this far above its ground is mid-step: it neither drops the pelvis nor gets pulled down.")]] float stepIgnore = 0.25f;
	[[nuke::prop(min=0, max=1,  label="Weight")]] float weight = 1.0f;
	[[nuke::prop(min=0, max=30, label="Pelvis Smoothing", tip="Exponential smoothing rate of the pelvis offset (0 = instant).")]] float pelvisSmooth = 12.0f;
	[[nuke::prop(label="Align To Normal", tip="Roll each planted foot onto the surface normal.")]] bool alignToNormal = true;

	FootIK();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	// Called by the Animator between pose sampling and its IK solve. `globals` = the clean
	// pre-IK model-space bone matrices (16 floats each, column-major), `nb` bones.
	void ApplyTo(Animator* an, const float* globals, int nb);

private:
	double pelvisCur = 0.0;   // smoothed pelvis offset
	bool   active = false;    // goals were set last frame (clear once on release)
};

// Aims a spine chain at a target atom through Animator::SetLookAt every frame: heads track
// the player, turrets track threats. Chain from the skeleton's IK rig (or a bare bone name).
class NUKEENGINE_API LookAtIK : public Component
{
	NUKE_CLASS(LookAtIK, Component, "Animation")
public:
	[[nuke::prop(label="Chain", tip="IK-rig chain (spine -> head) or a single bone name.")]] std::string chain;
	[[nuke::prop(label="Target", tip="Atom to look at; none = look-at off.")]] Atom* target = nullptr;
	[[nuke::prop(min=0, max=1, label="Weight")]] float weight = 1.0f;
	[[nuke::prop(min=0, max=180, label="Max Angle", tip="Total turn cap across the chain, degrees.")]] float maxAngle = 75.0f;

	LookAtIK();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
};

}  // namespace nuke

#endif // !NUKEE_ANIMIK_H
