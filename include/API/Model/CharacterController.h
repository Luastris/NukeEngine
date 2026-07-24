#pragma once
#ifndef NUKEE_CHARACTERCONTROLLER_H
#define NUKEE_CHARACTERCONTROLLER_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"

namespace nuke {

class Atom;

// Kinematic character controller (backed by the physics service's virtual character —
// Jolt CharacterVirtual today). NOT a rigid body: it slides along walls, walks stairs
// and slopes, sticks to the floor going down, pushes dynamic bodies and collides with
// other characters — all driven by a desired velocity you set every frame.
//
// PIVOT AT THE FEET: the atom's position is where the character stands.
//
// Two driving styles (the flexibility knob):
//  * autoGravity = true (default): you steer the HORIZONTAL motion (SetMove) and call
//    Jump(); the controller integrates gravity, zeroes the fall on landing and carries
//    you with moving platforms (inheritPlatform).
//  * autoGravity = false: the controller applies YOUR velocity verbatim (SetVelocity) —
//    write your own gravity, jumps, dashes, wall-runs; the capsule only resolves
//    collisions. This is the "professionals write their own" mode: the movement logic
//    is fully yours, the collision resolution is not your problem.
class NUKEENGINE_API CharacterController : public Component
{
	NUKE_CLASS(CharacterController, Component)
public:
	// Capsule (scale-baked at creation like Collider; live edits recreate the character).
	[[nuke::prop(label="Radius",      min=0.05)]] float radius = 0.35f;
	[[nuke::prop(label="Height",      min=0.2, tip="Total capsule height, feet to head.")]] float height = 1.8f;
	// Where the capsule sits relative to the atom: Feet = the atom's position is where the
	// character STANDS (feet-pivot rigs); Center = the capsule is centered on the atom
	// (meshes with a centered pivot — otherwise the visual sinks in waist-deep).
	enum Pivot { P_Feet = 0, P_Center = 1 };
	[[nuke::prop(label="Pivot", enum="Feet,Center")]] int pivot = P_Feet;
	[[nuke::prop(label="Capsule Offset", tip="Extra offset of the capsule relative to the pivot (local units).")]] Vector3 capsuleOffset;
	// Walking behavior.
	[[nuke::prop(label="Max Slope",   min=0, max=89, tip="Steeper ground is not walkable - the character slides off.")]] float maxSlope = 50.0f;
	[[nuke::prop(label="Step Height", min=0, tip="Highest ledge the character walks straight up (stairs). 0 disables.")]] float stepHeight = 0.35f;
	[[nuke::prop(label="Stick Distance", min=0, tip="How far the character is glued down when walking down slopes/stairs. 0 disables.")]] float stickDistance = 0.5f;
	// Interaction with the simulation.
	[[nuke::prop(label="Push Mass",   min=0.01, tip="How hard the character pushes dynamic bodies.")]] float pushMass = 70.0f;
	[[nuke::prop(label="Max Push Force", min=0)]] float maxPushForce = 100.0f;
	// Motion model.
	[[nuke::prop(label="Auto Gravity", tip="On: SetMove + Jump, gravity is integrated for you.\nOff: SetVelocity is applied verbatim - your own movement code owns gravity/jumps.")]] bool autoGravity = true;
	[[nuke::prop(label="Gravity Scale", tip="Multiplier over the world gravity (Auto Gravity only).")]] float gravityScale = 1.0f;
	[[nuke::prop(label="Inherit Platform", tip="Carried by moving ground (elevators, platforms). Auto Gravity only.")]] bool inheritPlatform = true;

	// ---- gameplay API (scriptable via [[nuke::func]], Lua/C# for free) ----------------
	// Desired HORIZONTAL velocity (world units/s). Vertical motion is the controller's
	// (gravity/jump) under autoGravity; ignored vertical otherwise (use SetVelocity).
	[[nuke::func]] void    SetMove(const Vector3& v);
	// Full desired velocity, applied verbatim (the autoGravity=false driving style).
	[[nuke::func]] void    SetVelocity(const Vector3& v);
	[[nuke::func]] Vector3 Velocity();                    // actual velocity after the last step
	[[nuke::func]] void    Jump(double speed);            // sets the vertical speed if grounded (autoGravity)
	[[nuke::func]] bool    IsGrounded();                  // standing on walkable ground
	[[nuke::func]] int     GroundState();                 // 0 ground / 1 steep / 2 unsupported / 3 air
	[[nuke::func]] Vector3 GroundNormal();
	[[nuke::func]] Vector3 GroundVelocity();              // the ground's own velocity (platforms)
	[[nuke::func]] Atom*   GroundAtom();                  // what the character stands on (null = none)
	[[nuke::func]] void    Teleport(const Vector3& pos);  // instant move (atom position)
	// Size + place the capsule from the sibling MeshRenderer's mesh bounds: pivot=Center,
	// offset = the mesh's local center, height/radius from the AABB. One click/call —
	// the capsule matches the visual, whatever the mesh's own pivot is.
	[[nuke::func]] bool    FitToMesh();                   // false = no sibling mesh

	CharacterController();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	// ---- driver state (World's fixed step owns these) ---------------------------------
	uint64_t charId = 0;          // backend handle (0 = not created yet)
	float    bakedRadius = 0, bakedHalf = 0;   // scale-baked shape the handle was built with
	float    liveSlope = 0, liveStep = 0, liveStick = 0;   // pushed params (change detection)
	Vector3  moveInput;           // SetMove: horizontal desire
	Vector3  rawVelocity;         // SetVelocity: verbatim desire (autoGravity off)
	bool     rawSet = false;      // SetVelocity was called since the last step
	double   verticalVel = 0;     // integrated fall/jump speed (autoGravity)
	double   pendingJump = 0;     // Jump() request consumed by the next step
	bool     pendingTeleport = false;
	Vector3  teleportPos;
	// Post-step snapshot (what the query funcs read; updated under the game lock).
	int      groundState = 3;     // NUKE_GROUND_* (air until the first step)
	Vector3  groundNormal { 0, 1, 0 };
	Vector3  groundVel;
	uint64_t groundBody = 0;      // backend body handle of the ground
	long long groundAtomId = 0;   // stable atom id of the ground (driver-resolved via bodyMap)
	Vector3  actualVel;
};

}  // namespace nuke

#endif // !NUKEE_CHARACTERCONTROLLER_H
