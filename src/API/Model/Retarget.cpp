#include "API/Model/Retarget.h"
#include "API/Model/AnimClip.h"
#include "API/Model/BoneMap.h"
#include "API/Model/Skeleton.h"
#include "API/Model/resdb.h"
#include "interface/AppInstance.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <vector>

namespace nuke {

namespace {

// Bind-pose GLOBAL positions (forward pass over the locals).
static void BindGlobals(const Skeleton* sk, std::vector<glm::vec3>& pos)
{
	const size_t nb = sk->bones.size();
	std::vector<glm::mat4> g(nb);
	pos.resize(nb);
	for (size_t i = 0; i < nb; ++i)
	{
		const MeshBone& b = sk->bones[i];
		glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(b.localPos[0], b.localPos[1], b.localPos[2]))
		                * glm::mat4_cast(glm::quat(b.localRot[3], b.localRot[0], b.localRot[1], b.localRot[2]))
		                * glm::scale(glm::mat4(1.0f), glm::vec3(b.localScale[0], b.localScale[1], b.localScale[2]));
		g[i] = b.parent >= 0 ? g[b.parent] * local : local;
		pos[i] = glm::vec3(g[i][3]);
	}
}

// The semantic pelvis: the common ancestor of the rig-chain roots (single-chain rigs use the
// chain root's parent). -1 when the skeleton has no chains.
static int ChainsCommonAncestor(const Skeleton* sk)
{
	std::vector<int> roots;
	for (const SkeletonChain& c : sk->chains)
		if (!c.bones.empty())
		{
			const int b = sk->BoneIndex(c.bones.front());
			if (b >= 0) roots.push_back(b);
		}
	if (roots.empty()) return -1;
	std::set<int> anc;
	for (int j = roots[0]; j >= 0; j = sk->bones[j].parent) anc.insert(j);
	int common = roots[0];
	for (size_t i = 1; i < roots.size(); ++i)
	{
		int j = roots[i];
		while (j >= 0 && !anc.count(j)) j = sk->bones[j].parent;
		if (j >= 0) common = j;
	}
	if (roots.size() == 1 && sk->bones[common].parent >= 0) common = sk->bones[common].parent;
	return common;
}

// src bone index -> dst bone index: identical names, then .nubonemap renames, then position
// inside SAME-NAMED rig chains (normalized index when the joint counts differ).
}  // namespace

void RetargetPairs(const Skeleton* from, const Skeleton* to, const BoneMap* renames,
                   std::vector<int>& srcToDst)
{
	srcToDst.assign(from->bones.size(), -1);
	for (size_t i = 0; i < from->bones.size(); ++i)
	{
		std::string want = from->bones[i].name;
		if (renames)
		{
			auto it = renames->map.find(want);
			if (it != renames->map.end()) want = it->second;
		}
		srcToDst[i] = to->BoneIndex(want);
	}
	for (const SkeletonChain& fc : from->chains)
	{
		const SkeletonChain* tc = nullptr;
		for (const SkeletonChain& c : to->chains)
			if (c.name == fc.name) { tc = &c; break; }
		if (!tc || fc.bones.empty() || tc->bones.empty()) continue;
		for (size_t k = 0; k < fc.bones.size(); ++k)
		{
			const int si = from->BoneIndex(fc.bones[k]);
			if (si < 0 || srcToDst[si] >= 0) continue;   // named/renamed matches win
			const size_t tk = fc.bones.size() <= 1 ? 0
				: (k * (tc->bones.size() - 1) + (fc.bones.size() - 1) / 2) / (fc.bones.size() - 1);
			const int di = to->BoneIndex(tc->bones[std::min(tk, tc->bones.size() - 1)]);
			if (di >= 0) srcToDst[si] = di;
		}
	}
	// unmatched ROOTS pair with each other: the pelvis carries the travel on any humanoid,
	// whatever it is called (chains cover limbs, not the root).
	for (size_t i = 0; i < from->bones.size(); ++i)
		if (srcToDst[i] < 0 && from->bones[i].parent < 0)
			for (size_t j = 0; j < to->bones.size(); ++j)
				if (to->bones[j].parent < 0) { srcToDst[i] = (int)j; break; }
	// ... and so does the PELVIS = the common ancestor of the rig-chain roots (importers park
	// static scene nodes above it, so the skeleton root itself is often meaningless).
	const int fp = ChainsCommonAncestor(from);
	const int tp = ChainsCommonAncestor(to);
	if (fp >= 0 && tp >= 0 && srcToDst[fp] < 0) srcToDst[fp] = tp;
}

namespace {
void BuildPairs(const Skeleton* from, const Skeleton* to, const BoneMap* renames,
                std::vector<int>& srcToDst) { RetargetPairs(from, to, renames, srcToDst); }
}  // namespace

AnimClip* RetargetClip(const AnimClip* src, const Skeleton* from, const Skeleton* to,
                       const BoneMap* renames)
{
	if (!src || !from || !to || from->bones.empty() || to->bones.empty()) return nullptr;
	std::vector<int> srcToDst;
	BuildPairs(from, to, renames, srcToDst);

	std::vector<glm::vec3> fromPos, toPos;
	BindGlobals(from, fromPos);
	BindGlobals(to, toPos);

	AnimClip* out = new AnimClip();
	out->guid = ResDB::NewGuid();
	out->name = src->name + "@" + (to->name.empty() ? std::string("retarget") : to->name);
	out->duration = src->duration;
	out->skelGuid = to->guid;
	out->events = src->events;
	out->notifies = src->notifies;
	out->curves = src->curves;
	out->propTracks = src->propTracks;

	int mapped = 0;
	for (const AnimClip::Channel& ch : src->channels)
	{
		const int si = from->BoneIndex(ch.bone);
		if (si < 0) continue;
		const int di = srcToDst[si];
		if (di < 0) continue;
		const MeshBone& sb = from->bones[si];
		const MeshBone& db = to->bones[di];
		const glm::quat sBind(sb.localRot[3], sb.localRot[0], sb.localRot[1], sb.localRot[2]);
		const glm::quat dBind(db.localRot[3], db.localRot[0], db.localRot[1], db.localRot[2]);
		const glm::quat sBindInv = glm::inverse(sBind);

		AnimClip::Channel oc;
		oc.bone = db.name;
		// rotations: pose diff — the source's delta vs its bind, applied on the target bind
		oc.rot.reserve(ch.rot.size());
		for (const AnimClip::Key& k : ch.rot)
		{
			const glm::quat q(k.v[3], k.v[0], k.v[1], k.v[2]);
			glm::quat r = glm::normalize((q * sBindInv) * dBind);
			AnimClip::Key ok = k;
			ok.v[0] = r.x; ok.v[1] = r.y; ok.v[2] = r.z; ok.v[3] = r.w;
			oc.rot.push_back(ok);
		}
		// translations: bind offset of the TARGET + the source's travel scaled by the
		// bind-height ratio of the paired bones (hips ride higher on a taller rig)
		if (!ch.pos.empty())
		{
			const float sh = fabsf(fromPos[si].y), dh = fabsf(toPos[di].y);
			const float s = (sh > 1e-4f && dh > 1e-4f) ? dh / sh : 1.0f;
			oc.pos.reserve(ch.pos.size());
			for (const AnimClip::Key& k : ch.pos)
			{
				AnimClip::Key ok = k;
				ok.v[0] = db.localPos[0] + (k.v[0] - sb.localPos[0]) * s;
				ok.v[1] = db.localPos[1] + (k.v[1] - sb.localPos[1]) * s;
				ok.v[2] = db.localPos[2] + (k.v[2] - sb.localPos[2]) * s;
				oc.pos.push_back(ok);
			}
		}
		oc.scl = ch.scl;   // scale is proportion-free
		out->channels.push_back(std::move(oc));
		++mapped;
	}
	if (!mapped) { delete out; return nullptr; }
	return out;
}

// (clip guid, target skeleton guid) -> converted clip. Game-thread only (Animator binds).
AnimClip* RetargetCached(AnimClip* src, const Skeleton* to, const BoneMap* renames)
{
	if (!src || !to) return src;
	if (src->skelGuid.empty() || src->skelGuid == to->guid) return src;
	const Skeleton* from = ResDB::getSingleton()->GetSkeleton(src->skelGuid);
	if (!from) return src;
	static std::map<std::pair<std::string, std::string>, AnimClip*> cache;
	const auto key = std::make_pair(src->guid, to->guid);
	auto it = cache.find(key);
	if (it != cache.end()) return it->second ? it->second : src;
	AnimClip* r = RetargetClip(src, from, to, renames);
	cache[key] = r;
	if (r)
		std::cout << "[Retarget]\t'" << src->name << "' -> skeleton '" << to->name << "' ("
		          << r->channels.size() << " channels)" << std::endl;
	return r ? r : src;
}

std::string Retargeter::Bake(const std::string& clipRef, const std::string& dstSkelGuid,
                             const std::string& outContentRel)
{
	ResDB* db = ResDB::getSingleton();
	AnimClip* src = db->GetClip(clipRef);
	if (!src) src = db->GetClipByName(clipRef);
	const Skeleton* to = db->GetSkeleton(dstSkelGuid);
	const Skeleton* from = src && !src->skelGuid.empty() ? db->GetSkeleton(src->skelGuid) : nullptr;
	if (!src || !to || !from)
	{
		std::cout << "[Retarget]\tBake: unresolved clip/skeleton" << std::endl;
		return std::string();
	}
	AnimClip* baked = RetargetClip(src, from, to, nullptr);
	if (!baked) { std::cout << "[Retarget]\tBake: no bones paired" << std::endl; return std::string(); }
	const std::string full = AppInstance::GetSingleton()->ResolveContent(outContentRel);
	if (!baked->SaveToFile(full))
	{
		delete baked;
		std::cout << "[Retarget]\tBake: write failed: " << full << std::endl;
		return std::string();
	}
	db->RegisterClip(baked);
	db->SetAssetPath(baked->guid, full);
	std::cout << "[Retarget]\tbaked '" << baked->name << "' -> " << outContentRel << std::endl;
	return baked->guid;
}

// without bind poses (old imports of the same collapsed-pivot files) never adopt.
void AdoptLooseClipSkeleton(AnimClip* clip)
{
	if (!clip || !clip->skelGuid.empty() || clip->channels.empty()) return;
	ResDB* db = ResDB::getSingleton();
	const Skeleton* best = nullptr;
	float bestCov = 0.0f;
	for (Skeleton* sk : db->skeletons)
	{
		if (!sk || sk->bones.empty()) continue;
		double span = 0.0;
		for (const MeshBone& b : sk->bones)
			span += std::fabs(b.localPos[0]) + std::fabs(b.localPos[1]) + std::fabs(b.localPos[2]);
		if (span < 1e-3) continue;   // degenerate bind: useless as a retarget source
		int hit = 0;
		for (const AnimClip::Channel& ch : clip->channels)
			if (sk->BoneIndex(ch.bone) >= 0) ++hit;
		const float cov = (float)hit / (float)clip->channels.size();
		if (cov > bestCov) { bestCov = cov; best = sk; }
	}
	if (best && bestCov >= 0.9f)
	{
		clip->skelGuid = best->guid;
		std::cout << "[Retarget]\tloose clip '" << clip->name << "' adopted skeleton '"
		          << best->name << "' (" << (int)(bestCov * 100.0f) << "% bone match)" << std::endl;
	}
}

// Loose clip (no source skeleton) whose bone-mapped names miss the target palette ENTIRELY:
// rename the channels to CANONICAL via the map, then chain-retarget from the canonical rig
// onto the target — canon is the hub (a Mixamo library clip + the mixamo bone-map preset
// lands on a VRoid VRM this way; direct name matching cannot, VRoid bones are not canon).
AnimClip* RetargetLooseCached(AnimClip* src, const Skeleton* to, const BoneMap* renames)
{
	if (!src || !to || !renames || !src->skelGuid.empty() || to->chains.empty()) return src;
	const Skeleton* canon = ResDB::getSingleton()->GetSkeleton("builtin:nukerig");
	if (!canon) return src;
	static std::map<std::pair<std::pair<std::string, std::string>, std::string>, AnimClip*> cache;
	const auto key = std::make_pair(std::make_pair(src->guid, to->guid), renames->guid);
	auto it = cache.find(key);
	if (it != cache.end()) return it->second ? it->second : src;
	AnimClip tmp = *src;   // canonical-named copy; keys stay as-authored
	for (AnimClip::Channel& ch : tmp.channels)
	{
		auto r = renames->map.find(ch.bone);
		if (r != renames->map.end()) ch.bone = r->second;
	}
	tmp.skelGuid = canon->guid;
	AnimClip* out = RetargetClip(&tmp, canon, to, nullptr);
	cache[key] = out;
	if (out)
		std::cout << "[Retarget]\tloose '" << src->name << "' -> canonical hub -> skeleton '"
		          << to->name << "' (" << out->channels.size() << " channels)" << std::endl;
	return out ? out : src;
}

}  // namespace nuke
