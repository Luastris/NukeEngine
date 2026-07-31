#pragma once
#ifndef NUKEE_IPHYSICS_H
#define NUKEE_IPHYSICS_H
#include <cstdint>

namespace nuke {

// Backend-neutral POD rigid-body description crossing the physics seam. Scale is BAKED into
// the shape dimensions by the caller, so changing an atom's scale requires a body recreate.
struct NukeBodyDesc
{
	// Shape (matches Collider::Shape): 0 = Box, 1 = Sphere, 2 = Capsule, 3 = Mesh.
	int   shape = 0;
	float halfExtents[3] = { 0.5f, 0.5f, 0.5f };   // Box: half sizes (scale-baked)
	float radius = 0.5f;                            // Sphere/Capsule
	float halfHeight = 0.5f;                        // Capsule: half of the CYLINDER part

	// Mesh shape (shape == 3): triangle soup, 3 floats per vertex, every 3 consecutive vertices
	// form one triangle. Read only DURING createBody. convex=false -> static triangle mesh
	// (motion is forced Static); convex=true -> convex hull (dynamic-capable).
	const float* meshVerts = nullptr;
	int   meshVertCount = 0;
	bool  convex = false;

	bool  isTrigger = false;                        // sensor: reports contacts, no collision response

	// Motion: 0 = Static, 1 = Dynamic, 2 = Kinematic (moved by gameplay, pushes others).
	int   motion = 0;
	float mass = 1.0f;                              // Dynamic only
	float friction = 0.5f;
	float restitution = 0.0f;                       // bounciness [0..1]
	float linearDamping = 0.05f;
	float angularDamping = 0.05f;
	bool  useGravity = true;                        // Dynamic only

	float pos[3]  = { 0, 0, 0 };                    // initial WORLD pose
	float quat[4] = { 0, 0, 0, 1 };                 // (x, y, z, w)
};

// A query shape (shape casts / overlaps) — the primitive subset of NukeBodyDesc.
struct NukeShapeDesc
{
	int   shape = 1;                                // 0 = Box, 1 = Sphere, 2 = Capsule
	float halfExtents[3] = { 0.5f, 0.5f, 0.5f };    // Box
	float radius = 0.5f;                            // Sphere/Capsule
	float halfHeight = 0.5f;                        // Capsule: half of the cylinder part
};

// One contact transition, reported by fetchContacts after a step. Trigger-vs-collision
// classification happens engine-side.
struct NukeContactEvent
{
	uint64_t bodyA = 0, bodyB = 0;
	int   phase = 0;                 // 0 = begin (enter), 1 = end (exit)
	float point[3]  = { 0, 0, 0 };   // world contact point (begin only; zeros on end)
	float normal[3] = { 0, 0, 0 };   // world contact normal, from A to B (begin only)
};

// Character controller: a virtual kinematic capsule (not a rigid body) driven by a desired
// velocity per fixed step. The PIVOT is at the FEET (bottom of the capsule).
struct NukeCharacterDesc
{
	float radius = 0.35f;            // capsule radius (scale-baked by the caller)
	float halfHeight = 0.55f;        // half of the CYLINDER part (total = 2*(halfHeight+radius))
	float maxSlopeDeg = 50.0f;       // steeper ground = not walkable (slide off)
	float stepHeight = 0.35f;        // stair climb: max ledge the character steps up (0 = off)
	float stickDistance = 0.5f;      // stick-to-floor probe when walking down slopes/stairs (0 = off)
	float mass = 70.0f;              // how hard the character pushes dynamic bodies
	float maxStrength = 100.0f;      // max push force (N)
	float padding = 0.02f;           // collision skin around the shape
	bool  innerBody = true;          // also add a kinematic capsule body so queries/contacts see the character
	float up[3] = { 0, 1, 0 };       // up axis
	float pos[3] = { 0, 0, 0 };      // initial FEET position (world)
};

// Ground classification after a step.
enum NukeGroundState
{
	NUKE_GROUND_ON        = 0,   // standing on walkable ground
	NUKE_GROUND_STEEP     = 1,   // touching ground too steep to stand on (sliding)
	NUKE_GROUND_UNSUPPORTED = 2, // touching something that can't carry the character
	NUKE_GROUND_AIR       = 3,   // airborne
};

// The physics service contract: the active backend implements it and hands it to the loader
// via NUKEModule::queryService(). Bodies are opaque uint64 handles (0 = invalid).
// Threading: called only from the game update thread; backends may parallelize internally.
class iPhysics
{
public:
	static constexpr const char* kServiceName = "physics";

	virtual ~iPhysics() {}

	// World lifecycle. init is idempotent; reset destroys ALL bodies without tearing the backend down.
	virtual bool init() = 0;
	virtual void reset() = 0;
	virtual void setGravity(const float g[3]) = 0;

	// Body lifecycle.
	virtual uint64_t createBody(const NukeBodyDesc& desc) = 0;   // 0 on failure
	virtual void     destroyBody(uint64_t body) = 0;

	// Pose. set = teleport (static moves / script teleports); get = simulated result.
	virtual void setBodyPose(uint64_t body, const float pos[3], const float quat[4]) = 0;
	virtual bool getBodyPose(uint64_t body, float pos[3], float quat[4]) = 0;

	// Move the body to the target pose over ONE fixed step, deriving velocities so riders get
	// carried. dt <= 0 falls back to a teleport.
	virtual void moveKinematic(uint64_t body, const float pos[3], const float quat[4], float dt) = 0;

	// Dynamics (Dynamic bodies).
	virtual void setLinearVelocity(uint64_t body, const float v[3]) = 0;
	virtual void getLinearVelocity(uint64_t body, float v[3]) = 0;
	virtual void setAngularVelocity(uint64_t body, const float v[3]) = 0;   // rad/s
	virtual void getAngularVelocity(uint64_t body, float v[3]) = 0;
	virtual void addForce(uint64_t body, const float f[3]) = 0;      // continuous (this step)
	virtual void addImpulse(uint64_t body, const float i[3]) = 0;    // instantaneous

	// Advance the simulation by ONE fixed step.
	virtual void step(float dt) = 0;

	// Drain contact transitions collected during step(), triggers included. Returns how many
	// events were written (up to `max`); call repeatedly until it returns less than `max`.
	// Events do not persist across steps.
	virtual int fetchContacts(NukeContactEvent* out, int max) = 0;

	// Nearest-hit ray cast. False on miss. hitBody receives the body handle.
	virtual bool raycast(const float from[3], const float dir[3], float maxDist,
	                     uint64_t& hitBody, float hitPoint[3], float hitNormal[3]) = 0;

	// Nearest-hit shape cast: sweep `shape` (oriented by `quat`) from `from` along `dir`
	// for up to maxDist. False on miss.
	virtual bool shapeCast(const NukeShapeDesc& shape, const float from[3], const float quat[4],
	                       const float dir[3], float maxDist,
	                       uint64_t& hitBody, float hitPoint[3], float hitNormal[3]) = 0;

	// Collect every body overlapping `shape` placed at pos/quat, triggers included. Returns how
	// many handles were written to `outBodies` (up to `max`).
	virtual int overlap(const NukeShapeDesc& shape, const float pos[3], const float quat[4],
	                    uint64_t* outBodies, int max) = 0;

	// ---- CHARACTER controllers (ABI: appended at the END of the vtable) ---------------
	// Characters step INSIDE step(dt): set the desired velocity before the step, read the
	// resulting state after. They collide with bodies and with each other.
	virtual uint64_t createCharacter(const NukeCharacterDesc& desc) = 0;   // 0 on failure
	virtual void     destroyCharacter(uint64_t ch) = 0;
	// Desired velocity for the NEXT step (world units/s). The caller owns gravity integration
	// and platform inheritance; the backend only resolves collisions.
	virtual void setCharacterVelocity(uint64_t ch, const float v[3]) = 0;
	// The ACTUAL velocity after the last step (post slide/step-up resolution).
	virtual void getCharacterVelocity(uint64_t ch, float v[3]) = 0;
	virtual void setCharacterPosition(uint64_t ch, const float pos[3]) = 0;   // teleport (feet)
	// Post-step state: feet position, ground classification, ground normal, the ground's own
	// velocity, and the ground body handle (0 = none).
	virtual bool getCharacterState(uint64_t ch, float pos[3], int& groundState,
	                               float groundNormal[3], float groundVel[3],
	                               uint64_t& groundBody) = 0;
	// Live tuning of the walk parameters; shape changes need a recreate.
	virtual void setCharacterParams(uint64_t ch, float maxSlopeDeg, float stepHeight,
	                                float stickDistance) = 0;

	// The character's inner kinematic body handle (0 = created without one). Feed it to
	// raycastIgnore so camera booms / aim rays skip the character itself.
	virtual uint64_t characterBodyId(uint64_t ch) = 0;

	// Nearest-hit ray cast that IGNORES one body — a ray starting inside a character's own
	// capsule would otherwise report an inside-hit at distance 0. ignoreBody 0 = plain cast.
	virtual bool raycastIgnore(const float from[3], const float dir[3], float maxDist,
	                           uint64_t ignoreBody,
	                           uint64_t& hitBody, float hitPoint[3], float hitNormal[3]) = 0;

	// Shape cast with the same one-body exclusion.
	virtual bool shapeCastIgnore(const NukeShapeDesc& shape, const float from[3], const float quat[4],
	                             const float dir[3], float maxDist, uint64_t ignoreBody,
	                             uint64_t& hitBody, float hitPoint[3], float hitNormal[3]) = 0;

	// ABI: appended at the END; never insert mid-vtable.
	// Force applied at a world point (generates torque about the COM).
	virtual void addForceAtPoint(uint64_t body, const float force[3], const float point[3]) = 0;
	// The body's velocity AT a world point (linear + omega x r).
	virtual void getPointVelocity(uint64_t body, const float point[3], float outVel[3]) = 0;
};

}  // namespace nuke

#endif // !NUKEE_IPHYSICS_H
