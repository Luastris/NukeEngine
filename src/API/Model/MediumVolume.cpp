#include "API/Model/MediumVolume.h"
#include "API/Model/Math.h"   // ScaleExtents / ScaleRadius: volumes follow the atom scale
#include "API/Model/Atom.h"
#include "render/irender.h"
#include <boost/thread/mutex.hpp>
#include <algorithm>
#include <cmath>

namespace nuke {

// Live volumes; the lock covers Collect() running from the render side.
static boost::mutex gVolLock;
static std::vector<MediumVolume*> gVolumes;

MediumVolume::MediumVolume(const char* typeName) : Component(typeName) {}

void MediumVolume::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	boost::mutex::scoped_lock l(gVolLock);
	if (std::find(gVolumes.begin(), gVolumes.end(), this) == gVolumes.end()) gVolumes.push_back(this);
}

void MediumVolume::Destroy()
{
	boost::mutex::scoped_lock l(gVolLock);
	gVolumes.erase(std::remove(gVolumes.begin(), gVolumes.end(), this), gVolumes.end());
}
void MediumVolume::Update() {}
void MediumVolume::FixedUpdate() {}
void MediumVolume::Pause() {}
void MediumVolume::Reset() {}

void MediumVolume::FillMedium(NukeFogVolumeDesc& d) const
{
	Transform& tr = *transform;
	const Vector3 c = tr.globalPosition();
	const Quaternion q = tr.globalRotation();
	const Vector3 scl = tr.globalScale();
	d.pos[0] = (float)c.x; d.pos[1] = (float)c.y; d.pos[2] = (float)c.z;
	d.shape = shape < 0 ? 0 : (shape > 2 ? 2 : shape);
	if (d.shape == 1)
	{
		const float r = std::max(ScaleRadius((float)halfExtents.x, scl), 0.01f);
		d.halfExt[0] = d.halfExt[1] = d.halfExt[2] = r;
	}
	else
	{
		const Vector3 he = ScaleExtents(halfExtents, scl);
		d.halfExt[0] = std::max((float)fabs(he.x), 0.01f); d.halfExt[1] = std::max((float)fabs(he.y), 0.01f); d.halfExt[2] = std::max((float)fabs(he.z), 0.01f);
	}
	d.rot[0] = (float)q.x; d.rot[1] = (float)q.y; d.rot[2] = (float)q.z; d.rot[3] = (float)q.w;
	d.falloff = falloff;
	d.id = atom ? (unsigned long long)atom->id.id : 0ull;
}

void MediumVolume::Collect(std::vector<NukeFogVolumeDesc>& out)
{
	boost::mutex::scoped_lock l(gVolLock);
	for (MediumVolume* v : gVolumes)
	{
		if (!v || !v->enabled || !v->transform || !v->HasMedium()) continue;
		NukeFogVolumeDesc d;
		v->FillMedium(d);
		out.push_back(d);
	}
}

}  // namespace nuke
