#pragma once
#ifndef NUKEE_SKINNEDMESHRENDERER_H
#define NUKEE_SKINNEDMESHRENDERER_H
#include "API/Model/MeshRenderer.h"
#include "API/Model/Skeleton.h"
#include "API/Model/Vector.h"
#include <string>
#include <vector>

namespace nuke {

// Skinned mesh on a SKELETON ASSET (.nuskel): the pose palette lives HERE — no bone atoms,
// ever. The Animator (or scripts) writes local bone poses; ApplyPose() runs the forward
// pass, skins on the job pool into an instance-owned mesh (this->mesh) and caches bone
// globals for sockets/attachments/gizmos. Multiple SkinnedMeshRenderers may share one
// skeleton asset (modular characters); each keeps its own pose.
class NUKEENGINE_API SkinnedMeshRenderer : public MeshRenderer
{
	NUKE_CLASS(SkinnedMeshRenderer, MeshRenderer, "Rendering")
public:
	// Explicit skeleton ref; empty = the mesh asset's own skelGuid (the import default).
	[[nuke::prop(asset="skeleton", label="Skeleton")]] std::string skelGuid;
	// Blend-shape weights, one per mesh morph target (mesh order); sized lazily.
	[[nuke::prop(label="Morph Weights", tip="Blend-shape weights, one per target (mesh order).")]] std::vector<float> morphWeights;

	Skeleton* skeleton = nullptr;   // resolved lazily (EnsureSkeleton)

	// Local pose palette, one TRS per skeleton bone; initialized to bind.
	struct BonePose
	{
		float pos[3]   = { 0, 0, 0 };
		float rot[4]   = { 0, 0, 0, 1 };   // (x, y, z, w)
		float scale[3] = { 1, 1, 1 };
	};
	std::vector<BonePose> pose;

	SkinnedMeshRenderer();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Reset() override;

	// Resolve skelGuid (falling back to the mesh's skelGuid) and size `pose` to the bind
	// pose on first contact. Returns the skeleton or null.
	Skeleton* EnsureSkeleton();
	// Forward pass over `pose` + CPU skinning into the instance mesh (job pool) + cached
	// bone globals. Call after writing poses; the Animator calls it every sampled frame.
	// (CPU LBS is stage-2 plumbing — stage 3 moves vertex skinning to a compute pre-pass.)
	void ApplyPose();

	// Model-space bone global (16 floats, column-major) from the LAST ApplyPose. False when
	// the bone is unknown or no pose was applied yet (bind is applied on EnsureSkeleton).
	bool BoneGlobal(const std::string& bone, float out[16]);
	// WORLD-space poses (atom transform composed in).
	bool SocketWorld(const std::string& socket, Vector3& outPos, Quaternion& outRot);
	bool BoneWorld(const std::string& bone, Vector3& outPos, Quaternion& outRot);

	// --- script surface (auto-bound) ---
	[[nuke::func]] void ResetPose();                                        // back to bind
	[[nuke::func]] void SetMorphWeight(const std::string& morph, double w); // by target name
	[[nuke::func]] double MorphWeight(const std::string& morph);
	[[nuke::func]] std::string MorphNames();                                // ';'-joined target names
	[[nuke::func]] void SetBonePosition(const std::string& bone, const Vector3& p);
	[[nuke::func]] void SetBoneRotation(const std::string& bone, const Quaternion& q);
	[[nuke::func]] void Apply();                                            // = ApplyPose()
	[[nuke::func]] Vector3    BonePosition(const std::string& bone);        // world
	[[nuke::func]] Vector3    SocketPosition(const std::string& socket);    // world
	[[nuke::func]] Quaternion SocketRotation(const std::string& socket);    // world

	// Bone globals of the last ApplyPose (16 floats per bone, model space) — gizmos/attach.
	const std::vector<float>& Globals() const { return globals; }

private:
	std::vector<float> globals;     // 16 * bones
	Mesh* srcMesh = nullptr;        // bind-pose asset (this->mesh points at the skinned instance)
	Mesh* skinnedMesh = nullptr;    // owned instance (pos/nrm own; uv/idx/etc shared with source)
	std::vector<float> morphScratchP, morphScratchN;   // CPU fallback: morphed bind streams
	bool EnsureInstance();          // resolve srcMesh + allocate the skinned copy
	void ReleaseInstance();
	// Conservative posed bounds: bind AABB corners + joint positions, inflated.
	void PoseBounds(const std::vector<float>& jointPos);
};

// Pins its atom to a skeleton SOCKET of the closest ancestor SkinnedMeshRenderer: a sword
// in "hand_r", a muzzle flash on "muzzle". The atom's world pose follows the socket every
// frame; local transform edits are overridden while enabled.
class NUKEENGINE_API SocketAttachment : public Component
{
	NUKE_CLASS(SocketAttachment, Component, "Animation")
public:
	[[nuke::prop(label="Socket", tip="Socket (or bare bone) name on the ancestor SkinnedMeshRenderer's skeleton.")]] std::string socket;

	SocketAttachment();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
};

}  // namespace nuke

#endif // !NUKEE_SKINNEDMESHRENDERER_H
