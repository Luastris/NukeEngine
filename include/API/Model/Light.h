#pragma once
#ifndef NUKEE_LIGHT_H
#define NUKEE_LIGHT_H
#include "Include.h"

namespace nuke {

class Light : public Component
{
	enum LightType 
	{
		directional,
		point,
		area,
		spot
	};
public:
	float intensity, brightness;
	Color color;
	LightType lightType = LightType::directional;
};
}  // namespace nuke

#endif // !NUKEE_LIGHT_H
