#pragma once
#ifndef NUKEE_MOTIONMATCHER_H
#define NUKEE_MOTIONMATCHER_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <string>
#include <vector>

namespace nuke {

class Animator;
class AnimClip;

// Motion matching over a locomotion clip set: a feature DB (foot positions from the IK-rig
// leg chains, pelvis velocity, past+future root trajectory — all in the pelvis frame) is
// built once from `clips`; every search interval the desired trajectory (SetDesiredVelocity)
// plus the CURRENT pose is matched against the DB and playback jumps to the best sample via
// an inertialized MatchTo. Sits next to the Animator (legacy drive mode, no controller).
class NUKEENGINE_API MotionMatcher : public Component
{
	NUKE_CLASS(MotionMatcher, Component, "Animation")
public:
	[[nuke::prop(asset="anim", label="Clips", tip="Locomotion set (idle/walk/run...); clips need a moving root for trajectories.")]] std::vector<std::string> clips;
	[[nuke::prop(min=2, max=60, label="Sample Rate", tip="DB samples per clip second.")]] float sampleRate = 10.0f;
	[[nuke::prop(min=0.02, max=1, label="Search Interval", tip="Seconds between DB queries.")]] float searchInterval = 0.15f;
	[[nuke::prop(min=0, max=1, label="Blend", tip="Inertialized blend seconds per jump.")]] float blendTime = 0.25f;
	[[nuke::prop(min=0, max=5, label="Pose Weight")]]       float poseWeight = 1.0f;
	[[nuke::prop(min=0, max=5, label="Velocity Weight")]]   float velocityWeight = 1.0f;
	[[nuke::prop(min=0, max=5, label="Trajectory Weight")]] float trajectoryWeight = 1.5f;
	// Jumps within this time of the CURRENT playhead (same clip) are free — no re-blend spam.
	[[nuke::prop(min=0, max=1, label="Keep Window", tip="Seconds around the current playhead that count as 'already there'.")]] float keepWindow = 0.25f;

	MotionMatcher();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	// Desired velocity, WORLD units/s (a controller feeds it; zero = settle into idle-ish data).
	[[nuke::func]] void SetDesiredVelocity(const Vector3& v);
	// The clip the matcher is currently inside + the DB size (diagnostics).
	[[nuke::func]] std::string MatchedClip();
	[[nuke::func]] double SampleCount();

private:
	struct Sample
	{
		int    clip = 0;                 // index into resolvedClips
		float  t = 0;                    // clip time
		float  footL[3] = { 0, 0, 0 };   // pelvis-frame foot positions
		float  footR[3] = { 0, 0, 0 };
		float  vel[3] = { 0, 0, 0 };     // pelvis velocity, pelvis frame
		float  traj[6] = { 0, 0, 0, 0, 0, 0 };   // root XZ at t+0.3 / t+0.6 rel. to t
	};
	std::vector<Sample> db;
	std::vector<AnimClip*> resolvedClips;
	bool   dbBuilt = false;
	double sinceSearch = 0.0;
	Vector3 desired;                     // desired world velocity
	int    curSample = -1;

	void   EnsureDB(Animator* an);
	double Cost(const Sample& s, const float curL[3], const float curR[3],
	            const float curVel[3], const float wantTraj[6]) const;
};

}  // namespace nuke

#endif // !NUKEE_MOTIONMATCHER_H
