// C4 spring bones: damped chain sim over the committed pose (see SpringBones.h).
#include "API/Model/SpringBones.h"
#include "API/Model/Atom.h"
#include "API/Model/Ragdoll.h"
#include "API/Model/Skeleton.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Time.h"
#include "API/Model/Wind.h"
#include "API/Model/resdb.h"
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <map>

namespace nuke {

void SpringBones::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void SpringBones::Reset() { primed = false; }

namespace {

// Subtree SkinnedMeshRenderers sharing ONE skeleton asset (first valid wins) — the same
// grouping the Animator drives; the self-driven spring solve hands its pose to all of them.
void CollectGroupSmrs(Atom* a, std::vector<SkinnedMeshRenderer*>& out, Skeleton*& sk)
{
	if (!a || !a->enabled) return;
	for (Component* c : a->components)
	{
		if (!c || !c->enabled || std::strcmp(c->name, "SkinnedMeshRenderer") != 0) continue;
		SkinnedMeshRenderer* s = (SkinnedMeshRenderer*)c;
		Skeleton* ssk = s->EnsureSkeleton();
		if (!ssk || ssk->bones.empty()) continue;
		if (!sk) sk = ssk;
		if (ssk == sk) out.push_back(s);
	}
	for (Atom* ch : a->children) CollectGroupSmrs(ch, out, sk);
}

}  // namespace

// Self-drive: springs simulate every frame even with NO Animator (or an idle one) — they
// are physics, not animation garnish. The FIRST spring of the atom that reaches its Update
// unsolved runs the WHOLE group (one pose, one skin handover, CommitPose's exact layering),
// stamping every sibling — their own Updates then skip.
void SpringBones::Update()
{
	const unsigned long long frame = Time::getSingleton()->frame;
	if (lastSolveFrame == frame) return;                            // already solved (Animator or a sibling)
	if (lastSolveByAnim && lastSolveFrame + 1 == frame) return;     // a committing Animator runs later this frame
	if (!atom) return;

	std::vector<SkinnedMeshRenderer*> group;
	Skeleton* sk = nullptr;
	CollectGroupSmrs(atom, group, sk);
	if (!sk || group.empty()) return;
	const size_t nb = sk->bones.size();
	SkinnedMeshRenderer* lead = group[0];
	if (lead->pose.size() != nb) return;   // palette not sized yet (EnsureSkeleton fills bind)

	std::vector<SpringPose> sp(nb);
	for (size_t i = 0; i < nb; ++i)
	{
		const SkinnedMeshRenderer::BonePose& b = lead->pose[i];
		sp[i].p[0] = b.pos[0];   sp[i].p[1] = b.pos[1];   sp[i].p[2] = b.pos[2];
		sp[i].s[0] = b.scale[0]; sp[i].s[1] = b.scale[1]; sp[i].s[2] = b.scale[2];
		sp[i].r[0] = b.rot[0];   sp[i].r[1] = b.rot[1];   sp[i].r[2] = b.rot[2]; sp[i].r[3] = b.rot[3];
	}
	float M[16];
	{
		const Vector3 gp = atom->GetTransform().globalPosition();
		const Quaternion gq = atom->GetTransform().globalRotation();
		const Vector3 gs = atom->GetTransform().globalScale();
		glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3((float)gp.x, (float)gp.y, (float)gp.z))
		                * glm::mat4_cast(glm::quat((float)gq.w, (float)gq.x, (float)gq.y, (float)gq.z))
		                * glm::scale(glm::mat4(1.0f), glm::vec3((float)gs.x, (float)gs.y, (float)gs.z));
		std::memcpy(M, glm::value_ptr(model), sizeof(M));
	}

	bool sprung = false;
	const double dt = Time::getSingleton()->delta;
	bool wantCaps = false;
	for (Component* c : atom->components)
		if (c && c->enabled && std::strcmp(c->name, "SpringBones") == 0)
			wantCaps = wantCaps || ((SpringBones*)c)->collision;
	SpringSolveCtx ctx;
	BuildSolveCtx(sk->bones, sk, sp.data(), (int)nb, M, atom, wantCaps, ctx);
	for (Component* c : atom->components)
		if (c && c->enabled && std::strcmp(c->name, "SpringBones") == 0)
		{
			SpringBones* sb = (SpringBones*)c;
			sprung |= sb->Apply(sk->bones, sk, sp.data(), (int)nb, M, atom, dt, &ctx);
			sb->lastSolveByAnim = false;
		}
	if (!sprung) return;   // chains at rest: nothing to re-skin

	// Skin with the SPRUNG rotations, then restore the clean base pose: the solver's target
	// must stay the un-sprung pose next frame (the Animator flow rebuilds it from the clip
	// every commit) — feeding the solved pose back in would make the target chase the spring
	// and the chain would never return to rest.
	std::vector<float> keep(nb * 4);
	for (SkinnedMeshRenderer* s : group)
	{
		if (s->pose.size() != nb) continue;
		for (size_t i = 0; i < nb; ++i)
		{
			std::memcpy(&keep[i * 4], s->pose[i].rot, sizeof(float) * 4);
			s->pose[i].rot[0] = sp[i].r[0]; s->pose[i].rot[1] = sp[i].r[1];
			s->pose[i].rot[2] = sp[i].r[2]; s->pose[i].rot[3] = sp[i].r[3];
		}
		s->ApplyPose();
		for (size_t i = 0; i < nb; ++i)
			std::memcpy(s->pose[i].rot, &keep[i * 4], sizeof(float) * 4);
	}
}

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

// Model-space forward pass over the pose slice, flattened (16 floats per bone).
static void BuildGlobals(const std::vector<MeshBone>& bones, const SpringPose* pose, int nb,
                         std::vector<float>& out)
{
	out.resize((size_t)nb * 16);
	std::vector<glm::mat4> g((size_t)nb);
	for (int i = 0; i < nb; ++i)
	{
		const glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::make_vec3(pose[i].p))
		                      * glm::mat4_cast(glm::quat(pose[i].r[3], pose[i].r[0], pose[i].r[1], pose[i].r[2]))
		                      * glm::scale(glm::mat4(1.0f), glm::make_vec3(pose[i].s));
		g[i] = bones[i].parent >= 0 ? g[bones[i].parent] * local : local;
		std::memcpy(&out[(size_t)i * 16], glm::value_ptr(g[i]), sizeof(float) * 16);
	}
}

}  // namespace

void SpringBones::BuildSolveCtx(const std::vector<MeshBone>& bones, const Skeleton* sk,
                                const SpringPose* pose, int nb, const float model[16],
                                Atom* owner, bool wantCaps, SpringSolveCtx& out)
{
	BuildGlobals(bones, pose, nb, out.global);
	out.caps.clear();
	if (!wantCaps || nb <= 0) return;
	RagdollDef* def = nullptr;
	if (Ragdoll* rd = owner ? owner->GetComponent<Ragdoll>() : nullptr) def = rd->Def();
	if (!def && sk)
		for (RagdollDef* rg : ResDB::getSingleton()->ragdolls)
			if (rg && rg->skelGuid == sk->guid) { def = rg; break; }
	if (!def) return;
	std::map<std::string, int> nameIdx;
	for (int i = 0; i < nb; ++i) nameIdx[bones[i].name] = i;
	const glm::mat4 M = glm::make_mat4(model);
	for (const RagdollDef::Body& bd : def->bodies)
	{
		auto it = nameIdx.find(bd.bone);
		if (it == nameIdx.end()) continue;
		const glm::mat4 W = M * glm::make_mat4(&out.global[(size_t)it->second * 16]);
		const glm::vec3 c = glm::make_vec3(bd.center);
		const glm::vec3 ax = glm::make_vec3(bd.axis) * bd.halfHeight;
		const glm::vec3 pa = glm::vec3(W * glm::vec4(c - ax, 1.0f));
		const glm::vec3 pb = glm::vec3(W * glm::vec4(c + ax, 1.0f));
		SpringSolveCtx::Capsule cp;
		cp.a[0] = pa.x; cp.a[1] = pa.y; cp.a[2] = pa.z;
		cp.b[0] = pb.x; cp.b[1] = pb.y; cp.b[2] = pb.z;
		cp.r = bd.radius;
		cp.bone = it->second;
		out.caps.push_back(cp);
	}
}

bool SpringBones::Apply(const std::vector<MeshBone>& bones, const Skeleton* sk, SpringPose* pose,
                        int nb, const float model[16], Atom* owner, double dt,
                        const SpringSolveCtx* ctx)
{
	lastSolveFrame = Time::getSingleton()->frame;   // this frame is handled (see Update)
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

	// ---- animated globals (model space): the group's shared context, else a private pass --
	const glm::mat4 M = glm::make_mat4(model);
	std::vector<float> ownG;
	const float* G = (ctx && ctx->global.size() == (size_t)nb * 16) ? ctx->global.data() : nullptr;
	if (!G)
	{
		BuildGlobals(bones, pose, nb, ownG);
		G = ownG.data();
	}
	auto GB = [&](int i) { return glm::make_mat4(G + (size_t)i * 16); };

	// ---- colliders: the atom's ragdoll capsules, world space (chain's own bones excluded) --
	struct Cap { glm::vec3 a, b; float r; };
	std::vector<Cap> caps;
	if (collision && ctx)
	{
		for (const SpringSolveCtx::Capsule& cp : ctx->caps)
		{
			bool onChain = false;
			for (int ci : chainIdx) if (ci == cp.bone) { onChain = true; break; }
			if (!onChain) caps.push_back({ glm::make_vec3(cp.a), glm::make_vec3(cp.b), cp.r });
		}
	}
	else if (collision)
	{
		SpringSolveCtx own;
		BuildSolveCtx(bones, sk, pose, nb, model, owner, true, own);
		for (const SpringSolveCtx::Capsule& cp : own.caps)
		{
			bool onChain = false;
			for (int ci : chainIdx) if (ci == cp.bone) { onChain = true; break; }
			if (!onChain) caps.push_back({ glm::make_vec3(cp.a), glm::make_vec3(cp.b), cp.r });
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
		if (par >= 0) parentW = M * GB(par);
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

		glm::vec3 ext = grav;
		if (windOn)
		{
			// Wind push on the tail, sampled AT the tail so gusts/turbulence/zones ripple
			// along the chain segment by segment. Scaled by dt (NOT dt^2) to match the
			// per-step stiffness normalization: the equilibrium deflection is then
			// wind/(60*stiffness) regardless of frame rate.
			const Vector3 wv = Wind::Sample(Vector3(c.x, c.y, c.z));
			ext += glm::vec3((float)wv.x, (float)wv.y, (float)wv.z) * (float)(dt * 0.05);
		}
		glm::vec3 next = c + (c - p) * (1.0f - drag) + ext + (target - c) * stiff;
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
