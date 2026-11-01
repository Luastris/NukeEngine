// ReverbZone — a room volume blending the mixer's aux reverb by the listener position (D2).
#include "API/Model/ReverbZone.h"
#include "API/Model/AudioVolume.h"   // VolumeInfluence
#include "API/Model/Atom.h"
#include "API/Model/Transform.h"
#include <algorithm>
#include <cmath>

namespace nuke {

static std::vector<ReverbZone*> gZones;
const std::vector<ReverbZone*>& ReverbZone::All() { return gZones; }

ReverbZone::ReverbZone() : Component("ReverbZone") {}

void ReverbZone::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	if (std::find(gZones.begin(), gZones.end(), this) == gZones.end()) gZones.push_back(this);
}

void ReverbZone::Destroy()
{
	gZones.erase(std::remove(gZones.begin(), gZones.end(), this), gZones.end());
}

// The shared volume math (AudioVolume.h): the atom's scale scales the volume; the weight fades
// to 0 over blendDistance outside the surface.
float ReverbZone::WeightAt(const Vector3& p) const
{
	if (!enabled) return 0.0f;
	return VolumeInfluence(transform, shape, radius, halfExtents, blendDistance, weight, p);
}

}  // namespace nuke
