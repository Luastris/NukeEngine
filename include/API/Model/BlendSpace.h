#pragma once
#ifndef NUKEE_BLENDSPACE_H
#define NUKEE_BLENDSPACE_H
#include "NukeAPI.h"
#include <string>
#include <vector>
#include "reflect/Reflect.h"

namespace nuke {

// Blend-space ASSET (.nublend, JSON): clips placed on a 1D axis or 2D plane, blended by the
// Animator's parameter values (paramX/paramY). A state machine state may use one as its motion.
class NUKEENGINE_API BlendSpace
{
	NUKE_CLASS(BlendSpace, Object)
public:
	std::string guid;                          // asset id (ResDB)
	[[nuke::prop(label="Name")]] std::string name;

	int dims = 1;                              // 1 = axis (x), 2 = plane (x, y)
	std::string paramX, paramY;                // Animator parameter names driving the position

	// One clip sample: position on the axis/plane + a per-point playback rate. `mirror`
	// plays the clip mirrored (left strafe from a right-strafe clip).
	struct Point
	{
		std::string clip;                      // clip guid (or name)
		float x = 0, y = 0;
		float speed = 1;
		bool  mirror = false;
	};
	std::vector<Point> points;

	// Blend weights at (x, y), one per point, summing to 1. 1D = segment lerp between the
	// two neighbours; 2D = gradient-band interpolation (stable for arbitrary layouts).
	void Weights(float x, float y, std::vector<float>& out) const;

	// Native asset format (.nublend): JSON — small, diff-able, hand-editable.
	bool               SaveToFile(const std::string& path) const;
	static BlendSpace* LoadFromFile(const std::string& path);
	static BlendSpace* LoadFromMemory(const std::string& data);   // packed content
	static BlendSpace* FromString(const std::string& json);
	std::string        ToString() const;
};

}  // namespace nuke

#endif // !NUKEE_BLENDSPACE_H
