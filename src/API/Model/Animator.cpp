#include "API/Model/Animator.h"
#include "API/Model/Atom.h"
#include "API/Model/BoneMap.h"
#include "API/Model/Mesh.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Jobs.h"
#include "API/Model/Time.h"
#include "API/Model/resdb.h"
#include "interface/AppInstance.h"   // renderer access: invalidateMesh on skinned release
#include "render/irender.h"
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>   // glm::rotation (shortest arc; GLM_ENABLE_EXPERIMENTAL is engine-wide)
#include <algorithm>
#include <cstring>
#include <iostream>

namespace nuke {

Animator::Animator() : Component("Animator") {}

void Animator::Init(Atom* parent)
{
	transform = &parent->GetTransform();
	atom = parent;
	parent->components.push_back(this);
}

void Animator::Destroy() { ReleaseSkinned(); }
void Animator::Pause() {}

void Animator::Reset()
{
	// Play stop / world reload: drop the runtime state; the sibling MeshRenderer re-resolves
	// its mesh from meshGuid on its own Reset, so the bind-pose source comes back by itself.
	ReleaseSkinned();
	mr = nullptr; srcMesh = nullptr;
	cur = Layer(); prev = Layer();
	fadeLeft = fadeDur = 0.0;
	playing = started = false;
	curState.clear();
	// smJson stays (it's the serialized source of truth); re-decode on next use.
	smLoaded = false;
	states.clear(); transitions.clear(); entryState.clear();
	boneMap.clear(); ikGoals.clear();
	atomBindClip = nullptr; channelAtoms.clear(); prevChanByBone.clear();
}

void Animator::ReleaseSkinned()
{
	if (!skinnedMesh) return;
	// The renderer caches GPU buffers keyed by Mesh* — drop that entry BEFORE deleting,
	// or a later allocation at the same address would be served the stale buffers.
	if (iRender* r = AppInstance::GetSingleton() ? AppInstance::GetSingleton()->render : nullptr)
		r->invalidateMesh(skinnedMesh);
	delete[] skinnedMesh->vertexArray;
	delete[] skinnedMesh->normalArray;
	skinnedMesh->vertexArray = nullptr;
	skinnedMesh->normalArray = nullptr;
	skinnedMesh->uvArray = nullptr;          // shared with the source — not ours to free
	delete skinnedMesh;
	skinnedMesh = nullptr;
}

AnimClip* Animator::ResolveClip(const std::string& ref) const
{
	if (ref.empty()) return nullptr;
	ResDB* db = ResDB::getSingleton();
	if (AnimClip* c = db->GetClip(ref)) return c;
	return db->GetClipByName(ref);
}

// Retarget resolution: runtime MapBone() renames win, then the .nubonemap asset, then as-is.
std::string Animator::MapName(const std::string& boneName) const
{
	auto rt = boneMap.find(boneName);
	if (rt != boneMap.end()) return rt->second;
	if (!boneMapGuid.empty())
		if (const BoneMap* asset = ResDB::getSingleton()->GetBoneMap(boneMapGuid))
		{
			auto am = asset->map.find(boneName);
			if (am != asset->map.end()) return am->second;
		}
	return boneName;
}

void Animator::BindLayer(Layer& l) const
{
	l.boneMap.clear();
	if (!l.clip || !srcMesh) return;
	l.boneMap.resize(l.clip->channels.size(), -1);
	for (size_t c = 0; c < l.clip->channels.size(); ++c)
	{
		const std::string want = MapName(l.clip->channels[c].bone);
		for (size_t b = 0; b < srcMesh->bones.size(); ++b)
			if (srcMesh->bones[b].name == want) { l.boneMap[c] = (int)b; break; }
	}
}

// Transform (node) animation: match the current clip's channels against the atom TREE
// (from the Animator's ROOT ancestor, so joint atoms that are siblings of the mesh atom
// are found too). Rebound whenever the playing clip changes.
static void CollectAtoms(Atom* a, std::map<std::string, Atom*>& out)
{
	if (!a) return;
	if (!a->name.empty() && !out.count(a->name)) out[a->name] = a;
	for (Atom* ch : a->children) CollectAtoms(ch, out);
}

void Animator::BindAtoms()
{
	atomBindClip = cur.clip;
	channelAtoms.clear();
	if (!cur.clip || !atom) return;
	Atom* root = atom;
	while (root->parent) root = root->parent;
	std::map<std::string, Atom*> byName;
	CollectAtoms(root, byName);
	channelAtoms.resize(cur.clip->channels.size(), nullptr);
	for (size_t c = 0; c < cur.clip->channels.size(); ++c)
	{
		auto it = byName.find(MapName(cur.clip->channels[c].bone));
		if (it != byName.end()) channelAtoms[c] = it->second;
	}
	// Outgoing clip's channels by bone name (cross-fade blending of atom TRS).
	prevChanByBone.clear();
	if (prev.clip)
		for (size_t c = 0; c < prev.clip->channels.size(); ++c)
			prevChanByBone[prev.clip->channels[c].bone] = (int)c;
}

// --- serialized state machine ---------------------------------------------------------------
void Animator::EnsureSM()
{
	if (smLoaded) return;
	smLoaded = true;
	if (smJson.empty()) return;
	try
	{
		nlohmann::json j = nlohmann::json::parse(smJson);
		states.clear(); transitions.clear();
		entryState = j.value("entry", "");
		if (j.contains("states"))
			for (auto it = j["states"].begin(); it != j["states"].end(); ++it)
			{
				StateDef d;
				d.clip  = it.value().value("clip", "");
				d.loop  = it.value().value("loop", true);
				d.speed = it.value().value("speed", 1.0);
				states[it.key()] = d;
			}
		if (j.contains("transitions"))
			for (const auto& t : j["transitions"])
				transitions[t.value("from", "")][t.value("to", "")] = t.value("fade", 0.0);
	}
	catch (const std::exception& e)
	{
		std::cout << "[Animator]\tbad state-machine json: " << e.what() << std::endl;
	}
}

void Animator::EncodeSM()
{
	nlohmann::json j;
	j["entry"] = entryState;
	nlohmann::json st = nlohmann::json::object();
	for (const auto& kv : states)
		st[kv.first] = { { "clip", kv.second.clip }, { "loop", kv.second.loop }, { "speed", kv.second.speed } };
	j["states"] = st;
	nlohmann::json tr = nlohmann::json::array();
	for (const auto& from : transitions)
		for (const auto& to : from.second)
			tr.push_back({ { "from", from.first }, { "to", to.first }, { "fade", to.second } });
	j["transitions"] = tr;
	smJson = j.dump();
}

bool Animator::EnsureTargets()
{
	if (!mr && atom)
		mr = atom->GetComponent<MeshRenderer>();
	if (!mr) return false;
	if (!srcMesh)
	{
		// The renderer's CURRENT mesh may already be our instance — resolve the asset by guid.
		Mesh* m = !mr->meshGuid.empty() ? ResDB::getSingleton()->GetMesh(mr->meshGuid) : mr->mesh;
		if (!m || !m->HasSkin()) return false;
		srcMesh = m;
	}
	if (!skinnedMesh)
	{
		skinnedMesh = new Mesh();
		strncpy(skinnedMesh->name, srcMesh->name, sizeof(skinnedMesh->name) - 1);
		skinnedMesh->numVerts    = srcMesh->numVerts;
		skinnedMesh->vertexArray = new float[(size_t)srcMesh->numVerts * 3];
		skinnedMesh->normalArray = new float[(size_t)srcMesh->numVerts * 3];
		skinnedMesh->uvArray     = srcMesh->uvArray;   // SHARED (uv never changes with pose)
		memcpy(skinnedMesh->vertexArray, srcMesh->vertexArray, sizeof(float) * 3 * srcMesh->numVerts);
		memcpy(skinnedMesh->normalArray, srcMesh->normalArray, sizeof(float) * 3 * srcMesh->numVerts);
		skinnedMesh->rtProxy = srcMesh;                // RT/BLAS uses the bind-pose source
	}
	return true;
}

void Animator::StartClip(AnimClip* c, bool clipLoop, double clipSpeed, double fade)
{
	if (!c) return;
	if (fade > 0.0 && cur.clip)
	{
		prev = cur;
		fadeLeft = fadeDur = fade;
	}
	else { prev = Layer(); fadeLeft = fadeDur = 0.0; }
	cur.clip = c; cur.t = 0.0; cur.loop = clipLoop; cur.speed = clipSpeed;
	BindLayer(cur);
	playing = true;
}

// --- script surface ------------------------------------------------------------------------
// A mesh is optional (transform-only animation) — EnsureTargets just resolves it when present.
void Animator::Play(const std::string& clip)                   { EnsureTargets(); StartClip(ResolveClip(clip), loop, speed, 0.0); }
void Animator::CrossFade(const std::string& clip, double fade) { EnsureTargets(); StartClip(ResolveClip(clip), loop, speed, fade); }
void Animator::Stop()        { playing = false; }
bool Animator::IsPlaying()   { return playing; }
std::string Animator::CurrentClip() { return cur.clip ? cur.clip->name : std::string(); }
double Animator::ClipTime()  { return cur.t; }
void Animator::SetClipTime(double t) { cur.t = t; }

void Animator::AddState(const std::string& name, const std::string& clip, bool stateLoop, double stateSpeed)
{
	EnsureSM();
	states[name] = { clip, stateLoop, stateSpeed };
	EncodeSM();
}
void Animator::RemoveState(const std::string& name)
{
	EnsureSM();
	states.erase(name);
	transitions.erase(name);
	for (auto& from : transitions) from.second.erase(name);
	if (entryState == name) entryState.clear();
	EncodeSM();
}
void Animator::AddTransition(const std::string& from, const std::string& to, double fade)
{
	EnsureSM();
	transitions[from][to] = fade;
	EncodeSM();
}
void Animator::RemoveTransition(const std::string& from, const std::string& to)
{
	EnsureSM();
	auto it = transitions.find(from);
	if (it != transitions.end()) { it->second.erase(to); if (it->second.empty()) transitions.erase(it); }
	EncodeSM();
}
void Animator::SetEntry(const std::string& name) { EnsureSM(); entryState = name; EncodeSM(); }
std::string Animator::Entry()                    { EnsureSM(); return entryState; }
void Animator::SetState(const std::string& name)
{
	EnsureSM();
	if (name == curState) return;
	auto it = states.find(name);
	if (it == states.end()) { std::cout << "[Animator]\tunknown state '" << name << "'" << std::endl; return; }
	double fade = 0.0;
	auto ft = transitions.find(curState);
	if (ft != transitions.end())
	{
		auto tt = ft->second.find(name);
		if (tt != ft->second.end()) fade = tt->second;
	}
	EnsureTargets();   // mesh optional (transform-only animation)
	StartClip(ResolveClip(it->second.clip), it->second.loop, it->second.speed, fade);
	curState = name;
}
std::string Animator::State() { return curState; }


void Animator::AddEvent(const std::string& clip, double t, const std::string& name)
{
	if (AnimClip* c = ResolveClip(clip)) c->AddEvent((float)t, name);
}

void Animator::MapBone(const std::string& from, const std::string& to)
{
	boneMap[from] = to;
	BindLayer(cur); BindLayer(prev);   // re-resolve active layers under the new map
}
void Animator::ClearBoneMap()
{
	boneMap.clear();
	BindLayer(cur); BindLayer(prev);
}

void Animator::SetIK(const std::string& tipBone, const Vector3& target, double weight)
{
	if (weight < 0.0) weight = 0.0;
	if (weight > 1.0) weight = 1.0;
	IKGoal& g = ikGoals[tipBone];   // keep any pole/segments already set for this tip
	g.target[0] = (float)target.x; g.target[1] = (float)target.y; g.target[2] = (float)target.z;
	g.weight = weight;
}
void Animator::SetIKPole(const std::string& tipBone, const Vector3& pole)
{
	IKGoal& g = ikGoals[tipBone];
	g.pole[0] = (float)pole.x; g.pole[1] = (float)pole.y; g.pole[2] = (float)pole.z;
	g.hasPole = true;
}
void Animator::SetIKChain(const std::string& tipBone, double segments)
{
	int s = (int)segments;
	if (s < 2) s = 2;
	ikGoals[tipBone].segments = s;
}
void Animator::ClearIK(const std::string& tipBone) { ikGoals.erase(tipBone); }

// --- sampling -------------------------------------------------------------------------------
namespace {

struct BonePose { glm::vec3 p, s; glm::quat r; };

// Piecewise-linear sample of a key track at time t (clamped at the ends).
void SampleKeys(const std::vector<AnimClip::Key>& keys, double t, float out[4])
{
	if (keys.empty()) return;
	if (keys.size() == 1 || t <= keys.front().t) { memcpy(out, keys.front().v, sizeof(float) * 4); return; }
	if (t >= keys.back().t) { memcpy(out, keys.back().v, sizeof(float) * 4); return; }
	size_t hi = 1;
	while (hi < keys.size() && keys[hi].t < (float)t) ++hi;   // tracks are short; linear scan is fine
	const AnimClip::Key& a = keys[hi - 1];
	const AnimClip::Key& b = keys[hi];
	const float f = (b.t > a.t) ? ((float)t - a.t) / (b.t - a.t) : 0.0f;
	for (int k = 0; k < 4; ++k) out[k] = a.v[k] + (b.v[k] - a.v[k]) * f;
}

glm::quat SampleRot(const std::vector<AnimClip::Key>& keys, double t, const glm::quat& def)
{
	if (keys.empty()) return def;
	if (keys.size() == 1 || t <= keys.front().t) { const float* v = keys.front().v; return glm::quat(v[3], v[0], v[1], v[2]); }
	if (t >= keys.back().t) { const float* v = keys.back().v; return glm::quat(v[3], v[0], v[1], v[2]); }
	size_t hi = 1;
	while (hi < keys.size() && keys[hi].t < (float)t) ++hi;
	const AnimClip::Key& a = keys[hi - 1];
	const AnimClip::Key& b = keys[hi];
	const float f = (b.t > a.t) ? ((float)t - a.t) / (b.t - a.t) : 0.0f;
	glm::quat qa(a.v[3], a.v[0], a.v[1], a.v[2]);
	glm::quat qb(b.v[3], b.v[0], b.v[1], b.v[2]);
	return glm::slerp(qa, qb, f);
}

// Sample one layer into `pose` (bind pose preloaded; only animated channels overwrite).
void SampleLayer(const Animator* /*unused*/, const AnimClip* clip, const std::vector<int>& boneMap,
                 double t, std::vector<BonePose>& pose)
{
	for (size_t c = 0; c < clip->channels.size(); ++c)
	{
		const int b = (c < boneMap.size()) ? boneMap[c] : -1;
		if (b < 0) continue;
		const AnimClip::Channel& ch = clip->channels[c];
		float v[4];
		if (!ch.pos.empty()) { SampleKeys(ch.pos, t, v); pose[b].p = glm::vec3(v[0], v[1], v[2]); }
		if (!ch.scl.empty()) { SampleKeys(ch.scl, t, v); pose[b].s = glm::vec3(v[0], v[1], v[2]); }
		if (!ch.rot.empty()) pose[b].r = SampleRot(ch.rot, t, pose[b].r);
	}
}

void BindPoseOf(const Mesh* m, std::vector<BonePose>& pose)
{
	pose.resize(m->bones.size());
	for (size_t i = 0; i < m->bones.size(); ++i)
	{
		const MeshBone& b = m->bones[i];
		pose[i].p = glm::vec3(b.localPos[0], b.localPos[1], b.localPos[2]);
		pose[i].r = glm::quat(b.localRot[3], b.localRot[0], b.localRot[1], b.localRot[2]);
		pose[i].s = glm::vec3(b.localScale[0], b.localScale[1], b.localScale[2]);
	}
}

double Advance(double t, double dt, double duration, bool loop)
{
	t += dt;
	if (duration <= 0.0) return 0.0;
	if (loop) { t = fmod(t, duration); if (t < 0.0) t += duration; }
	else if (t > duration) t = duration;
	return t;
}

}  // namespace

void Animator::FixedUpdate() {}

// Write the current clip's sampled TRS onto matched atoms (transform/node animation).
// Runs whether or not a skinned mesh exists; cross-fade blends against the outgoing clip.
void Animator::ApplyAtomAnimation()
{
	if (!cur.clip) return;
	if (atomBindClip != cur.clip) BindAtoms();
	const float w = (fadeLeft > 0.0 && fadeDur > 0.0 && prev.clip) ? (float)(fadeLeft / fadeDur) : 0.0f;
	for (size_t c = 0; c < cur.clip->channels.size(); ++c)
	{
		Atom* a = (c < channelAtoms.size()) ? channelAtoms[c] : nullptr;
		if (!a) continue;
		const AnimClip::Channel& ch = cur.clip->channels[c];
		const AnimClip::Channel* pch = nullptr;
		if (w > 0.0f)
		{
			auto it = prevChanByBone.find(ch.bone);
			if (it != prevChanByBone.end()) pch = &prev.clip->channels[it->second];
		}
		Transform& t = a->GetTransform();
		float v[4];
		if (!ch.pos.empty())
		{
			SampleKeys(ch.pos, cur.t, v);
			glm::vec3 p(v[0], v[1], v[2]);
			if (pch && !pch->pos.empty()) { SampleKeys(pch->pos, prev.t, v); p = glm::mix(p, glm::vec3(v[0], v[1], v[2]), w); }
			t.position = Vector3(p.x, p.y, p.z);
		}
		if (!ch.scl.empty())
		{
			SampleKeys(ch.scl, cur.t, v);
			glm::vec3 s(v[0], v[1], v[2]);
			if (pch && !pch->scl.empty()) { SampleKeys(pch->scl, prev.t, v); s = glm::mix(s, glm::vec3(v[0], v[1], v[2]), w); }
			t.scale = Vector3(s.x, s.y, s.z);
		}
		if (!ch.rot.empty())
		{
			glm::quat q = SampleRot(ch.rot, cur.t, glm::quat(1, 0, 0, 0));
			if (pch && !pch->rot.empty()) q = glm::slerp(q, SampleRot(pch->rot, prev.t, q), w);
			t.rotation = Quaternion(q.x, q.y, q.z, q.w);
		}
	}
}

void Animator::Update()
{
	// Auto-start once (play mode only — World doesn't Update otherwise): the serialized
	// machine's ENTRY state wins; the bare initial clip is the no-machine fallback.
	if (!started)
	{
		started = true;
		EnsureSM();
		if (playOnStart)
		{
			if (!entryState.empty() && states.count(entryState)) SetState(entryState);
			else if (!clipGuid.empty())
				StartClip(ResolveClip(clipGuid), loop, speed, 0.0);
		}
	}
	if (!playing || !cur.clip) return;
	const bool haveSkin = EnsureTargets();   // false = transform-only animation (no mesh needed)
	if (haveSkin)
	{
		// A clip started BEFORE the mesh resolved (auto-start order) has no bone map yet.
		if (cur.boneMap.size() != cur.clip->channels.size()) BindLayer(cur);
		if (prev.clip && prev.boneMap.size() != prev.clip->channels.size()) BindLayer(prev);
	}

	const double dt = Time::getSingleton()->delta;
	const double prevT = cur.t;
	cur.t = Advance(cur.t, dt * cur.speed, cur.clip->duration, cur.loop);   // non-loop clamps: holds the last pose

	// Events crossed this step (collected now, DISPATCHED after skinning — a handler may
	// legally Play()/SetState() and must not mutate the pose mid-computation).
	std::vector<std::string> fired;
	if (dt > 0.0)
	{
		auto collect = [&](double lo, double hi)
		{
			for (const AnimClip::Event& e : cur.clip->events)
				if (e.t > lo && e.t <= hi) fired.push_back(e.name);
		};
		if (cur.t >= prevT) collect(prevT, cur.t);
		else { collect(prevT, cur.clip->duration); collect(-1e-6, cur.t); }   // loop wrap
	}
	if (fadeLeft > 0.0 && prev.clip)
	{
		prev.t = Advance(prev.t, dt * prev.speed, prev.clip->duration, prev.loop);
		fadeLeft -= dt;
	}

	// Transform (node) animation: drive matching atoms regardless of skinning.
	ApplyAtomAnimation();

	if (haveSkin)
	{
	// --- pose: bind -> current clip; cross-fade blends the OUTGOING clip on top ---------
	std::vector<BonePose> pose;
	BindPoseOf(srcMesh, pose);
	SampleLayer(this, cur.clip, cur.boneMap, cur.t, pose);
	if (fadeLeft > 0.0 && prev.clip)
	{
		std::vector<BonePose> old;
		BindPoseOf(srcMesh, old);
		SampleLayer(this, prev.clip, prev.boneMap, prev.t, old);
		const float w = (float)(fadeLeft / fadeDur);   // 1 -> 0 over the fade
		for (size_t i = 0; i < pose.size(); ++i)
		{
			pose[i].p = glm::mix(pose[i].p, old[i].p, w);
			pose[i].s = glm::mix(pose[i].s, old[i].s, w);
			pose[i].r = glm::slerp(pose[i].r, old[i].r, w);
		}
	}

	// --- globals (bones are parent-before-child by construction) ------------------------
	const size_t nb = srcMesh->bones.size();
	std::vector<glm::mat4> global(nb), palette(nb);
	auto forwardPass = [&]()
	{
		for (size_t i = 0; i < nb; ++i)
		{
			glm::mat4 local = glm::translate(glm::mat4(1.0f), pose[i].p)
			                * glm::mat4_cast(pose[i].r)
			                * glm::scale(glm::mat4(1.0f), pose[i].s);
			const int par = srcMesh->bones[i].parent;
			global[i] = (par >= 0) ? global[par] * local : local;
		}
	};
	forwardPass();

	// --- IK post-pass: FABRIK over the goal's chain (2..N segments) + optional pole ------
	// Solved on joint POSITIONS, converted back to LOCAL rotations (weighted slerp), then
	// the forward pass re-runs — so IK composes with clips, fades and multiple goals.
	if (!ikGoals.empty())
	{
		// World-space goals -> the mesh's model space (skinning happens pre-transform).
		Vector3 gp = transform->globalPosition();
		Quaternion gq = transform->globalRotation();
		Vector3 gs = transform->globalScale();
		glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3((float)gp.x, (float)gp.y, (float)gp.z))
		                * glm::mat4_cast(glm::quat((float)gq.w, (float)gq.x, (float)gq.y, (float)gq.z))
		                * glm::scale(glm::mat4(1.0f), glm::vec3((float)gs.x, (float)gs.y, (float)gs.z));
		const glm::mat4 invModel = glm::inverse(model);
		bool touched = false;

		for (const auto& goal : ikGoals)
		{
			int tip = -1;
			for (size_t b = 0; b < nb; ++b)
				if (srcMesh->bones[b].name == goal.first) { tip = (int)b; break; }
			if (tip < 0) continue;

			// Chain joints, root-first: tip walking up `segments` parents (shrunk to what exists).
			std::vector<int> chain;                       // chain[0] = root ... chain.back() = tip
			{
				std::vector<int> up{ tip };
				int j = tip;
				for (int s = 0; s < goal.second.segments && srcMesh->bones[j].parent >= 0; ++s)
					up.push_back(j = srcMesh->bones[j].parent);
				if (up.size() < 3) continue;              // need >= 2 segments
				chain.assign(up.rbegin(), up.rend());
			}
			const size_t S = chain.size() - 1;            // segment count

			std::vector<glm::vec3> p(chain.size());       // current joint positions (model space)
			for (size_t i = 0; i < chain.size(); ++i) p[i] = glm::vec3(global[chain[i]][3]);
			std::vector<float> len(S);
			float total = 0.0f;
			bool degenerate = false;
			for (size_t i = 0; i < S; ++i)
			{
				len[i] = glm::length(p[i + 1] - p[i]);
				if (len[i] < 1e-5f) degenerate = true;
				total += len[i];
			}
			if (degenerate) continue;

			const glm::vec3 t = glm::vec3(invModel * glm::vec4(goal.second.target[0], goal.second.target[1], goal.second.target[2], 1.0f));
			const glm::vec3 root0 = p[0];
			std::vector<glm::vec3> q = p;

			if (glm::length(t - root0) >= total)          // unreachable: stretch straight at it
			{
				const glm::vec3 dir = glm::normalize(t - root0);
				q[0] = root0;
				for (size_t i = 0; i < S; ++i) q[i + 1] = q[i] + dir * len[i];
			}
			else
			{
				// Pole pre-swing: rotate intermediate joints around the root->target axis so the
				// bend plane faces the pole (keeps radii; FABRIK repairs lengths right after).
				auto polePass = [&]()
				{
					if (!goal.second.hasPole) return;
					const glm::vec3 poleM = glm::vec3(invModel * glm::vec4(goal.second.pole[0], goal.second.pole[1], goal.second.pole[2], 1.0f));
					glm::vec3 axis = t - root0;
					const float al2 = glm::dot(axis, axis);
					if (al2 < 1e-10f) return;
					axis /= std::sqrt(al2);
					glm::vec3 pPerp = (poleM - root0) - axis * glm::dot(poleM - root0, axis);
					if (glm::dot(pPerp, pPerp) < 1e-10f) return;
					pPerp = glm::normalize(pPerp);
					for (size_t i = 1; i < S; ++i)
					{
						const glm::vec3 rel = q[i] - root0;
						const float along = glm::dot(rel, axis);
						const float r = glm::length(rel - axis * along);
						if (r < 1e-6f) continue;
						q[i] = root0 + axis * along + pPerp * r;
					}
				};
				for (int it = 0; it < 12; ++it)           // FABRIK: backward + forward reach
				{
					polePass();
					q[S] = t;
					for (size_t i = S; i-- > 0; ) q[i] = q[i + 1] + glm::normalize(q[i] - q[i + 1]) * len[i];
					q[0] = root0;
					for (size_t i = 0; i < S; ++i) q[i + 1] = q[i] + glm::normalize(q[i + 1] - q[i]) * len[i];
					if (glm::length(q[S] - t) < 1e-4f && !goal.second.hasPole) break;
				}
			}

			// Positions -> LOCAL rotations, root to tip; the forward pass after each joint
			// keeps the "current direction" honest for the next one.
			const float w = (float)goal.second.weight;
			for (size_t i = 0; i < S; ++i)
			{
				const int bi = chain[i];
				const glm::vec3 cur0 = glm::vec3(global[chain[i]][3]);
				const glm::vec3 cur1 = glm::vec3(global[chain[i + 1]][3]);
				const glm::vec3 d0 = cur1 - cur0, d1 = q[i + 1] - q[i];
				if (glm::dot(d0, d0) < 1e-10f || glm::dot(d1, d1) < 1e-10f) continue;
				const glm::quat delta = glm::rotation(glm::normalize(d0), glm::normalize(d1));
				const int par = srcMesh->bones[bi].parent;
				const glm::quat parentG = par >= 0 ? glm::quat_cast(global[par]) : glm::quat(1, 0, 0, 0);
				const glm::quat newLocal = glm::inverse(parentG) * (delta * glm::quat_cast(global[bi]));
				pose[bi].r = glm::slerp(pose[bi].r, glm::normalize(newLocal), w);
				forwardPass();                            // few bones — recompute is nothing
			}
			touched = true;
		}
		if (touched) forwardPass();
	}

	for (size_t i = 0; i < nb; ++i)
		palette[i] = global[i] * glm::make_mat4(srcMesh->bones[i].invBind);

	// --- skin on the job pool into the instance mesh ------------------------------------
	const int n = srcMesh->numVerts;
	const float* sv = srcMesh->vertexArray;
	const float* sn = srcMesh->normalArray;
	const unsigned short* bi = srcMesh->boneIndex;
	const float* bw = srcMesh->boneWeight;
	float* dv = skinnedMesh->vertexArray;
	float* dn = skinnedMesh->normalArray;
	const glm::mat4* pal = palette.data();
	const int palCount = (int)palette.size();
	Jobs::ParallelFor(0, n, 4096, [sv, sn, bi, bw, dv, dn, pal, palCount](int v)
	{
		const glm::vec4 sp(sv[v * 3 + 0], sv[v * 3 + 1], sv[v * 3 + 2], 1.0f);
		const glm::vec4 np(sn[v * 3 + 0], sn[v * 3 + 1], sn[v * 3 + 2], 0.0f);
		glm::vec3 p(0.0f), nn(0.0f);
		for (int k = 0; k < 4; ++k)
		{
			const float w = bw[v * 4 + k];
			if (w <= 0.0f) continue;
			const int b = bi[v * 4 + k];
			if (b >= palCount) continue;
			p  += w * glm::vec3(pal[b] * sp);
			nn += w * glm::vec3(pal[b] * np);
		}
		const float len = glm::length(nn);
		if (len > 1e-6f) nn /= len;
		dv[v * 3 + 0] = p.x;  dv[v * 3 + 1] = p.y;  dv[v * 3 + 2] = p.z;
		dn[v * 3 + 0] = nn.x; dn[v * 3 + 1] = nn.y; dn[v * 3 + 2] = nn.z;
	});

	++skinnedMesh->version;              // renderer re-uploads its cached buffers
	skinnedMesh->boundsValid = false;    // culling AABB follows the pose
	mr->mesh = skinnedMesh;              // the sibling renderer draws the posed instance
	}   // haveSkin

	// Fire the crossed events LAST: handlers may Play()/SetState() freely — this frame's
	// pose is already committed. Game thread, game lock held (same contract as OnGUI).
	for (const std::string& name : fired)
		if (atom)
			for (Component* c : atom->components)
				if (c && c->enabled) c->OnAnimEvent(name.c_str());
}

}  // namespace nuke
