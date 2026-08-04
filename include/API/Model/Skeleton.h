#pragma once
#ifndef NUKEE_SKELETON_H
#define NUKEE_SKELETON_H
#include "NukeAPI.h"
#include "Mesh.h"   // MeshBone (palette entry — shared with the legacy embedded-skeleton path)
#include <istream>
#include <string>
#include <vector>
#include "reflect/Reflect.h"

namespace nuke {

// A named attach point riding a bone: weapons in hands, effects on joints. World pose =
// atomWorld * boneWorld * socket local TRS.
struct SkeletonSocket
{
	std::string name;
	std::string bone;                       // palette bone name
	float localPos[3]   = { 0, 0, 0 };
	float localRot[4]   = { 0, 0, 0, 1 };   // (x, y, z, w)
	float localScale[3] = { 1, 1, 1 };
};

// A named bone set — layer masks ("UpperBody"), per-part toggles. Membership by bone NAME.
struct SkeletonGroup
{
	std::string name;
	std::vector<std::string> bones;
};

// A named kinematic chain, root -> tip ("LeftLeg" = hip/knee/foot). The IK rig AND the
// retargeter address the skeleton through these instead of raw bone names.
struct SkeletonChain
{
	std::string name;
	std::vector<std::string> bones;
};

// Skeleton ASSET (.nuskel, JSON): the bone palette + sockets + groups + IK rig, shared by
// every mesh skinned to it (modular characters = N meshes, ONE skeleton). Bones stay in
// hierarchy order (parent index < own index) so one forward pass computes globals.
class NUKEENGINE_API Skeleton
{
	NUKE_CLASS(Skeleton, Object)
public:
	std::string guid;                     // asset id (ResDB)
	[[nuke::prop(label="Name")]] std::string name;

	std::vector<MeshBone>       bones;
	std::vector<SkeletonSocket> sockets;
	std::vector<SkeletonGroup>  groups;
	std::vector<SkeletonChain>  chains;

	int  BoneIndex(const std::string& boneName) const;      // -1 when absent
	const SkeletonSocket* Socket(const std::string& socketName) const;
	// Group membership as a per-bone weight mask (1 = in the group), sized to bones.
	void GroupMask(const std::string& groupName, std::vector<float>& out) const;

	// Native asset format (.nuskel): JSON — small, diff-able, hand-editable.
	bool             SaveToFile(const std::string& path) const;
	static Skeleton* LoadFromFile(const std::string& path);
	static Skeleton* LoadFromMemory(const std::string& data);   // packed content
	static Skeleton* FromString(const std::string& json);
	std::string      ToString() const;
};

}  // namespace nuke

#endif // !NUKEE_SKELETON_H
