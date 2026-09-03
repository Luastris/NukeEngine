#include "API/Model/PoseClone.h"
#include "API/Model/Atom.h"
#include "API/Model/BoneMap.h"
#include "API/Model/resdb.h"
#include "API/Model/Transform.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>
#include <iostream>

namespace nuke {

PoseClone::~PoseClone()
{
	if (hidden) Atom::SetRuntimeHidden(hidden, false);   // key only: the source may already be gone
}

void PoseClone::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void PoseClone::Destroy()
{
	if (hidden) { Atom::SetRuntimeHidden(hidden, false); hidden = nullptr; }
	bind.reset();
}

void PoseClone::Rebind() { bind.reset(); }

Animator* PoseClone::Host()
{
	Animator* an = atom ? atom->GetComponent<Animator>() : nullptr;
	if (!an && !warned)
	{
		warned = true;
		std::cout << "[PoseClone]\tWARNING: '" << (atom ? atom->name : std::string("?"))
		          << "' has no Animator — the pose-cloning API lives there; add one (it may stay disabled)" << std::endl;
	}
	return an;
}

bool PoseClone::EnsureBind()
{
	if (!atom || !source) return false;
	Animator* an = Host();
	if (!an) return false;
	if (bind && Animator::PoseBindValid(*bind, atom, source, poseJson)) return true;
	const BoneMap* renames = boneMapGuid.empty() ? nullptr : ResDB::getSingleton()->GetBoneMap(boneMapGuid);
	bind = an->PoseBindTo(source, renames, poseJson);
	if (!bind)
	{
		if (!warned)
		{
			warned = true;
			std::cout << "[PoseClone]\tWARNING: '" << atom->name << "' cannot clone from '" << source->name
			          << "': no rig on a side, a cycle (self / an ancestor), or no bone pairs" << std::endl;
		}
		return false;
	}
	warned = false;
	if (autoAlign && poseJson.empty()) AutoAlign();
	return true;
}

void PoseClone::AutoAlign()
{
	if (!bind && !EnsureBind()) return;
	poseJson = Animator::PoseBindAlign(*bind);
	std::cout << "[PoseClone]\t'" << atom->name << "': auto-aligned " << Animator::PoseBindPaired(*bind)
	          << " paired bones from '" << source->name << "'" << std::endl;
}

void PoseClone::SetBoneOffset(const std::string& bone, const Vector3& eulerDeg)
{
	if (!bind && !EnsureBind()) return;
	poseJson = Animator::PoseOffsetSet(*bind, poseJson, bone, eulerDeg);
	bind.reset();
}

Vector3 PoseClone::BoneOffset(const std::string& bone)
{
	if (!bind && !EnsureBind()) return Vector3(0, 0, 0);
	return Animator::PoseOffsetGet(*bind, poseJson, bone);
}

void PoseClone::ClearOffsets()
{
	poseJson.clear();
	bind.reset();
}

double PoseClone::PairedCount() { return (bind || EnsureBind()) ? (double)Animator::PoseBindPaired(*bind) : 0.0; }

std::string PoseClone::PairOf(const std::string& targetBone)
{
	if (!bind && !EnsureBind()) return std::string();
	return Animator::PoseBindPairOf(*bind, targetBone);
}

// The source's Animator extracts root motion by MOVING the source atom (its local transform).
// Take that travel since the last frame, apply it to THIS atom in world space, and put the
// source back on its pin — the character walks, the hidden rig never wanders off.
void PoseClone::ApplyRootMotion()
{
	if (!rootMotion || !source || !atom) { pinAtom = nullptr; return; }
	Transform& st = source->GetTransform();
	if (pinAtom != source)
	{
		pinAtom = source; pinPos = st.position; pinRot = st.rotation;
		return;
	}
	// Horizontal travel + turn, like the Animator's own extraction: no vertical drift.
	const glm::vec3 dp((float)(st.position.x - pinPos.x), 0.0f, (float)(st.position.z - pinPos.z));
	const glm::quat now((float)st.rotation.w, (float)st.rotation.x, (float)st.rotation.y, (float)st.rotation.z);
	const glm::quat pin((float)pinRot.w, (float)pinRot.x, (float)pinRot.y, (float)pinRot.z);
	const glm::quat dq = glm::normalize(now * glm::inverse(pin));
	const bool moved = glm::dot(dp, dp) > 1e-12f || std::fabs(dq.w) < 0.9999999f;
	if (moved)
	{
		// the source's local delta lives in its PARENT's frame: conjugate into the world
		glm::quat P(1, 0, 0, 0);
		if (source->parent)
		{
			const Quaternion pr = source->parent->GetTransform().globalRotation();
			P = glm::quat((float)pr.w, (float)pr.x, (float)pr.y, (float)pr.z);
		}
		const glm::vec3 wdp = P * dp;
		const glm::quat wdq = glm::normalize(P * dq * glm::inverse(P));
		Transform& tt = atom->GetTransform();
		const Vector3 gp = tt.globalPosition();
		const Quaternion gr = tt.globalRotation();
		const glm::quat ng = glm::normalize(wdq * glm::quat((float)gr.w, (float)gr.x, (float)gr.y, (float)gr.z));
		tt.SetGlobal(Vector3(gp.x + wdp.x, gp.y + wdp.y, gp.z + wdp.z), Quaternion(ng.x, ng.y, ng.z, ng.w), tt.globalScale());
		st.position = pinPos;   // re-pin: the travel now belongs to this atom
		st.rotation = pinRot;
	}
}

void PoseClone::LateUpdate()
{
	// Hide Source follows the current source and flag (runtime-only, registry on the Atom).
	Atom* want = (source && hideSource) ? source : nullptr;
	if (hidden != want)
	{
		if (hidden) Atom::SetRuntimeHidden(hidden, false);
		if (want)   Atom::SetRuntimeHidden(want, true);
		hidden = want;
	}
	if (!source || !EnsureBind()) return;
	ApplyRootMotion();
	Host()->PoseBindApply(*bind, source, translation, weight);
}

}  // namespace nuke
