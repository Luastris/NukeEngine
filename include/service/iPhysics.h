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

	// ABI: appended fields only (the engine is the sole producer of this struct).
	// Center-of-mass shift in shape-local units (vehicles/boats: lower = stabler).
	float comOffset[3] = { 0, 0, 0 };
};

// A query shape (shape casts / overlaps) — the primitive subset of NukeBodyDesc.
struct NukeShapeDesc
{
	int   shape = 1;                                // 0 = Box, 1 = Sphere, 2 = Capsule
	float halfExtents[3] = { 0.5f, 0.5f, 0.5f };    // Box
	float radius = 0.5f;                            // Sphere/Capsule
	float halfHeight = 0.5f;                        // Capsule: half of the cylinder part
};

// A SwingTwist ragdoll joint between two bodies (see createSwingTwistJoint).
struct NukeJointDesc
{
	uint64_t bodyA = 0, bodyB = 0;       // parent, child
	float pivot[3]     = { 0, 0, 0 };    // WORLD anchor (the joint the bodies hinge about)
	float twistAxis[3] = { 0, 1, 0 };    // WORLD bone direction at creation
	float planeAxis[3] = { 1, 0, 0 };    // WORLD swing reference, perpendicular to twistAxis
	float twistMin = -0.5f, twistMax = 0.5f;   // radians about twistAxis
	float swing1 = 0.7f, swing2 = 0.7f;        // half-cone angles (radians): plane / normal
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

// A generic two-body constraint (see createConstraint). All positions/axes in WORLD space at
// creation time; the backend converts to body-local frames.
struct NukeConstraintDesc
{
	int type = 0;                       // 0 Hinge, 1 Slider, 2 Distance, 3 Spring, 4 Cone
	uint64_t bodyA = 0;                 // 0 = pinned to the world
	uint64_t bodyB = 0;                 // required
	float pivot[3]  = { 0, 0, 0 };      // anchor point (distance/spring: the end on bodyA)
	float pivotB[3] = { 0, 0, 0 };      // distance/spring: the end on bodyB (others: same as pivot)
	float axis[3]   = { 0, 0, 1 };      // hinge rotation axis / slider travel axis / cone twist axis
	float normal[3] = { 1, 0, 0 };      // hinge zero-angle reference, perpendicular to axis
	bool  limit = false;
	float min = 0.0f, max = 0.0f;       // hinge: radians; slider: units; distance: length range
	float frequency = 0.0f, damping = 0.0f;   // distance/spring softness (0 = rigid); Spring REQUIRES > 0
	float halfCone = 0.5f;              // cone half-angle, radians
};

// One wheel of a vehicle (see createVehicle). Suspension hangs DOWN from the chassis-local
// attachment point.
struct NukeWheelDesc
{
	float pos[3] = { 0, 0, 0 };        // chassis-local suspension attachment
	float radius = 0.3f;
	float width = 0.2f;
	float suspensionMin = 0.2f, suspensionMax = 0.5f;
	float frequency = 1.5f, damping = 0.5f;   // suspension spring
	float maxSteerDeg = 0.0f;          // 0 = fixed
	bool  driven = false;              // receives engine torque
	float maxBrakeTorque = 1500.0f;
	float maxHandBrakeTorque = 0.0f;   // usually the rear wheels
};

// A wheeled vehicle over an existing chassis body. Driven wheels pair into differentials in
// declaration order.
struct NukeVehicleDesc
{
	uint64_t chassis = 0;
	const NukeWheelDesc* wheels = nullptr;   // read only DURING createVehicle
	int   wheelCount = 0;
	float maxTorque = 500.0f;          // engine Nm
	float maxRPM = 6000.0f;
};

// Per-wheel state after a step.
struct NukeWheelState
{
	float pos[3] = { 0, 0, 0 };        // world wheel center
	float quat[4] = { 0, 0, 0, 1 };    // world wheel orientation (spin + steer)
	float suspension = 0.0f;           // current suspension length
	int   contact = 0;                 // touching ground
	float longSlip = 0.0f;             // longitudinal slip (velocity ratio)
	float latSlip = 0.0f;              // lateral slip (radians)
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

	// ---- ragdoll joints (stage 9) — ABI: appended at the END ------------------------------
	// SwingTwist between two bodies: a shoulder/hip-style cone (swing1/swing2 half-angles
	// around the plane/normal axes) + a twist range about twistAxis. All axes/pivot in WORLD
	// space at creation time. The optional motor drives bodyB toward a target orientation
	// relative to bodyA (powered ragdolls / hit reactions).
	virtual uint64_t createSwingTwistJoint(const NukeJointDesc& d) = 0;   // 0 on failure
	virtual void     destroyJoint(uint64_t joint) = 0;
	// frequency Hz / damping ratio of the position motor; enabled=false = free joint.
	virtual void setJointMotor(uint64_t joint, bool enabled, float frequency, float damping) = 0;
	// Target rotation of bodyB in bodyA's local frame (x,y,z,w).
	virtual void setJointTarget(uint64_t joint, const float localQuat[4]) = 0;

	// ---- serialized collision shapes (terrain bake) — ABI: appended at the END --------------
	// Cook a static triangle soup (3 floats per vertex, 3 vertices per triangle) into the
	// backend's serialized shape blob. The buffer lives until freeCookedBlob. Blobs are backend-
	// AND version-specific — a failed restore means "re-cook from source", never an error.
	// Thread-safe (no world access): bake jobs cook off the game thread.
	virtual bool cookMeshShape(const float* verts, int vertCount, void** outBlob, int* outSize) = 0;
	virtual void freeCookedBlob(void* blob) = 0;
	// Create a STATIC body from a cooked blob at a world pose — pure deserialization, no
	// triangle processing. Returns 0 when the blob doesn't match the backend/version.
	virtual uint64_t createBodyFromCooked(const void* blob, int size, const float pos[3],
	                                      const float quat[4], float friction, float restitution) = 0;
	// Wake every sleeping body intersecting the world AABB. Terrain edits pull the ground from
	// under SLEEPING bodies — without a wake they keep floating on the removed surface.
	virtual void activateBodies(const float mn[3], const float mx[3]) = 0;

	// ---- generic constraints — ABI: appended at the END -------------------------------------
	// Hinge/Slider/Distance/Spring/Cone between two bodies (bodyA 0 = the world). Freed with
	// destroyJoint (constraints and ragdoll joints share the handle space).
	virtual uint64_t createConstraint(const NukeConstraintDesc& d) = 0;   // 0 on failure
	// mode: 0 = off, 1 = velocity (hinge rad/s, slider units/s), 2 = position (hinge radians,
	// slider units). maxForce > 0 caps the motor's torque/force.
	virtual void setConstraintMotor(uint64_t c, int mode, float target, float maxForce) = 0;
	// |total position impulse| of the last step (N*s) — break-threshold checks (force = /dt).
	virtual float constraintImpulse(uint64_t c) = 0;

	// ---- wheeled vehicles — ABI: appended at the END ----------------------------------------
	virtual uint64_t createVehicle(const NukeVehicleDesc& d) = 0;   // 0 on failure
	virtual void     destroyVehicle(uint64_t v) = 0;
	// forward/right in -1..1, brake/handBrake in 0..1; any input wakes the chassis.
	virtual void  setVehicleInput(uint64_t v, float forward, float right, float brake, float handBrake) = 0;
	virtual bool  getWheelState(uint64_t v, int wheel, NukeWheelState& out) = 0;
	virtual float vehicleRPM(uint64_t v) = 0;      // engine RPM (audio hooks)
	virtual float vehicleSpeed(uint64_t v) = 0;    // signed forward speed, m/s

	// ---- swept-sphere distance — ABI: appended at the END -----------------------------------
	// How far the sphere's CENTER travels before first contact (camera booms, ledge probes) —
	// unlike shapeCast this returns the travel distance, not the contact point. False = clear.
	virtual bool sphereCastDist(float radius, const float from[3], const float dir[3],
	                            float maxDist, uint64_t ignoreBody, float& outDist) = 0;
};

}  // namespace nuke

#endif // !NUKEE_IPHYSICS_H
