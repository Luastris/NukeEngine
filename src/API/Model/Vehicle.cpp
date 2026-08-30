// Wheeled vehicle components: thin drivers over the iPhysics vehicle seam (see Vehicle.h).
#include "API/Model/Physics.h"
#include "API/Model/Vehicle.h"
#include "API/Model/Atom.h"
#include "API/Model/Collider.h"
#include "API/Model/Events.h"
#include "API/Model/Game.h"
#include "API/Model/Time.h"
#include "API/Model/World.h"
#include "input/Input.h"
#include "interface/Services.h"
#include "service/iPhysics.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace nuke {

void Wheel::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void Vehicle::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void Vehicle::Destroy() { Teardown(); }
void Vehicle::Reset()   { Teardown(); }

void Vehicle::Teardown()
{
	if (handle)
		if (iPhysics* ph = Physics::Scene()) ph->destroyVehicle(handle);
	handle = 0;
	wheelAtoms.clear();
	skidCool.clear();
	inF = inR = inB = inH = 0;
}

void Vehicle::Build()
{
	iPhysics* ph = Physics::Scene();
	Collider* chassis = atom->GetComponent<Collider>();
	if (!ph || !chassis || !chassis->bodyId) return;
	std::vector<NukeWheelDesc> descs;
	wheelAtoms.clear();
	for (Atom* c : atom->children)
	{
		Wheel* w = c ? c->GetComponent<Wheel>() : nullptr;
		if (!w) continue;
		NukeWheelDesc d;
		Vector3 lp = c->GetTransform().position;
		d.pos[0] = (float)lp.x; d.pos[1] = (float)lp.y; d.pos[2] = (float)lp.z;
		d.radius = w->radius;
		d.width = w->width;
		d.suspensionMin = w->suspensionMin;
		d.suspensionMax = w->suspensionMax;
		d.frequency = w->frequency;
		d.damping = w->damping;
		d.maxSteerDeg = w->maxSteerDeg;
		d.driven = w->driven;
		d.maxBrakeTorque = w->maxBrakeTorque;
		d.maxHandBrakeTorque = w->maxHandBrakeTorque;
		descs.push_back(d);
		wheelAtoms.push_back(c->id.id);
	}
	if (descs.empty()) return;
	NukeVehicleDesc vd;
	vd.chassis = chassis->bodyId;
	vd.wheels = descs.data();
	vd.wheelCount = (int)descs.size();
	vd.maxTorque = maxTorque;
	vd.maxRPM = maxRPM;
	handle = ph->createVehicle(vd);
	skidCool.assign(descs.size(), 0.0);
}

void Vehicle::FixedUpdate()
{
	if (!atom || !Game::IsPlaying()) return;
	if (!handle) { Build(); if (!handle) return; }
	iPhysics* ph = Physics::Scene();
	if (!ph) return;
	if (readInput)
	{
		inF = Input::Value(throttleAction);
		inR = Input::Value(steerAction);
		inB = Input::Value(brakeAction);
		inH = Input::Value(handbrakeAction);
	}
	ph->setVehicleInput(handle, inF, inR, inB, inH);
	// Skid hooks, throttled per wheel so audio/VFX get onsets, not a firehose.
	const double now = Time::getSingleton()->elapsed;
	for (int i = 0; i < (int)wheelAtoms.size(); ++i)
	{
		NukeWheelState ws;
		if (!ph->getWheelState(handle, i, ws) || !ws.contact) continue;
		const float slip = std::max(std::fabs(ws.longSlip), std::fabs(ws.latSlip));
		if (slip < skidSlip || now < skidCool[i]) continue;
		skidCool[i] = now + 0.25;
		nlohmann::json j;
		j["atom"] = (double)atom->id.id;
		j["wheel"] = (double)i;
		j["slip"] = (double)slip;
		Events::Emit("vehicle.skid", j.dump());
	}
}

// Wheel pose write-back at render cadence: the child atoms carry the visual wheels.
void Vehicle::Update()
{
	if (!handle || !atom) return;
	iPhysics* ph = Physics::Scene();
	World* w = Game::GetWorld();
	if (!ph || !w) return;
	for (int i = 0; i < (int)wheelAtoms.size(); ++i)
	{
		Atom* wa = w->GetById(wheelAtoms[i]);
		NukeWheelState ws;
		if (!wa || !ph->getWheelState(handle, i, ws)) continue;
		Transform& t = wa->GetTransform();
		t.SetGlobal(Vector3(ws.pos[0], ws.pos[1], ws.pos[2]),
		            Quaternion(ws.quat[0], ws.quat[1], ws.quat[2], ws.quat[3]),
		            t.globalScale());
	}
}

void Vehicle::SetInput(double forward, double right, double brake, double handBrake)
{
	inF = (float)forward; inR = (float)right; inB = (float)brake; inH = (float)handBrake;
}

bool   Vehicle::Built()      { return handle != 0; }
double Vehicle::WheelCount() { return (double)wheelAtoms.size(); }

double Vehicle::RPM()
{
	iPhysics* ph = Physics::Scene();
	return ph && handle ? (double)ph->vehicleRPM(handle) : 0.0;
}

double Vehicle::SpeedKmh()
{
	iPhysics* ph = Physics::Scene();
	return ph && handle ? (double)ph->vehicleSpeed(handle) * 3.6 : 0.0;
}

bool Vehicle::WheelContact(double i)
{
	iPhysics* ph = Physics::Scene();
	NukeWheelState ws;
	return ph && handle && ph->getWheelState(handle, (int)i, ws) && ws.contact != 0;
}

double Vehicle::WheelSlip(double i)
{
	iPhysics* ph = Physics::Scene();
	NukeWheelState ws;
	if (!ph || !handle || !ph->getWheelState(handle, (int)i, ws)) return 0.0;
	return std::max(std::fabs(ws.longSlip), std::fabs(ws.latSlip));
}

}  // namespace nuke
