#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Atom.h"
#include "API/Model/resdb.h"
#include "API/Model/Jobs.h"
#include "interface/AppInstance.h"
#include "render/irender.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <iostream>

namespace nuke {

SkinnedMeshRenderer::SkinnedMeshRenderer() : MeshRenderer("SkinnedMeshRenderer") {}

void SkinnedMeshRenderer::Init(Atom* parent)
{
	MeshRenderer::Init(parent);   // mesh/material resolve (mesh = the bind-pose ASSET for now)
	EnsureSkeleton();
}

void SkinnedMeshRenderer::Destroy()
{
	ReleaseInstance();
	MeshRenderer::Destroy();
}

void SkinnedMeshRenderer::Reset()
{
	ReleaseInstance();
	skeleton = nullptr;
	pose.clear(); globals.clear();
	MeshRenderer::Reset();
}

Skeleton* SkinnedMeshRenderer::EnsureSkeleton()
{
	if (skeleton) return skeleton;
	ResDB* db = ResDB::getSingleton();
	if (!skelGuid.empty()) skeleton = db->GetSkeleton(skelGuid);
	if (!skeleton)
	{
		// Fall back to the mesh asset's own skeleton reference (the import default).
		Mesh* m = srcMesh ? srcMesh : (!meshGuid.empty() ? db->GetMesh(meshGuid) : mesh);
		if (m && !m->skelGuid.empty()) skeleton = db->GetSkeleton(m->skelGuid);
	}
	if (skeleton && pose.size() != skeleton->bones.size())
	{
		pose.resize(skeleton->bones.size());
		for (size_t i = 0; i < skeleton->bones.size(); ++i)
		{
			const MeshBone& b = skeleton->bones[i];
			memcpy(pose[i].pos,   b.localPos,   sizeof(float) * 3);
			memcpy(pose[i].rot,   b.localRot,   sizeof(float) * 4);
			memcpy(pose[i].scale, b.localScale, sizeof(float) * 3);
		}
	}
	return skeleton;
}

bool SkinnedMeshRenderer::EnsureInstance()
{
	if (skinnedMesh) return true;
	if (!srcMesh)
	{
		// The renderer's CURRENT mesh may already be our instance — resolve the asset by guid.
		srcMesh = !meshGuid.empty() ? ResDB::getSingleton()->GetMesh(meshGuid) : mesh;
	}
	if (!srcMesh || !srcMesh->boneIndex || !srcMesh->boneWeight || srcMesh->numVerts <= 0) return false;
	skinnedMesh = new Mesh();
	strncpy(skinnedMesh->name, srcMesh->name, sizeof(skinnedMesh->name) - 1);
	skinnedMesh->numVerts    = srcMesh->numVerts;
	skinnedMesh->vertexArray = new float[(size_t)srcMesh->numVerts * 3];
	skinnedMesh->normalArray = new float[(size_t)srcMesh->numVerts * 3];
	memcpy(skinnedMesh->vertexArray, srcMesh->vertexArray, sizeof(float) * 3 * srcMesh->numVerts);
	memcpy(skinnedMesh->normalArray, srcMesh->normalArray, sizeof(float) * 3 * srcMesh->numVerts);
	// SHARED with the source (pose never changes them) — ReleaseInstance must null, not free.
	skinnedMesh->uvArray      = srcMesh->uvArray;
	skinnedMesh->indexArray   = srcMesh->indexArray;
	skinnedMesh->numIndices   = srcMesh->numIndices;
	skinnedMesh->tangentArray = srcMesh->tangentArray;
	skinnedMesh->uv2Array     = srcMesh->uv2Array;
	skinnedMesh->colorArray   = srcMesh->colorArray;
	skinnedMesh->sections     = srcMesh->sections;
	skinnedMesh->lods         = srcMesh->lods;
	skinnedMesh->numSlots     = srcMesh->numSlots;
	skinnedMesh->slotNames    = srcMesh->slotNames;
	skinnedMesh->skelGuid     = srcMesh->skelGuid;
	skinnedMesh->rtProxy      = srcMesh;   // RT/BLAS traces the bind-pose source
	return true;
}

void SkinnedMeshRenderer::ReleaseInstance()
{
	if (!skinnedMesh) return;
	if (iRender* r = AppInstance::GetSingleton() ? AppInstance::GetSingleton()->render : nullptr)
		r->invalidateMesh(skinnedMesh);   // Mesh*-keyed caches; addresses get reused
	delete[] skinnedMesh->vertexArray;
	delete[] skinnedMesh->normalArray;
	skinnedMesh->vertexArray = nullptr;
	skinnedMesh->normalArray = nullptr;
	skinnedMesh->uvArray = nullptr;        // shared with the source — not ours to free
	skinnedMesh->indexArray = nullptr;
	skinnedMesh->tangentArray = nullptr;
	skinnedMesh->uv2Array = nullptr;
	skinnedMesh->colorArray = nullptr;
	delete skinnedMesh;
	skinnedMesh = nullptr;
	if (mesh && mesh != srcMesh) mesh = srcMesh;   // never leave the renderer on a dead instance
}

void SkinnedMeshRenderer::PoseBounds(const std::vector<float>& jointPos)
{
	// Conservative posed AABB: bind AABB corners + every joint, inflated by 15% of the
	// bind diagonal (skin extends past the joints by roughly the limb thickness).
	srcMesh->EnsureBounds();
	float mn[3], mx[3];
	memcpy(mn, srcMesh->aabbMin, sizeof(mn));
	memcpy(mx, srcMesh->aabbMax, sizeof(mx));
	for (size_t j = 0; j + 2 < jointPos.size(); j += 3)
		for (int c = 0; c < 3; ++c)
		{
			if (jointPos[j + c] < mn[c]) mn[c] = jointPos[j + c];
			if (jointPos[j + c] > mx[c]) mx[c] = jointPos[j + c];
		}
	const float dx = srcMesh->aabbMax[0] - srcMesh->aabbMin[0];
	const float dy = srcMesh->aabbMax[1] - srcMesh->aabbMin[1];
	const float dz = srcMesh->aabbMax[2] - srcMesh->aabbMin[2];
	const float pad = 0.15f * std::sqrt(dx * dx + dy * dy + dz * dz);
	for (int c = 0; c < 3; ++c) { mn[c] -= pad; mx[c] += pad; }
	memcpy(skinnedMesh->aabbMin, mn, sizeof(mn));
	memcpy(skinnedMesh->aabbMax, mx, sizeof(mx));
	skinnedMesh->boundsValid = true;
}

void SkinnedMeshRenderer::ApplyPose()
{
	Skeleton* sk = EnsureSkeleton();
	if (!sk || sk->bones.empty() || pose.size() != sk->bones.size()) return;
	if (!EnsureInstance()) return;
	if (morphWeights.size() != srcMesh->morphs.size()) morphWeights.resize(srcMesh->morphs.size(), 0.0f);

	const size_t nb = sk->bones.size();
	std::vector<glm::mat4> global(nb), palette(nb);
	for (size_t i = 0; i < nb; ++i)
	{
		glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(pose[i].pos[0], pose[i].pos[1], pose[i].pos[2]))
		                * glm::mat4_cast(glm::quat(pose[i].rot[3], pose[i].rot[0], pose[i].rot[1], pose[i].rot[2]))
		                * glm::scale(glm::mat4(1.0f), glm::vec3(pose[i].scale[0], pose[i].scale[1], pose[i].scale[2]));
		const int par = sk->bones[i].parent;
		global[i] = (par >= 0) ? global[par] * local : local;
	}
	globals.resize(nb * 16);
	std::vector<float> jointPos(nb * 3);
	for (size_t i = 0; i < nb; ++i)
	{
		memcpy(globals.data() + i * 16, glm::value_ptr(global[i]), sizeof(float) * 16);
		palette[i] = global[i] * glm::make_mat4(sk->bones[i].invBind);
		jointPos[i * 3 + 0] = global[i][3][0];
		jointPos[i * 3 + 1] = global[i][3][1];
		jointPos[i * 3 + 2] = global[i][3][2];
	}

	// --- GPU path: the renderer skins in a compute pre-pass (palette + morph weights per
	// frame); vertices never touch the CPU and prev-positions feed TAA motion vectors.
	iRender* r = AppInstance::GetSingleton() ? AppInstance::GetSingleton()->render : nullptr;
	if (r && r->gpuSkin())
	{
		skinnedMesh->rtProxy = nullptr;   // RT traces the ANIMATED positions (renderer refits)
		r->setSkinPalette(skinnedMesh, srcMesh, palette.empty() ? nullptr : glm::value_ptr(palette[0]), (int)nb,
		                  morphWeights.empty() ? nullptr : morphWeights.data(), (int)morphWeights.size());
		PoseBounds(jointPos);
		mesh = skinnedMesh;
		return;
	}

	// --- CPU fallback (headless/legacy renderers): morphs into scratch bind streams + LBS.
	const int n = srcMesh->numVerts;
	const float* sv = srcMesh->vertexArray;
	const float* sn = srcMesh->normalArray;
	bool anyMorph = false;
	for (float w : morphWeights) if (w != 0.0f) { anyMorph = true; break; }
	if (anyMorph)
	{
		morphScratchP.assign(sv, sv + (size_t)n * 3);
		morphScratchN.assign(sn, sn + (size_t)n * 3);
		for (size_t t = 0; t < srcMesh->morphs.size(); ++t)
		{
			const float w = t < morphWeights.size() ? morphWeights[t] : 0.0f;
			if (w == 0.0f) continue;
			const Mesh::MorphTarget& mt = srcMesh->morphs[t];
			if (mt.posDelta.size() == (size_t)n * 3)
				for (int v = 0; v < n * 3; ++v) morphScratchP[v] += w * mt.posDelta[v];
			if (mt.nrmDelta.size() == (size_t)n * 3)
				for (int v = 0; v < n * 3; ++v) morphScratchN[v] += w * mt.nrmDelta[v];
		}
		sv = morphScratchP.data();
		sn = morphScratchN.data();
	}
	const unsigned short* bi = srcMesh->boneIndex;
	const float* bw = srcMesh->boneWeight;
	float* dv = skinnedMesh->vertexArray;
	float* dn = skinnedMesh->normalArray;
	const glm::mat4* pal = palette.data();
	const int palCount = (int)palette.size();
	Jobs::ParallelFor(0, n, 4096, [sv, sn, bi, bw, dv, dn, pal, palCount](int v)
	{
		const glm::vec4 sp(sv[v * 3 + 0], sv[v * 3 + 1], sv[v * 3 + 2], 1.0f);
		const glm::vec4 np(sn[v * 3 + 0], sn[v * 3 + 1], sn[v * 3 + 2], 0.0f);
		glm::vec3 p(0.0f), nn(0.0f);
		float wsum = 0.0f;
		for (int k = 0; k < 4; ++k)
		{
			const float w = bw[v * 4 + k];
			if (w <= 0.0f) continue;
			const int b = bi[v * 4 + k];
			if (b >= palCount) continue;
			p  += w * glm::vec3(pal[b] * sp);
			nn += w * glm::vec3(pal[b] * np);
			wsum += w;
		}
		if (wsum <= 0.0f) return;   // unweighted vertex (merged rigid part) stays at bind
		const float len = glm::length(nn);
		if (len > 1e-6f) nn /= len;
		dv[v * 3 + 0] = p.x;  dv[v * 3 + 1] = p.y;  dv[v * 3 + 2] = p.z;
		dn[v * 3 + 0] = nn.x; dn[v * 3 + 1] = nn.y; dn[v * 3 + 2] = nn.z;
	});

	++skinnedMesh->version;              // renderer re-uploads its cached buffers
	skinnedMesh->boundsValid = false;    // culling AABB follows the pose
	mesh = skinnedMesh;                  // this renderer draws the posed instance
}

bool SkinnedMeshRenderer::BoneGlobal(const std::string& bone, float out[16])
{
	Skeleton* sk = EnsureSkeleton();
	if (!sk) return false;
	const int i = sk->BoneIndex(bone);
	if (i < 0) return false;
	if (globals.size() == sk->bones.size() * 16)
	{
		memcpy(out, globals.data() + (size_t)i * 16, sizeof(float) * 16);
		return true;
	}
	// No ApplyPose yet: compute the BIND global on the fly.
	glm::mat4 g(1.0f);
	std::vector<glm::mat4> chain;
	for (int j = i; j >= 0; j = sk->bones[j].parent)
	{
		const MeshBone& b = sk->bones[j];
		chain.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(b.localPos[0], b.localPos[1], b.localPos[2]))
		              * glm::mat4_cast(glm::quat(b.localRot[3], b.localRot[0], b.localRot[1], b.localRot[2]))
		              * glm::scale(glm::mat4(1.0f), glm::vec3(b.localScale[0], b.localScale[1], b.localScale[2])));
	}
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) g = g * (*it);
	memcpy(out, glm::value_ptr(g), sizeof(float) * 16);
	return true;
}

// Compose a model-space matrix with the atom's world transform into world pos + rot.
static void ModelToWorld(Transform* t, const glm::mat4& model, Vector3& outPos, Quaternion& outRot)
{
	Vector3 gp = t->globalPosition();
	Quaternion gq = t->globalRotation();
	Vector3 gs = t->globalScale();
	glm::mat4 atomW = glm::translate(glm::mat4(1.0f), glm::vec3((float)gp.x, (float)gp.y, (float)gp.z))
	                * glm::mat4_cast(glm::quat((float)gq.w, (float)gq.x, (float)gq.y, (float)gq.z))
	                * glm::scale(glm::mat4(1.0f), glm::vec3((float)gs.x, (float)gs.y, (float)gs.z));
	glm::mat4 w = atomW * model;
	glm::vec3 p(w[3]);
	glm::quat q = glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(w[0])), glm::normalize(glm::vec3(w[1])), glm::normalize(glm::vec3(w[2]))));
	outPos = Vector3(p.x, p.y, p.z);
	outRot = Quaternion(q.x, q.y, q.z, q.w);
}

bool SkinnedMeshRenderer::BoneWorld(const std::string& bone, Vector3& outPos, Quaternion& outRot)
{
	float g[16];
	if (!BoneGlobal(bone, g) || !transform) return false;
	ModelToWorld(transform, glm::make_mat4(g), outPos, outRot);
	return true;
}

bool SkinnedMeshRenderer::SocketWorld(const std::string& socketName, Vector3& outPos, Quaternion& outRot)
{
	Skeleton* sk = EnsureSkeleton();
	if (!sk || !transform) return false;
	const SkeletonSocket* s = sk->Socket(socketName);
	// A bare BONE name works as an implicit identity socket.
	const std::string& boneName = s ? s->bone : socketName;
	float g[16];
	if (!BoneGlobal(boneName, g)) return false;
	glm::mat4 model = glm::make_mat4(g);
	if (s)
		model = model
		      * glm::translate(glm::mat4(1.0f), glm::vec3(s->localPos[0], s->localPos[1], s->localPos[2]))
		      * glm::mat4_cast(glm::quat(s->localRot[3], s->localRot[0], s->localRot[1], s->localRot[2]))
		      * glm::scale(glm::mat4(1.0f), glm::vec3(s->localScale[0], s->localScale[1], s->localScale[2]));
	ModelToWorld(transform, model, outPos, outRot);
	return true;
}

// --- script surface --------------------------------------------------------------------------

void SkinnedMeshRenderer::ResetPose()
{
	pose.clear();
	if (Skeleton* sk = EnsureSkeleton()) { (void)sk; }   // EnsureSkeleton refills from bind
}

void SkinnedMeshRenderer::SetMorphWeight(const std::string& morph, double w)
{
	if (!EnsureInstance()) return;
	const int i = srcMesh->MorphIndex(morph);
	if (i < 0) return;
	if (morphWeights.size() != srcMesh->morphs.size()) morphWeights.resize(srcMesh->morphs.size(), 0.0f);
	morphWeights[i] = (float)w;
}

double SkinnedMeshRenderer::MorphWeight(const std::string& morph)
{
	if (!EnsureInstance()) return 0.0;
	const int i = srcMesh->MorphIndex(morph);
	return (i >= 0 && i < (int)morphWeights.size()) ? morphWeights[i] : 0.0;
}

std::string SkinnedMeshRenderer::MorphNames()
{
	if (!EnsureInstance()) return "";
	std::string out;
	for (const Mesh::MorphTarget& mt : srcMesh->morphs)
	{
		if (!out.empty()) out += ';';
		out += mt.name;
	}
	return out;
}

void SkinnedMeshRenderer::SetBonePosition(const std::string& bone, const Vector3& p)
{
	Skeleton* sk = EnsureSkeleton();
	int i = sk ? sk->BoneIndex(bone) : -1;
	if (i < 0 || i >= (int)pose.size()) return;
	pose[i].pos[0] = (float)p.x; pose[i].pos[1] = (float)p.y; pose[i].pos[2] = (float)p.z;
}

void SkinnedMeshRenderer::SetBoneRotation(const std::string& bone, const Quaternion& q)
{
	Skeleton* sk = EnsureSkeleton();
	int i = sk ? sk->BoneIndex(bone) : -1;
	if (i < 0 || i >= (int)pose.size()) return;
	pose[i].rot[0] = (float)q.x; pose[i].rot[1] = (float)q.y; pose[i].rot[2] = (float)q.z; pose[i].rot[3] = (float)q.w;
}

void SkinnedMeshRenderer::Apply() { ApplyPose(); }

Vector3 SkinnedMeshRenderer::BonePosition(const std::string& bone)
{
	Vector3 p(0, 0, 0); Quaternion r;
	BoneWorld(bone, p, r);
	return p;
}

Vector3 SkinnedMeshRenderer::SocketPosition(const std::string& socketName)
{
	Vector3 p(0, 0, 0); Quaternion r;
	SocketWorld(socketName, p, r);
	return p;
}

Quaternion SkinnedMeshRenderer::SocketRotation(const std::string& socketName)
{
	Vector3 p; Quaternion r(0, 0, 0, 1);
	SocketWorld(socketName, p, r);
	return r;
}

// =============================================================================================
// SocketAttachment
// =============================================================================================

SocketAttachment::SocketAttachment() : Component("SocketAttachment") {}
void SocketAttachment::Init(Atom* parent)
{
	atom = parent; transform = &parent->GetTransform();
	parent->components.push_back(this);
}
void SocketAttachment::Destroy() {}
void SocketAttachment::FixedUpdate() {}
void SocketAttachment::Pause() {}
void SocketAttachment::Reset() {}

void SocketAttachment::Update()
{
	if (socket.empty() || !atom || !transform) return;
	// Closest ancestor with a SkinnedMeshRenderer owns the skeleton we ride. Parent atoms
	// update before children, so the pose this frame is already applied.
	SkinnedMeshRenderer* smr = nullptr;
	for (Atom* a = atom->parent; a && !smr; a = a->parent)
		smr = a->GetComponent<SkinnedMeshRenderer>();
	if (!smr) return;
	Vector3 p; Quaternion r;
	if (smr->SocketWorld(socket, p, r))
		transform->SetGlobal(p, r, transform->globalScale());
}

}  // namespace nuke
