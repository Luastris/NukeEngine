#pragma once
#ifndef NUKEE_POSECLONE_H
#define NUKEE_POSECLONE_H
#include "NukeAPI.h"
#include "API/Model/Component.h"
#include "API/Model/Animator.h"
#include "API/Model/Vector.h"
#include "reflect/Reflect.h"
#include <memory>
#include <string>

namespace nuke {

class Atom;

// Drives this rig from another one every frame through the Animator's pose-cloning API:
// the source atom plays its clips natively on its own skeleton, this atom strikes the same
// pose. On top of the API: the late tick (after every Animator committed), Hide Source,
// root motion transfer and a stored, editable alignment pose. Needs an Animator on this atom.
class NUKEENGINE_API PoseClone : public Component
{
	NUKE_CLASS(PoseClone, Component, "Animation")
public:
	[[nuke::prop(asset="Animator", widget="foreign", label="Source", tip="Atom whose skeleton the pose is copied FROM every frame (its Animator plays the clips natively).")]] Atom* source = nullptr;
	[[nuke::prop(asset="bonemap", label="Bone Map", tip="Explicit source->target bone renames (.nubonemap). Identical names and same-named rig chains pair automatically.")]] std::string boneMapGuid;
	[[nuke::prop(label="Hide Source", tip="Skip the source's renderers while this component drives (runtime only, nothing is written into the source). Off = the source stays visible for side-by-side checks.")]] bool hideSource = true;
	[[nuke::prop(label="Auto Align", tip="Build the alignment pose on bind when none is stored: every paired bone is turned so its segment points where the source's does (UE 'Align All Bones').")]] bool autoAlign = true;
	[[nuke::prop(enum="None,Root Scaled,All Scaled", label="Translation", tip="Root Scaled: the pelvis travel scaled by the rigs' height ratio (UE 'Globally Scaled'). All Scaled: every paired bone's travel.")]] int translation = 1;
	[[nuke::prop(min=0, max=1, label="Weight", tip="Blend against the rig's own pose (its Animator or bind).")]] float weight = 1.0f;
	[[nuke::prop(label="Root Motion", tip="Move THIS atom by the travel the source extracts (its Animator's Root Motion must be on) and pin the source in place: a nested source stays under the character, a separate one treads in place.")]] bool rootMotion = true;
	// The alignment pose: per-bone LOCAL rotation offsets on top of the target bind
	// ({"bone":[x,y,z,w]}). Filled by AutoAlign / SetBoneOffset; empty = bind.
	[[nuke::prop(hidden)]] std::string poseJson;

	// Rebuild the alignment pose from the bind directions (UE 'Align All Bones'), stored in poseJson.
	[[nuke::func]] void    AutoAlign();
	// Edit one bone of the alignment pose: a local rotation offset (euler degrees) over its bind.
	[[nuke::func]] void    SetBoneOffset(const std::string& bone, const Vector3& eulerDeg);
	// The stored offset of a bone, euler degrees (zero when unset).
	[[nuke::func]] Vector3 BoneOffset(const std::string& bone);
	// Alignment pose = plain bind (drops every offset; Auto Align rebuilds it on the next frame when on).
	[[nuke::func]] void    ClearOffsets();
	// Drop the cached pairing (after a skeleton or bone-map change).
	[[nuke::func]] void    Rebind();
	// Paired bone count (0 = not bound yet: no source, no skeleton, nothing pairs).
	[[nuke::func]] double  PairedCount();
	// The source bone driving a target bone, "" when unpaired.
	[[nuke::func]] std::string PairOf(const std::string& targetBone);

	PoseClone() : Component("PoseClone") {}
	~PoseClone();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override { Rebind(); }
	void LateUpdate() override;                // after every Animator committed its pose

private:
	std::shared_ptr<Animator::PoseBind> bind;  // the Animator API's bind, rebuilt on demand
	Animator* Host();                          // the Animator on this atom (the API host)
	bool EnsureBind();
	void ApplyRootMotion();                    // source travel since the pin -> this atom; source re-pinned
	bool warned = false;
	Atom* hidden = nullptr;                    // the source currently hidden by this component
	Atom* pinAtom = nullptr;                   // the source the pin below belongs to
	Vector3 pinPos; Quaternion pinRot;         // source LOCAL transform the travel is measured from
};

}  // namespace nuke

#endif // !NUKEE_POSECLONE_H
