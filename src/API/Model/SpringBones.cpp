// C4 spring bones: damped chain sim over the committed pose (see SpringBones.h).
#include "API/Model/SpringBones.h"
#include "API/Model/Atom.h"
#include "API/Model/Ragdoll.h"
#include "API/Model/Skeleton.h"
#include "API/Model/Time.h"
#include "API/Model/resdb.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace nuke {

void SpringBones::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void SpringBones::Reset() { primed = false; }

namespace {

// Closest point on segment [a, b] to p.
static glm::vec3 ClosestOnSegment(const glm::vec3& a, const glm::vec3& b, const glm::vec3& p)
{
	const glm::vec3 ab = b - a;
	const float d = glm::dot(ab, ab);
	if (d < 1e-12f) return a;
	const float t = glm::clamp(glm::dot(p - a, ab) / d, 0.0f, 1.0f);
	return a + ab * t;
}

}  // namespace

bool SpringBones::Apply(const std::vector<MeshBone>& bones, const Skeleton* sk, SpringPose* pose,
                        int nb, const float model[16], Atom* owner, double dt)
{
	if (chain.empty() || nb <= 0 || dt <= 0.0) return false;

	// ---- chain resolve (cached): rig chain by name, else bone -> first children to leaf ----
	if (chain != cachedChain || (size_t)nb != cachedBones)
	{
		cachedChain = chain;
		cachedBones = (size_t)nb;
		chainIdx.clear();
		primed = false;
		auto boneIndex = [&](const std::string& n) -> int
		{
			for (int i = 0; i < nb; ++i) if (bones[i].name == n) return i;
			return -1;
		};
		const SkeletonChain* sc = nullptr;
		if (sk)
			for (const SkeletonChain& c : sk->chains)
				if (c.name == chain) { sc = &c; break; }
		if (sc)
		{
			for (const std::string& bn : sc->bones)
			{
				const int b = boneIndex(bn);
				if (b >= 0) chainIdx.push_back(b);
			}
		}
		else
		{
			int b = boneIndex(chain);
			while (b >= 0)
			{
				chainIdx.push_back(b);
				int child = -1;
				for (int i = 0; i < nb; ++i)
					if (bones[i].parent == b) { child = i; break; }
				b = child;
			}
		}
		if (chainIdx.empty()) return false;
	}
	if (chainIdx.empty()) return false;

	// ---- animated globals (model space) --------------------------------------------------
	const glm::mat4 M = glm::make_mat4(model);
	std::vector<glm::mat4> global((size_t)nb);
	for (int i = 0; i < nb; ++i)
	{
		const glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::make_vec3(pose[i].p))
		                      * glm::mat4_cast(glm::quat(pose[i].r[3], pose[i].r[0], pose[i].r[1], pose[i].r[2]))
		                      * glm::scale(glm::mat4(1.0f), glm::make_vec3(pose[i].s));
		global[i] = bones[i].parent >= 0 ? global[bones[i].parent] * local : local;
	}

	// ---- colliders: the atom's ragdoll capsules, world space (chain's own bones excluded) --
	struct Cap { glm::vec3 a, b; float r; };
	std::vector<Cap> caps;
	if (collision)
	{
		RagdollDef* def = nullptr;
		if (Ragdoll* rd = owner ? owner->GetComponent<Ragdoll>() : nullptr) def = rd->Def();
		if (!def && sk)
			for (RagdollDef* rg : ResDB::getSingleton()->ragdolls)
				if (rg && rg->skelGuid == sk->guid) { def = rg; break; }
		if (def)
			for (const RagdollDef::Body& bd : def->bodies)
			{
				int b = -1;
				for (int i = 0; i < nb; ++i) if (bones[i].name == bd.bone) { b = i; break; }
				if (b < 0) continue;
				bool onChain = false;
				for (int ci : chainIdx) if (ci == b) { onChain = true; break; }
				if (onChain) continue;
				const glm::mat4 W = M * global[b];
				const glm::vec3 c = glm::make_vec3(bd.center);
				const glm::vec3 ax = glm::make_vec3(bd.axis) * bd.halfHeight;
				caps.push_back({ glm::vec3(W * glm::vec4(c - ax, 1.0f)),
				                 glm::vec3(W * glm::vec4(c + ax, 1.0f)), bd.radius });
			}
	}

	// ---- verlet over the chain, root -> tip ----------------------------------------------
	// Segment k springs bone chainIdx[k] toward its child joint (virtual continuation tail
	// on the last bone). State = world tail positions.
	const size_t segs = chainIdx.size();
	if (cur.size() != segs * 3) { cur.assign(segs * 3, 0.0f); prev.assign(segs * 3, 0.0f); primed = false; }

	const double t = Time::getSingleton()->elapsed;
	const float stiff = glm::clamp((float)(stiffness * 60.0 * dt), 0.0f, 1.0f);
	const float drag = glm::clamp(damping, 0.0f, 1.0f);
	const glm::vec3 grav(0.0f, -gravity * (float)(dt * dt), 0.0f);

	// running SPRUNG transform along the chain (parents above the chain stay animated)
	glm::mat4 parentW = M;   // world of the chain root's parent
	{
		const int par = bones[chainIdx[0]].parent;
		if (par >= 0) parentW = M * global[par];
	}

	bool changed = false;
	for (size_t k = 0; k < segs; ++k)
	{
		const int b = chainIdx[k];
		// animated local, with the procedural wave layered on top (target, pre-spring)
		glm::quat animR(pose[b].r[3], pose[b].r[0], pose[b].r[1], pose[b].r[2]);
		if (waveDeg > 0.0f && waveHz > 0.0f)
		{
			const float ang = glm::radians(waveDeg)
			                * std::sin((float)(6.283185307 * waveHz * t) + (float)k * glm::radians(waveTravel));
			const glm::vec3 axes[3] = { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };   // yaw/pitch/roll
			animR = animR * glm::angleAxis(ang, axes[glm::clamp(waveAxis, 0, 2)]);
		}
		const glm::mat4 animLocal = glm::translate(glm::mat4(1.0f), glm::make_vec3(pose[b].p))
		                          * glm::mat4_cast(animR)
		                          * glm::scale(glm::mat4(1.0f), glm::make_vec3(pose[b].s));
		const glm::mat4 boneW = parentW * animLocal;   // target frame under SPRUNG parents
		const glm::vec3 head(boneW[3]);

		// target tail: the child joint under this frame, or a virtual continuation
		glm::vec3 tailLocal;
		if (k + 1 < segs) tailLocal = glm::make_vec3(pose[chainIdx[k + 1]].p);
		else if (k > 0)   tailLocal = glm::make_vec3(pose[b].p);   // repeat the last offset
		else              tailLocal = glm::vec3(0.0f, 0.1f, 0.0f); // single-bone chain (jiggle)
		if (glm::dot(tailLocal, tailLocal) < 1e-10f) tailLocal = glm::vec3(0.0f, 0.05f, 0.0f);
		const glm::vec3 target = glm::vec3(boneW * glm::vec4(tailLocal, 1.0f));
		const float segLen = glm::length(target - head);

		glm::vec3 c = glm::make_vec3(&cur[k * 3]);
		glm::vec3 p = glm::make_vec3(&prev[k * 3]);
		if (!primed || glm::length(target - c) > 1.0f) { c = target; p = target; }   // prime/teleport

		glm::vec3 next = c + (c - p) * (1.0f - drag) + grav + (target - c) * stiff;
		// keep the bone length under the sprung head
		{
			const glm::vec3 d = next - head;
			const float l = glm::length(d);
			next = l > 1e-6f ? head + d * (segLen / l) : target;
		}
		for (const Cap& cp : caps)
		{
			const glm::vec3 q = ClosestOnSegment(cp.a, cp.b, next);
			const glm::vec3 d = next - q;
			const float l = glm::length(d);
			const float minD = cp.r + radius;
			if (l < minD && l > 1e-6f) next = q + d * (minD / l);
		}
		prev[k * 3] = c.x; prev[k * 3 + 1] = c.y; prev[k * 3 + 2] = c.z;
		cur[k * 3] = next.x; cur[k * 3 + 1] = next.y; cur[k * 3 + 2] = next.z;

		// rotate the bone so its tail direction follows the spring
		const glm::vec3 want = next - head;
		const glm::vec3 have = target - head;
		glm::quat newR = animR;
		if (glm::length(want) > 1e-6f && glm::length(have) > 1e-6f)
		{
			const glm::quat delta = glm::rotation(glm::normalize(have), glm::normalize(want));
			const glm::quat parentRot = glm::normalize(glm::quat_cast(parentW));
			const glm::quat springWorld = delta * parentRot * animR;
			newR = glm::normalize(glm::inverse(parentRot) * springWorld);
		}
		if (std::fabs(newR.x - pose[b].r[0]) + std::fabs(newR.y - pose[b].r[1])
		  + std::fabs(newR.z - pose[b].r[2]) + std::fabs(newR.w - pose[b].r[3]) > 1e-6f)
		{
			pose[b].r[0] = newR.x; pose[b].r[1] = newR.y; pose[b].r[2] = newR.z; pose[b].r[3] = newR.w;
			changed = true;
		}
		// advance the running frame with the SPRUNG local
		parentW = parentW * (glm::translate(glm::mat4(1.0f), glm::make_vec3(pose[b].p))
		                   * glm::mat4_cast(newR)
		                   * glm::scale(glm::mat4(1.0f), glm::make_vec3(pose[b].s)));
	}
	primed = true;
	return changed;
}

}  // namespace nuke
