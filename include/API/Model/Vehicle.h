#pragma once
#ifndef NUKEE_VEHICLE_H
#define NUKEE_VEHICLE_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nuke {

// One wheel of a Vehicle, on a CHILD atom of the chassis. The child's local position is the
// suspension attachment point (place it with the gizmo); the simulated wheel pose is written
// back into the child every frame, so a wheel mesh on it spins and steers for free.
class NUKEENGINE_API Wheel : public Component
{
	NUKE_CLASS(Wheel, Component, "Physics")
public:
	[[nuke::prop(label="Radius", min=0.02)]] float radius = 0.3f;
	[[nuke::prop(label="Width", min=0.01)]]  float width = 0.2f;
	[[nuke::prop(label="Suspension Min", min=0)]] float suspensionMin = 0.2f;
	[[nuke::prop(label="Suspension Max", min=0.01)]] float suspensionMax = 0.5f;
	[[nuke::prop(label="Frequency", min=0.1, tip="Suspension spring stiffness, Hz")]] float frequency = 1.5f;
	[[nuke::prop(label="Damping", min=0, max=5)]] float damping = 0.5f;
	[[nuke::prop(label="Max Steer", min=0, max=80, tip="Degrees; 0 = fixed wheel")]] float maxSteerDeg = 0.0f;
	[[nuke::prop(label="Driven", tip="Receives engine torque")]] bool driven = false;
	[[nuke::prop(label="Brake Torque", min=0)]] float maxBrakeTorque = 1500.0f;
	[[nuke::prop(label="Handbrake Torque", min=0, tip="Usually the rear wheels")]] float maxHandBrakeTorque = 0.0f;
	[[nuke::prop(label="Track Side", enum="Auto,Left,Right", tip="Tracked vehicle: which track this road wheel runs in (Auto = by the sign of its X)")]] int trackSide = 0;

	Wheel() : Component("Wheel") {}
	void Init(Atom* parent) override;
	void Destroy() override {}
	void Update() override {}
	TimeDomain timeDomain() const override { return TimeDomain::Physics; }
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
};

// One lift thruster of a hover Vehicle, on a CHILD atom of the chassis: the child's local
// position is where the thruster pushes (place it with the gizmo). A thruster is a spring
// toward Hover Height over whatever is below it; with nothing within reach it lifts nothing.
class NUKEENGINE_API HoverThruster : public Component
{
	NUKE_CLASS(HoverThruster, Component, "Physics")
public:
	[[nuke::prop(label="Hover Height", min=0.05, tip="Rest height over the ground, m")]] float hoverHeight = 1.0f;
	[[nuke::prop(label="Frequency", min=0.1, tip="Lift spring stiffness, Hz")]] float frequency = 2.0f;
	[[nuke::prop(label="Damping", min=0, max=5)]] float damping = 0.7f;

	HoverThruster() : Component("HoverThruster") {}
	void Init(Atom* parent) override;
	void Destroy() override {}
	void Update() override {}
	TimeDomain timeDomain() const override { return TimeDomain::Physics; }
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
};

// Vehicle over this atom's Collider + Rigidbody chassis, built from its CHILDREN at play:
// Wheeled (Wheel children, differentials from the driven wheels), Tracked (Wheel children
// split into a left and a right track; steering slows / reverses a track, pivots in place)
// or Hover (HoverThruster children lift it, thrust / turn / grip forces push the chassis).
// Inputs come from scripts (SetInput) or straight from the input map (Read Input + action
// names; New-menu ships a "Vehicle Input" preset). Wheel slip past Skid Slip broadcasts
// "vehicle.skid" {atom,wheel,slip} — audio/VFX hooks; engine sound reads RPM()/SpeedKmh().
class NUKEENGINE_API Vehicle : public Component
{
	NUKE_CLASS(Vehicle, Component, "Physics")
public:
	[[nuke::prop(label="Type", enum="Wheeled,Tracked,Hover")]] int type = 0;
	[[nuke::prop(label="Max Torque", min=1, tip="Engine torque, Nm (wheeled / tracked)")]] float maxTorque = 500.0f;
	[[nuke::prop(label="Max RPM", min=100)]] float maxRPM = 6000.0f;
	[[nuke::prop(label="Track Inertia", section="Tracks", min=0.1, tip="Moment of inertia of a track with its wheels, kg m^2, seen at the driven wheel")]] float trackInertia = 10.0f;
	[[nuke::prop(label="Track Damping", section="Tracks", min=0, tip="Angular damping of a track, dw/dt = -c w")]] float trackDamping = 0.5f;
	[[nuke::prop(label="Track Brake Torque", section="Tracks", min=0, tip="Nm the brakes apply on each track")]] float trackBrakeTorque = 15000.0f;
	[[nuke::prop(label="Track Diff Ratio", section="Tracks", min=0.1, tip="Gear box to driven wheel ratio")]] float trackDiffRatio = 6.0f;
	[[nuke::prop(label="Track Grip Long", section="Tracks", min=0, tip="Longitudinal friction of the track pads")]] float trackLongFriction = 4.0f;
	[[nuke::prop(label="Track Grip Lat", section="Tracks", min=0, tip="Lateral friction of the track pads")]] float trackLatFriction = 2.0f;
	[[nuke::prop(label="Thrust", section="Hover", min=0, tip="N along the chassis forward at full throttle")]] float hoverThrust = 20000.0f;
	[[nuke::prop(label="Turn Torque", section="Hover", min=0, tip="Nm about the chassis up at full steer")]] float hoverTurn = 8000.0f;
	[[nuke::prop(label="Side Grip", section="Hover", min=0, tip="Lateral velocity damping, 1/s; 0 = skates freely")]] float hoverGrip = 2.0f;
	[[nuke::prop(label="Upright", section="Hover", min=0, tip="Self-righting torque per radian of tilt (x mass)")]] float hoverUpright = 20.0f;
	[[nuke::prop(label="Spin Damping", section="Hover", min=0, tip="Angular velocity damping, 1/s (x mass)")]] float hoverAngularDamping = 2.0f;
	[[nuke::prop(label="Brake Force", section="Hover", min=0, tip="N against the forward velocity at full brake")]] float hoverBrake = 15000.0f;
	[[nuke::prop(label="Read Input", tip="Poll the input actions below every step (off = scripts drive SetInput)")]]
	bool readInput = false;
	[[nuke::prop(label="Throttle Action")]]  std::string throttleAction = "Throttle";
	[[nuke::prop(label="Steer Action")]]     std::string steerAction = "Steer";
	[[nuke::prop(label="Brake Action")]]     std::string brakeAction = "Brake";
	[[nuke::prop(label="Handbrake Action")]] std::string handbrakeAction = "Handbrake";
	[[nuke::prop(label="Skid Slip", min=0.05, max=3, tip="Slip that fires \"vehicle.skid\" (longitudinal ratio / lateral radians)")]]
	float skidSlip = 0.4f;

	Vehicle() : Component("Vehicle") {}
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override {}
	void Reset() override;

	[[nuke::func]] void   SetInput(double forward, double right, double brake, double handBrake);
	[[nuke::func]] bool   Built();
	[[nuke::func]] double RPM();
	[[nuke::func]] double SpeedKmh();                // signed
	[[nuke::func]] double WheelCount();              // wheels, or thrusters of a hover
	[[nuke::func]] bool   WheelContact(double i);
	[[nuke::func]] double WheelSlip(double i);       // max of |longitudinal| / |lateral|

private:
	uint64_t handle = 0;
	std::vector<long> wheelAtoms;    // child atom ids, wheel order
	float inF = 0, inR = 0, inB = 0, inH = 0;
	std::vector<double> skidCool;    // per-wheel event cooldown (game time)
	void Build();
	void Teardown();
	float lastTimeScale = 1.0f;      // local time (TimeVolume): what the chassis was last told
};

}  // namespace nuke

#endif // !NUKEE_VEHICLE_H
