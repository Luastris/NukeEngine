#pragma once
#ifndef NUKEE_COLLIDER_H
#define NUKEE_COLLIDER_H

namespace nuke {

// PLACEHOLDER (intentional, do not remove): the collision component. Its real API
// (shape types box/sphere/capsule/mesh, [[nuke::prop]] fields, queries) is owned by the
// physics service (roadmap 1.1, Jolt) — the shapes/filters must match what the physics
// backend supports, so the API lands together with NukePhysicsJolt.
class Collider
{
public:
};
}  // namespace nuke

#endif // !NUKEE_COLLIDER_H
