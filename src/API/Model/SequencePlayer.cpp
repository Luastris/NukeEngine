#include "API/Model/SequencePlayer.h"
#include "API/Model/AnimClip.h"
#include "API/Model/Animator.h"
#include "API/Model/Atom.h"
#include "API/Model/Audio.h"
#include "API/Model/Camera.h"
#include "API/Model/Events.h"
#include "API/Model/Sequence.h"
#include "API/Model/Skeleton.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Time.h"
#include "API/Model/Transform.h"
#include "API/Model/resdb.h"
#include "interface/AppInstance.h"
#include "reflect/ReflectBind.h"
#include <cmath>
#include <functional>
#include <iostream>

namespace nuke {

SequencePlayer::SequencePlayer() : Component("SequencePlayer") {}

void SequencePlayer::Init(Atom* parent)
{
	transform = &parent->GetTransform();
	atom = parent;
	parent->components.push_back(this);
}

void SequencePlayer::Destroy() {}
void SequencePlayer::FixedUpdate() {}
void SequencePlayer::Pause() {}
void SequencePlayer::Reset()
{
	t = 0.0;
	playing = started = false;
	seq = nullptr;
}

Sequence* SequencePlayer::EnsureSeq()
{
	if (!seq && !seqGuid.empty()) seq = ResDB::getSingleton()->GetSequence(seqGuid);
	return seq;
}

Atom* SequencePlayer::ResolvePath(const std::string& path) const
{
	// Leading '/' = absolute from the live world's roots (editor-authored sequences);
	// otherwise relative to this component's atom.
	Atom* a = atom;
	std::string p = path;
	if (!p.empty() && p[0] == '/')
	{
		p.erase(0, 1);
		a = nullptr;
		World* w = AppInstance::GetSingleton() ? AppInstance::GetSingleton()->currentWorld : nullptr;
		if (!w || p.empty()) return nullptr;
		const size_t s = p.find('/');
		const std::string seg = (s == std::string::npos) ? p : p.substr(0, s);
		p = (s == std::string::npos) ? std::string() : p.substr(s + 1);
		for (Atom* root : w->GetHierarchy())
			if (root && root->name == seg) { a = root; break; }
	}
	while (a && !p.empty())
	{
		const size_t s = p.find('/');
		const std::string seg = (s == std::string::npos) ? p : p.substr(0, s);
		p = (s == std::string::npos) ? std::string() : p.substr(s + 1);
		Atom* next = nullptr;
		for (Atom* ch : a->children)
			if (ch && ch->name == seg) { next = ch; break; }
		a = next;
	}
	return a;
}

void SequencePlayer::Play()  { EnsureSeq(); playing = true; }
void SequencePlayer::Stop()  { playing = false; t = 0.0; }
void SequencePlayer::SetPaused(bool paused) { playing = !paused && EnsureSeq(); }
bool SequencePlayer::IsPlaying() { return playing; }
double SequencePlayer::Time() { return t; }
void SequencePlayer::SetTime(double nt)
{
	EnsureSeq();
	t = nt < 0.0 ? 0.0 : nt;
	ApplyAt(t, t, false);   // scrub: values only, no crossings
}

void SequencePlayer::ApplyAt(double prevT, double curT, bool fire)
{
	Sequence* s = EnsureSeq();
	if (!s) return;
	float v[4];

	for (const Sequence::TransformTrack& tr : s->transformTracks)
	{
		Atom* a = ResolvePath(tr.path);
		if (!a) continue;
		Transform& tf = a->GetTransform();
		if (!tr.pos.empty())
		{
			AnimClip::Sample(tr.pos, curT, v);
			tf.position = Vector3(v[0], v[1], v[2]);
		}
		if (!tr.rot.empty())
		{
			AnimClip::SampleQuat(tr.rot, curT, v);
			tf.rotation = Quaternion(v[0], v[1], v[2], v[3]);
		}
		if (!tr.scale.empty())
		{
			AnimClip::Sample(tr.scale, curT, v);
			tf.scale = Vector3(v[0], v[1], v[2]);
		}
	}

	for (const Sequence::PropTrack& tr : s->propTracks)
	{
		if (tr.keys.empty()) continue;
		Atom* a = ResolvePath(tr.path);
		if (!a) continue;
		// "Host:selector" = a SCRIPT class: the host component carries it as a dynamic prop.
		std::string compType = tr.comp;
		std::string scriptSel;
		if (const size_t sp = tr.comp.find(':'); sp != std::string::npos)
		{
			compType = tr.comp.substr(0, sp);
			scriptSel = tr.comp.substr(sp + 1);
		}
		Component* comp = nullptr;
		for (Component* c : a->components)
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
		if (!comp) continue;
		if (tr.dim == 0)
		{
			const AnimClip::Key* last = &tr.keys.front();
			for (const AnimClip::Key& k : tr.keys)
				if (k.t <= curT) last = &k;
			v[0] = last->v[0]; v[1] = v[2] = v[3] = 0;
		}
		else AnimClip::Sample(tr.keys, curT, v);
		if (!scriptSel.empty())
		{
			// The host may be running a different class than the track was authored against —
			// only props the live instance actually exposes are written (a miss is a no-op).
			bool has = false;
			for (const DynProp& d : comp->DynamicProps())
				if (d.name == tr.prop) { has = true; break; }
			if (!has) continue;
			NukeVar nv;
			nv.kind = NukeVar::Kind::Number;
			nv.num = v[0];
			comp->SetDynamicProp(tr.prop, nv);
			continue;
		}
		TypeInfo* ti = Registry_Find(compType);
		if (!ti) continue;
		for (const Field& f : ti->fields)
		{
			if (f.name != tr.prop) continue;
			void* ad = f.addr(comp);
			if (!ad) break;
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
			default: break;
			}
			Reflect_ComponentFieldChanged(comp, f);
			break;
		}
	}

	for (const Sequence::BoneTrack& tr : s->boneTracks)
	{
		Atom* a = ResolvePath(tr.path);
		if (!a) continue;
		SkinnedMeshRenderer* smr = a->GetComponent<SkinnedMeshRenderer>();
		if (!smr) continue;
		bool touched = false;
		if (!tr.pos.empty())
		{
			AnimClip::Sample(tr.pos, curT, v);
			smr->SetBonePosition(tr.bone, Vector3(v[0], v[1], v[2]));
			touched = true;
		}
		if (!tr.rot.empty())
		{
			AnimClip::SampleQuat(tr.rot, curT, v);
			smr->SetBoneRotation(tr.bone, Quaternion(v[0], v[1], v[2], v[3]));
			touched = true;
		}
		if (touched) smr->Apply();
	}

	if (!fire) return;
	auto crossed = [&](float et) { return et > prevT && et <= curT; };

	for (const Sequence::ClipTrack& tr : s->clipTracks)
		for (const Sequence::ClipEntry& e : tr.entries)
			if (crossed(e.t))
			{
				Atom* a = ResolvePath(tr.path);
				Animator* an = a ? a->GetComponent<Animator>() : nullptr;
				if (an) an->Play(e.clip);
			}
	for (const Sequence::Event& e : s->events)
		if (crossed(e.t)) Events::Emit(e.name, e.payload);
	for (const Sequence::Cut& c : s->cuts)
		if (crossed(c.t))
		{
			Atom* target = ResolvePath(c.camera);
			Camera* cam = target ? target->GetComponent<Camera>() : nullptr;
			if (!cam) continue;
			// exclusive Main flag across the whole world = the actual cut
			World* w = AppInstance::GetSingleton() ? AppInstance::GetSingleton()->currentWorld : nullptr;
			if (w)
			{
				std::function<void(Atom*)> walk = [&](Atom* x)
				{
					if (!x) return;
					for (Component* cc : x->components)
						if (Camera* other = dynamic_cast<Camera*>(cc)) other->mainCamera = false;
					for (Atom* ch : x->children) walk(ch);
				};
				for (Atom* root : w->GetHierarchy()) walk(root);
			}
			cam->mainCamera = true;
		}
	for (const Sequence::Sting& st : s->stings)
		if (crossed(st.t)) Audio::Play(st.clip, st.volume, false, st.bus);
}

void SequencePlayer::Update()
{
	if (!started)
	{
		started = true;
		if (playOnStart && EnsureSeq()) playing = true;
	}
	if (!playing) return;
	Sequence* s = EnsureSeq();
	if (!s || s->duration <= 0.0) return;
	const double prevT = t;
	t += Time::getSingleton()->delta * speed;
	bool wrapped = false;
	if (t >= s->duration)
	{
		if (loop) { ApplyAt(prevT, s->duration, true); t = fmod(t, s->duration); wrapped = true; }
		else      { t = s->duration; playing = false; }
	}
	if (wrapped) ApplyAt(0.0, t, true);
	else         ApplyAt(prevT, t, true);
}

std::string SequencePlayer::BakeSkeletal(const std::string& outContentRel)
{
	Sequence* s = EnsureSeq();
	if (!s || s->boneTracks.empty())
	{
		std::cout << "[Sequence]\tBakeSkeletal: no bone tracks" << std::endl;
		return std::string();
	}
	// the skeleton = the first bone-track atom's SMR
	Skeleton* sk = nullptr;
	for (const Sequence::BoneTrack& tr : s->boneTracks)
	{
		Atom* a = ResolvePath(tr.path);
		SkinnedMeshRenderer* smr = a ? a->GetComponent<SkinnedMeshRenderer>() : nullptr;
		if (smr && smr->EnsureSkeleton()) { sk = smr->skeleton; break; }
	}
	AnimClip* clip = new AnimClip();
	clip->guid = ResDB::NewGuid();
	clip->name = s->name.empty() ? std::string("baked") : s->name;
	clip->duration = s->duration;
	if (sk) clip->skelGuid = sk->guid;
	for (const Sequence::BoneTrack& tr : s->boneTracks)
	{
		AnimClip::Channel ch;
		ch.bone = tr.bone;
		ch.pos = tr.pos;
		ch.rot = tr.rot;
		clip->channels.push_back(std::move(ch));
	}
	const std::string full = AppInstance::GetSingleton()->ResolveContent(outContentRel);
	if (!clip->SaveToFile(full))
	{
		delete clip;
		std::cout << "[Sequence]\tBakeSkeletal: write failed: " << full << std::endl;
		return std::string();
	}
	ResDB::getSingleton()->RegisterClip(clip);
	ResDB::getSingleton()->SetAssetPath(clip->guid, full);
	std::cout << "[Sequence]\tbaked " << clip->channels.size() << " bone tracks -> "
	          << outContentRel << std::endl;
	return clip->guid;
}

}  // namespace nuke
