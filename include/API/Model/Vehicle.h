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

	Wheel() : Component("Wheel") {}
	void Init(Atom* parent) override;
	void Destroy() override {}
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
};

// Wheeled vehicle over this atom's Collider + Rigidbody chassis: builds the backend vehicle
// from the Wheel CHILDREN at play. Inputs come from scripts (SetInput) or straight from the
// input map (Read Input + action names; New-menu ships a "Vehicle Input" preset). Wheel slip
// past Skid Slip broadcasts "vehicle.skid" {atom,wheel,slip} — audio/VFX hooks; engine sound
// reads RPM()/SpeedKmh().
class NUKEENGINE_API Vehicle : public Component
{
	NUKE_CLASS(Vehicle, Component, "Physics")
public:
	[[nuke::prop(label="Max Torque", min=1, tip="Engine torque, Nm")]] float maxTorque = 500.0f;
	[[nuke::prop(label="Max RPM", min=100)]] float maxRPM = 6000.0f;
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
	[[nuke::func]] double WheelCount();
	[[nuke::func]] bool   WheelContact(double i);
	[[nuke::func]] double WheelSlip(double i);       // max of |longitudinal| / |lateral|

private:
	uint64_t handle = 0;
	std::vector<long> wheelAtoms;    // child atom ids, wheel order
	float inF = 0, inR = 0, inB = 0, inH = 0;
	std::vector<double> skidCool;    // per-wheel event cooldown (game time)
	void Build();
	void Teardown();
};

}  // namespace nuke

#endif // !NUKEE_VEHICLE_H
