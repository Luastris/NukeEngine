#pragma once
#ifndef NUKEE_MEDIUMVOLUME_H
#define NUKEE_MEDIUMVOLUME_H
#include "API/Model/Include.h"
#include "API/Model/Vector.h"
#include "reflect/Reflect.h"
#include <vector>

namespace nuke {

struct NukeFogVolumeDesc;

// A volume of medium in the World's volumetric grid: the shape (box / sphere / ellipsoid, from
// the atom's transform) and the registry the World collects every frame. Abstract: what fills
// the shape - scattering air, fog, smoke - is a module's business (NukeVFX: ScatterVolume,
// FogVolume); a subclass fills the medium into the renderer's description.
class NUKEENGINE_API MediumVolume : public Component
{
	NUKE_CLASS_NOCREATE(MediumVolume, Component, "World")
public:
	[[nuke::prop(label="Shape", enum="Box,Sphere,Ellipsoid")]] int shape = 0;
	[[nuke::prop(label="Half Extents", tip="Box half size / ellipsoid radii (local, scaled by the atom). Sphere uses X.")]] Vector3 halfExtents = Vector3(5, 5, 5);
	[[nuke::prop(label="Edge Falloff", min=0, max=1, tip="0 = hard edge, 1 = fades from the centre to the edge.")]] float falloff = 0.5f;

	explicit MediumVolume(const char* typeName);
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	// The shape as the renderer sees it (from the world transform); a subclass adds its medium.
	virtual void FillMedium(NukeFogVolumeDesc& d) const;
	virtual bool HasMedium() const = 0;
	// Every enabled volume with a medium, as the renderer sees it.
	static void Collect(std::vector<NukeFogVolumeDesc>& out);
};

}  // namespace nuke

#endif
