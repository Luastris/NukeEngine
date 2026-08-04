#include "API/Model/Animator.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/AnimIK.h"
#include "API/Model/Ragdoll.h"
#include "API/Model/AnimSM.h"
#include "API/Model/Atom.h"
#include "API/Model/Audio.h"
#include "API/Model/BlendSpace.h"
#include "API/Model/BoneMap.h"
#include "API/Model/Camera.h"
#include "API/Model/Mesh.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Jobs.h"
#include "API/Model/Prefab.h"
#include "API/Model/Retarget.h"
#include "API/Model/Time.h"
#include "API/Model/resdb.h"
#include "interface/AppInstance.h"
#include "reflect/ReflectBind.h"   // Reflect_ComponentFieldChanged (prop tracks)
#include "render/irender.h"
#include <set>
#include <mutex>
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>   // glm::rotation; needs GLM_ENABLE_EXPERIMENTAL (set engine-wide)
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

// Destroy/Reset live BELOW the AnimGraph definition (they delete it).
void Animator::Pause() {}

void Animator::ReleaseSkinned()
{
	if (!skinnedMesh) return;
	// Drop the renderer's Mesh*-keyed cache entry BEFORE deleting: addresses get reused.
	if (iRender* r = AppInstance::GetSingleton() ? AppInstance::GetSingleton()->render : nullptr)
		r->invalidateMesh(skinnedMesh);
	delete[] skinnedMesh->vertexArray;
	delete[] skinnedMesh->normalArray;
	skinnedMesh->vertexArray = nullptr;
	skinnedMesh->normalArray = nullptr;
	skinnedMesh->uvArray = nullptr;          // shared with the source — not ours to free
	skinnedMesh->indexArray = nullptr;       // v4 streams are shared too (see EnsureTargets)
	skinnedMesh->tangentArray = nullptr;
	skinnedMesh->uv2Array = nullptr;
	skinnedMesh->colorArray = nullptr;
	delete skinnedMesh;
	skinnedMesh = nullptr;
}

AnimClip* Animator::ResolveClip(const std::string& ref) const
{
	if (ref.empty()) return nullptr;
	if (previewClip && (ref == previewClip->guid || ref == previewClip->name)) return previewClip;
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
	if (!l.clip || !bonesRef) return;
	// Foreign-skeleton clip: swap in the cached chain retarget before name matching.
	if (smr && smr->skeleton)
		l.clip = RetargetCached(l.clip, smr->skeleton,
		                        boneMapGuid.empty() ? nullptr : ResDB::getSingleton()->GetBoneMap(boneMapGuid));
	l.boneMap.resize(l.clip->channels.size(), -1);
	for (size_t c = 0; c < l.clip->channels.size(); ++c)
	{
		const std::string want = MapName(l.clip->channels[c].bone);
		for (size_t b = 0; b < bonesRef->size(); ++b)
			if ((*bonesRef)[b].name == want) { l.boneMap[c] = (int)b; break; }
	}
}

// Collect a subtree's atoms by name (first one wins).
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

// Depth-first: every SkinnedMeshRenderer under `a` (including `a` itself).
static void CollectSMRs(Atom* a, std::vector<SkinnedMeshRenderer*>& out)
{
	if (!a) return;
	for (Component* c : a->components)
		if (SkinnedMeshRenderer* s = dynamic_cast<SkinnedMeshRenderer*>(c))
			out.push_back(s);
	for (Atom* ch : a->children) CollectSMRs(ch, out);
}

bool Animator::EnsureTargets()
{
	if (!mr && atom)
		mr = atom->GetComponent<MeshRenderer>();
	// NEW pipeline: SkinnedMeshRenderers supply the skeleton ASSET and own pose palettes +
	// skinned instances — the Animator samples/blends/IKs ONE pose and hands it to EVERY
	// renderer in the SUBTREE sharing that skeleton (modular characters: the Animator sits on
	// the prefab root, the meshes on child nodes).
	if (!mr || dynamic_cast<SkinnedMeshRenderer*>(mr))
	{
		if (smrs.empty())
		{
			std::vector<SkinnedMeshRenderer*> found;
			CollectSMRs(atom, found);
			Skeleton* sk = nullptr;
			for (SkinnedMeshRenderer* s : found)
			{
				Skeleton* ssk = s->EnsureSkeleton();
				if (!ssk || ssk->bones.empty()) continue;
				if (!sk) sk = ssk;                    // first valid skeleton wins
				if (ssk == sk) smrs.push_back(s);     // same asset = same palette = one pose
			}
			if (!smrs.empty())
			{
				smr = smrs[0];
				bonesRef = &sk->bones;
			}
		}
		if (!smrs.empty()) return true;
		// No usable skeleton: transform-only animation. An SMR without a skeleton must NOT
		// fall through to the legacy embedded path (it would clobber the SMR's instance).
		if (!mr || dynamic_cast<SkinnedMeshRenderer*>(mr)) return false;
	}
	if (!srcMesh)
	{
		// The renderer's CURRENT mesh may already be our instance — resolve the asset by guid.
		Mesh* m = !mr->meshGuid.empty() ? ResDB::getSingleton()->GetMesh(mr->meshGuid) : mr->mesh;
		if (!m || !m->HasSkin()) return false;
		srcMesh = m;
	}
	bonesRef = &srcMesh->bones;
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
		// v4 indexed data: SHARED with the source — topology/sections don't change with pose
		// (ReleaseSkinned must not free these).
		skinnedMesh->indexArray   = srcMesh->indexArray;
		skinnedMesh->numIndices   = srcMesh->numIndices;
		skinnedMesh->tangentArray = srcMesh->tangentArray;
		skinnedMesh->uv2Array     = srcMesh->uv2Array;
		skinnedMesh->colorArray   = srcMesh->colorArray;
		skinnedMesh->sections     = srcMesh->sections;
		skinnedMesh->lods         = srcMesh->lods;
		skinnedMesh->numSlots     = srcMesh->numSlots;
		skinnedMesh->slotNames    = srcMesh->slotNames;
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
	if (!smGuid.empty()) { ForceGraphState(name); return; }   // controller: forced jump
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
	EnsureTargets();
	StartClip(ResolveClip(it->second.clip), it->second.loop, it->second.speed, fade);
	curState = name;
}
std::string Animator::State()
{
	if (!smGuid.empty()) return GraphStateName();
	return curState;
}


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

void Animator::SetChainIK(const std::string& chain, const Vector3& target, double weight)
{
	if (weight < 0.0) weight = 0.0;
	if (weight > 1.0) weight = 1.0;
	IKGoal& g = ikGoals[chain];
	g.isChain = true;
	g.target[0] = (float)target.x; g.target[1] = (float)target.y; g.target[2] = (float)target.z;
	g.weight = weight;
}
void Animator::SetChainIKPole(const std::string& chain, const Vector3& pole)
{
	IKGoal& g = ikGoals[chain];
	g.isChain = true;
	g.pole[0] = (float)pole.x; g.pole[1] = (float)pole.y; g.pole[2] = (float)pole.z;
	g.hasPole = true;
}
void Animator::SetChainIKNormal(const std::string& chain, const Vector3& normal, double weight)
{
	IKGoal& g = ikGoals[chain];
	g.isChain = true;
	g.hasNormal = true;
	g.normal[0] = (float)normal.x; g.normal[1] = (float)normal.y; g.normal[2] = (float)normal.z;
	g.normalWeight = weight < 0.0 ? 0.0 : (weight > 1.0 ? 1.0 : weight);
}
void Animator::ClearChainIK(const std::string& chain) { ikGoals.erase(chain); }

Skeleton* Animator::CurrentSkeleton() const { return smr ? smr->skeleton : nullptr; }

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
	while (hi < keys.size() && keys[hi].t < (float)t) ++hi;
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

void BindPoseOf(const std::vector<MeshBone>& bones, std::vector<BonePose>& pose)
{
	pose.resize(bones.size());
	for (size_t i = 0; i < bones.size(); ++i)
	{
		const MeshBone& b = bones[i];
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

// One evaluated clip stream this frame: time window + blend weight. `primary` = the stream the
// legacy events fire from (dominant clip); notifies also fire from weight > 0.45 streams.
struct AnimCursor
{
	AnimClip* clip = nullptr;
	double prevT = 0, newT = 0;
	float  weight = 1;
	bool   loop = true;
	bool   primary = false;
};

// Inertialization decay (Bollo, GDC 2018): quintic from offset x0 / velocity v0 to zero with
// zero end velocity+acceleration inside T seconds; the T-clamp prevents overshoot.
float InertEval(float x0, float v0, float T, float t)
{
	if (x0 == 0.0f || t >= T) return 0.0f;
	if (v0 > 0.0f) v0 = 0.0f;
	if (v0 < 0.0f) T = std::min(T, -5.0f * x0 / v0);
	if (T < 1e-5f || t >= T) return 0.0f;
	const float T2 = T * T;
	float a0 = (-8.0f * v0 * T - 20.0f * x0) / T2;
	if (a0 < 0.0f) a0 = 0.0f;
	const float T3 = T2 * T, T4 = T3 * T, T5 = T4 * T;
	const float A = -(a0 * T2 + 6.0f * v0 * T + 12.0f * x0) / (2.0f * T5);
	const float B =  (3.0f * a0 * T2 + 16.0f * v0 * T + 30.0f * x0) / (2.0f * T4);
	const float C = -(3.0f * a0 * T2 + 12.0f * v0 * T + 20.0f * x0) / (2.0f * T3);
	return ((((A * t + B) * t + C) * t + a0 * 0.5f) * t + v0) * t + x0;
}

// Mirror-pair name guess: first matching L/R token swapped ("" = no pair).
std::string MirrorName(const std::string& n)
{
	static const char* pairs[][2] = {
		{ "Left", "Right" }, { "left", "right" }, { "LEFT", "RIGHT" },
		{ "_L", "_R" }, { "_l", "_r" }, { ".L", ".R" }, { ".l", ".r" },
		{ "L_", "R_" }, { "l_", "r_" },
	};
	for (const auto& p : pairs)
		for (int d = 0; d < 2; ++d)
		{
			const std::string a = p[d], b = p[1 - d];
			const size_t at = n.find(a);
			if (at != std::string::npos)
			{
				std::string m = n;
				m.replace(at, a.size(), b);
				return m;
			}
		}
	return std::string();
}

}  // namespace

// ---- controller-mode runtime -----------------------------------------------------------------
// Owns the flattened .nusm layers, parameters, transition/inertialization state and the shared
// clip-extras evaluation (events/notifies/curves/prop tracks) used by BOTH drive modes.
struct Animator::AnimGraph
{
	// parameters
	std::map<std::string, float> floats;
	std::map<std::string, bool>  bools;
	std::set<std::string>        triggers;
	std::map<std::string, float> curveVals;   // blended clip curves this frame

	// flattened controller
	AnimSM* sm = nullptr;
	std::string loadedGuid;
	struct RTrans { int to = -1; const AnimSM::Transition* def = nullptr; };
	struct FState
	{
		std::string name;                       // full path ("Combat/Slash")
		const AnimSM::State* def = nullptr;
		std::vector<RTrans> trans;              // any-state edges first, then explicit (inner scope first)
		AnimClip* clip = nullptr;
		BlendSpace* blend = nullptr;
		std::vector<AnimClip*> blendClips;      // per blend point
		std::vector<std::vector<int>> maps;     // per clip: channel -> bone
		bool resolved = false;
	};
	struct IChan
	{
		glm::vec3 pDir = glm::vec3(0); float pX0 = 0, pV0 = 0;
		glm::vec3 rAxis = glm::vec3(1, 0, 0); float rX0 = 0, rV0 = 0;
		glm::vec3 sDir = glm::vec3(0); float sX0 = 0, sV0 = 0;
	};
	struct LayerRT
	{
		const AnimSM::Layer* def = nullptr;
		std::vector<FState> states;
		std::map<std::string, int> byName;
		int    entry = -1;
		int    cur = -1;
		double t = 0;                           // clip state: seconds; blend state: normalized
		double prevT = 0;                       // state time before this frame's advance
		double cycA = 0, cycB = 0;              // continuous cycle window (exit-time crossing)
		int    src = -1;                        // crossfade source state
		double srcT = 0, srcPrevT = 0;
		double transLeft = 0, transDur = 0;
		int    transMode = 0, transInterrupt = 0;
		std::vector<IChan> inert;
		double inertT = 0, inertDur = 0; bool inertActive = false;
		std::vector<BonePose> lastPose, prevPose; int hist = 0; double lastDt = 1.0 / 60.0;
		std::vector<float> mask;                // per-bone weights (skeleton group)
		float  weightOverride = -1;             // < 0 = asset weight
	};
	std::vector<LayerRT> layers;
	std::map<std::string, double> syncPhase;    // group -> normalized phase
	std::vector<int> mirrorMap; bool mirrorBuilt = false;
	std::map<AnimClip*, std::vector<BonePose>> addRef;     // additive first-frame reference
	std::map<AnimClip*, int> rootChan;                     // clip -> root-bone channel

	// IK v2 runtime (kept here so the exported Animator layout stays stable)
	struct LookGoal { float target[3]; double weight; double maxAngle; };
	std::map<std::string, LookGoal> lookAts;               // chain/bone -> world-space aim
	double pelvisOffset = 0.0;                             // vertical hips shift, world units

	// Committed-pose history (both drive modes) + the legacy-path inertializer MatchTo uses
	// for motion-matching jumps.
	std::vector<BonePose> commitLast, commitPrev;
	int    commitHist = 0;
	double commitDt = 1.0 / 60.0;
	std::vector<IChan> legInert;
	double legInertT = 0, legInertDur = 0;
	bool   legInertActive = false;

	// Inertialization capture/add over raw pose buffers (LayerRT wraps these with its own state).
	static void CaptureInert(std::vector<IChan>& ch, const std::vector<BonePose>& last,
	                         const std::vector<BonePose>& prev, const std::vector<BonePose>& tgt,
	                         double dt)
	{
		const size_t nb = tgt.size();
		ch.resize(nb);
		const float idt = (float)std::max(dt, 1e-4);
		for (size_t i = 0; i < nb; ++i)
		{
			IChan& c = ch[i];
			const glm::vec3 dp = last[i].p - tgt[i].p;
			c.pX0 = glm::length(dp);
			c.pDir = c.pX0 > 1e-6f ? dp / c.pX0 : glm::vec3(0);
			c.pV0 = c.pX0 > 1e-6f ? glm::dot((last[i].p - prev[i].p) / idt, c.pDir) : 0.0f;
			glm::quat dq = last[i].r * glm::inverse(tgt[i].r);
			if (dq.w < 0) dq = -dq;
			const glm::vec3 dv(dq.x, dq.y, dq.z);
			const float vl = glm::length(dv);
			c.rX0 = 2.0f * atan2f(vl, dq.w);
			c.rAxis = vl > 1e-6f ? dv / vl : glm::vec3(1, 0, 0);
			glm::quat pq = prev[i].r * glm::inverse(tgt[i].r);
			if (pq.w * dq.w + pq.x * dq.x + pq.y * dq.y + pq.z * dq.z < 0) pq = -pq;
			const float angPrev = 2.0f * atan2f(glm::dot(glm::vec3(pq.x, pq.y, pq.z), c.rAxis), pq.w);
			c.rV0 = (c.rX0 - angPrev) / idt;
			const glm::vec3 ds = last[i].s - tgt[i].s;
			c.sX0 = glm::length(ds);
			c.sDir = c.sX0 > 1e-6f ? ds / c.sX0 : glm::vec3(0);
			c.sV0 = c.sX0 > 1e-6f ? glm::dot((last[i].s - prev[i].s) / idt, c.sDir) : 0.0f;
		}
	}
	static void AddInert(const std::vector<IChan>& ch, double T, double t, std::vector<BonePose>& pose)
	{
		if (ch.size() != pose.size()) return;
		for (size_t i = 0; i < pose.size(); ++i)
		{
			const IChan& c = ch[i];
			const float xp = InertEval(c.pX0, c.pV0, (float)T, (float)t);
			if (xp != 0.0f) pose[i].p += c.pDir * xp;
			const float xr = InertEval(c.rX0, c.rV0, (float)T, (float)t);
			if (xr != 0.0f) pose[i].r = glm::normalize(glm::angleAxis(xr, c.rAxis) * pose[i].r);
			const float xs = InertEval(c.sX0, c.sV0, (float)T, (float)t);
			if (xs != 0.0f) pose[i].s += c.sDir * xs;
		}
	}

	static void Ensure(Animator* a) { if (!a->graph) a->graph = new AnimGraph(); }

	// ---- controller build --------------------------------------------------------------------
	struct Scope
	{
		std::string prefix;
		const std::vector<AnimSM::Transition>* trans = nullptr;
		std::string entry, firstChild;
	};

	static void CollectStates(const std::vector<AnimSM::State>& src, const std::string& prefix,
	                          LayerRT& lr, std::vector<Scope>& scopes)
	{
		for (const AnimSM::State& s : src)
		{
			const std::string full = prefix.empty() ? s.name : prefix + "/" + s.name;
			if (s.states.empty())
			{
				FState f;
				f.name = full;
				f.def = &s;
				lr.byName[full] = (int)lr.states.size();
				lr.states.push_back(f);
			}
			else
			{
				Scope sc;
				sc.prefix = full;
				sc.trans = &s.transitions;
				sc.entry = s.entry;
				sc.firstChild = s.states.front().name;
				scopes.push_back(sc);
				CollectStates(s.states, full, lr, scopes);
			}
		}
	}

	static int ResolveEntry(LayerRT& lr, const std::vector<Scope>& scopes,
	                        const std::string& prefix, const std::string& entry,
	                        const std::string& firstChild, int depth = 0)
	{
		if (depth > 8) return -1;
		const std::string name = entry.empty() ? firstChild : entry;
		if (name.empty()) return lr.states.empty() ? -1 : 0;
		const std::string full = prefix.empty() ? name : prefix + "/" + name;
		auto it = lr.byName.find(full);
		if (it != lr.byName.end()) return it->second;
		for (const Scope& sc : scopes)
			if (sc.prefix == full)
				return ResolveEntry(lr, scopes, full, sc.entry, sc.firstChild, depth + 1);
		return -1;
	}

	static int ResolveTarget(LayerRT& lr, const std::vector<Scope>& scopes,
	                         const std::string& prefix, const std::string& to)
	{
		if (to.empty()) return -1;
		const std::string full = prefix.empty() ? to : prefix + "/" + to;
		auto it = lr.byName.find(full);
		if (it != lr.byName.end()) return it->second;
		for (const Scope& sc : scopes)
			if (sc.prefix == full)
				return ResolveEntry(lr, scopes, full, sc.entry, sc.firstChild);
		// absolute path fallback (cross-machine targets)
		it = lr.byName.find(to);
		if (it != lr.byName.end()) return it->second;
		for (const Scope& sc : scopes)
			if (sc.prefix == to)
				return ResolveEntry(lr, scopes, to, sc.entry, sc.firstChild);
		return -1;
	}

	void EnsureController(Animator* a)
	{
		AnimSM* want = a->previewSm ? a->previewSm : ResDB::getSingleton()->GetAnimSM(a->smGuid);
		if (want == sm && sm) return;
		sm = want;
		loadedGuid = a->smGuid;
		layers.clear();
		syncPhase.clear();
		if (!sm) return;
		for (const AnimSM::Param& p : sm->params)
		{
			if (p.type == 1) { if (!bools.count(p.name)) bools[p.name] = p.def != 0; }
			else if (p.type == 0) { if (!floats.count(p.name)) floats[p.name] = p.def; }
		}
		for (const AnimSM::Layer& l : sm->layers)
		{
			LayerRT lr;
			lr.def = &l;
			std::vector<Scope> scopes;
			Scope root;
			root.prefix = "";
			root.trans = &l.transitions;
			root.entry = l.entry;
			root.firstChild = l.states.empty() ? "" : l.states.front().name;
			scopes.push_back(root);
			CollectStates(l.states, "", lr, scopes);
			// per-state edges: any-state first, then explicit, inner scope before outer
			std::vector<const Scope*> byDepth;                // innermost (longest prefix) first
			for (const Scope& sc : scopes) byDepth.push_back(&sc);
			std::sort(byDepth.begin(), byDepth.end(),
			          [](const Scope* x, const Scope* y) { return x->prefix.size() > y->prefix.size(); });
			for (size_t si = 0; si < lr.states.size(); ++si)
			{
				FState& f = lr.states[si];
				std::vector<RTrans> anyV, expV;
				for (const Scope* sc : byDepth)
				{
					const bool inScope = sc->prefix.empty() ||
					                     (f.name.size() > sc->prefix.size() &&
					                      f.name.compare(0, sc->prefix.size(), sc->prefix) == 0 &&
					                      f.name[sc->prefix.size()] == '/');
					if (!inScope) continue;
					for (const AnimSM::Transition& T : *sc->trans)
					{
						const int to = ResolveTarget(lr, scopes, sc->prefix, T.to);
						if (to < 0) continue;
						if (T.from.empty() || T.from == "*")
						{
							if (to != (int)si) anyV.push_back({ to, &T });
							continue;
						}
						const std::string fromFull = sc->prefix.empty() ? T.from : sc->prefix + "/" + T.from;
						const bool match = f.name == fromFull ||
						                   (f.name.size() > fromFull.size() &&
						                    f.name.compare(0, fromFull.size(), fromFull) == 0 &&
						                    f.name[fromFull.size()] == '/');
						if (match) expV.push_back({ to, &T });
					}
				}
				f.trans = anyV;
				f.trans.insert(f.trans.end(), expV.begin(), expV.end());
			}
			lr.entry = ResolveEntry(lr, scopes, "", l.entry, root.firstChild);
			lr.cur = lr.entry;
			layers.push_back(std::move(lr));
		}
	}

	void BindMap(Animator* a, AnimClip* clip, std::vector<int>& map)
	{
		map.assign(clip ? clip->channels.size() : 0, -1);
		if (!clip || !a->bonesRef) return;
		for (size_t c = 0; c < clip->channels.size(); ++c)
		{
			const std::string want = a->MapName(clip->channels[c].bone);
			for (size_t b = 0; b < a->bonesRef->size(); ++b)
				if ((*a->bonesRef)[b].name == want) { map[c] = (int)b; break; }
		}
	}

	void ResolveMotion(Animator* a, FState& st)
	{
		if (st.resolved) return;
		st.resolved = true;
		ResDB* db = ResDB::getSingleton();
		const BoneMap* renames = a->boneMapGuid.empty() ? nullptr : db->GetBoneMap(a->boneMapGuid);
		Skeleton* sk = a->smr ? a->smr->skeleton : nullptr;
		auto adopt = [&](AnimClip* c) { return sk ? RetargetCached(c, sk, renames) : c; };
		BlendSpace* bs = db->GetBlendSpace(st.def->motion);
		if (a->previewBlend && st.def->motion == a->previewBlend->guid) bs = a->previewBlend;
		if (BlendSpace* b = bs)
		{
			st.blend = b;
			st.blendClips.resize(b->points.size(), nullptr);
			st.maps.resize(b->points.size());
			for (size_t i = 0; i < b->points.size(); ++i)
			{
				AnimClip* c = db->GetClip(b->points[i].clip);
				if (!c) c = db->GetClipByName(b->points[i].clip);
				st.blendClips[i] = adopt(c);
				BindMap(a, st.blendClips[i], st.maps[i]);
			}
			return;
		}
		st.clip = adopt(a->ResolveClip(st.def->motion));
		st.maps.resize(1);
		BindMap(a, st.clip, st.maps[0]);
	}

	void BuildMask(Animator* a, LayerRT& lr)
	{
		const size_t nb = a->bonesRef->size();
		lr.mask.assign(nb, 1.0f);
		if (!lr.def || lr.def->mask.empty()) return;
		if (a->smr && a->smr->skeleton)
		{
			std::vector<float> m;
			a->smr->skeleton->GroupMask(lr.def->mask, m);
			if (m.size() == nb) lr.mask = m;
		}
	}

	void EnsureMirror(const std::vector<MeshBone>& bones)
	{
		if (mirrorBuilt && mirrorMap.size() == bones.size()) return;
		mirrorBuilt = true;
		mirrorMap.resize(bones.size());
		for (size_t i = 0; i < bones.size(); ++i)
		{
			mirrorMap[i] = (int)i;
			const std::string m = MirrorName(bones[i].name);
			if (m.empty()) continue;
			for (size_t j = 0; j < bones.size(); ++j)
				if (bones[j].name == m) { mirrorMap[i] = (int)j; break; }
		}
	}

	void MirrorPose(std::vector<BonePose>& pose)
	{
		if (mirrorMap.size() != pose.size()) return;
		std::vector<BonePose> src = pose;
		for (size_t i = 0; i < pose.size(); ++i)
		{
			const BonePose& s = src[mirrorMap[i]];
			pose[i].p = glm::vec3(-s.p.x, s.p.y, s.p.z);
			pose[i].r = glm::quat(s.r.w, s.r.x, -s.r.y, -s.r.z);
			pose[i].s = s.s;
		}
	}

	double StateDuration(FState& st)
	{
		if (st.clip) return st.clip->duration;
		if (st.blend)
		{
			std::vector<float> w;
			st.blend->Weights(floats[st.blend->paramX], floats[st.blend->paramY], w);
			double d = 0;
			for (size_t i = 0; i < w.size() && i < st.blendClips.size(); ++i)
				if (st.blendClips[i] && w[i] > 0)
					d += w[i] * (st.blendClips[i]->duration /
					             std::max(0.01, (double)st.blend->points[i].speed));
			return d;
		}
		return 0;
	}

	double NormTime(LayerRT& lr, FState& st)
	{
		if (st.clip) return st.clip->duration > 1e-6 ? lr.t / st.clip->duration : 0.0;
		return lr.t;
	}

	// Sample a state's pose at `t` (bind preloaded by the caller); registers cursors when asked.
	void SampleState(Animator* a, FState& st, double t, double tPrev, std::vector<BonePose>& pose,
	                 std::vector<AnimCursor>* cursors, float cw, bool primary)
	{
		if (st.clip)
		{
			SampleLayer(nullptr, st.clip, st.maps[0], t, pose);
			if (cursors) cursors->push_back({ st.clip, tPrev, t, cw, st.def->loop, primary });
		}
		else if (st.blend)
		{
			std::vector<float> w;
			st.blend->Weights(floats[st.blend->paramX], floats[st.blend->paramY], w);
			int dominant = -1;
			float wmax = 0;
			for (size_t i = 0; i < w.size(); ++i)
				if (w[i] > wmax) { wmax = w[i]; dominant = (int)i; }
			float acc = 0;
			std::vector<BonePose> part, tmp;
			for (size_t i = 0; i < w.size() && i < st.blendClips.size(); ++i)
			{
				AnimClip* c = st.blendClips[i];
				if (!c || w[i] <= 1e-4f) continue;
				const double ct = t * c->duration;
				const double cp = tPrev * c->duration;
				BindPoseOf(*a->bonesRef, tmp);
				SampleLayer(nullptr, c, st.maps[i], ct, tmp);
				if (st.blend->points[i].mirror) MirrorPose(tmp);
				if (acc <= 0.0f) part = tmp;
				else
				{
					const float aw = w[i] / (acc + w[i]);
					for (size_t b = 0; b < part.size(); ++b)
					{
						part[b].p = glm::mix(part[b].p, tmp[b].p, aw);
						part[b].s = glm::mix(part[b].s, tmp[b].s, aw);
						part[b].r = glm::slerp(part[b].r, tmp[b].r, aw);
					}
				}
				acc += w[i];
				if (cursors && (w[i] > 0.3f || (int)i == dominant))
					cursors->push_back({ c, cp, ct, cw * w[i], st.def->loop, primary && (int)i == dominant });
			}
			if (acc > 0.0f) pose = part;
		}
		if (st.def->mirror) MirrorPose(pose);
	}

	// ---- transitions -------------------------------------------------------------------------
	bool CondPass(const AnimSM::Cond& c)
	{
		switch (c.op)
		{
		case 0: return floats[c.param] > c.value;
		case 1: return floats[c.param] < c.value;
		case 2: return fabsf(floats[c.param] - c.value) < 1e-4f;
		case 3: return fabsf(floats[c.param] - c.value) >= 1e-4f;
		case 4: return bools.count(c.param) && bools[c.param];
		case 5: return !bools.count(c.param) || !bools[c.param];
		case 6: return triggers.count(c.param) != 0;
		}
		return false;
	}

	bool TransitionReady(LayerRT& lr, FState& st, const AnimSM::Transition& T)
	{
		for (const AnimSM::Cond& c : T.conds)
			if (!CondPass(c)) return false;
		if (T.hasExit)
		{
			const double e = T.exitTime;
			const bool crossed = st.def->loop
				? ((lr.cycB >= e && lr.cycA < e) || lr.cycB >= e + 1.0)
				: (lr.cycB >= e);
			if (T.conds.empty()) { if (!crossed) return false; }
			else
			{
				const bool past = st.def->loop ? (crossed || fmod(lr.cycB, 1.0) >= e) : (lr.cycB >= e);
				if (!past) return false;
			}
		}
		else if (T.conds.empty()) return false;
		return true;
	}

	void BeginInert(LayerRT& lr, const std::vector<BonePose>& tgt, double duration)
	{
		const size_t nb = tgt.size();
		lr.inert.resize(nb);
		const float idt = (float)std::max(lr.lastDt, 1e-4);
		for (size_t i = 0; i < nb; ++i)
		{
			IChan& c = lr.inert[i];
			const glm::vec3 dp = lr.lastPose[i].p - tgt[i].p;
			c.pX0 = glm::length(dp);
			c.pDir = c.pX0 > 1e-6f ? dp / c.pX0 : glm::vec3(0);
			c.pV0 = c.pX0 > 1e-6f ? glm::dot((lr.lastPose[i].p - lr.prevPose[i].p) / idt, c.pDir) : 0.0f;
			glm::quat dq = lr.lastPose[i].r * glm::inverse(tgt[i].r);
			if (dq.w < 0) dq = -dq;
			const glm::vec3 dv(dq.x, dq.y, dq.z);
			const float vl = glm::length(dv);
			c.rX0 = 2.0f * atan2f(vl, dq.w);
			c.rAxis = vl > 1e-6f ? dv / vl : glm::vec3(1, 0, 0);
			glm::quat pq = lr.prevPose[i].r * glm::inverse(tgt[i].r);
			if (pq.w * dq.w + pq.x * dq.x + pq.y * dq.y + pq.z * dq.z < 0) pq = -pq;
			const float angPrev = 2.0f * atan2f(glm::dot(glm::vec3(pq.x, pq.y, pq.z), c.rAxis), pq.w);
			c.rV0 = (c.rX0 - angPrev) / idt;
			const glm::vec3 ds = lr.lastPose[i].s - tgt[i].s;
			c.sX0 = glm::length(ds);
			c.sDir = c.sX0 > 1e-6f ? ds / c.sX0 : glm::vec3(0);
			c.sV0 = c.sX0 > 1e-6f ? glm::dot((lr.lastPose[i].s - lr.prevPose[i].s) / idt, c.sDir) : 0.0f;
		}
		lr.inertT = 0;
		lr.inertDur = duration;
		lr.inertActive = true;
	}

	void ApplyInert(LayerRT& lr, std::vector<BonePose>& pose)
	{
		if (lr.inert.size() != pose.size()) return;
		const float T = (float)lr.inertDur, t = (float)lr.inertT;
		for (size_t i = 0; i < pose.size(); ++i)
		{
			const IChan& c = lr.inert[i];
			const float xp = InertEval(c.pX0, c.pV0, T, t);
			if (xp != 0.0f) pose[i].p += c.pDir * xp;
			const float xr = InertEval(c.rX0, c.rV0, T, t);
			if (xr != 0.0f) pose[i].r = glm::normalize(glm::angleAxis(xr, c.rAxis) * pose[i].r);
			const float xs = InertEval(c.sX0, c.sV0, T, t);
			if (xs != 0.0f) pose[i].s += c.sDir * xs;
		}
	}

	void TakeTransition(Animator* a, LayerRT& lr, int to, const AnimSM::Transition* T)
	{
		const double duration = T ? T->duration : 0.25;
		const int mode = T ? T->mode : 0;
		FState& tost = lr.states[to];
		ResolveMotion(a, tost);
		double startT = 0;
		if (!tost.def->sync.empty())
		{
			auto it = syncPhase.find(tost.def->sync);
			if (it != syncPhase.end())
				startT = tost.clip ? it->second * tost.clip->duration : it->second;
		}
		if (mode == 0 && duration > 0 && lr.hist >= 2 && a->bonesRef)
		{
			std::vector<BonePose> tgt;
			BindPoseOf(*a->bonesRef, tgt);
			SampleState(a, tost, startT, startT, tgt, nullptr, 1.0f, false);
			BeginInert(lr, tgt, duration);
			lr.src = -1;
		}
		else if (duration > 0 && lr.cur >= 0)
		{
			lr.src = lr.cur;
			lr.srcT = lr.srcPrevT = lr.t;
			lr.inertActive = false;
		}
		else { lr.src = -1; lr.inertActive = false; }
		lr.transLeft = lr.transDur = duration;
		lr.transMode = mode;
		lr.transInterrupt = T ? T->interrupt : 0;
		lr.cur = to;
		lr.t = lr.prevT = startT;
		lr.cycA = lr.cycB = 0;
		if (T)
			for (const AnimSM::Cond& c : T->conds)
				if (c.op == 6) triggers.erase(c.param);
	}

	// ---- per-frame controller drive ----------------------------------------------------------
	void UpdateController(Animator* a, double dt, std::vector<AnimCursor>& cursors,
	                      std::vector<std::string>& fired)
	{
		(void)fired;
		EnsureController(a);
		if (!sm || layers.empty()) return;
		if (!a->EnsureTargets() || !a->bonesRef || a->bonesRef->empty()) return;
		const size_t nb = a->bonesRef->size();
		EnsureMirror(*a->bonesRef);

		std::vector<BonePose> base;
		bool haveBase = false;
		for (size_t li = 0; li < layers.size(); ++li)
		{
			LayerRT& lr = layers[li];
			if (lr.cur < 0 && lr.entry >= 0) lr.cur = lr.entry;
			if (lr.cur < 0) continue;
			FState* st = &lr.states[lr.cur];
			ResolveMotion(a, *st);
			if (lr.mask.size() != nb) BuildMask(a, lr);

			// advance the state clock (+ the crossfade source's)
			double eff = st->def->speed * a->speed;
			if (!st->def->speedParam.empty()) eff *= floats[st->def->speedParam];
			lr.prevT = lr.t;
			const double dur = StateDuration(*st);
			if (st->clip)
			{
				const double prevNt = dur > 1e-6 ? lr.prevT / dur : 0.0;
				const double adv = dur > 1e-6 ? dt * eff / dur : 0.0;
				lr.cycA = st->def->loop ? fmod(prevNt, 1.0) : prevNt;
				lr.cycB = lr.cycA + adv;
				lr.t = Advance(lr.t, dt * eff, st->clip->duration, st->def->loop);
			}
			else
			{
				const double adv = dur > 1e-6 ? dt * eff / dur : 0.0;
				lr.cycA = st->def->loop ? fmod(lr.prevT, 1.0) : lr.prevT;
				lr.cycB = lr.cycA + adv;
				lr.t += adv;
				if (st->def->loop) { lr.t = fmod(lr.t, 1.0); if (lr.t < 0) lr.t += 1.0; }
				else if (lr.t > 1.0) lr.t = 1.0;
			}
			if (lr.src >= 0)
			{
				FState& ss = lr.states[lr.src];
				double seff = ss.def->speed * a->speed;
				if (!ss.def->speedParam.empty()) seff *= floats[ss.def->speedParam];
				lr.srcPrevT = lr.srcT;
				if (ss.clip) lr.srcT = Advance(lr.srcT, dt * seff, ss.clip->duration, ss.def->loop);
				else
				{
					const double sdur = StateDuration(ss);
					if (sdur > 1e-6) lr.srcT += dt * seff / sdur;
					if (ss.def->loop) { lr.srcT = fmod(lr.srcT, 1.0); if (lr.srcT < 0) lr.srcT += 1.0; }
					else if (lr.srcT > 1.0) lr.srcT = 1.0;
				}
			}
			if (lr.transLeft > 0) { lr.transLeft -= dt; if (lr.transLeft <= 0) lr.src = -1; }
			if (lr.inertActive) { lr.inertT += dt; if (lr.inertT >= lr.inertDur) lr.inertActive = false; }
			if (!st->def->sync.empty()) syncPhase[st->def->sync] = NormTime(lr, *st);

			// transitions (in-flight ones block unless marked interruptible)
			if (lr.transLeft <= 0 || lr.transInterrupt == 1)
				for (const RTrans& tr : st->trans)
				{
					if (tr.to < 0 || tr.to == lr.cur) continue;
					if (!TransitionReady(lr, *st, *tr.def)) continue;
					TakeTransition(a, lr, tr.to, tr.def);
					st = &lr.states[lr.cur];
					ResolveMotion(a, *st);
					break;
				}

			// layer pose
			const bool isBase = !haveBase;
			const float fadeW = (lr.src >= 0 && lr.transDur > 0) ? (float)(lr.transLeft / lr.transDur) : 0.0f;
			std::vector<BonePose> pose;
			BindPoseOf(*a->bonesRef, pose);
			SampleState(a, *st, lr.t, lr.prevT, pose, isBase ? &cursors : nullptr, 1.0f - fadeW, isBase);
			if (fadeW > 0.0f)
			{
				FState& ss = lr.states[lr.src];
				std::vector<BonePose> old;
				BindPoseOf(*a->bonesRef, old);
				SampleState(a, ss, lr.srcT, lr.srcPrevT, old, isBase ? &cursors : nullptr, fadeW, false);
				for (size_t i = 0; i < pose.size(); ++i)
				{
					pose[i].p = glm::mix(pose[i].p, old[i].p, fadeW);
					pose[i].s = glm::mix(pose[i].s, old[i].s, fadeW);
					pose[i].r = glm::slerp(pose[i].r, old[i].r, fadeW);
				}
			}
			if (lr.inertActive) ApplyInert(lr, pose);
			lr.prevPose = lr.lastPose;
			lr.lastPose = pose;
			if (lr.hist < 2) ++lr.hist;
			lr.lastDt = dt > 0 ? dt : lr.lastDt;

			// composite
			if (isBase) { base = pose; haveBase = true; continue; }
			float w = lr.weightOverride >= 0 ? lr.weightOverride : lr.def->weight;
			if (w <= 0.0f) continue;
			if (lr.def->additive)
			{
				std::vector<BonePose> ref;
				BindPoseOf(*a->bonesRef, ref);
				SampleState(a, *st, 0.0, 0.0, ref, nullptr, 1.0f, false);
				for (size_t i = 0; i < base.size(); ++i)
				{
					const float m = w * lr.mask[i];
					if (m <= 0.0f) continue;
					glm::quat d = pose[i].r * glm::inverse(ref[i].r);
					if (d.w < 0) d = -d;
					base[i].r = glm::normalize(glm::slerp(glm::quat(1, 0, 0, 0), d, m) * base[i].r);
					base[i].p += (pose[i].p - ref[i].p) * m;
					base[i].s += (pose[i].s - ref[i].s) * m;
				}
			}
			else
				for (size_t i = 0; i < base.size(); ++i)
				{
					const float m = w * lr.mask[i];
					if (m <= 0.0f) continue;
					base[i].p = glm::mix(base[i].p, pose[i].p, m);
					base[i].s = glm::mix(base[i].s, pose[i].s, m);
					base[i].r = glm::slerp(base[i].r, pose[i].r, m);
				}
		}
		if (!haveBase) return;
		if (a->mirrorOverride) MirrorPose(base);
		ApplyRootMotion(a, cursors, base);
		CommitPose(a, base);
	}

	// Forced jump (SetState in controller mode): inertialized 0.25 s by default.
	void ForceState(Animator* a, const std::string& name)
	{
		EnsureController(a);
		if (layers.empty()) return;
		LayerRT& lr = layers[0];
		auto it = lr.byName.find(name);
		if (it == lr.byName.end())
		{
			for (auto& kv : lr.byName)   // leaf-name fallback ("Walk" -> "Locomotion/Walk")
			{
				const size_t s = kv.first.rfind('/');
				if (kv.first.substr(s == std::string::npos ? 0 : s + 1) == name) { it = lr.byName.find(kv.first); break; }
			}
			if (it == lr.byName.end())
			{
				std::cout << "[Animator]\tunknown controller state '" << name << "'" << std::endl;
				return;
			}
		}
		if (it->second == lr.cur) return;
		TakeTransition(a, lr, it->second, nullptr);
	}

	std::string Layer0StateName()
	{
		if (layers.empty() || layers[0].cur < 0) return std::string();
		return layers[0].states[layers[0].cur].name;
	}

	// ---- root motion -------------------------------------------------------------------------
	int BoneIdxOf(Animator* a, const std::string& boneName)
	{
		const std::string want = a->MapName(boneName);
		for (size_t b = 0; b < a->bonesRef->size(); ++b)
			if ((*a->bonesRef)[b].name == want) return (int)b;
		return -1;
	}

	// The clip's TRAVEL channel: the animated bone closest to the skeleton root (importers often
	// park static scene nodes above the armature, so bone 0 itself may never be animated).
	int RootChannel(Animator* a, AnimClip* clip)
	{
		auto it = rootChan.find(clip);
		if (it != rootChan.end()) return it->second;
		int ch = -1, bestDepth = 1 << 30;
		for (size_t c = 0; c < clip->channels.size(); ++c)
		{
			if (clip->channels[c].pos.empty() && clip->channels[c].rot.empty()) continue;
			const int b = BoneIdxOf(a, clip->channels[c].bone);
			if (b < 0) continue;
			int depth = 0;
			for (int j = b; (*a->bonesRef)[j].parent >= 0; j = (*a->bonesRef)[j].parent) ++depth;
			if (depth < bestDepth) { bestDepth = depth; ch = (int)c; }
		}
		rootChan[clip] = ch;
		return ch;
	}

	void ApplyRootMotion(Animator* a, const std::vector<AnimCursor>& cursors, std::vector<BonePose>& pose)
	{
		if (!a->rootMotion || !a->bonesRef || a->bonesRef->empty() || pose.empty())
		{
			a->lastRootDelta = Vector3(0, 0, 0);
			return;
		}
		// travel bone: the primary cursor's root channel decides where to pin the pose
		int root = -1;
		for (const AnimCursor& cu : cursors)
		{
			if (!cu.clip || !cu.primary) continue;
			const int ch = RootChannel(a, cu.clip);
			if (ch >= 0) root = BoneIdxOf(a, cu.clip->channels[ch].bone);
			break;
		}
		if (root < 0)
		{
			a->lastRootDelta = Vector3(0, 0, 0);
			return;
		}
		const MeshBone& rb = (*a->bonesRef)[root];
		glm::vec3 delta(0);
		float yaw = 0;
		for (const AnimCursor& cu : cursors)
		{
			if (!cu.clip || cu.weight <= 0.0f) continue;
			const int ch = RootChannel(a, cu.clip);
			if (ch < 0) continue;
			const AnimClip::Channel& C = cu.clip->channels[ch];
			if (!C.pos.empty())
			{
				auto P = [&](double t) { float v[4]; SampleKeys(C.pos, t, v); return glm::vec3(v[0], v[1], v[2]); };
				glm::vec3 d;
				if (cu.newT >= cu.prevT - 1e-9) d = P(cu.newT) - P(cu.prevT);
				else d = (P(cu.clip->duration) - P(cu.prevT)) + (P(cu.newT) - P(0.0));
				delta += d * cu.weight;
			}
			if (!C.rot.empty())
			{
				auto Q = [&](double t) { return SampleRot(C.rot, t, glm::quat(1, 0, 0, 0)); };
				glm::quat dq;
				if (cu.newT >= cu.prevT - 1e-9) dq = Q(cu.newT) * glm::inverse(Q(cu.prevT));
				else dq = (Q(cu.newT) * glm::inverse(Q(0.0))) * (Q(cu.clip->duration) * glm::inverse(Q(cu.prevT)));
				if (dq.w < 0) dq = -dq;
				yaw += 2.0f * atan2f(dq.y, dq.w) * cu.weight;
			}
		}
		// bake the travel out of the pose: pin the root horizontally + strip its yaw twist
		pose[root].p.x = rb.localPos[0];
		pose[root].p.z = rb.localPos[2];
		{
			const glm::quat bind(rb.localRot[3], rb.localRot[0], rb.localRot[1], rb.localRot[2]);
			glm::quat rel = pose[root].r * glm::inverse(bind);
			if (rel.w < 0) rel = -rel;
			if (fabsf(rel.y) > 1e-6f)
			{
				const glm::quat twist = glm::normalize(glm::quat(rel.w, 0.0f, rel.y, 0.0f));
				pose[root].r = glm::inverse(twist) * pose[root].r;
			}
		}
		// move the atom by the extracted travel (world space)
		Vector3 gp = a->transform->globalPosition();
		Quaternion gq = a->transform->globalRotation();
		Vector3 gs = a->transform->globalScale();
		const glm::quat gr((float)gq.w, (float)gq.x, (float)gq.y, (float)gq.z);
		const glm::vec3 wd = gr * glm::vec3(delta.x * (float)gs.x, delta.y * (float)gs.y, delta.z * (float)gs.z);
		const glm::quat nr = gr * glm::angleAxis(yaw, glm::vec3(0, 1, 0));
		a->transform->SetGlobal(Vector3(gp.x + wd.x, gp.y + wd.y, gp.z + wd.z),
		                        Quaternion(nr.x, nr.y, nr.z, nr.w), gs);
		a->lastRootDelta = Vector3(wd.x, wd.y, wd.z);
	}

	// A named chain from the skeleton's IK rig, resolved to bone indices (root -> tip).
	bool ResolveRigChain(Animator* a, const std::string& name, std::vector<int>& out)
	{
		Skeleton* sk = a->smr ? a->smr->skeleton : nullptr;
		if (!sk) return false;
		for (const SkeletonChain& c : sk->chains)
		{
			if (c.name != name) continue;
			out.clear();
			for (const std::string& b : c.bones)
			{
				int idx = -1;
				for (size_t i = 0; i < a->bonesRef->size(); ++i)
					if ((*a->bonesRef)[i].name == b) { idx = (int)i; break; }
				if (idx < 0) return false;
				out.push_back(idx);
			}
			return out.size() >= 2;
		}
		return false;
	}

	// ---- shared pose commit: IK post-pass + SMR handover / legacy CPU skin -------------------
	void CommitPose(Animator* a, std::vector<BonePose>& pose)
	{
		const std::vector<MeshBone>& bones = *a->bonesRef;
		const size_t nb = bones.size();
		std::vector<glm::mat4> global(nb), palette(nb);
		auto forwardPass = [&]()
		{
			for (size_t i = 0; i < nb; ++i)
			{
				glm::mat4 local = glm::translate(glm::mat4(1.0f), pose[i].p)
				                * glm::mat4_cast(pose[i].r)
				                * glm::scale(glm::mat4(1.0f), pose[i].s);
				const int par = bones[i].parent;
				global[i] = (par >= 0) ? global[par] * local : local;
			}
		};
		forwardPass();

		// Foot placement sees the CLEAN animation globals and files its chain goals + pelvis
		// offset BEFORE the solve of this same frame (zero-latency, no feedback loop).
		if (a->atom)
			if (FootIK* fik = a->atom->GetComponent<FootIK>())
				if (fik->enabled)
					fik->ApplyTo(a, (const float*)global.data(), (int)nb);

		// Pelvis shift (FootIK / SetPelvisOffset): world units onto the common ancestor of the
		// chain-goal roots (the hips), falling back to the skeleton root.
		if (a->graph && a->graph->pelvisOffset != 0.0 && nb > 0)
		{
			Vector3 gsv = a->transform->globalScale();
			const float dy = (float)(a->graph->pelvisOffset / (gsv.y != 0.0 ? gsv.y : 1.0));
			int pb = -1;
			{
				std::vector<int> roots;
				for (const auto& kv : a->ikGoals)
					if (kv.second.isChain)
					{
						std::vector<int> ch;
						if (ResolveRigChain(a, kv.first, ch)) roots.push_back(ch.front());
					}
				if (roots.size() == 1)
					pb = bones[roots[0]].parent >= 0 ? bones[roots[0]].parent : roots[0];
				else if (roots.size() > 1)
				{
					std::set<int> anc;
					for (int j = roots[0]; j >= 0; j = bones[j].parent) anc.insert(j);
					int j = roots[1];
					while (j >= 0 && !anc.count(j)) j = bones[j].parent;
					pb = j;
				}
				if (pb < 0)
					for (size_t i = 0; i < nb; ++i)
						if (bones[i].parent < 0) { pb = (int)i; break; }
			}
			if (pb >= 0)
			{
				const int par = bones[pb].parent;
				const glm::quat parentG = par >= 0 ? glm::quat_cast(global[par]) : glm::quat(1, 0, 0, 0);
				pose[pb].p += glm::inverse(parentG) * glm::vec3(0, dy, 0);
				forwardPass();
			}
		}

		const bool wantIK   = !a->ikGoals.empty();
		const bool wantLook = a->graph && !a->graph->lookAts.empty();
		glm::mat4 invModel(1.0f);
		if (wantIK || wantLook)
		{
			Vector3 gp = a->transform->globalPosition();
			Quaternion gq = a->transform->globalRotation();
			Vector3 gs = a->transform->globalScale();
			glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3((float)gp.x, (float)gp.y, (float)gp.z))
			                * glm::mat4_cast(glm::quat((float)gq.w, (float)gq.x, (float)gq.y, (float)gq.z))
			                * glm::scale(glm::mat4(1.0f), glm::vec3((float)gs.x, (float)gs.y, (float)gs.z));
			invModel = glm::inverse(model);
		}

		if (wantIK)
		{
			bool touched = false;

			for (const auto& goal : a->ikGoals)
			{
				std::vector<int> chain;
				if (goal.second.isChain)
				{
					// named chain from the skeleton's IK rig (root -> tip)
					if (!ResolveRigChain(a, goal.first, chain)) continue;
				}
				else
				{
					int tip = -1;
					for (size_t b = 0; b < nb; ++b)
						if (bones[b].name == goal.first) { tip = (int)b; break; }
					if (tip < 0) continue;
					std::vector<int> up{ tip };
					int j = tip;
					for (int s = 0; s < goal.second.segments && bones[j].parent >= 0; ++s)
						up.push_back(j = bones[j].parent);
					if (up.size() < 3) continue;
					chain.assign(up.rbegin(), up.rend());
				}
				const size_t S = chain.size() - 1;
				if (S < 1) continue;

				std::vector<glm::vec3> p(chain.size());
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

				if (glm::length(t - root0) >= total)
				{
					const glm::vec3 dir = glm::normalize(t - root0);
					q[0] = root0;
					for (size_t i = 0; i < S; ++i) q[i + 1] = q[i] + dir * len[i];
				}
				else
				{
					// Dead-straight chain aimed along its own axis (straight legs) never bends
					// in FABRIK: nudge the interior joints off the axis so the fold has a
					// direction. A pole (when set) decides instead.
					if (!goal.second.hasPole)
					{
						glm::vec3 axis = t - root0;
						const float al = glm::length(axis);
						if (al > 1e-6f)
						{
							axis /= al;
							bool degen = true;
							for (size_t i = 1; i < S && degen; ++i)
							{
								const glm::vec3 rel = q[i] - root0;
								if (glm::length(rel - axis * glm::dot(rel, axis)) > 1e-4f * total) degen = false;
							}
							if (degen)
							{
								const glm::vec3 perp = fabsf(axis.y) < 0.99f
									? glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)))
									: glm::vec3(0, 0, 1);   // vertical chain: knees fold forward
								for (size_t i = 1; i < S; ++i) q[i] += perp * (0.02f * total);
							}
						}
					}
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
							float r = glm::length(rel - axis * along);
							// ON-AXIS joint (dead-straight leg): the radius is zero, but the
							// pole still dictates the bend PLANE — seed a minimal offset so
							// FABRIK folds toward it instead of oscillating along the axis.
							if (r < 0.02f * total) r = 0.02f * total;
							q[i] = root0 + axis * along + pPerp * r;
						}
					};
					for (int it = 0; it < 12; ++it)
					{
						polePass();
						q[S] = t;
						for (size_t i = S; i-- > 0; ) q[i] = q[i + 1] + glm::normalize(q[i] - q[i + 1]) * len[i];
						q[0] = root0;
						for (size_t i = 0; i < S; ++i) q[i + 1] = q[i] + glm::normalize(q[i + 1] - q[i]) * len[i];
						if (glm::length(q[S] - t) < 1e-4f && !goal.second.hasPole) break;
					}
				}

				const float w = (float)goal.second.weight;
				for (size_t i = 0; i < S; ++i)
				{
					const int bi = chain[i];
					const glm::vec3 cur0 = glm::vec3(global[chain[i]][3]);
					const glm::vec3 cur1 = glm::vec3(global[chain[i + 1]][3]);
					const glm::vec3 d0 = cur1 - cur0, d1 = q[i + 1] - q[i];
					if (glm::dot(d0, d0) < 1e-10f || glm::dot(d1, d1) < 1e-10f) continue;
					const glm::quat delta = glm::rotation(glm::normalize(d0), glm::normalize(d1));
					const int par = bones[bi].parent;
					const glm::quat parentG = par >= 0 ? glm::quat_cast(global[par]) : glm::quat(1, 0, 0, 0);
					const glm::quat newLocal = glm::inverse(parentG) * (delta * glm::quat_cast(global[bi]));
					pose[bi].r = glm::slerp(pose[bi].r, glm::normalize(newLocal), w);
					forwardPass();
				}

				// foot roll: re-aim the tip's up-axis onto the surface normal after the reach
				if (goal.second.hasNormal && goal.second.normalWeight > 0.0)
				{
					const int tip = chain.back();
					const glm::vec3 nW(goal.second.normal[0], goal.second.normal[1], goal.second.normal[2]);
					glm::vec3 nM = glm::vec3(invModel * glm::vec4(nW, 0.0f));
					const float nl = glm::length(nM);
					if (nl > 1e-6f)
					{
						nM /= nl;
						const glm::vec3 up = glm::normalize(glm::vec3(global[tip][1]));
						if (glm::dot(up, nM) < 0.9999f)
						{
							const glm::quat delta = glm::rotation(up, nM);
							const glm::quat qn = glm::slerp(glm::quat(1, 0, 0, 0), delta, (float)goal.second.normalWeight);
							const int par = bones[tip].parent;
							const glm::quat parentG = par >= 0 ? glm::quat_cast(global[par]) : glm::quat(1, 0, 0, 0);
							pose[tip].r = glm::normalize(glm::inverse(parentG) * (qn * glm::quat_cast(global[tip])));
							forwardPass();
						}
					}
				}
				touched = true;
			}
			if (touched) forwardPass();
		}

		// Look-at: spread the turn along the weighted chain (weights grow toward the tip),
		// capped at maxAngle. Runs AFTER the reach IK so aims win over reaches on shared bones.
		if (wantLook)
		{
			for (const auto& kv : a->graph->lookAts)
			{
				std::vector<int> chain;
				if (!ResolveRigChain(a, kv.first, chain))
				{
					int b = -1;
					for (size_t i = 0; i < nb; ++i)
						if (bones[i].name == kv.first) { b = (int)i; break; }
					if (b < 0) continue;
					chain.assign(1, b);
				}
				const int head = chain.back();
				const glm::vec3 hp = glm::vec3(global[head][3]);
				const glm::vec3 tM = glm::vec3(invModel * glm::vec4(kv.second.target[0], kv.second.target[1], kv.second.target[2], 1.0f));
				glm::vec3 dir = tM - hp;
				if (glm::dot(dir, dir) < 1e-8f) continue;
				dir = glm::normalize(dir);
				const glm::vec3 fwd = glm::normalize(glm::vec3(global[head][2]));
				const float ang = acosf(glm::clamp(glm::dot(fwd, dir), -1.0f, 1.0f));
				if (ang < 1e-4f) continue;
				const float maxA = (float)(kv.second.maxAngle * 0.01745329252);
				const float scaled = std::min(ang, maxA) / ang * (float)kv.second.weight;
				const glm::quat full = glm::rotation(fwd, dir);
				float wsum = 0;
				for (size_t i = 0; i < chain.size(); ++i) wsum += (float)(i + 1);
				for (size_t i = 0; i < chain.size(); ++i)
				{
					const float wi = (float)(i + 1) / wsum * scaled;
					const glm::quat qi = glm::slerp(glm::quat(1, 0, 0, 0), full, wi);
					const int b = chain[i];
					const int par = bones[b].parent;
					const glm::quat parentG = par >= 0 ? glm::quat_cast(global[par]) : glm::quat(1, 0, 0, 0);
					pose[b].r = glm::normalize(glm::inverse(parentG) * (qi * glm::quat_cast(global[b])));
					forwardPass();
				}
			}
		}

		// committed-pose history: inertialized jumps (MatchTo) capture their offsets from it
		commitPrev.swap(commitLast);
		commitLast = pose;
		if (commitHist < 2) ++commitHist;
		{
			const double d = Time::getSingleton()->delta;
			if (d > 0.0) commitDt = d;
		}

		// hand the finished pose over: every subtree SMR shares the ONE skeleton pose
		// (GPU/CPU skinning happens inside each renderer), or fall to the legacy CPU skin
		if (!a->smrs.empty())
		{
			Ragdoll* rd = a->atom ? a->atom->GetComponent<Ragdoll>() : nullptr;
			if (rd && (!rd->enabled || !rd->Active())) rd = nullptr;
			for (SkinnedMeshRenderer* s : a->smrs)
			{
				s->pose.resize(nb);
				for (size_t i = 0; i < nb; ++i)
				{
					s->pose[i].pos[0] = pose[i].p.x; s->pose[i].pos[1] = pose[i].p.y; s->pose[i].pos[2] = pose[i].p.z;
					s->pose[i].rot[0] = pose[i].r.x; s->pose[i].rot[1] = pose[i].r.y;
					s->pose[i].rot[2] = pose[i].r.z; s->pose[i].rot[3] = pose[i].r.w;
					s->pose[i].scale[0] = pose[i].s.x; s->pose[i].scale[1] = pose[i].s.y; s->pose[i].scale[2] = pose[i].s.z;
				}
				if (rd) rd->BlendInto(s, false);   // physics pose over the animated one, one skin
				s->ApplyPose();
			}
			return;
		}
		if (!a->srcMesh || !a->skinnedMesh) return;
		for (size_t i = 0; i < nb; ++i)
			palette[i] = global[i] * glm::make_mat4(bones[i].invBind);

		const int n = a->srcMesh->numVerts;
		const float* sv = a->srcMesh->vertexArray;
		const float* sn = a->srcMesh->normalArray;
		const unsigned short* bi = a->srcMesh->boneIndex;
		const float* bw = a->srcMesh->boneWeight;
		float* dv = a->skinnedMesh->vertexArray;
		float* dn = a->skinnedMesh->normalArray;
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

		++a->skinnedMesh->version;
		a->skinnedMesh->boundsValid = false;
		a->mr->mesh = a->skinnedMesh;
	}

	// ---- clip extras: legacy events + typed notifies + curves + prop tracks ------------------
	bool SocketPose(Animator* a, const std::string& socket, Vector3& p, Quaternion& r)
	{
		if (socket.empty() || !a->smr) return false;
		if (a->smr->SocketWorld(socket, p, r)) return true;
		return a->smr->BoneWorld(socket, p, r);
	}

	// A clip outlives the atoms it plays on: the same animation runs on characters that do not
	// carry the component (or the script class) a track addresses. Every miss below is a silent
	// no-op reported ONCE, never a crash and never per-frame spam.
	void MissedOnce(const std::string& key, const std::string& what)
	{
		static std::mutex m;
		static std::set<std::string> seen;
		{
			std::lock_guard<std::mutex> lk(m);
			if (!seen.insert(key).second) return;
		}
		std::cout << "[Animator]	clip track skipped: " << what << std::endl;
	}

	void DispatchNotify(Animator* a, const AnimClip::Notify& n, std::vector<std::string>& fired)
	{
		if (!a || !a->atom || !a->transform) return;   // atom torn down mid-frame
		if (a->muteNotifies) return;   // editor preview rig: no side effects on the live world
		if (!n.name.empty()) fired.push_back(n.name);
		if ((n.type == 1 || n.type == 2) && n.asset.empty())
		{
			MissedOnce("notify:" + n.name, "notify '" + n.name + "' has no asset");
			return;
		}
		switch (n.type)
		{
		case 1:   // SpawnPrefab at socket
		{
			Atom* sp = Prefabs::Spawn(n.asset);
			if (!sp) { MissedOnce("spawn:" + n.asset, "prefab '" + n.asset + "' not found"); break; }
			Vector3 p;
			Quaternion r(0, 0, 0, 1);
			if (!SocketPose(a, n.socket, p, r))
			{
				p = a->transform->globalPosition();
				r = a->transform->globalRotation();
			}
			sp->GetTransform().SetGlobal(p, r, Vector3(1, 1, 1));
			if (n.a > 0)
				a->notifySpawns.push_back({ (long)sp->id.id, Time::getSingleton()->elapsed + n.a });
			break;
		}
		case 2:   // Sound (3D at socket, 2D without one)
		{
			const double vol = n.a > 0 ? n.a : 1.0;
			if (n.socket.empty()) Audio::Play(n.asset, vol, false, 1);
			else
			{
				Vector3 p;
				Quaternion r;
				if (!SocketPose(a, n.socket, p, r)) p = a->transform->globalPosition();
				Audio::PlayAt(n.asset, p, vol, 1.0, 50.0, 1);
			}
			break;
		}
		case 3:   // main game camera shake
		{
			World* w = AppInstance::GetSingleton() ? AppInstance::GetSingleton()->currentWorld : nullptr;
			if (Camera* cam = w ? w->GetMainCamera() : nullptr) cam->AddShake(n.a, n.b, n.c);
			break;
		}
		}
	}

	void ApplyProp(Animator* a, const AnimClip::PropTrack& tr, const float v[4])
	{
		Atom* t = a->atom;
		if (!t) return;
		std::string p = tr.path;
		while (t && !p.empty())
		{
			const size_t s = p.find('/');
			const std::string seg = (s == std::string::npos) ? p : p.substr(0, s);
			p = (s == std::string::npos) ? std::string() : p.substr(s + 1);
			Atom* next = nullptr;
			for (Atom* ch : t->children)
				if (ch && ch->name == seg) { next = ch; break; }
			t = next;
		}
		if (!t) return;
		// "Host:selector" addresses a SCRIPT class (Reflect_ScriptClasses): the component is the
		// host (ScriptComponent / CSharpScript) and the prop is one of its dynamic props.
		std::string compType = tr.comp;
		std::string scriptSel;
		if (const size_t sp = tr.comp.find(':'); sp != std::string::npos)
		{
			compType = tr.comp.substr(0, sp);
			scriptSel = tr.comp.substr(sp + 1);
		}
		Component* comp = nullptr;
		for (Component* c : t->components)
		{
			if (!c) continue;
			TypeInfo* ti = c->GetType();
			while (ti)
			{
				if (ti->name == compType) { comp = c; break; }
				ti = ti->base.empty() ? nullptr : Registry_Find(ti->base);
			}
			if (comp) break;
		}
		if (!comp)
		{
			MissedOnce(tr.comp + "|" + tr.prop, "'" + tr.comp + "." + tr.prop
			           + "' — this atom has no such component");
			return;
		}
		if (!scriptSel.empty())   // script prop: values cross as scalars
		{
			// The host component may be running a DIFFERENT class than the track was authored
			// against — writing then would poke a prop that class never declared. Only props the
			// live instance actually exposes are written.
			bool has = false;
			for (const DynProp& d : comp->DynamicProps())
				if (d.name == tr.prop) { has = true; break; }
			if (!has)
			{
				MissedOnce(tr.comp + "|" + tr.prop, "'" + scriptSel + "." + tr.prop
				           + "' — the live script exposes no such prop");
				return;
			}
			NukeVar nv;
			nv.kind = NukeVar::Kind::Number;
			nv.num = v[0];
			comp->SetDynamicProp(tr.prop, nv);
			return;
		}
		TypeInfo* ti = Registry_Find(compType);
		if (!ti)
		{
			MissedOnce(tr.comp, "component type '" + compType + "' is not registered (module not loaded?)");
			return;
		}
		bool found = false;
		for (const Field& f : ti->fields)
		{
			if (f.name != tr.prop) continue;
			found = true;
			void* ad = f.addr(comp);
			if (!ad) return;
			switch (f.type)
			{
			case FT::Float:  *(float*)ad = v[0]; break;
			case FT::Double: *(double*)ad = v[0]; break;
			case FT::Int:    *(int*)ad = (int)lroundf(v[0]); break;
			case FT::Bool:   *(bool*)ad = v[0] >= 0.5f; break;
			case FT::Vec2:   { Vector2* o = (Vector2*)ad; o->x = v[0]; o->y = v[1]; break; }
			case FT::Vec3:   { Vector3* o = (Vector3*)ad; o->x = v[0]; o->y = v[1]; o->z = v[2]; break; }
			case FT::Vec4:   { Vector4* o = (Vector4*)ad; o->x = v[0]; o->y = v[1]; o->z = v[2]; o->w = v[3]; break; }
			case FT::Quat:   { Quaternion* o = (Quaternion*)ad; o->x = v[0]; o->y = v[1]; o->z = v[2]; o->w = v[3]; break; }
			case FT::Color:  { Color* o = (Color*)ad; o->r = v[0]; o->g = v[1]; o->b = v[2]; o->a = v[3]; break; }
			default: return;
			}
			Reflect_ComponentFieldChanged(comp, f);
			return;
		}
		if (!found)
			MissedOnce(tr.comp + "|" + tr.prop, "'" + compType + "." + tr.prop
			           + "' — no such prop on this component");
	}

	void EvalExtras(Animator* a, const std::vector<AnimCursor>& cursors,
	                std::vector<std::string>& fired, double dt)
	{
		curveVals.clear();
		struct PAcc { const AnimClip::PropTrack* tr = nullptr; float v[4] = { 0, 0, 0, 0 }; float w = 0; };
		std::map<std::string, PAcc> props;
		for (const AnimCursor& cu : cursors)
		{
			if (!cu.clip) continue;
			auto crossed = [&](float et)
			{
				if (cu.newT >= cu.prevT) return et > cu.prevT && et <= cu.newT;
				return (et > cu.prevT && et <= cu.clip->duration) || (et >= -1e-6 && et <= cu.newT);
			};
			if (dt > 0 && cu.primary)
				for (const AnimClip::Event& e : cu.clip->events)
					if (crossed(e.t)) fired.push_back(e.name);
			if (dt > 0 && (cu.primary || cu.weight > 0.45f))
				for (const AnimClip::Notify& n : cu.clip->notifies)
					if (crossed(n.t)) DispatchNotify(a, n, fired);
			for (const AnimClip::Curve& c : cu.clip->curves)
				if (!c.keys.empty())
				{
					float v[4];
					SampleKeys(c.keys, cu.newT, v);
					curveVals[c.name] += v[0] * cu.weight;
				}
			for (const AnimClip::PropTrack& tr : cu.clip->propTracks)
			{
				if (tr.keys.empty()) continue;
				float v[4] = { 0, 0, 0, 0 };
				if (tr.dim == 0)
				{
					const AnimClip::Key* last = &tr.keys.front();
					for (const AnimClip::Key& k : tr.keys)
						if (k.t <= cu.newT) last = &k;
					v[0] = last->v[0];
				}
				else SampleKeys(tr.keys, cu.newT, v);
				PAcc& acc = props[tr.path + "|" + tr.comp + "|" + tr.prop];
				acc.tr = &tr;
				for (int k = 0; k < 4; ++k) acc.v[k] += v[k] * cu.weight;
				acc.w += cu.weight;
			}
		}
		for (auto& kv : props)
		{
			PAcc& pa = kv.second;
			if (!pa.tr || pa.w <= 1e-4f) continue;
			float v[4];
			for (int k = 0; k < 4; ++k) v[k] = pa.v[k] / pa.w;
			ApplyProp(a, *pa.tr, v);
		}
	}
};

void Animator::Destroy()
{
	ReleaseSkinned();
	delete graph;
	graph = nullptr;
	notifySpawns.clear();
}

void Animator::Reset()
{
	ReleaseSkinned();
	mr = nullptr; srcMesh = nullptr;
	smr = nullptr; smrs.clear(); bonesRef = nullptr;
	cur = Layer(); prev = Layer();
	fadeLeft = fadeDur = 0.0;
	playing = started = false;
	curState.clear();
	// smJson stays (it's the serialized source of truth); re-decode on next use.
	smLoaded = false;
	states.clear(); transitions.clear(); entryState.clear();
	boneMap.clear(); ikGoals.clear();
	atomBindClip = nullptr; channelAtoms.clear(); prevChanByBone.clear();
	delete graph;                  // controller runtime restarts clean (params re-seed from the asset)
	graph = nullptr;
	notifySpawns.clear();
	lastRootDelta = Vector3(0, 0, 0);
	mirrorOverride = false;
}

void Animator::FixedUpdate() {}

// Editor preview seam: point the controller at a live EDITING copy (SequencePlayer::SetSequence
// pattern). Drops the runtime graph so the next Update rebinds against the new structure —
// stale FState/LayerRT pointers into a rebuilt states vector must never survive an edit.
void Animator::SetPreviewController(AnimSM* sm, BlendSpace* blend)
{
	previewSm = sm;
	previewBlend = blend;
	delete graph;
	graph = nullptr;
}

// Write the current clip's sampled TRS onto matched atoms; cross-fade blends the outgoing clip.
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
	AnimGraph::Ensure(this);

	// Expired SpawnPrefab notify atoms (runs even while stopped).
	if (!notifySpawns.empty())
	{
		const double now = Time::getSingleton()->elapsed;
		World* w = AppInstance::GetSingleton() ? AppInstance::GetSingleton()->currentWorld : nullptr;
		for (size_t i = 0; i < notifySpawns.size(); )
		{
			if (now >= notifySpawns[i].second)
			{
				if (w) w->QueueDestroy(notifySpawns[i].first);
				notifySpawns.erase(notifySpawns.begin() + i);
			}
			else ++i;
		}
	}

	// Auto-start once: the controller asset wins, then the machine's ENTRY state, then the clip.
	if (!started)
	{
		started = true;
		EnsureSM();
		if (playOnStart)
		{
			if (!smGuid.empty()) playing = true;
			else if (!entryState.empty() && states.count(entryState)) SetState(entryState);
			else if (!clipGuid.empty())
				StartClip(ResolveClip(clipGuid), loop, speed, 0.0);
		}
	}

	const double dt = Time::getSingleton()->delta;
	std::vector<AnimCursor> cursors;
	std::vector<std::string> fired;

	if (!smGuid.empty())
	{
		// CONTROLLER mode: the graph advances states, blends layers and commits the pose.
		if (playing) graph->UpdateController(this, dt, cursors, fired);
	}
	else if (playing && cur.clip)
	{
		// LEGACY mode: single clip + cross-fade; channels may drive atom transforms directly.
		const bool haveSkin = EnsureTargets();   // false = transform-only animation
		if (haveSkin)
		{
			// A clip started BEFORE the mesh resolved (auto-start order) has no bone map yet.
			if (cur.boneMap.size() != cur.clip->channels.size()) BindLayer(cur);
			if (prev.clip && prev.boneMap.size() != prev.clip->channels.size()) BindLayer(prev);
		}

		const double prevT = cur.t;
		cur.t = Advance(cur.t, dt * cur.speed, cur.clip->duration, cur.loop);   // non-loop clamps
		double prevPrevT = prev.t;
		if (fadeLeft > 0.0 && prev.clip)
		{
			prev.t = Advance(prev.t, dt * prev.speed, prev.clip->duration, prev.loop);
			fadeLeft -= dt;
		}
		const float fw = (fadeLeft > 0.0 && fadeDur > 0.0 && prev.clip) ? (float)(fadeLeft / fadeDur) : 0.0f;
		cursors.push_back({ cur.clip, prevT, cur.t, 1.0f - fw, cur.loop, true });
		if (fw > 0.0f) cursors.push_back({ prev.clip, prevPrevT, prev.t, fw, prev.loop, false });

		ApplyAtomAnimation();

		if (haveSkin)
		{
			// bind -> current clip; cross-fade blends the OUTGOING clip on top
			std::vector<BonePose> pose;
			BindPoseOf(*bonesRef, pose);
			SampleLayer(this, cur.clip, cur.boneMap, cur.t, pose);
			if (fw > 0.0f)
			{
				std::vector<BonePose> old;
				BindPoseOf(*bonesRef, old);
				SampleLayer(this, prev.clip, prev.boneMap, prev.t, old);
				for (size_t i = 0; i < pose.size(); ++i)
				{
					pose[i].p = glm::mix(pose[i].p, old[i].p, fw);
					pose[i].s = glm::mix(pose[i].s, old[i].s, fw);
					pose[i].r = glm::slerp(pose[i].r, old[i].r, fw);
				}
			}
			if (mirrorOverride)
			{
				graph->EnsureMirror(*bonesRef);
				graph->MirrorPose(pose);
			}
			// motion-matching jump: decay the captured offset on top of the new stream
			if (graph->legInertActive)
			{
				graph->legInertT += dt;
				if (graph->legInertT >= graph->legInertDur) graph->legInertActive = false;
				else AnimGraph::AddInert(graph->legInert, graph->legInertDur, graph->legInertT, pose);
			}
			graph->ApplyRootMotion(this, cursors, pose);
			graph->CommitPose(this, pose);
		}
	}

	// v3 clip extras: legacy events + typed notifies + float curves + prop tracks.
	graph->EvalExtras(this, cursors, fired, dt);

	// Fired LAST, after the pose is committed. Game thread, game lock held.
	for (const std::string& name : fired)
		if (atom)
			for (Component* c : atom->components)
				if (c && c->enabled) c->OnAnimEvent(name.c_str());
}

// --- controller parameters + runtime v2 script surface ------------------------------------
void Animator::SetFloat(const std::string& param, double v)
{
	AnimGraph::Ensure(this);
	graph->floats[param] = (float)v;
}
double Animator::GetFloat(const std::string& param)
{
	return graph && graph->floats.count(param) ? graph->floats[param] : 0.0;
}
void Animator::SetBool(const std::string& param, bool v)
{
	AnimGraph::Ensure(this);
	graph->bools[param] = v;
}
bool Animator::GetBool(const std::string& param)
{
	return graph && graph->bools.count(param) && graph->bools[param];
}
void Animator::SetTrigger(const std::string& param)
{
	AnimGraph::Ensure(this);
	graph->triggers.insert(param);
}
void Animator::ResetTrigger(const std::string& param)
{
	if (graph) graph->triggers.erase(param);
}
double Animator::CurveValue(const std::string& curve)
{
	return graph && graph->curveVals.count(curve) ? graph->curveVals[curve] : 0.0;
}
std::string Animator::LayerState(double layer)
{
	const int li = (int)layer;
	if (!graph || li < 0 || li >= (int)graph->layers.size()) return std::string();
	AnimGraph::LayerRT& lr = graph->layers[li];
	return lr.cur >= 0 ? lr.states[lr.cur].name : std::string();
}
void Animator::SetLayerWeight(double layer, double weight)
{
	AnimGraph::Ensure(this);
	const int li = (int)layer;
	if (li >= 0 && li < (int)graph->layers.size())
		graph->layers[li].weightOverride = (float)std::max(0.0, std::min(1.0, weight));
}
Vector3 Animator::RootDelta() { return lastRootDelta; }
void Animator::SetMirror(bool mirrored) { mirrorOverride = mirrored; }

void Animator::SetLookAt(const std::string& chain, const Vector3& target, double weight, double maxAngle)
{
	AnimGraph::Ensure(this);
	AnimGraph::LookGoal g;
	g.target[0] = (float)target.x; g.target[1] = (float)target.y; g.target[2] = (float)target.z;
	g.weight = weight < 0.0 ? 0.0 : (weight > 1.0 ? 1.0 : weight);
	g.maxAngle = maxAngle;
	graph->lookAts[chain] = g;
}
void Animator::ClearLookAt() { if (graph) graph->lookAts.clear(); }
void Animator::SetPelvisOffset(double dy)
{
	AnimGraph::Ensure(this);
	graph->pelvisOffset = dy;
}

void Animator::MatchTo(const std::string& clipRef, double time, double blend)
{
	AnimGraph::Ensure(this);
	AnimClip* c = ResolveClip(clipRef);
	if (!c) return;
	EnsureTargets();
	if (smr && smr->skeleton)
		c = RetargetCached(c, smr->skeleton,
		                   boneMapGuid.empty() ? nullptr : ResDB::getSingleton()->GetBoneMap(boneMapGuid));
	if (bonesRef && graph->commitHist >= 2 && blend > 0.0)
	{
		std::vector<BonePose> tgt;
		BindPoseOf(*bonesRef, tgt);
		std::vector<int> map;
		graph->BindMap(this, c, map);
		SampleLayer(nullptr, c, map, time, tgt);
		AnimGraph::CaptureInert(graph->legInert, graph->commitLast, graph->commitPrev, tgt, graph->commitDt);
		graph->legInertT = 0;
		graph->legInertDur = blend;
		graph->legInertActive = true;
	}
	StartClip(c, true, speed, 0.0);
	cur.t = time;
}

bool Animator::SamplePoseGlobals(AnimClip* clip, double time, std::vector<float>& outGlobals)
{
	if (!clip || !EnsureTargets() || !bonesRef) return false;
	AnimGraph::Ensure(this);
	std::vector<BonePose> pose;
	BindPoseOf(*bonesRef, pose);
	std::vector<int> map;
	graph->BindMap(this, clip, map);
	SampleLayer(nullptr, clip, map, time, pose);
	const size_t nb = bonesRef->size();
	std::vector<glm::mat4> g(nb);
	for (size_t i = 0; i < nb; ++i)
	{
		glm::mat4 local = glm::translate(glm::mat4(1.0f), pose[i].p)
		                * glm::mat4_cast(pose[i].r)
		                * glm::scale(glm::mat4(1.0f), pose[i].s);
		const int par = (*bonesRef)[i].parent;
		g[i] = (par >= 0) ? g[par] * local : local;
	}
	outGlobals.resize(nb * 16);
	memcpy(outGlobals.data(), g.data(), nb * 16 * sizeof(float));
	return true;
}

void Animator::ForceGraphState(const std::string& name)
{
	AnimGraph::Ensure(this);
	graph->ForceState(this, name);
}
std::string Animator::GraphStateName()
{
	return graph ? graph->Layer0StateName() : std::string();
}

}  // namespace nuke
