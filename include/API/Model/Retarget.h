#pragma once
#ifndef NUKEE_RETARGET_H
#define NUKEE_RETARGET_H
#include "NukeAPI.h"
#include <string>
#include <vector>
#include "reflect/Reflect.h"

namespace nuke {

class AnimClip;
class Skeleton;
class BoneMap;

// Animation retargeting across skeletons of DIFFERENT proportions and bone names.
// Bone pairing: identical names > .nubonemap renames > position inside SAME-NAMED IK-rig
// chains (.nuskel `chains` give the semantics when nothing else matches). Transfer:
// local-rotation POSE DIFF (delta vs the source bind applied onto the target bind) +
// translation keys scaled by the bind-height ratio of the paired bones.

// Bone pairing shared by the clip retarget and the runtime PoseClone component:
// src bone index -> dst bone index (-1 = unpaired). Identical names > .nubonemap renames >
// position inside SAME-NAMED rig chains > the roots and the pelvis pair with each other.
NUKEENGINE_API void RetargetPairs(const Skeleton* from, const Skeleton* to,
                                  const BoneMap* renames, std::vector<int>& srcToDst);

// A NEW clip re-authored on `to` (fresh guid, channels renamed, keys converted; events,
// notifies, curves and prop tracks copied). Null when nothing pairs. Caller owns the clip.
NUKEENGINE_API AnimClip* RetargetClip(const AnimClip* src, const Skeleton* from,
                                      const Skeleton* to, const BoneMap* renames = nullptr);

// Cached runtime variant: same conversion, memoized per (clip, target skeleton) — the
// Animator routes foreign clips through this on bind. Returns `src` itself when it already
// matches `to` (or can't be converted). The cache owns the produced clips.
NUKEENGINE_API AnimClip* RetargetCached(AnimClip* src, const Skeleton* to,
                                        const BoneMap* renames = nullptr);

// LOOSE-clip adoption: bind a skeleton-less clip (collapsed-pivot animation packs) to the
// registered skeleton covering its channel names (>= 90% match, real bind poses only).
// Mutates clip->skelGuid in place; safe to call every bind (no-op once adopted).
NUKEENGINE_API void AdoptLooseClipSkeleton(AnimClip* clip);

// LOOSE clip (no source skeleton) through the CANONICAL HUB: `renames` maps its channel
// names to canonical, then the canonical rig chain-retargets onto `to`. For targets whose
// bones are NOT canonically named (VRM/VRoid) — plain renaming can never bind there.
// Cached like RetargetCached; returns `src` when the route does not apply.
NUKEENGINE_API AnimClip* RetargetLooseCached(AnimClip* src, const Skeleton* to,
                                             const BoneMap* renames);

// Script/editor face: bake a foreign clip onto a target skeleton as a .nuanim ASSET.
class NUKEENGINE_API Retargeter
{
	NUKE_CLASS_NOCREATE(Retargeter, Object)
public:
	// clipRef = guid or name; dstSkelGuid = target .nuskel; outContentRel = content-relative
	// .nuanim path to write. Returns the new clip's guid ("" on failure). The baked clip is
	// registered in the ResDB immediately.
	[[nuke::func]] static std::string Bake(const std::string& clipRef, const std::string& dstSkelGuid,
	                                       const std::string& outContentRel);
};

}  // namespace nuke

#endif // !NUKEE_RETARGET_H
