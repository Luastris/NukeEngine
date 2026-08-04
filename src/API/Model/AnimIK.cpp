#include "API/Model/AnimIK.h"
#include "API/Model/Animator.h"
#include "API/Model/Atom.h"
#include "API/Model/Physics.h"
#include "API/Model/Skeleton.h"
#include "API/Model/Time.h"
#include "API/Model/Transform.h"
#include <algorithm>
#include <cctype>
#include <iostream>

namespace nuke {

// ---- FootIK ---------------------------------------------------------------------------------

FootIK::FootIK() : Component("FootIK") {}

void FootIK::Init(Atom* parent)
{
	transform = &parent->GetTransform();
	atom = parent;
	parent->components.push_back(this);
}

void FootIK::Destroy() {}
void FootIK::FixedUpdate() {}
void FootIK::Pause() {}
void FootIK::Reset()
{
	pelvisCur = 0.0;
	active = false;
}

// The Animator drives the whole pass through ApplyTo; the component itself only clears its
// goals when disabled mid-run (the Animator skips disabled components).
void FootIK::Update()
{
	if (enabled || !active || !atom) return;
	if (Animator* an = atom->GetComponent<Animator>())
	{
		if (!leftChain.empty())  an->ClearChainIK(leftChain);
		if (!rightChain.empty()) an->ClearChainIK(rightChain);
		an->SetPelvisOffset(0.0);
	}
	active = false;
	pelvisCur = 0.0;
}

// Case-insensitive left/right marker in a chain name.
static bool NameHasSide(const std::string& n, bool left)
{
	std::string s = n;
	for (char& c : s) c = (char)std::tolower((unsigned char)c);
	if (left)  return s.find("left")  != std::string::npos || s.find("l_") == 0 ||
	                  s.find("_l") != std::string::npos || s.find(".l") != std::string::npos;
	return s.find("right") != std::string::npos || s.find("r_") == 0 ||
	       s.find("_r") != std::string::npos || s.find(".r") != std::string::npos;
}

void FootIK::ApplyTo(Animator* an, const float* globals, int nb)
{
	if (!an || !globals || nb <= 0 || !Physics::Available()) return;
	Skeleton* sk = an->CurrentSkeleton();
	if (!sk) return;

	// resolve the two leg chains (explicit names win, else the first left/right rig chains)
	std::string chains[2] = { leftChain, rightChain };
	for (int side = 0; side < 2; ++side)
		if (chains[side].empty())
			for (const SkeletonChain& c : sk->chains)
				if (NameHasSide(c.name, side == 0)) { chains[side] = c.name; break; }

	Vector3    gp = transform->globalPosition();
	Quaternion gq = transform->globalRotation();
	Vector3    gs = transform->globalScale();
	auto toWorld = [&](float x, float y, float z)
	{
		const double sx = x * gs.x, sy = y * gs.y, sz = z * gs.z;
		// q * v (quaternion rotate, w-last)
		const double qx = gq.x, qy = gq.y, qz = gq.z, qw = gq.w;
		const double cx = 2.0 * (qy * sz - qz * sy);
		const double cy = 2.0 * (qz * sx - qx * sz);
		const double cz = 2.0 * (qx * sy - qy * sx);
		return Vector3(gp.x + sx + qw * cx + qy * cz - qz * cy,
		               gp.y + sy + qw * cy + qz * cx - qx * cz,
		               gp.z + sz + qw * cz + qx * cy - qy * cx);
	};

	struct Leg { std::string chain; Vector3 foot; double offset = 0; Vector3 normal; bool hit = false; };
	Leg legs[2];
	int used = 0;
	for (int side = 0; side < 2; ++side)
	{
		if (chains[side].empty()) continue;
		const SkeletonChain* sc = nullptr;
		for (const SkeletonChain& c : sk->chains)
			if (c.name == chains[side]) { sc = &c; break; }
		if (!sc || sc->bones.empty()) continue;
		const int tip = sk->BoneIndex(sc->bones.back());
		if (tip < 0 || tip >= nb) continue;
		Leg& L = legs[used];
		L.chain = chains[side];
		const float* m = globals + tip * 16;
		L.foot = toWorld(m[12], m[13], m[14]);
		const Vector3 from(L.foot.x, L.foot.y + rayUp, L.foot.z);
		if (Physics::RaycastIgnore(from, Vector3(0, -1, 0), rayUp + rayDown, atom))
		{
			const RayHit& h = Physics::LastHit();
			L.hit = true;
			L.offset = (h.point.y + footHeight) - L.foot.y;   // + = ground above the animated foot
			L.normal = h.normal;
		}
		++used;
	}
	if (!used) return;

	// pelvis: sink to the DEEPEST planted foot; mid-step feet (way above their ground) opt out
	double drop = 0.0;
	for (int i = 0; i < used; ++i)
		if (legs[i].hit && legs[i].offset < 0.0 && legs[i].offset > -(double)stepIgnore)
			drop = std::min(drop, legs[i].offset);
	drop = std::max(drop, -(double)maxDrop);
	const double dt = Time::getSingleton()->delta;
	if (pelvisSmooth > 0.0f && dt > 0.0)
	{
		double k = 1.0 - std::exp(-(double)pelvisSmooth * dt);
		pelvisCur += (drop - pelvisCur) * k;
	}
	else pelvisCur = drop;
	an->SetPelvisOffset(pelvisCur * (double)weight);

	for (int i = 0; i < used; ++i)
	{
		Leg& L = legs[i];
		if (!L.hit || L.offset < -(double)stepIgnore)
		{
			an->ClearChainIK(L.chain);   // airborne / over a hole: the animation stands
			continue;
		}
		// target: the animated foot moved onto its ground (never above the ray start)
		const double ty = L.foot.y + std::min(L.offset, (double)rayUp);
		an->SetChainIK(L.chain, Vector3(L.foot.x, ty, L.foot.z), weight);
		if (alignToNormal) an->SetChainIKNormal(L.chain, L.normal, weight);
		// knees bend FORWARD: pole ahead of the chain middle along the atom's facing
		{
			const SkeletonChain* sc = nullptr;
			for (const SkeletonChain& c : sk->chains)
				if (c.name == L.chain) { sc = &c; break; }
			if (sc && sc->bones.size() >= 3)
			{
				const int mid = sk->BoneIndex(sc->bones[sc->bones.size() / 2]);
				if (mid >= 0 && mid < nb)
				{
					const float* mm = globals + mid * 16;
					const Vector3 kneeW = toWorld(mm[12], mm[13], mm[14]);
					const Vector3 fwd = transform->direction();
					an->SetChainIKPole(L.chain, Vector3(kneeW.x + fwd.x, kneeW.y + fwd.y, kneeW.z + fwd.z));
				}
			}
		}
	}
	active = true;
}

// ---- LookAtIK -------------------------------------------------------------------------------

LookAtIK::LookAtIK() : Component("LookAtIK") {}

void LookAtIK::Init(Atom* parent)
{
	transform = &parent->GetTransform();
	atom = parent;
	parent->components.push_back(this);
}

void LookAtIK::Destroy() {}
void LookAtIK::FixedUpdate() {}
void LookAtIK::Pause() {}
void LookAtIK::Reset() {}

void LookAtIK::Update()
{
	if (!atom) return;
	Animator* an = atom->GetComponent<Animator>();
	if (!an) return;
	if (!enabled || !target || chain.empty()) { an->ClearLookAt(); return; }
	an->SetLookAt(chain, target->GetTransform().globalPosition(), weight, maxAngle);
}

}  // namespace nuke
