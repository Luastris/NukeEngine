#include "API/Model/TimeVolume.h"
#include "API/Model/Atom.h"
#include "API/Model/Transform.h"
#include "API/Model/Camera.h"
#include "API/Model/Time.h"
#include <algorithm>
#include <cmath>

namespace nuke {

static std::vector<TimeVolume*> gVolumes;   // game thread only
static std::vector<TimeVolume*> gSorted;    // gVolumes by ascending priority, rebuilt once per frame
static unsigned long long       gSortedFrame = ~0ull;

TimeVolume::TimeVolume() : Component("TimeVolume") {}

void TimeVolume::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	if (std::find(gVolumes.begin(), gVolumes.end(), this) == gVolumes.end()) gVolumes.push_back(this);
	gSortedFrame = ~0ull;
}
void TimeVolume::Destroy()     { gVolumes.erase(std::remove(gVolumes.begin(), gVolumes.end(), this), gVolumes.end()); gSortedFrame = ~0ull; }
void TimeVolume::Update()      {}
void TimeVolume::FixedUpdate() {}
void TimeVolume::Pause()       {}
void TimeVolume::Reset()       {}

const std::vector<TimeVolume*>& TimeVolume::All() { return gVolumes; }
bool TimeVolume::Any() { return !gVolumes.empty(); }

float TimeVolume::WeightAt(const Vector3& p) const
{
	if (!transform || !enabled) return 0.0f;
	const Vector3 c = transform->globalPosition();
	const Vector3 scl = transform->globalScale();
	double outside;   // metres from the shape's surface, 0 inside
	if (shape == 0)
	{
		const double d = std::sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y) + (p.z - c.z) * (p.z - c.z));
		outside = std::max(d - (double)ScaleRadius(radius, scl), 0.0);
	}
	else
	{
		const Vector3 he = ScaleExtents(halfExtents, scl);
		const Quaternion q = transform->globalRotation();
		const Quaternion inv(-q.x, -q.y, -q.z, q.w);
		const Vector3 l = inv.Rotate(Vector3(p.x - c.x, p.y - c.y, p.z - c.z));
		const double dx = std::max(std::fabs(l.x) - std::fabs((double)he.x), 0.0);
		const double dy = std::max(std::fabs(l.y) - std::fabs((double)he.y), 0.0);
		const double dz = std::max(std::fabs(l.z) - std::fabs((double)he.z), 0.0);
		outside = std::sqrt(dx * dx + dy * dy + dz * dz);
	}
	float w = 1.0f;
	if (outside > 0.0) w = (blendDistance > 1e-4f) ? (float)std::max(1.0 - outside / blendDistance, 0.0) : 0.0f;
	return w;
}

void TimeVolume::ScalesFor(Atom* a, float out[5])
{
	for (int i = 0; i < 5; ++i) out[i] = 1.0f;
	if (!a || gVolumes.empty()) return;
	const unsigned long long frame = Time::getSingleton()->frame;
	if (gSortedFrame != frame)
	{
		gSorted = gVolumes;
		std::stable_sort(gSorted.begin(), gSorted.end(), [](const TimeVolume* x, const TimeVolume* y) { return x->priority < y->priority; });
		gSortedFrame = frame;
	}
	const Vector3 pos = a->GetTransform().globalPosition();
	const bool hasCamera = a->GetComponent<Camera>() != nullptr;
	for (TimeVolume* v : gSorted)
	{
		if (!v || v->weight <= 0.0f) continue;
		if (v->exemptCameras && hasCamera) continue;
		if (!v->exemptTag.empty())
		{
			bool ex = false;
			for (Atom* p = a; p && !ex; p = p->parent) ex = (p->tag == v->exemptTag);
			if (ex) continue;
		}
		const float w = v->WeightAt(pos) * v->weight;
		if (w <= 0.0f) continue;
		const float s = v->timeScale < 0.0f ? 0.0f : v->timeScale;
		const bool on[5] = { v->affectLogic, v->affectAnimation, v->affectPhysics, v->affectParticles, v->affectAudio };
		for (int i = 0; i < 5; ++i)
			if (on[i]) out[i] += (s - out[i]) * w;
	}
}

}  // namespace nuke
