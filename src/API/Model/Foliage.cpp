#include "API/Model/Foliage.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Atom.h"
#include "API/Model/Noise.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <algorithm>
#include <cmath>

namespace nuke {

// Deterministic local rng, kept separate from the engine's global seeded streams.
struct FolRng
{
	unsigned s;
	float Next() { s = s * 1664525u + 1013904223u; return (float)(s >> 8) * (1.0f / 16777216.0f); }
};

static glm::mat4 AtomWorldM(Atom* a)
{
	Transform& t = a->GetTransform();
	Vector3 P = t.globalPosition(); Quaternion Q = t.globalRotation(); Vector3 S = t.globalScale();
	return glm::translate(glm::mat4(1.0f), glm::vec3((float)P.x, (float)P.y, (float)P.z))
	     * glm::mat4_cast(glm::quat((float)Q.w, (float)Q.x, (float)Q.y, (float)Q.z))
	     * glm::scale(glm::mat4(1.0f), glm::vec3((float)S.x, (float)S.y, (float)S.z));
}

// The foliage layer's frame: position + rotation, no scale. Must mirror the render
// composition (ScaleWithAtom() == false) or painted points and drawn instances drift.
static glm::mat4 AtomTRM(Atom* a)
{
	Transform& t = a->GetTransform();
	Vector3 P = t.globalPosition(); Quaternion Q = t.globalRotation();
	return glm::translate(glm::mat4(1.0f), glm::vec3((float)P.x, (float)P.y, (float)P.z))
	     * glm::mat4_cast(glm::quat((float)Q.w, (float)Q.x, (float)Q.y, (float)Q.z));
}

// Shortest-arc rotation taking (0,1,0) onto `axis` (unit).
static glm::quat UpTo(const glm::vec3& axis)
{
	const glm::vec3 up(0, 1, 0);
	float d = glm::dot(up, axis);
	if (d > 0.9999f) return glm::quat(1, 0, 0, 0);
	if (d < -0.9999f) return glm::angleAxis(3.14159265f, glm::vec3(1, 0, 0));
	glm::vec3 c = glm::cross(up, axis);
	glm::quat q(1.0f + d, c.x, c.y, c.z);
	return glm::normalize(q);
}

Foliage::Foliage()
{
	name = (char*)"Foliage";
	meshGuid = "builtin:grassclump";
	matGuid  = "builtin:grass";
	// Foliage joins the RT TLAS by default; dense layers are stride-subsampled above the cap.
	inReflections  = true;
	rtMaxInstances = 8192;
}

Atom* Foliage::SurfaceRoot() { return surface ? surface : atom; }

float Foliage::MeshHeight()
{
	if (!mesh) return -1.0f;
	mesh->EnsureBounds();
	if (!mesh->boundsValid) return 1.0f;
	return std::max(0.05f, mesh->aabbMax[1] - mesh->aabbMin[1]);
}

// Write the bend coefficients into every instance's custom.zw — the vertex shader reads
// z = wind, w = interaction, both normalized by mesh height².
void Foliage::SyncBendParams(bool force)
{
	if (!mesh || instances.empty()) return;
	const float h = MeshHeight();
	if (!force && windBend == windBendRes && interactionBend == interBendRes && h == meshHeightRes) return;
	windBendRes = windBend; interBendRes = interactionBend; meshHeightRes = h;
	const float inv2 = 1.0f / (h * h);
	const float wz = windBend * inv2, ww = interactionBend * inv2;
	for (Inst& in : instances) { in.custom[2] = wz; in.custom[3] = ww; }
	MarkDirty();
}

bool Foliage::EnsureRenderReady(iRender* r)
{
	EnsureDecoded();
	SyncBendParams(false);   // mesh resolves inside the base call; a swap syncs next frame
	return InstancedMesh::EnsureRenderReady(r);
}

// ---- scatter ------------------------------------------------------------------------------

void Foliage::Scatter(const Vector3& brushPos, float brushR, float densMul)
{
	Atom* root = SurfaceRoot();
	if (!root || !atom || density <= 0.0f || densMul <= 0.0f) return;

	const glm::mat4 aw = AtomTRM(atom);   // layer frame: T*R only (render ignores atom scale)
	const glm::mat4 inv = glm::inverse(aw);
	Quaternion AQ = atom->GetTransform().globalRotation();
	const glm::quat invARot = glm::inverse(glm::quat((float)AQ.w, (float)AQ.x, (float)AQ.y, (float)AQ.z));

	// Brush strokes hash in the position so strokes differ; Fill stays deterministic by `seed`.
	unsigned s0 = (unsigned)seed * 2654435761u + 12345u;
	if (brushR > 0.0f)
		s0 ^= (unsigned)(brushPos.x * 73856093.0) ^ (unsigned)(brushPos.y * 19349663.0) ^ (unsigned)(brushPos.z * 83492791.0);
	FolRng rng{ s0 };

	const glm::vec3 bp((float)brushPos.x, (float)brushPos.y, (float)brushPos.z);
	const float brushR2 = brushR * brushR;
	const float slopeCosMin = cosf(glm::radians(std::min(std::max(maxSlope, 0.0f), 90.0f)));
	const float sMinV = std::min(scaleMin, scaleMax), sMaxV = std::max(scaleMin, scaleMax);

	std::vector<Atom*> stack{ root };
	while (!stack.empty())
	{
		Atom* a = stack.back(); stack.pop_back();
		if (!a) continue;
		for (Atom* c : a->children) stack.push_back(c);
		MeshRenderer* mr = a->GetComponent<MeshRenderer>();
		if (!mr || !mr->enabled || !mr->mesh || !mr->mesh->vertexArray || mr->mesh->numVerts < 3) continue;
		const glm::mat4 w = AtomWorldM(a);
		const Mesh* m = mr->mesh;
		const int tris = m->numVerts / 3;
		for (int t = 0; t < tris; ++t)
		{
			const float* vp = m->vertexArray + (size_t)t * 9;
			glm::vec3 A = glm::vec3(w * glm::vec4(vp[0], vp[1], vp[2], 1));
			glm::vec3 B = glm::vec3(w * glm::vec4(vp[3], vp[4], vp[5], 1));
			glm::vec3 C = glm::vec3(w * glm::vec4(vp[6], vp[7], vp[8], 1));
			glm::vec3 n = glm::cross(B - A, C - A);
			const float n2 = glm::length(n);
			if (n2 < 1e-9f) continue;
			const float area = 0.5f * n2;
			n /= n2;
			if (n.y < 0.0f) n = -n;               // ground is ground from either winding
			if (n.y < slopeCosMin) continue;      // slope mask (per triangle)
			if (brushR > 0.0f)
			{
				// cheap pre-cull: brush sphere vs triangle AABB
				glm::vec3 mn = glm::min(A, glm::min(B, C)) - brushR;
				glm::vec3 mx = glm::max(A, glm::max(B, C)) + brushR;
				if (bp.x < mn.x || bp.x > mx.x || bp.y < mn.y || bp.y > mx.y || bp.z < mn.z || bp.z > mx.z) continue;
			}
			const float expect = area * density * densMul;
			int cnt = (int)expect;
			if (rng.Next() < expect - (float)cnt) ++cnt;
			for (int k = 0; k < cnt; ++k)
			{
				float u = rng.Next(), v = rng.Next();
				if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
				glm::vec3 P = A + (B - A) * u + (C - A) * v;
				if (brushR > 0.0f)
				{
					glm::vec3 d = P - bp;
					if (glm::dot(d, d) > brushR2) continue;
				}
				if (P.y < heightMin || P.y > heightMax) continue;   // height band
				if (noiseCover < 1.0f)                              // patchiness
				{
					const double ns = noiseScale > 0.1f ? noiseScale : 0.1f;
					const float nz = 0.5f + 0.5f * (float)Noise::Perlin2((double)seed, P.x / ns, P.z / ns);
					if (nz > noiseCover) continue;
				}
				glm::vec3 axis = glm::normalize(glm::mix(glm::vec3(0, 1, 0), n, std::min(std::max(alignToNormal, 0.0f), 1.0f)));
				glm::quat q = UpTo(axis);
				if (randomYaw) q = q * glm::angleAxis(rng.Next() * 6.2831853f, glm::vec3(0, 1, 0));
				P += axis * surfaceOffset;
				const float sc = sMinV + rng.Next() * (sMaxV - sMinV);

				const glm::vec3 lp = glm::vec3(inv * glm::vec4(P, 1));
				const glm::quat lq = invARot * q;
				Inst in{};
				in.pos[0] = lp.x; in.pos[1] = lp.y; in.pos[2] = lp.z;
				in.quat[0] = lq.x; in.quat[1] = lq.y; in.quat[2] = lq.z; in.quat[3] = lq.w;
				in.scale[0] = in.scale[1] = in.scale[2] = sc;
				in.color[0] = in.color[1] = in.color[2] = in.color[3] = 1.0f;
				instances.push_back(in);
			}
		}
	}
}

void Foliage::Rebuild()
{
	EnsureDecoded();
	instances.clear();
	data.clear();
	Scatter(Vector3(0, 0, 0), -1.0f, 1.0f);
	MarkDirty();
	SyncBendParams(true);
}

void Foliage::PaintAt(const Vector3& worldPos, double radius, double densityMul)
{
	EnsureDecoded();
	Scatter(worldPos, (float)(radius > 0.05 ? radius : 0.05), (float)(densityMul > 0.0 ? densityMul : 1.0));
	MarkDirty();
	SyncBendParams(true);
}

void Foliage::EraseAt(const Vector3& worldPos, double radius)
{
	EnsureDecoded();
	if (instances.empty()) return;
	const glm::mat4 aw = AtomTRM(atom);   // must mirror the render frame (no atom scale)
	const glm::vec3 bp((float)worldPos.x, (float)worldPos.y, (float)worldPos.z);
	const float r2 = (float)(radius * radius);
	instances.erase(std::remove_if(instances.begin(), instances.end(), [&](const Inst& in)
	{
		glm::vec3 wp = glm::vec3(aw * glm::vec4(in.pos[0], in.pos[1], in.pos[2], 1));
		glm::vec3 d = wp - bp;
		return glm::dot(d, d) <= r2;
	}), instances.end());
	MarkDirty();
}

}  // namespace nuke
