#ifndef RAY_H
#define RAY_H
#include "Vector.h"
#include "Collider.h"

namespace nuke {

struct Ray
{
    Vector3 start;
    Vector3 direction;
    double length;
    int w, h;

	Ray();
	bool Collide(Collider collider);
};

}  // namespace nuke

#endif // RAY_H
