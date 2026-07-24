#pragma once
#ifndef NUKEE_IPHYSICS_H
#define NUKEE_IPHYSICS_H
#include <cstdint>

namespace nuke {

// Backend-neutral rigid-body description crossing the physics seam. POD only — no
// backend (Jolt/PhysX) and no engine model types, exactly like the iRender seam.
// Scale is BAKED into the shape dimensions by the caller (physics engines don't scale
// bodies); changing an atom's scale after body creation requires a recreate.
struct NukeBodyDesc
{
	// Shape (matches Collider::Shape): 0 = Box, 1 = Sphere, 2 = Capsule, 3 = Mesh.
	int   shape = 0;
	float halfExtents[3] = { 0.5f, 0.5f, 0.5f };   // Box: half sizes (scale-baked)
	float radius = 0.5f;                            // Sphere/Capsule
	float halfHeight = 0.5f;                        // Capsule: half of the CYLINDER part

	// Mesh shape (shape == 3): TRIANGLE SOUP — 3 floats per vertex, every 3 consecutive
	// vertices form one triangle (the engine bakes world scale in before the call).
	// The pointer is only read DURING createBody; the backend copies what it needs.
	// convex=false -> static triangle mesh (motion is forced Static by the backend);
	// convex=true  -> convex hull of the vertices (dynamic-capable).
	const float* meshVerts = nullptr;
	int   meshVertCount = 0;
	bool  convex = false;

	// Trigger (sensor): reports contact events, applies NO collision response.
	bool  isTrigger = false;

	// Motion (matches the component model): 0 = Static (Collider without Rigidbody),
	// 1 = Dynamic, 2 = Kinematic (Rigidbody.isKinematic — moved by gameplay, pushes others).
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
// classification happens ENGINE-side (it knows the components' isTrigger flags).
struct NukeContactEvent
{
	uint64_t bodyA = 0, bodyB = 0;
	int   phase = 0;                 // 0 = begin (enter), 1 = end (exit)
	float point[3]  = { 0, 0, 0 };   // world contact point (begin only; zeros on end)
	float normal[3] = { 0, 0, 0 };   // world contact normal, from A to B (begin only)
};

// Backend-neutral CHARACTER controller description. A character is a virtual kinematic
// capsule (not a rigid body): it slides along walls, walks stairs/slopes and pushes
// dynamic bodies, driven purely by a desired velocity per fixed step. The PIVOT is at
// the FEET (bottom of the capsule) — transforms map 1:1 to standing position.
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
	bool  innerBody = true;          // also add a kinematic capsule BODY at the character's pose:
	                                 // raycasts/queries/contacts see the character (recommended)
	float up[3] = { 0, 1, 0 };       // up axis (arbitrary — wall-walking games can tilt it)
	float pos[3] = { 0, 0, 0 };      // initial FEET position (world)
};

// Ground classification after a step (matches Jolt's EGroundState semantics).
enum NukeGroundState
{
	NUKE_GROUND_ON        = 0,   // standing on walkable ground
	NUKE_GROUND_STEEP     = 1,   // touching ground too steep to stand on (sliding)
	NUKE_GROUND_UNSUPPORTED = 2, // touching something that can't carry the character
	NUKE_GROUND_AIR       = 3,   // airborne
};

// The PHYSICS service contract (unified plugin model). The active physics backend
// (NukePhysicsJolt today, a PhysX module later) implements this and hands it to the
// loader via NUKEModule::queryService(); the ENGINE drives it from World's fixed-step
// update — the backend only simulates. Bodies are opaque uint64 handles (0 = invalid).
//
// Threading: everything is called from the game update thread (the fixed-step loop);
// backends may parallelize INTERNALLY (job system) but the API is single-threaded.
class iPhysics
{
public:
	static constexpr const char* kServiceName = "physics";

	virtual ~iPhysics() {}

	// World lifecycle. init is idempotent; reset destroys ALL bodies (play stop / world
	// switch) without tearing the backend down.
	virtual bool init() = 0;
	virtual void reset() = 0;
	virtual void setGravity(const float g[3]) = 0;

	// Body lifecycle.
	virtual uint64_t createBody(const NukeBodyDesc& desc) = 0;   // 0 on failure
	virtual void     destroyBody(uint64_t body) = 0;

	// Pose. set = teleport (static moves / script teleports); get = simulated result.
	virtual void setBodyPose(uint64_t body, const float pos[3], const float quat[4]) = 0;
	virtual bool getBodyPose(uint64_t body, float pos[3], float quat[4]) = 0;

	// Kinematic drive: move the body to the target pose over ONE fixed step, deriving the
	// velocities — riders standing on it get carried (a teleport would leave them behind).
	// dt <= 0 falls back to a teleport.
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

	// Drain contact transitions collected during step() (begin/end, triggers included).
	// Returns how many events were written to `out` (up to `max`); call repeatedly until
	// it returns less than `max` to drain a burst. Events do not persist across steps.
	virtual int fetchContacts(NukeContactEvent* out, int max) = 0;

	// Nearest-hit ray cast. False on miss. hitBody receives the body handle.
	virtual bool raycast(const float from[3], const float dir[3], float maxDist,
	                     uint64_t& hitBody, float hitPoint[3], float hitNormal[3]) = 0;

	// Nearest-hit SHAPE cast: sweep `shape` (oriented by `quat`) from `from` along `dir`
	// for up to maxDist. Catches what a thin ray slips past. False on miss.
	virtual bool shapeCast(const NukeShapeDesc& shape, const float from[3], const float quat[4],
	                       const float dir[3], float maxDist,
	                       uint64_t& hitBody, float hitPoint[3], float hitNormal[3]) = 0;

	// Collect every body overlapping `shape` placed at pos/quat. Returns how many handles
	// were written to `outBodies` (up to `max`); triggers are included.
	virtual int overlap(const NukeShapeDesc& shape, const float pos[3], const float quat[4],
	                    uint64_t* outBodies, int max) = 0;

	// ---- CHARACTER controllers (END of vtable — appended after the 1.1 surface) --------
	// Characters step INSIDE step(dt): set the desired velocity before the step, read the
	// resulting state after. Characters collide with bodies AND with each other.
	virtual uint64_t createCharacter(const NukeCharacterDesc& desc) = 0;   // 0 on failure
	virtual void     destroyCharacter(uint64_t ch) = 0;
	// Desired velocity for the NEXT step (world units/s, full 3D — the caller owns gravity
	// integration and platform inheritance; the backend only resolves collisions).
	virtual void setCharacterVelocity(uint64_t ch, const float v[3]) = 0;
	// The ACTUAL velocity after the last step (post slide/step-up resolution).
	virtual void getCharacterVelocity(uint64_t ch, float v[3]) = 0;
	virtual void setCharacterPosition(uint64_t ch, const float pos[3]) = 0;   // teleport (feet)
	// Post-step state: feet position, ground classification, ground normal, the ground's
	// own velocity (moving platforms) and the ground body handle (0 = none).
	virtual bool getCharacterState(uint64_t ch, float pos[3], int& groundState,
	                               float groundNormal[3], float groundVel[3],
	                               uint64_t& groundBody) = 0;
	// Live tuning of the walk parameters (inspector edits mid-play; shape changes recreate).
	virtual void setCharacterParams(uint64_t ch, float maxSlopeDeg, float stepHeight,
	                                float stickDistance) = 0;

	// The character's INNER kinematic body handle (0 = created without one). Feed it to
	// raycastIgnore so camera booms / aim rays skip the character itself.
	virtual uint64_t characterBodyId(uint64_t ch) = 0;

	// Nearest-hit ray cast that IGNORES one body (rays starting inside a character's own
	// capsule would otherwise report an inside-hit at distance 0). ignoreBody 0 = plain cast.
	virtual bool raycastIgnore(const float from[3], const float dir[3], float maxDist,
	                           uint64_t ignoreBody,
	                           uint64_t& hitBody, float hitPoint[3], float hitNormal[3]) = 0;

	// Shape cast with the same one-body exclusion (camera booms probe with a SPHERE so
	// they don't flicker on edges a thin ray alternately hits and misses).
	virtual bool shapeCastIgnore(const NukeShapeDesc& shape, const float from[3], const float quat[4],
	                             const float dir[3], float maxDist, uint64_t ignoreBody,
	                             uint64_t& hitBody, float hitPoint[3], float hitNormal[3]) = 0;
};

}  // namespace nuke

#endif // !NUKEE_IPHYSICS_H
