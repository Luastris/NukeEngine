// Paired animations: .nupair asset cache + the session list (see PairedAnim.h).
#include "API/Model/PairedAnim.h"
#include "API/Model/Animator.h"
#include "API/Model/AnimClip.h"
#include "API/Model/Atom.h"
#include "API/Model/Events.h"
#include "API/Model/World.h"
#include "API/Model/resdb.h"
#include "interface/AppInstance.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <map>
#include <vector>

namespace nuke {

namespace {

struct PairRole
{
	std::string clip;
	bool  align = false;
	float pos[3] = { 0, 0, 0 };
	float yaw = 0.0f;              // degrees around Y, applied on top of the anchor rotation
};

struct PairAsset
{
	bool ok = false;
	bool loop = false;
	float speed = 1.0f;
	PairRole role[2];
};

std::map<std::string, PairAsset> g_pairs;

const PairAsset& LoadPair(const std::string& rel)
{
	auto it = g_pairs.find(rel);
	if (it != g_pairs.end()) return it->second;
	PairAsset a;
	std::string text;
	if (!AppInstance::GetSingleton()->ReadContent(rel, text))
		std::cout << "[PairedAnim]\tasset not found: " << rel << std::endl;
	else
	{
		nlohmann::json j = nlohmann::json::parse(text, nullptr, false, true);
		if (j.is_discarded() || !j.is_object() || !j.contains("roles") || !j["roles"].is_array())
			std::cout << "[PairedAnim]\tparse error: " << rel << std::endl;
		else
		{
			a.loop = j.value("loop", false);
			a.speed = j.value("speed", 1.0f);
			for (int i = 0; i < 2 && i < (int)j["roles"].size(); ++i)
			{
				const nlohmann::json& r = j["roles"][i];
				a.role[i].clip = r.value("clip", std::string());
				a.role[i].align = r.value("align", false);
				a.role[i].yaw = r.value("yaw", 0.0f);
				if (r.contains("pos") && r["pos"].is_array() && r["pos"].size() >= 3)
					for (int k = 0; k < 3; ++k) a.role[i].pos[k] = r["pos"][k].get<float>();
			}
			a.ok = !a.role[0].clip.empty() || !a.role[1].clip.empty();
		}
	}
	return g_pairs.emplace(rel, a).first->second;
}

struct Session
{
	std::string pair;
	long aId = 0, bId = 0;
	std::string prevA, prevB;      // clips to restore (captured at start)
	bool endEmitted = false;
};

std::vector<Session> g_sessions;

AnimClip* ResolveRef(const std::string& ref)
{
	if (ref.empty()) return nullptr;
	ResDB* db = ResDB::getSingleton();
	if (AnimClip* c = db->GetClip(ref)) return c;
	return db->GetClipByName(ref);
}

void EmitPair(const Session& s, bool begin)
{
	nlohmann::json j;
	j["a"] = (double)s.aId;
	j["b"] = (double)s.bId;
	j["pair"] = s.pair;
	j["begin"] = begin ? 1 : 0;
	Events::Emit("anim.pair", j.dump());
}

// The animator still plays OUR role clip (someone else's Play() mid-session must win).
bool OwnsAnimator(Animator* an, const std::string& roleClip)
{
	if (!an || roleClip.empty()) return false;
	AnimClip* c = ResolveRef(roleClip);
	return c && an->CurrentClip() == c->name;
}

// Restore a participant's previous clip (crossfade back; nothing was playing = stop).
void RestoreRole(Atom* atom, const std::string& roleClip, const std::string& prevClip)
{
	Animator* an = atom ? atom->GetComponent<Animator>() : nullptr;
	if (!OwnsAnimator(an, roleClip)) return;
	if (!prevClip.empty()) an->CrossFade(prevClip, 0.2);
	else an->Stop();
}

void EndSession(Session& s, World* w)
{
	if (s.endEmitted) return;
	s.endEmitted = true;
	if (w)
	{
		const PairAsset& asset = LoadPair(s.pair);
		RestoreRole(w->GetById(s.aId), asset.role[0].clip, s.prevA);
		if (s.bId) RestoreRole(w->GetById(s.bId), asset.role[1].clip, s.prevB);
	}
	EmitPair(s, false);
}

}  // namespace

bool PairedAnim::StartAt(const std::string& pair, Atom* a, Atom* b, Atom* anchor)
{
	if (!a || pair.empty()) return false;
	const PairAsset& asset = LoadPair(pair);
	if (!asset.ok) return false;
	Stop(a);
	if (b) Stop(b);

	Session s;
	s.pair = pair;
	s.aId = a->id.id;
	s.bId = b ? b->id.id : 0;

	Vector3 anchorPos(0, 0, 0);
	Quaternion anchorRot = Quaternion::Identity();
	Atom* anc = anchor ? anchor : b;
	if (anc)
	{
		anchorPos = anc->GetTransform().globalPosition();
		anchorRot = anc->GetTransform().globalRotation();
	}
	Atom* atoms[2] = { a, b };
	for (int i = 0; i < 2; ++i)
	{
		Atom* at = atoms[i];
		if (!at) continue;
		const PairRole& r = asset.role[i];
		if (r.align && anc)
		{
			Vector3 off((double)r.pos[0], (double)r.pos[1], (double)r.pos[2]);
			Vector3 wp = anchorPos + anchorRot.Rotate(off);
			Quaternion wr = anchorRot * Quaternion::FromAxisAngle(Vector3(0, 1, 0), (double)r.yaw);
			Transform& t = at->GetTransform();
			t.SetGlobal(wp, wr, t.globalScale());
		}
		if (Animator* an = at->GetComponent<Animator>())
		{
			if (!r.clip.empty())
			{
				(i == 0 ? s.prevA : s.prevB) = an->CurrentClip();
				an->PlayClip(r.clip, asset.loop, (double)asset.speed, 0.2);
			}
		}
	}
	g_sessions.push_back(s);
	EmitPair(s, true);
	return true;
}

bool PairedAnim::Start(const std::string& pair, Atom* a, Atom* b)
{
	return StartAt(pair, a, b, b);
}

void PairedAnim::Stop(Atom* any)
{
	if (!any) return;
	World* w = AppInstance::GetSingleton()->currentWorld;
	const long id = any->id.id;
	for (size_t i = g_sessions.size(); i-- > 0;)
		if (g_sessions[i].aId == id || g_sessions[i].bId == id)
		{
			EndSession(g_sessions[i], w);
			g_sessions.erase(g_sessions.begin() + i);
		}
}

bool PairedAnim::Active(Atom* any)
{
	if (!any) return false;
	const long id = any->id.id;
	for (const Session& s : g_sessions)
		if (s.aId == id || s.bId == id) return true;
	return false;
}

double PairedAnim::PairTime(Atom* any)
{
	if (!any) return -1.0;
	World* w = AppInstance::GetSingleton()->currentWorld;
	const long id = any->id.id;
	for (const Session& s : g_sessions)
		if (s.aId == id || s.bId == id)
		{
			Atom* a = w ? w->GetById(s.aId) : nullptr;
			Animator* an = a ? a->GetComponent<Animator>() : nullptr;
			return an ? an->ClipTime() : -1.0;
		}
	return -1.0;
}

// Lockstep + lifetime: the master (role A) owns the clock, B is snapped back on drift; a
// non-loop pair ends when the master clip reaches its duration; dead atoms end the session.
void PairedAnim::Tick(World* w)
{
	if (g_sessions.empty() || !w) return;
	for (size_t i = g_sessions.size(); i-- > 0;)
	{
		Session& s = g_sessions[i];
		const PairAsset& asset = LoadPair(s.pair);
		Atom* a = w->GetById(s.aId);
		Atom* b = s.bId ? w->GetById(s.bId) : nullptr;
		if (!a || (s.bId && !b))
		{
			EndSession(s, w);
			g_sessions.erase(g_sessions.begin() + i);
			continue;
		}
		Animator* anA = a->GetComponent<Animator>();
		Animator* anB = b ? b->GetComponent<Animator>() : nullptr;
		const bool ownA = OwnsAnimator(anA, asset.role[0].clip);
		if (ownA && OwnsAnimator(anB, asset.role[1].clip))
		{
			const double tA = anA->ClipTime();
			if (std::fabs(anB->ClipTime() - tA) > 0.034) anB->SetClipTime(tA);
		}
		if (anA && !asset.role[0].clip.empty())
		{
			AnimClip* c = ResolveRef(asset.role[0].clip);
			const bool done = !asset.loop && ownA && anA->IsPlaying()
			               && c && c->duration > 0.0 && anA->ClipTime() >= c->duration - 1e-3;
			if (!ownA || !anA->IsPlaying() || done)
			{
				EndSession(s, w);
				g_sessions.erase(g_sessions.begin() + i);
			}
		}
	}
}

void PairedAnim::Reload(const std::string& contentRel)
{
	if (contentRel.empty()) g_pairs.clear();
	else g_pairs.erase(contentRel);
}

}  // namespace nuke
