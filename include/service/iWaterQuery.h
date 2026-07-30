#pragma once
#ifndef NUKE_IWATERQUERY_H
#define NUKE_IWATERQUERY_H

namespace nuke {

// Water query service (7.5, stage 3): the "where is the water, and poke it" contract for
// modules that must react to water WITHOUT linking the water module — NukeVFX rain drops
// splash rings where they land, gameplay code checks wetness. Provided by NukeWater while
// it is loaded (Services registry rules: never cache the pointer across plugin toggles).
struct iWaterQuery
{
	static constexpr const char* kServiceName = "waterquery";
	virtual ~iWaterQuery() {}

	// Water surface height (world Y) at world XZ; < -1e8 when no water covers the point.
	virtual double HeightAt(double x, double z) = 0;
	// True when the point is below a water surface.
	virtual bool IsUnderwater(const float pos[3]) = 0;
	// Ripple impulse on the surface (rings) at the position.
	virtual void Splash(const float pos[3], float radius, float strength) = 0;
};

}  // namespace nuke

#endif // !NUKE_IWATERQUERY_H
