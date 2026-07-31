#include "API/Model/CharacterController.h"
#include "API/Model/Atom.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Mesh.h"
#include "API/Model/World.h"
#include "interface/NUKEEInteface.h"
#include "interface/Services.h"
#include "service/iPhysics.h"

namespace nuke {

CharacterController::CharacterController() : Component("CharacterController") {}

void CharacterController::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void CharacterController::Destroy()
{
	if (charId)
	{
		if (iPhysics* p = GetService<iPhysics>())
			p->destroyCharacter(charId);
		charId = 0;
	}
}

// ---- gameplay API --------

void CharacterController::SetMove(const Vector3& v)
{
	moveInput = v;
	rawSet = false;   // steering via SetMove supersedes a stale raw velocity
}

void CharacterController::SetVelocity(const Vector3& v)
{
	rawVelocity = v;
	rawSet = true;
}

Vector3 CharacterController::Velocity() { return actualVel; }

void CharacterController::Jump(double speed)
{
	// Consumed by the next fixed step; only lifts off walkable ground (autoGravity).
	pendingJump = speed;
}

bool    CharacterController::IsGrounded()     { return groundState == NUKE_GROUND_ON; }
int     CharacterController::GroundState()    { return groundState; }
Vector3 CharacterController::GroundNormal()   { return groundNormal; }
Vector3 CharacterController::GroundVelocity() { return groundVel; }

Atom* CharacterController::GroundAtom()
{
	// Resolve by stable id — safe if the atom died since the last fixed step.
	if (!groundAtomId) return nullptr;
	AppInstance* app = AppInstance::GetSingleton();
	return (app && app->currentWorld) ? app->currentWorld->GetById((long)groundAtomId) : nullptr;
}

void CharacterController::Teleport(const Vector3& pos)
{
	teleportPos = pos;
	pendingTeleport = true;
}

bool CharacterController::FitToMesh()
{
	MeshRenderer* mr = atom ? atom->GetComponent<MeshRenderer>() : nullptr;
	Mesh* mesh = mr ? mr->mesh : nullptr;
	if (!mesh) return false;
	mesh->EnsureBounds();
	const float* mn = mesh->aabbMin;
	const float* mx = mesh->aabbMax;
	// Capsule from LOCAL bounds: pivot=Center plus the bounds center offset lands it on the visual.
	pivot = P_Center;
	capsuleOffset = Vector3((mn[0] + mx[0]) * 0.5, (mn[1] + mx[1]) * 0.5, (mn[2] + mx[2]) * 0.5);
	height = std::max(0.2f, mx[1] - mn[1]);
	const float rx = (mx[0] - mn[0]) * 0.5f, rz = (mx[2] - mn[2]) * 0.5f;
	radius = std::max(0.05f, std::min(std::max(rx, rz), height * 0.5f - 0.01f));
	return true;
}

void CharacterController::Update()      {}
void CharacterController::FixedUpdate() {}
void CharacterController::Pause()       {}
void CharacterController::Reset()
{
	Destroy();   // next fixed step recreates from fresh data
	verticalVel = 0; pendingJump = 0; pendingTeleport = false;
	moveInput = Vector3(); rawVelocity = Vector3(); rawSet = false;
	groundState = NUKE_GROUND_AIR; groundBody = 0;
}

}  // namespace nuke
