#include "API/Model/MotionMatcher.h"
#include "API/Model/AnimClip.h"
#include "API/Model/Animator.h"
#include "API/Model/Atom.h"
#include "API/Model/Skeleton.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Time.h"
#include "API/Model/Transform.h"
#include "API/Model/resdb.h"
#include <cmath>
#include <cstring>
#include <iostream>

namespace nuke {

MotionMatcher::MotionMatcher() : Component("MotionMatcher") {}

void MotionMatcher::Init(Atom* parent)
{
	transform = &parent->GetTransform();
	atom = parent;
	parent->components.push_back(this);
}

void MotionMatcher::Destroy() {}
void MotionMatcher::FixedUpdate() {}
void MotionMatcher::Pause() {}
void MotionMatcher::Reset()
{
	db.clear();
	resolvedClips.clear();
	dbBuilt = false;
	sinceSearch = 0.0;
	curSample = -1;
	desired = Vector3(0, 0, 0);
}

void MotionMatcher::SetDesiredVelocity(const Vector3& v) { desired = v; }
std::string MotionMatcher::MatchedClip()
{
	if (curSample < 0 || curSample >= (int)db.size()) return std::string();
	AnimClip* c = resolvedClips[db[curSample].clip];
	return c ? c->name : std::string();
}
double MotionMatcher::SampleCount() { return (double)db.size(); }

// Feature frame = the pelvis: positions/velocities are expressed relative to the root-ground
// projection so samples from travelling clips compare cleanly.
namespace {

struct Feet { int left = -1, right = -1, pelvis = -1, root = -1; };

static Feet ResolveFeet(const Skeleton* sk)
{
	Feet f;
	auto side = [&](bool left) -> int
	{
		for (const SkeletonChain& c : sk->chains)
		{
			std::string s = c.name;
			for (char& ch : s) ch = (char)tolower((unsigned char)ch);
			const bool isL = s.find("left") != std::string::npos;
			const bool isR = s.find("right") != std::string::npos;
			if ((left && isL) || (!left && isR))
				return c.bones.empty() ? -1 : sk->BoneIndex(c.bones.back());
		}
		return -1;
	};
	f.left = side(true);
	f.right = side(false);
	// pelvis: the common ancestor of the leg-chain roots (fallback: bone 0)
	int rl = -1, rr = -1;
	for (const SkeletonChain& c : sk->chains)
	{
		if (c.bones.empty()) continue;
		std::string s = c.name;
		for (char& ch : s) ch = (char)tolower((unsigned char)ch);
		if (s.find("left") != std::string::npos)  rl = sk->BoneIndex(c.bones.front());
		if (s.find("right") != std::string::npos) rr = sk->BoneIndex(c.bones.front());
	}
	if (rl >= 0 && rr >= 0)
	{
		std::vector<bool> anc(sk->bones.size(), false);
		for (int j = rl; j >= 0; j = sk->bones[j].parent) anc[j] = true;
		int j = rr;
		while (j >= 0 && !anc[j]) j = sk->bones[j].parent;
		f.pelvis = j;
	}
	if (f.pelvis < 0 && !sk->bones.empty()) f.pelvis = 0;
	f.root = f.pelvis;
	return f;
}

static void GPos(const std::vector<float>& g, int bone, float out[3])
{
	out[0] = g[bone * 16 + 12];
	out[1] = g[bone * 16 + 13];
	out[2] = g[bone * 16 + 14];
}

}  // namespace

void MotionMatcher::EnsureDB(Animator* an)
{
	if (dbBuilt) return;
	dbBuilt = true;
	Skeleton* sk = an->CurrentSkeleton();
	if (!sk || clips.empty()) return;
	const Feet ft = ResolveFeet(sk);
	if (ft.left < 0 || ft.right < 0 || ft.pelvis < 0)
	{
		std::cout << "[MotionMatch]\tskeleton has no left/right leg chains — no DB" << std::endl;
		return;
	}
	ResDB* rdb = ResDB::getSingleton();
	for (const std::string& ref : clips)
	{
		AnimClip* c = rdb->GetClip(ref);
		if (!c) c = rdb->GetClipByName(ref);
		if (!c || c->duration <= 0.0) continue;
		const int ci = (int)resolvedClips.size();
		resolvedClips.push_back(c);
		const double step = 1.0 / std::max(2.0f, sampleRate);
		std::vector<float> g0, g1, gf;
		for (double t = 0.0; t < c->duration; t += step)
		{
			if (!an->SamplePoseGlobals(c, t, g0)) return;
			Sample s;
			s.clip = ci;
			s.t = (float)t;
			float pelvis[3], fl[3], fr[3];
			GPos(g0, ft.pelvis, pelvis);
			GPos(g0, ft.left, fl);
			GPos(g0, ft.right, fr);
			for (int k = 0; k < 3; ++k)
			{
				s.footL[k] = fl[k] - pelvis[k];
				s.footR[k] = fr[k] - pelvis[k];
			}
			// pelvis velocity by central-ish difference (wrap-aware for loops)
			const double dt = std::min(step, c->duration * 0.25);
			const double t1 = t + dt < c->duration ? t + dt : t + dt - c->duration;
			if (an->SamplePoseGlobals(c, t1, g1))
			{
				float p1[3];
				GPos(g1, ft.pelvis, p1);
				for (int k = 0; k < 3; ++k)
				{
					float d = p1[k] - pelvis[k];
					if (t1 < t)   // wrapped: undo the loop jump using the clip's net travel
					{
						float pe[3], p0[3];
						std::vector<float> ge, gs0;
						if (an->SamplePoseGlobals(c, c->duration - 1e-4, ge) &&
						    an->SamplePoseGlobals(c, 0.0, gs0))
						{
							float e[3], b[3];
							GPos(ge, ft.pelvis, e);
							GPos(gs0, ft.pelvis, b);
							d += e[k] - b[k];
						}
						(void)pe; (void)p0;
					}
					s.vel[k] = (float)(d / dt);
				}
			}
			// future root trajectory at +0.3 / +0.6 s (XZ, wrap-aware via net travel)
			auto rootAt = [&](double tt, float out[3])
			{
				double w = tt;
				float extra[3] = { 0, 0, 0 };
				while (w >= c->duration)
				{
					w -= c->duration;
					std::vector<float> ge, gb;
					if (an->SamplePoseGlobals(c, c->duration - 1e-4, ge) &&
					    an->SamplePoseGlobals(c, 0.0, gb))
					{
						float e[3], b[3];
						GPos(ge, ft.root, e);
						GPos(gb, ft.root, b);
						for (int k = 0; k < 3; ++k) extra[k] += e[k] - b[k];
					}
				}
				if (an->SamplePoseGlobals(c, w, gf))
				{
					GPos(gf, ft.root, out);
					for (int k = 0; k < 3; ++k) out[k] += extra[k];
				}
				else { out[0] = out[1] = out[2] = 0; }
			};
			float r3[3], r6[3];
			rootAt(t + 0.3, r3);
			rootAt(t + 0.6, r6);
			s.traj[0] = r3[0] - pelvis[0]; s.traj[1] = r3[2] - pelvis[2];
			s.traj[2] = r6[0] - pelvis[0]; s.traj[3] = r6[2] - pelvis[2];
			s.traj[4] = s.vel[0]; s.traj[5] = s.vel[2];
			db.push_back(s);
		}
	}
	std::cout << "[MotionMatch]\tDB ready: " << db.size() << " samples over "
	          << resolvedClips.size() << " clips" << std::endl;
}

double MotionMatcher::Cost(const Sample& s, const float curL[3], const float curR[3],
                           const float curVel[3], const float wantTraj[6]) const
{
	double c = 0.0;
	for (int k = 0; k < 3; ++k)
	{
		const double dl = s.footL[k] - curL[k];
		const double dr = s.footR[k] - curR[k];
		c += poseWeight * (dl * dl + dr * dr);
		const double dv = s.vel[k] - curVel[k];
		c += velocityWeight * dv * dv;
	}
	for (int k = 0; k < 4; ++k)
	{
		const double d = s.traj[k] - wantTraj[k];
		c += trajectoryWeight * d * d;
	}
	return c;
}

void MotionMatcher::Update()
{
	if (!enabled || !atom) return;
	Animator* an = atom->GetComponent<Animator>();
	SkinnedMeshRenderer* smr = atom->GetComponent<SkinnedMeshRenderer>();
	if (!smr)
		for (Atom* ch : atom->children)
			if ((smr = ch->GetComponent<SkinnedMeshRenderer>())) break;
	if (!an || !smr || !smr->skeleton) return;
	EnsureDB(an);
	if (db.empty()) return;

	sinceSearch += Time::getSingleton()->delta;
	if (sinceSearch < searchInterval) return;
	sinceSearch = 0.0;

	// current pose features from the LIVE bone globals (pelvis frame)
	Skeleton* sk = smr->skeleton;
	const Feet ft = ResolveFeet(sk);
	const std::vector<float>& g = smr->Globals();
	if ((int)g.size() < (int)sk->bones.size() * 16) return;
	float pelvis[3], fl[3], fr[3];
	GPos(g, ft.pelvis, pelvis);
	GPos(g, ft.left, fl);
	GPos(g, ft.right, fr);
	float curL[3], curR[3];
	for (int k = 0; k < 3; ++k)
	{
		curL[k] = fl[k] - pelvis[k];
		curR[k] = fr[k] - pelvis[k];
	}
	// desired velocity into the CHARACTER frame (inverse atom yaw)
	Quaternion gq = transform->globalRotation();
	const double yaw = atan2(2.0 * (gq.w * gq.y + gq.x * gq.z), 1.0 - 2.0 * (gq.y * gq.y + gq.x * gq.x));
	const float cy = (float)cos(-yaw), sy = (float)sin(-yaw);
	float dvx = (float)(desired.x * cy + desired.z * sy);
	float dvz = (float)(-desired.x * sy + desired.z * cy);
	const float curVel[3] = { dvx, (float)desired.y, dvz };   // matcher steers TOWARD the wish
	const float wantTraj[6] = { dvx * 0.3f, dvz * 0.3f, dvx * 0.6f, dvz * 0.6f, dvx, dvz };

	int best = -1;
	double bestC = 1e30;
	for (size_t i = 0; i < db.size(); ++i)
	{
		const double c = Cost(db[i], curL, curR, curVel, wantTraj);
		if (c < bestC) { bestC = c; best = (int)i; }
	}
	if (best < 0) return;

	// already inside the winning stretch: keep playing, no re-blend spam
	const Sample& s = db[best];
	AnimClip* cur = nullptr;
	if (curSample >= 0 && curSample < (int)db.size()) cur = resolvedClips[db[curSample].clip];
	AnimClip* want = resolvedClips[s.clip];
	if (cur == want && want)
	{
		const double now = an->ClipTime();
		double d = fabs((double)s.t - now);
		if (want->duration > 0.0) d = std::min(d, want->duration - d);
		if (d <= (double)keepWindow) { curSample = best; return; }
	}
	an->MatchTo(want->guid, (double)s.t, (double)blendTime);
	curSample = best;
}

}  // namespace nuke
