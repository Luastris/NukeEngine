// C3 soft-body cloth (see Cloth.h): weld the render mesh into a sim sheet, simulate it in
// the scene's physics, write the result back into a private render instance every step.
// Header-only boost.chrono must come BEFORE any boost include: the lib flavor double-defines
// steady_clock::now inside the engine DLL.
#define BOOST_CHRONO_HEADER_ONLY
#include <boost/chrono.hpp>
#include "API/Model/Cloth.h"
#include "API/Model/Atom.h"
#include "API/Model/Mesh.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Skeleton.h"
#include "API/Model/Ragdoll.h"
#include "API/Model/Physics.h"
#include "API/Model/Wind.h"
#include "API/Model/resdb.h"
#include "service/iPhysics.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <tuple>

namespace nuke {

// Live components, for the post-traversal render pass (game thread, under the game lock).
void Cloth::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void Cloth::Destroy()
{
	ReleaseBody();
}

Cloth::~Cloth()
{
}

void Cloth::LateUpdate()
{
	Update();
}

void Cloth::Rebuild()
{
	ReleaseBody();
	m_failed = false;
}

double Cloth::SimVertexCount() { return (double)m_numSim; }
double Cloth::PinnedCount()    { return (double)m_pinIdx.size(); }

Vector3 Cloth::CenterOfMass()
{
	if (m_numSim <= 0 || m_simPos.size() < (size_t)m_numSim * 3) return Vector3(0, 0, 0);
	double x = 0, y = 0, z = 0;
	for (int s = 0; s < m_numSim; ++s)
	{
		x += m_simPos[s * 3 + 0];
		y += m_simPos[s * 3 + 1];
		z += m_simPos[s * 3 + 2];
	}
	x /= m_numSim; y /= m_numSim; z /= m_numSim;
	if (m_skinnedMode)   // anchor-space sim: report in world
	{
		x += m_curAnchor[0]; y += m_curAnchor[1]; z += m_curAnchor[2];
	}
	return Vector3(x, y, z);
}

void Cloth::ReleaseBody()
{
	if (m_body)
	{
		// The body lives in the scene it was built in; at teardown time the caller wraps us
		// in the right PhysicsSceneScope (colliders die the same way).
		if (iPhysics* p = Physics::Scene())
		{
			p->destroySoftBody(m_body);
			for (const BodyProxy& px : m_proxies) p->destroyBody(px.body);
		}
		m_body = 0;
	}
	m_proxies.clear();
	m_clipCaps.clear();
	m_bindPos.clear(); m_bindNrm.clear();
	m_fleshBone.clear(); m_fleshLocal.clear();
	m_hardVert.clear();
	m_scene = nullptr;
	if (m_clothMesh)
	{
		MeshRenderer* mr = atom ? atom->GetComponent<MeshRenderer>() : nullptr;
		if (mr && mr->mesh == m_clothMesh) mr->mesh = m_srcMesh;
		if (m_smr) m_smr->externalMesh = false;
		delete[] m_clothMesh->vertexArray;
		delete[] m_clothMesh->normalArray;
		m_clothMesh->vertexArray = nullptr;
		m_clothMesh->normalArray = nullptr;
		m_clothMesh->uvArray = nullptr;       // shared with the source - not ours to free
		m_clothMesh->indexArray = nullptr;
		m_clothMesh->tangentArray = nullptr;
		m_clothMesh->uv2Array = nullptr;
		m_clothMesh->colorArray = nullptr;
		delete m_clothMesh;
		m_clothMesh = nullptr;
	}
	m_smr = nullptr;
	m_srcMesh = nullptr;
	m_renderToSim.clear(); m_simToRender.clear();
	m_invMass.clear(); m_pinIdx.clear(); m_simTris.clear(); m_simPos.clear();
	m_effInvBind.clear(); m_joints16.clear();
	m_prevPos.clear(); m_lerpPos.clear();
	m_havePrev = false;
	m_haveAnchor = false;
	m_prevAnchor[0] = m_prevAnchor[1] = m_prevAnchor[2] = 0.0f;
	m_curAnchor[0] = m_curAnchor[1] = m_curAnchor[2] = 0.0f;
	m_lastFixSec = 0.0;
	m_skinnedMode = false;
	m_numSim = 0;
}

namespace {

// World matrix of the atom (double TRS -> float mat).
glm::mat4 AtomWorld(Atom* a)
{
	Transform& t = a->GetTransform();
	const Vector3 p = t.globalPosition();
	const Quaternion q = t.globalRotation();
	const Vector3 s = t.globalScale();
	return glm::translate(glm::mat4(1.0f), glm::vec3((float)p.x, (float)p.y, (float)p.z))
	     * glm::mat4_cast(glm::quat((float)q.w, (float)q.x, (float)q.y, (float)q.z))
	     * glm::scale(glm::mat4(1.0f), glm::vec3((float)s.x, (float)s.y, (float)s.z));
}

// Shortest rotation taking `from` onto `to` (both unit).
glm::quat QuatFromTo(const glm::vec3& from, const glm::vec3& to)
{
	const float d = glm::dot(from, to);
	if (d > 0.9999f) return glm::quat(1, 0, 0, 0);
	if (d < -0.9999f) return glm::quat(0, 1, 0, 0);   // 180 deg about any perpendicular
	const glm::vec3 c = glm::cross(from, to);
	return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

// The ragdoll definition for a skeleton: the atom's (or an ancestor's) Ragdoll component,
// else the ResDB rig matching the skeleton (same lookup the spring bones use).
RagdollDef* FindRagdollDef(Atom* a, const Skeleton* sk)
{
	for (Atom* p = a; p; p = p->parent)
		if (Ragdoll* rd = p->GetComponent<Ragdoll>())
			if (RagdollDef* def = rd->Def()) return def;
	if (sk)
		for (RagdollDef* rg : ResDB::getSingleton()->ragdolls)
			if (rg && rg->skelGuid == sk->guid) return rg;
	return nullptr;
}

}  // namespace

bool Cloth::EnsureBody()
{
	if (m_body)
	{
		if (Physics::Scene() == m_scene) return true;
		ReleaseBody();   // the atom changed worlds (PIE copy etc.): rebuild in the new scene
	}
	if (m_failed || !atom) return false;
	iPhysics* phys = Physics::Scene();
	if (!phys) return false;

	MeshRenderer* mr = atom->GetComponent<MeshRenderer>();
	if (!mr) { m_failed = true; return false; }
	m_smr = dynamic_cast<SkinnedMeshRenderer*>(mr);
	Mesh* src = !mr->meshGuid.empty() ? ResDB::getSingleton()->GetMesh(mr->meshGuid) : mr->mesh;
	if (!src || !src->vertexArray || src->numVerts < 3 || !src->indexArray || src->numIndices < 3)
		return false;   // mesh may still be loading - retry next step
	m_srcMesh = src;
	const int n = src->numVerts;
	const glm::mat4 world = AtomWorld(atom);

	// Skinned start positions: LBS with the SMR's current globals (bind if nothing played),
	// preferring the mesh's own inverse binds (heterogeneous VRM outfits).
	Skeleton* sk = m_smr ? m_smr->EnsureSkeleton() : nullptr;
	const std::vector<float>* globals = nullptr;
	if (m_smr && sk)
	{
		if (m_smr->Globals().size() != sk->bones.size() * 16) return false;   // pose not applied yet
		globals = &m_smr->Globals();
		const size_t nb = sk->bones.size();
		m_effInvBind.resize(nb * 16);
		for (size_t i = 0; i < nb; ++i)
		{
			const float* bsrc = sk->bones[i].invBind;
			if (!src->bones.empty())
			{
				if (i < src->bones.size() && src->bones[i].name == sk->bones[i].name)
					bsrc = src->bones[i].invBind;
				else
					for (const MeshBone& mb : src->bones)
						if (mb.name == sk->bones[i].name) { bsrc = mb.invBind; break; }
			}
			memcpy(&m_effInvBind[i * 16], bsrc, sizeof(float) * 16);
		}
	}
	auto skinnedWorld = [&](int v) -> glm::vec3
	{
		const glm::vec4 p(src->vertexArray[v * 3 + 0], src->vertexArray[v * 3 + 1],
		                  src->vertexArray[v * 3 + 2], 1.0f);
		if (!globals || !src->boneIndex || !src->boneWeight)
			return glm::vec3(world * p);
		glm::vec3 out(0.0f);
		float wsum = 0.0f;
		for (int k = 0; k < 4; ++k)
		{
			const float bw = src->boneWeight[v * 4 + k];
			if (bw <= 0.0f) continue;
			const int b = src->boneIndex[v * 4 + k];
			if ((size_t)b * 16 >= m_effInvBind.size()) continue;
			const glm::mat4 pal = glm::make_mat4(globals->data() + (size_t)b * 16)
			                    * glm::make_mat4(&m_effInvBind[(size_t)b * 16]);
			out += bw * glm::vec3(pal * p);
			wsum += bw;
		}
		if (wsum <= 0.0f) return glm::vec3(world * p);
		return glm::vec3(world * glm::vec4(out / wsum, 1.0f));
	};

	// Weld UV-seam duplicates: quantized BIND positions key the sim vertex.
	const float wq = weldDistance > 1e-6f ? weldDistance : 1e-6f;
	std::map<std::tuple<long, long, long>, int> weld;
	m_renderToSim.assign(n, -1);
	m_simToRender.clear();
	for (int v = 0; v < n; ++v)
	{
		const std::tuple<long, long, long> key(
			(long)std::lround(src->vertexArray[v * 3 + 0] / wq),
			(long)std::lround(src->vertexArray[v * 3 + 1] / wq),
			(long)std::lround(src->vertexArray[v * 3 + 2] / wq));
		auto it = weld.find(key);
		if (it == weld.end())
		{
			it = weld.emplace(key, (int)m_simToRender.size()).first;
			m_simToRender.push_back(v);
		}
		m_renderToSim[v] = it->second;
	}
	m_numSim = (int)m_simToRender.size();

	// Sim triangles (welded, degenerates dropped).
	m_simTris.clear();
	for (int i = 0; i + 2 < src->numIndices; i += 3)
	{
		const int a0 = m_renderToSim[src->indexArray[i + 0]];
		const int a1 = m_renderToSim[src->indexArray[i + 1]];
		const int a2 = m_renderToSim[src->indexArray[i + 2]];
		if (a0 == a1 || a1 == a2 || a0 == a2) continue;
		m_simTris.push_back((unsigned int)a0);
		m_simTris.push_back((unsigned int)a1);
		m_simTris.push_back((unsigned int)a2);
	}
	if (m_numSim < 3 || m_simTris.size() < 3) { m_failed = true; return false; }

	// Fitted clothes ride Jolt's SKINNED CONSTRAINTS: bind-pose verts + per-vertex joints,
	// the solver leashes every vertex to its skinned position (face-normal backstop keeps
	// the sheet OUT of the body). Free cloth (flags, tablecloths) starts at world positions.
	m_skinnedMode = (globals != nullptr && src->boneIndex && src->boneWeight);

	std::vector<float> start((size_t)m_numSim * 3);
	glm::vec3 mn(1e9f), mx(-1e9f);
	for (int s = 0; s < m_numSim; ++s)
	{
		const int v = m_simToRender[s];
		glm::vec3 w;
		if (m_skinnedMode)   // BIND pose, model space (the backend skins it)
			w = glm::vec3(src->vertexArray[v * 3 + 0], src->vertexArray[v * 3 + 1],
			              src->vertexArray[v * 3 + 2]);
		else
			w = skinnedWorld(v);
		start[s * 3 + 0] = w.x; start[s * 3 + 1] = w.y; start[s * 3 + 2] = w.z;
		mn = glm::min(mn, w); mx = glm::max(mx, w);
	}
	// BODY GAP: inflate the fitted sheet along its bind normals. The skin targets are built
	// from these vertices, so the cloth hugs an AIR CUSHION over the skin — a thigh that
	// flexes outward past its capsule stays under the sheet instead of poking through.
	// The inflated bind + its normals also feed the RENDER BACKSTOP (WriteRenderMesh).
	if (m_skinnedMode)
	{
		std::vector<glm::vec3> nrm((size_t)m_numSim, glm::vec3(0.0f));
		for (size_t t = 0; t + 2 < m_simTris.size(); t += 3)
		{
			const unsigned int a0 = m_simTris[t], a1 = m_simTris[t + 1], a2 = m_simTris[t + 2];
			const glm::vec3 p0 = glm::make_vec3(&start[a0 * 3]);
			const glm::vec3 p1 = glm::make_vec3(&start[a1 * 3]);
			const glm::vec3 p2 = glm::make_vec3(&start[a2 * 3]);
			const glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
			nrm[a0] += fn; nrm[a1] += fn; nrm[a2] += fn;
		}
		// The HARD-skinned zone (above Sim Band, and thus the waistband) gets NO gap: it must
		// render EXACTLY as authored so the skirt merges seamlessly with the top above it.
		// The gap ramps in over 10% of the sheet's height below the sim cut.
		float bindLo = 1e9f, bindHi = -1e9f;
		for (int s = 0; s < m_numSim; ++s)
		{
			bindLo = std::min(bindLo, start[s * 3 + 1]);
			bindHi = std::max(bindHi, start[s * 3 + 1]);
		}
		const float bindH = std::max(1e-4f, bindHi - bindLo);
		const float gapCut = bindLo + glm::clamp(simBand, 0.0f, 1.0f) * bindH;
		m_bindNrm.assign((size_t)m_numSim * 3, 0.0f);
		for (int s = 0; s < m_numSim; ++s)
		{
			const float len = glm::length(nrm[s]);
			if (len < 1e-6f) continue;
			const glm::vec3 un = nrm[s] / len;
			m_bindNrm[s * 3 + 0] = un.x; m_bindNrm[s * 3 + 1] = un.y; m_bindNrm[s * 3 + 2] = un.z;
			const float gapScale = glm::clamp((gapCut - start[s * 3 + 1]) / (0.1f * bindH), 0.0f, 1.0f);
			if (bodyGap > 0.0f && gapScale > 0.0f)
			{
				start[s * 3 + 0] += un.x * bodyGap * gapScale;
				start[s * 3 + 1] += un.y * bodyGap * gapScale;
				start[s * 3 + 2] += un.z * bodyGap * gapScale;
			}
		}
		// SHRINK-WRAP over the flesh (bind pose, skeleton-common space): wherever the BODY
		// is thicker than the authored sheet, lift only THOSE vertices just above it. No
		// ballooning — vertices already clear of the flesh stay exactly where the artist
		// put them. This is what makes the skin targets sit OVER the body everywhere.
		if (collision && sk && src->boneIndex && src->boneWeight)
		{
			ComputeBodyCaps();
			const size_t nbk = sk->bones.size();
			std::vector<glm::mat4> bindG(nbk), toCloth(nbk);
			for (size_t i = 0; i < nbk; ++i)
			{
				bindG[i] = glm::inverse(glm::make_mat4(sk->bones[i].invBind));
				toCloth[i] = bindG[i] * glm::make_mat4(&m_effInvBind[i * 16]);
			}
			// Body vertices in common bind space, on a 2 cm hash grid.
			struct BodyPt { glm::vec3 p; int bone; };
			std::vector<BodyPt> body;
			if (atom->parent)
				for (Atom* sib : atom->parent->children)
				{
					if (!sib || sib == atom || !sib->enabled) continue;
					SkinnedMeshRenderer* ssmr = sib->GetComponent<SkinnedMeshRenderer>();
					if (!ssmr || !ssmr->enabled) continue;
					Mesh* m = !ssmr->meshGuid.empty() ? ResDB::getSingleton()->GetMesh(ssmr->meshGuid) : ssmr->mesh;
					if (!m || !m->vertexArray || !m->boneIndex || !m->boneWeight) continue;
					std::vector<glm::mat4> toCommon(nbk);
					for (size_t i = 0; i < nbk; ++i)
					{
						const float* ib = sk->bones[i].invBind;
						if (!m->bones.empty())
						{
							if (i < m->bones.size() && m->bones[i].name == sk->bones[i].name)
								ib = m->bones[i].invBind;
							else
								for (const MeshBone& mb : m->bones)
									if (mb.name == sk->bones[i].name) { ib = mb.invBind; break; }
						}
						toCommon[i] = bindG[i] * glm::make_mat4(ib);
					}
					for (int v = 0; v < m->numVerts; ++v)
					{
						int bb = -1; float bw = 0.0f;
						for (int k = 0; k < 4; ++k)
							if (m->boneWeight[v * 4 + k] > bw) { bw = m->boneWeight[v * 4 + k]; bb = m->boneIndex[v * 4 + k]; }
						if (bb < 0 || bw < 0.4f || (size_t)bb >= nbk) continue;
						body.push_back({ glm::vec3(toCommon[bb] * glm::vec4(m->vertexArray[v * 3 + 0],
							m->vertexArray[v * 3 + 1], m->vertexArray[v * 3 + 2], 1.0f)), bb });
					}
				}
			if (!body.empty())
			{
				// Bind each sheet vertex to the MOST PROTRUDING body vertex in the cylinder
				// under it. Per frame that one point skins with a single joint matrix and the
				// drawn sheet is clamped above it (WriteRenderMesh) — pose-accurate clipping
				// by the real flesh, immune to how differently the two meshes skin.
				const float cell = 0.02f;
				auto keyOf = [&](const glm::vec3& p) {
					return ((long long)std::floor(p.x / cell) * 73856093LL)
					     ^ ((long long)std::floor(p.y / cell) * 19349663LL)
					     ^ ((long long)std::floor(p.z / cell) * 83492791LL);
				};
				std::map<long long, std::vector<int>> grid;
				for (int i = 0; i < (int)body.size(); ++i) grid[keyOf(body[i].p)].push_back(i);
				m_fleshBone.assign(m_numSim, -1);
				m_fleshLocal.assign((size_t)m_numSim * 3, 0.0f);
				int bound = 0;
				for (int s = 0; s < m_numSim; ++s)
				{
					const int v = m_simToRender[s];
					int bd = -1; float bw = 0.0f;
					for (int k = 0; k < 4; ++k)
						if (src->boneWeight[v * 4 + k] > bw) { bw = src->boneWeight[v * 4 + k]; bd = src->boneIndex[v * 4 + k]; }
					if (bd < 0 || (size_t)bd >= nbk) continue;
					glm::vec3 pc = glm::vec3(toCloth[bd] * glm::vec4(start[s * 3 + 0], start[s * 3 + 1], start[s * 3 + 2], 1.0f));
					glm::vec3 nc = glm::mat3(toCloth[bd]) * glm::vec3(m_bindNrm[s * 3 + 0], m_bindNrm[s * 3 + 1], m_bindNrm[s * 3 + 2]);
					const float nl = glm::length(nc);
					if (nl < 1e-6f) continue;
					nc /= nl;
					int best = -1;
					float bestH = -0.06f;   // consider flesh up to 6 cm under the sheet
					for (int dx = -2; dx <= 2; ++dx)
						for (int dy = -2; dy <= 2; ++dy)
							for (int dz = -2; dz <= 2; ++dz)
							{
								const glm::vec3 probe = pc + glm::vec3(dx * cell, dy * cell, dz * cell);
								auto it = grid.find(keyOf(probe));
								if (it == grid.end()) continue;
								for (int bi : it->second)
								{
									const glm::vec3 d = body[bi].p - pc;
									const float h = glm::dot(d, nc);
									if (h < -0.06f || h > 0.03f) continue;
									const float lat = glm::length(d - nc * h);
									if (lat > 0.02f) continue;               // under THIS vertex
									if (h > bestH) { bestH = h; best = bi; }
								}
							}
					if (best < 0) continue;
					// Store the body point in ITS dominant bone's space: pose = J_bone * local.
					const int bb = body[best].bone;
					const glm::vec3 local = glm::vec3(glm::make_mat4(sk->bones[bb].invBind)
					                                * glm::vec4(body[best].p, 1.0f));
					m_fleshBone[s] = bb;
					m_fleshLocal[s * 3 + 0] = local.x; m_fleshLocal[s * 3 + 1] = local.y; m_fleshLocal[s * 3 + 2] = local.z;
					++bound;
				}
				std::cout << "[Cloth]\t\t'" << atom->GetName() << "': " << bound << "/" << m_numSim
				          << " verts bound to the flesh under them" << std::endl;
			}
		}
		m_bindPos.assign(start.begin(), start.end());
	}

	// Pins.
	m_invMass.assign(m_numSim, 1.0f);
	m_pinIdx.clear();
	if (pinMode == 1)   // Top Edge: the top band of the BIND-pose local Y
	{
		float ymin = 1e9f, ymax = -1e9f;
		for (int v = 0; v < n; ++v)
		{
			ymin = std::min(ymin, src->vertexArray[v * 3 + 1]);
			ymax = std::max(ymax, src->vertexArray[v * 3 + 1]);
		}
		const float cut = ymax - glm::clamp(pinBand, 0.0f, 1.0f) * (ymax - ymin);
		for (int s = 0; s < m_numSim; ++s)
			if (src->vertexArray[m_simToRender[s] * 3 + 1] >= cut)
				m_invMass[s] = 0.0f;
	}
	else if (pinMode == 2 && sk && src->boneIndex && src->boneWeight)   // Bone Weights
	{
		std::vector<char> pinBone(sk->bones.size(), 0);
		size_t p0 = 0;
		while (p0 <= pinBones.size())
		{
			const size_t sc = pinBones.find(';', p0);
			const std::string nm = pinBones.substr(p0, sc == std::string::npos ? std::string::npos : sc - p0);
			for (size_t i = 0; i < sk->bones.size(); ++i)
				if (sk->bones[i].name == nm) { pinBone[i] = 1; break; }
			if (sc == std::string::npos) break;
			p0 = sc + 1;
		}
		for (int s = 0; s < m_numSim; ++s)
		{
			const int v = m_simToRender[s];
			float wp = 0.0f;
			for (int k = 0; k < 4; ++k)
			{
				const int b = src->boneIndex[v * 4 + k];
				if (b >= 0 && b < (int)pinBone.size() && pinBone[b])
					wp += src->boneWeight[v * 4 + k];
			}
			if (wp >= 0.5f) m_invMass[s] = 0.0f;
		}
	}
	for (int s = 0; s < m_numSim; ++s)
		if (m_invMass[s] == 0.0f) m_pinIdx.push_back(s);
	if (pinMode != 0 && m_pinIdx.empty())
		std::cout << "[Cloth]\t\t'" << atom->GetName() << "': pin mode found NO vertices to pin "
		             "(check Pin Bones / Pin Band) - the sheet is fully free" << std::endl;

	// Create the body.
	NukeSoftBodyDesc d;
	d.verts = start.data();
	d.numVerts = m_numSim;
	d.indices = m_simTris.data();
	d.numTris = (int)m_simTris.size() / 3;
	const float es = 1.0f - glm::clamp(stiffness, 0.0f, 1.0f);
	const float bs = 1.0f - glm::clamp(bendStiffness, 0.0f, 1.0f);
	d.compliance = es * es * 0.01f;
	d.bendCompliance = bs * bs * 0.5f;
	d.vertexRadius = thickness;
	d.friction = friction;
	d.linearDamping = damping;
	d.gravityFactor = gravityFactor;
	d.pressure = pressure;
	d.iterations = iterations;
	std::vector<unsigned short> skinJ;
	std::vector<float> skinW, skinMax;
	if (m_skinnedMode)
	{
		// pins glue to the skin (max dist 0); the rest drape on the leash (0 = free of it)
		for (float& im : m_invMass) im = 1.0f;   // the constraints do the pinning
		skinJ.resize((size_t)m_numSim * 4, 0);
		skinW.resize((size_t)m_numSim * 4, 0.0f);
		skinMax.resize(m_numSim, 0.0f);
		m_hardVert.assign(m_numSim, 0);
		// Sim Band: only the bottom fraction of the sheet (by bind-pose Y) simulates; the
		// rest hard-skins — a fitted waist can never be pushed through by a body that is
		// thicker than its capsules, and the hem still drapes.
		float ymin = 1e9f, ymax = -1e9f;
		for (int v = 0; v < n; ++v)
		{
			ymin = std::min(ymin, src->vertexArray[v * 3 + 1]);
			ymax = std::max(ymax, src->vertexArray[v * 3 + 1]);
		}
		const float simCut = ymin + glm::clamp(simBand, 0.0f, 1.0f) * (ymax - ymin);
		for (int s = 0; s < m_numSim; ++s)
		{
			const int v = m_simToRender[s];
			for (int k = 0; k < 4; ++k)
			{
				skinJ[s * 4 + k] = src->boneIndex[v * 4 + k];
				skinW[s * 4 + k] = src->boneWeight[v * 4 + k];
			}
			const bool pinned = std::find(m_pinIdx.begin(), m_pinIdx.end(), s) != m_pinIdx.end()
			                 || src->vertexArray[v * 3 + 1] > simCut;
			skinMax[s] = pinned ? 0.0f : (maxDistance > 0.0f ? maxDistance : 1e9f);
			m_hardVert[s] = pinned ? 1 : 0;   // hard verts render EXACTLY as authored (no clips)
		}
		d.invBind = m_effInvBind.data();
		d.numJoints = (int)(m_effInvBind.size() / 16);
		d.skinJoints = skinJ.data();
		d.skinWeights = skinW.data();
		d.skinMaxDist = skinMax.data();
		d.backstopDistance = backstop > 0.0f ? backstop : 1e9f;
		d.pos[0] = d.pos[1] = d.pos[2] = 0.0f;   // bind space IS the body space
		d.localSpace = true;   // anchor-space: world travel never reaches the solver
	}
	else
	{
		const glm::vec3 c = (mn + mx) * 0.5f;
		d.pos[0] = c.x; d.pos[1] = c.y; d.pos[2] = c.z;
	}
	d.invMass = m_invMass.data();
	m_body = phys->createSoftBody(d);
	if (!m_body) { m_failed = true; return false; }
	m_scene = phys;
	if (m_skinnedMode)   // snap onto the current pose before the first step
	{
		const glm::mat4 world = AtomWorld(atom);
		const size_t nj = m_effInvBind.size() / 16;
		m_joints16.resize(nj * 16);
		glm::vec3 anchor(0.0f);
		for (size_t j = 0; j < nj; ++j)
			anchor += glm::vec3(glm::vec3((world * glm::make_mat4(globals->data() + j * 16))[3]));
		anchor /= (float)nj;
		for (size_t j = 0; j < nj; ++j)
		{
			glm::mat4 wj = world * glm::make_mat4(globals->data() + j * 16);
			wj[3] -= glm::vec4(anchor, 0.0f);   // anchor-space: translation only
			memcpy(&m_joints16[j * 16], glm::value_ptr(wj), sizeof(float) * 16);
		}
		m_prevAnchor[0] = m_curAnchor[0] = anchor.x;
		m_prevAnchor[1] = m_curAnchor[1] = anchor.y;
		m_prevAnchor[2] = m_curAnchor[2] = anchor.z;
		m_haveAnchor = true;
		phys->setSoftBodyJoints(m_body, m_joints16.data(), (int)nj, true);
	}

	// The private render instance (positions/normals own; the rest shared with the source).
	m_clothMesh = new Mesh();
	strncpy(m_clothMesh->name, src->name, sizeof(m_clothMesh->name) - 1);
	m_clothMesh->numVerts = n;
	m_clothMesh->vertexArray = new float[(size_t)n * 3];
	m_clothMesh->normalArray = new float[(size_t)n * 3];
	memcpy(m_clothMesh->vertexArray, src->vertexArray, sizeof(float) * 3 * n);
	if (src->normalArray) memcpy(m_clothMesh->normalArray, src->normalArray, sizeof(float) * 3 * n);
	m_clothMesh->uvArray = src->uvArray;
	m_clothMesh->uv2Array = src->uv2Array;
	m_clothMesh->colorArray = src->colorArray;
	m_clothMesh->tangentArray = src->tangentArray;
	m_clothMesh->indexArray = src->indexArray;
	m_clothMesh->numIndices = src->numIndices;
	m_clothMesh->sections = src->sections;
	m_clothMesh->lods = src->lods;
	m_clothMesh->numSlots = src->numSlots;
	m_clothMesh->slotNames = src->slotNames;
	m_clothMesh->skelGuid = src->skelGuid;
	m_clothMesh->rtProxy = src;   // RT/BLAS keys off the source; positions refresh by version
	mr->mesh = m_clothMesh;
	if (m_smr) m_smr->externalMesh = true;
	m_simPos.assign((size_t)m_numSim * 3, 0.0f);
	m_prevPos.clear();
	m_havePrev = false;
	if (collision && m_smr)
		BuildBodyProxies();
	std::cout << "[Cloth]\t\t'" << atom->GetName() << "': " << m_numSim << " sim verts ("
	          << n << " render), " << m_simTris.size() / 3 << " tris, "
	          << m_pinIdx.size() << " pinned" << (m_skinnedMode ? ", skinned" : ", FREE")
	          << std::endl;
	return true;
}

void Cloth::GatherPins(std::vector<float>& outWorld)
{
	outWorld.resize(m_pinIdx.size() * 3);
	const glm::mat4 world = AtomWorld(atom);
	Skeleton* sk = m_smr ? m_smr->skeleton : nullptr;
	const std::vector<float>* globals = (m_smr && sk && m_smr->Globals().size() == sk->bones.size() * 16)
	                                  ? &m_smr->Globals() : nullptr;
	for (size_t i = 0; i < m_pinIdx.size(); ++i)
	{
		const int v = m_simToRender[m_pinIdx[i]];
		const glm::vec4 p(m_srcMesh->vertexArray[v * 3 + 0], m_srcMesh->vertexArray[v * 3 + 1],
		                  m_srcMesh->vertexArray[v * 3 + 2], 1.0f);
		glm::vec3 w;
		if (globals && m_srcMesh->boneIndex && m_srcMesh->boneWeight)
		{
			glm::vec3 acc(0.0f);
			float wsum = 0.0f;
			for (int k = 0; k < 4; ++k)
			{
				const float bw = m_srcMesh->boneWeight[v * 4 + k];
				if (bw <= 0.0f) continue;
				const int b = m_srcMesh->boneIndex[v * 4 + k];
				if ((size_t)b * 16 >= m_effInvBind.size()) continue;
				const glm::mat4 pal = glm::make_mat4(globals->data() + (size_t)b * 16)
				                    * glm::make_mat4(&m_effInvBind[(size_t)b * 16]);
				acc += bw * glm::vec3(pal * p);
				wsum += bw;
			}
			w = wsum > 0.0f ? glm::vec3(world * glm::vec4(acc / wsum, 1.0f)) : glm::vec3(world * p);
		}
		else w = glm::vec3(world * p);
		outWorld[i * 3 + 0] = w.x; outWorld[i * 3 + 1] = w.y; outWorld[i * 3 + 2] = w.z;
	}
}

// Full-size body capsules from the ragdoll, AUTO-FIT to the actual flesh: every sibling
// mesh's bind vertices (skinned into the skeleton's common bind space - heterogeneous
// per-mesh binds) inflate the capsule of the bone they are weighted to. These drive BOTH
// the solver's kinematic proxies and the render clip; the sheet's start positions are
// pushed outside them, so full-size capsules never wad the sheet up.
void Cloth::ComputeBodyCaps()
{
	m_clipCaps.clear();
	Skeleton* sk = m_smr ? m_smr->skeleton : nullptr;
	RagdollDef* def = FindRagdollDef(atom, sk);
	if (!def || !sk || !m_srcMesh) return;
	for (const RagdollDef::Body& bd : def->bodies)
	{
		const int b = sk->BoneIndex(bd.bone);
		if (b < 0 || (size_t)b * 16 >= m_effInvBind.size()) continue;
		const glm::vec3 ax = glm::normalize(glm::make_vec3(bd.axis));
		// Keep only the capsules the sheet actually comes near (bind pose).
		const glm::mat4 boneBind = glm::inverse(glm::make_mat4(&m_effInvBind[(size_t)b * 16]));
		const glm::vec3 a0 = glm::vec3(boneBind * glm::vec4(glm::make_vec3(bd.center) - ax * bd.halfHeight, 1.0f));
		const glm::vec3 a1 = glm::vec3(boneBind * glm::vec4(glm::make_vec3(bd.center) + ax * bd.halfHeight, 1.0f));
		const glm::vec3 seg = a1 - a0;
		const float segDD = glm::dot(seg, seg);
		float nearest = 1e9f;
		for (int s = 0; s < m_numSim; ++s)
		{
			const int v = m_simToRender[s];
			const glm::vec3 p(m_srcMesh->vertexArray[v * 3 + 0], m_srcMesh->vertexArray[v * 3 + 1],
			                  m_srcMesh->vertexArray[v * 3 + 2]);
			const float t = segDD > 1e-12f ? glm::clamp(glm::dot(p - a0, seg) / segDD, 0.0f, 1.0f) : 0.0f;
			nearest = std::min(nearest, glm::length(p - (a0 + seg * t)));
		}
		if (nearest > bd.radius + 0.15f) continue;           // the sheet never comes near
		ClipCap cc;
		cc.bone = b;
		cc.center[0] = bd.center[0]; cc.center[1] = bd.center[1]; cc.center[2] = bd.center[2];
		cc.axis[0] = ax.x; cc.axis[1] = ax.y; cc.axis[2] = ax.z;
		cc.halfHeight = bd.halfHeight;
		cc.radius = bd.radius;
		m_clipCaps.push_back(cc);
	}
	if (m_clipCaps.empty() || !atom->parent) return;

	// Auto-fit each capsule's radius to the flesh around its bone.
	const size_t nb = sk->bones.size();
	std::vector<glm::mat4> bindG(nb);   // skeleton bind globals
	for (size_t i = 0; i < nb; ++i)
		bindG[i] = glm::inverse(glm::make_mat4(sk->bones[i].invBind));
	struct BodyVert { glm::vec3 p; int bone; };
	std::vector<BodyVert> verts;
	for (Atom* sib : atom->parent->children)
	{
		if (!sib || sib == atom || !sib->enabled) continue;
		SkinnedMeshRenderer* smr = sib->GetComponent<SkinnedMeshRenderer>();
		if (!smr || !smr->enabled) continue;
		Mesh* m = !smr->meshGuid.empty() ? ResDB::getSingleton()->GetMesh(smr->meshGuid) : smr->mesh;
		if (!m || !m->vertexArray || !m->boneIndex || !m->boneWeight) continue;
		// This mesh's binds by bone name (its own pose), skeleton's as the fallback.
		std::vector<glm::mat4> toCommon(nb);
		for (size_t i = 0; i < nb; ++i)
		{
			const float* ib = sk->bones[i].invBind;
			if (!m->bones.empty())
			{
				if (i < m->bones.size() && m->bones[i].name == sk->bones[i].name)
					ib = m->bones[i].invBind;
				else
					for (const MeshBone& mb : m->bones)
						if (mb.name == sk->bones[i].name) { ib = mb.invBind; break; }
			}
			toCommon[i] = bindG[i] * glm::make_mat4(ib);
		}
		for (int v = 0; v < m->numVerts; ++v)
		{
			// dominant bone only - enough for a radius fit
			int bb = -1; float bw = 0.0f;
			for (int k = 0; k < 4; ++k)
				if (m->boneWeight[v * 4 + k] > bw) { bw = m->boneWeight[v * 4 + k]; bb = m->boneIndex[v * 4 + k]; }
			if (bb < 0 || bw < 0.4f || (size_t)bb >= nb) continue;
			const glm::vec4 p(m->vertexArray[v * 3 + 0], m->vertexArray[v * 3 + 1],
			                  m->vertexArray[v * 3 + 2], 1.0f);
			verts.push_back({ glm::vec3(toCommon[bb] * p), bb });
		}
	}
	for (ClipCap& cc : m_clipCaps)
	{
		const glm::mat4 Wb = bindG[cc.bone];
		const glm::vec3 ax = glm::make_vec3(cc.axis) * cc.halfHeight;
		const glm::vec3 c = glm::make_vec3(cc.center);
		const glm::vec3 a0 = glm::vec3(Wb * glm::vec4(c - ax, 1.0f));
		const glm::vec3 a1 = glm::vec3(Wb * glm::vec4(c + ax, 1.0f));
		const glm::vec3 seg = a1 - a0;
		const float segDD = glm::dot(seg, seg);
		std::vector<float> dists;
		for (const BodyVert& bv : verts)
		{
			if (bv.bone != cc.bone) continue;
			const float t = segDD > 1e-12f ? glm::clamp(glm::dot(bv.p - a0, seg) / segDD, 0.0f, 1.0f) : 0.0f;
			dists.push_back(glm::length(bv.p - (a0 + seg * t)));
		}
		if (dists.size() < 8) continue;
		// 97.5th percentile: the flesh radius without stray-vertex outliers.
		const size_t q = dists.size() - 1 - dists.size() / 40;
		std::nth_element(dists.begin(), dists.begin() + q, dists.end());
		cc.radius = std::max(cc.radius, dists[q] + 0.002f);
	}
}

// The fitted capsules become KINEMATIC bodies on the softOnly layer, FULL flesh size: the
// solver keeps the sheet out of the body continuously (contacts + skinned constraints
// solved together), so a walking leg can never pass THROUGH the sheet - the start
// positions were pushed outside these capsules, so they never wad it up either.
void Cloth::BuildBodyProxies()
{
	Skeleton* sk = m_smr ? m_smr->skeleton : nullptr;
	if (!sk || !m_scene || m_clipCaps.empty()) return;
	const std::vector<float>& globals = m_smr->Globals();
	if (globals.size() != sk->bones.size() * 16) return;
	const glm::mat4 world = AtomWorld(atom);
	for (const ClipCap& cc : m_clipCaps)
	{
		// The SOLVER capsule fits under the (shrink-wrapped) sheet: the sheet now clears the
		// flesh, so this barely deflates — but it must never eject start vertices sideways.
		float simR = cc.radius;
		if (m_skinnedMode && m_bindPos.size() == (size_t)m_numSim * 3
		    && (size_t)cc.bone * 16 < m_effInvBind.size())
		{
			const glm::mat4 Wb = glm::inverse(glm::make_mat4(&m_effInvBind[(size_t)cc.bone * 16]));
			const glm::vec3 axh = glm::make_vec3(cc.axis) * cc.halfHeight;
			const glm::vec3 c = glm::make_vec3(cc.center);
			const glm::vec3 a0 = glm::vec3(Wb * glm::vec4(c - axh, 1.0f));
			const glm::vec3 a1 = glm::vec3(Wb * glm::vec4(c + axh, 1.0f));
			const glm::vec3 seg = a1 - a0;
			const float segDD = glm::dot(seg, seg);
			float nearest = 1e9f;
			for (int s = 0; s < m_numSim; ++s)
			{
				const glm::vec3 p(m_bindPos[s * 3 + 0], m_bindPos[s * 3 + 1], m_bindPos[s * 3 + 2]);
				const float t = segDD > 1e-12f ? glm::clamp(glm::dot(p - a0, seg) / segDD, 0.0f, 1.0f) : 0.0f;
				nearest = std::min(nearest, glm::length(p - (a0 + seg * t)));
			}
			simR = std::min(simR, std::max(0.015f, nearest - thickness - 0.003f));
		}
		const glm::mat4 W = world * glm::make_mat4(globals.data() + (size_t)cc.bone * 16);
		// Capsule axis local +Y; orient the body so +Y matches the def axis. Skinned cloth
		// simulates in anchor space - its proxies live there too.
		const glm::vec3 ax = glm::make_vec3(cc.axis);
		const glm::quat toAxis = QuatFromTo(glm::vec3(0, 1, 0), ax);
		glm::vec3 wc = glm::vec3(W * glm::vec4(glm::make_vec3(cc.center), 1.0f));
		if (m_skinnedMode) wc -= glm::make_vec3(m_curAnchor);
		const glm::quat wq = glm::quat_cast(glm::mat3(W)) * toAxis;
		NukeBodyDesc d;
		d.shape = 2;
		d.radius = simR;
		d.halfHeight = cc.halfHeight;
		d.motion = 2;   // kinematic: MoveKinematic feeds the pose (correct contact velocities)
		d.friction = friction;
		d.softOnly = true;
		d.pos[0] = wc.x; d.pos[1] = wc.y; d.pos[2] = wc.z;
		d.quat[0] = wq.x; d.quat[1] = wq.y; d.quat[2] = wq.z; d.quat[3] = wq.w;
		const uint64_t body = m_scene->createBody(d);
		if (!body) continue;
		BodyProxy px;
		px.body = body;
		px.bone = cc.bone;
		px.center[0] = cc.center[0]; px.center[1] = cc.center[1]; px.center[2] = cc.center[2];
		px.axis[0] = ax.x; px.axis[1] = ax.y; px.axis[2] = ax.z;
		px.halfHeight = cc.halfHeight;
		m_proxies.push_back(px);
	}
	if (!m_proxies.empty())
		std::cout << "[Cloth]		'" << atom->GetName() << "': " << m_proxies.size()
		          << " full-size body capsule(s) ride the pose" << std::endl;
}

void Cloth::DriveBodyProxies()
{
	if (m_proxies.empty() || !m_scene) return;
	Skeleton* sk = m_smr ? m_smr->skeleton : nullptr;
	if (!sk) return;
	const std::vector<float>& globals = m_smr->Globals();
	if (globals.size() != sk->bones.size() * 16) return;
	const glm::mat4 world = AtomWorld(atom);
	const float dt = (float)(m_fixDtSec > 1e-4 ? m_fixDtSec : 1.0 / 60.0);
	for (const BodyProxy& px : m_proxies)
	{
		const glm::mat4 W = world * glm::make_mat4(globals.data() + (size_t)px.bone * 16);
		const glm::quat toAxis = QuatFromTo(glm::vec3(0, 1, 0), glm::make_vec3(px.axis));
		glm::vec3 wc = glm::vec3(W * glm::vec4(glm::make_vec3(px.center), 1.0f));
		if (m_skinnedMode) wc -= glm::make_vec3(m_curAnchor);
		const glm::quat wq = glm::quat_cast(glm::mat3(W)) * toAxis;
		const float pos[3] = { wc.x, wc.y, wc.z };
		const float quat[4] = { wq.x, wq.y, wq.z, wq.w };
		float cur[3] = { wc.x, wc.y, wc.z }, cq[4];
		m_scene->getBodyPose(px.body, cur, cq);
		const glm::vec3 jump = wc - glm::make_vec3(cur);
		if (glm::dot(jump, jump) > 0.25f)   // teleport (> 0.5 m): don't smear the sheet
			m_scene->setBodyPose(px.body, pos, quat);
		else
			m_scene->moveKinematic(px.body, pos, quat, dt);
	}
}

// Sim positions (+ their anchor, for anchor-space sheets) -> the private render instance
// (atom-local) + smooth face normals. The write clamps every vertex OUTSIDE the body's
// FULL-size capsules: whatever the sim does, clothing never draws inside the body.
void Cloth::WriteRenderMesh(const float* simPos, const float anchor[3])
{
	const glm::mat4 inv = glm::inverse(AtomWorld(atom));
	const glm::vec3 anc = m_skinnedMode ? glm::make_vec3(anchor) : glm::vec3(0.0f);

	const float* pos = simPos;
	std::vector<float> clipped;
	if (m_skinnedMode && collision && m_smr && m_smr->skeleton
	    && m_smr->Globals().size() == m_smr->skeleton->bones.size() * 16
	    && !m_joints16.empty())
	{
		clipped.assign(simPos, simPos + (size_t)m_numSim * 3);

		// 1) RENDER BACKSTOP, gap-free: skin every vertex's inflated bind position + normal
		//    with the CURRENT joints and clamp the drawn vertex in FRONT of that surface.
		//    The clothing mesh is authored over the body (+ Body Gap), so its own skinned
		//    surface covers the whole sheet — no capsule-seam leaks at the groin/glutes.
		if (m_bindPos.size() == (size_t)m_numSim * 3 && m_bindNrm.size() == (size_t)m_numSim * 3
		    && m_srcMesh && m_srcMesh->boneIndex && m_srcMesh->boneWeight)
		{
			for (int s = 0; s < m_numSim; ++s)
			{
				if (s < (int)m_hardVert.size() && m_hardVert[s]) continue;   // authored skin: untouched
				const int v = m_simToRender[s];
				glm::vec3 tgt(0.0f), nrm(0.0f);
				float wsum = 0.0f;
				const glm::vec4 bp(m_bindPos[s * 3 + 0], m_bindPos[s * 3 + 1], m_bindPos[s * 3 + 2], 1.0f);
				const glm::vec3 bn(m_bindNrm[s * 3 + 0], m_bindNrm[s * 3 + 1], m_bindNrm[s * 3 + 2]);
				for (int k = 0; k < 4; ++k)
				{
					const float bw = m_srcMesh->boneWeight[v * 4 + k];
					if (bw <= 0.0f) continue;
					const int b = m_srcMesh->boneIndex[v * 4 + k];
					if ((size_t)b * 16 >= m_joints16.size() || (size_t)b * 16 >= m_effInvBind.size()) continue;
					const glm::mat4 pal = glm::make_mat4(&m_joints16[(size_t)b * 16])
					                    * glm::make_mat4(&m_effInvBind[(size_t)b * 16]);
					tgt += bw * glm::vec3(pal * bp);
					nrm += bw * (glm::mat3(pal) * bn);
					wsum += bw;
				}
				if (wsum <= 0.0f) continue;
				tgt /= wsum;
				const float nl = glm::length(nrm);
				if (nl < 1e-6f) continue;
				nrm /= nl;
				glm::vec3 p(clipped[s * 3 + 0], clipped[s * 3 + 1], clipped[s * 3 + 2]);
				// Clamp a margin IN FRONT of the surface: an edge between two on-surface
				// vertices still cuts through the flesh bulging between them.
				const float sink = glm::dot(tgt - p, nrm) + 0.004f;   // > 0 = behind surface+margin
				if (sink > 0.0f)
					p += nrm * sink;
				// FLESH clamp: the bound body point, skinned by its ONE joint, must stay
				// under the sheet — pose-accurate, whatever the two skinnings disagree on.
				if (s < (int)m_fleshBone.size() && m_fleshBone[s] >= 0
				    && (size_t)m_fleshBone[s] * 16 < m_joints16.size())
				{
					const glm::vec3 bodyP = glm::vec3(glm::make_mat4(&m_joints16[(size_t)m_fleshBone[s] * 16])
					                                * glm::vec4(m_fleshLocal[s * 3 + 0], m_fleshLocal[s * 3 + 1],
					                                            m_fleshLocal[s * 3 + 2], 1.0f));
					const float hb = glm::dot(bodyP - p, nrm) + thickness * 0.5f + 0.003f;
					if (hb > 0.0f)
						p += nrm * hb;
				}
				clipped[s * 3 + 0] = p.x; clipped[s * 3 + 1] = p.y; clipped[s * 3 + 2] = p.z;
			}
		}

		// 2) Body capsules (fitted to the flesh), for vertices whose normals face sideways.
		if (!m_clipCaps.empty())
		{
			const std::vector<float>& globals = m_smr->Globals();
			const glm::mat4 world = AtomWorld(atom);
			struct Cap { glm::vec3 a, b; float r; };
			std::vector<Cap> caps;
			caps.reserve(m_clipCaps.size());
			for (const ClipCap& cc : m_clipCaps)
			{
				const glm::mat4 W = world * glm::make_mat4(globals.data() + (size_t)cc.bone * 16);
				const glm::vec3 axh = glm::make_vec3(cc.axis) * cc.halfHeight;
				const glm::vec3 c = glm::make_vec3(cc.center);
				caps.push_back({ glm::vec3(W * glm::vec4(c - axh, 1.0f)) - anc,
				                 glm::vec3(W * glm::vec4(c + axh, 1.0f)) - anc,
				                 cc.radius + thickness + 0.008f });   // margin: edges must clear the flesh too
			}
			for (int s = 0; s < m_numSim; ++s)
			{
				if (s < (int)m_hardVert.size() && m_hardVert[s]) continue;   // authored skin: untouched
				glm::vec3 p(clipped[s * 3 + 0], clipped[s * 3 + 1], clipped[s * 3 + 2]);
				bool moved = false;
				for (const Cap& cp : caps)
				{
					const glm::vec3 ab = cp.b - cp.a;
					const float dd = glm::dot(ab, ab);
					const float t = dd > 1e-12f ? glm::clamp(glm::dot(p - cp.a, ab) / dd, 0.0f, 1.0f) : 0.0f;
					const glm::vec3 q = cp.a + ab * t;
					const glm::vec3 d = p - q;
					const float l = glm::length(d);
					if (l < cp.r && l > 1e-6f) { p = q + d * (cp.r / l); moved = true; }
				}
				if (moved) { clipped[s * 3 + 0] = p.x; clipped[s * 3 + 1] = p.y; clipped[s * 3 + 2] = p.z; }
			}
		}
		pos = clipped.data();
	}

	// Hard-skinned vertices bypass the sim/lerp entirely: they skin DIRECTLY with the SMR's
	// CURRENT globals — the exact palette the neighbouring garments render with THIS frame.
	// The sim path lags half a step behind; on a fast hip swing that lag is what pulled the
	// waistband off the top (a bare gap on one side, cloth over the midriff on the other).
	const std::vector<float>* hg = nullptr;
	if (m_skinnedMode && m_smr && m_smr->skeleton
	    && m_smr->Globals().size() == m_smr->skeleton->bones.size() * 16
	    && m_srcMesh && m_srcMesh->boneIndex && m_srcMesh->boneWeight)
		hg = &m_smr->Globals();

	// PROBE: the frontmost hard vertex — direct skin vs the sim's own (target-pinned) position.
	static const bool dbgP = std::getenv("NUKE_CLOTH_DEBUG") != nullptr;
	if (dbgP && hg && m_skinnedMode)
	{
		static int pt = 0;
		if (++pt <= 2 || pt % 120 == 0)
		{
			int fs = -1; float bz = -1e9f;
			for (int s2 = 0; s2 < m_numSim; ++s2)
				if (s2 < (int)m_hardVert.size() && m_hardVert[s2])
				{
					const int v2 = m_simToRender[s2];
					if (m_srcMesh->vertexArray[v2 * 3 + 2] > bz)
					{ bz = m_srcMesh->vertexArray[v2 * 3 + 2]; fs = s2; }
				}
			if (fs >= 0)
			{
				const int v2 = m_simToRender[fs];
				const glm::vec4 bp(m_srcMesh->vertexArray[v2 * 3 + 0], m_srcMesh->vertexArray[v2 * 3 + 1],
				                   m_srcMesh->vertexArray[v2 * 3 + 2], 1.0f);
				glm::vec3 acc(0.0f); float ws = 0.0f;
				for (int k = 0; k < 4; ++k)
				{
					const float bw = m_srcMesh->boneWeight[v2 * 4 + k];
					if (bw <= 0.0f) continue;
					const int b = m_srcMesh->boneIndex[v2 * 4 + k];
					if ((size_t)b * 16 >= m_effInvBind.size() || (size_t)b * 16 >= hg->size()) continue;
					acc += bw * glm::vec3((glm::make_mat4(hg->data() + (size_t)b * 16)
					                     * glm::make_mat4(&m_effInvBind[(size_t)b * 16])) * bp);
					ws += bw;
				}
				if (ws > 0.0f) acc /= ws;
				std::cout << "[ClothDbg]	frontHard direct=" << acc.x << " " << acc.y << " " << acc.z
				          << " simA=" << m_simPos[fs * 3 + 0] + m_curAnchor[0] << " "
				          << m_simPos[fs * 3 + 1] + m_curAnchor[1] << " "
				          << m_simPos[fs * 3 + 2] + m_curAnchor[2] << std::endl;
			}
		}
	}
	float* dv = m_clothMesh->vertexArray;
	const int n = m_clothMesh->numVerts;
	for (int v = 0; v < n; ++v)
	{
		const int s = m_renderToSim[v];
		const glm::vec3 l = glm::vec3(inv * glm::vec4(pos[s * 3 + 0] + anc.x,
		                                              pos[s * 3 + 1] + anc.y,
		                                              pos[s * 3 + 2] + anc.z, 1.0f));
		dv[v * 3 + 0] = l.x; dv[v * 3 + 1] = l.y; dv[v * 3 + 2] = l.z;
	}
	// smooth normals over the welded sheet, fanned back to the render vertices
	std::vector<glm::vec3> nrm((size_t)m_numSim, glm::vec3(0.0f));
	for (size_t t = 0; t + 2 < m_simTris.size(); t += 3)
	{
		const unsigned int a0 = m_simTris[t], a1 = m_simTris[t + 1], a2 = m_simTris[t + 2];
		const glm::vec3 p0(pos[a0 * 3], pos[a0 * 3 + 1], pos[a0 * 3 + 2]);
		const glm::vec3 p1(pos[a1 * 3], pos[a1 * 3 + 1], pos[a1 * 3 + 2]);
		const glm::vec3 p2(pos[a2 * 3], pos[a2 * 3 + 1], pos[a2 * 3 + 2]);
		const glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
		nrm[a0] += fn; nrm[a1] += fn; nrm[a2] += fn;
	}
	const glm::mat3 invRot(inv);   // rotate normals into atom space (uniform-ish scales)
	float* dn = m_clothMesh->normalArray;
	for (int v = 0; v < n; ++v)
	{
		const int s = m_renderToSim[v];
		glm::vec3 nn = invRot * nrm[s];
		const float len = glm::length(nn);
		if (len > 1e-6f) nn /= len;
		dn[v * 3 + 0] = nn.x; dn[v * 3 + 1] = nn.y; dn[v * 3 + 2] = nn.z;
	}
	++m_clothMesh->version;
	m_clothMesh->boundsValid = false;
	static const bool dbg = std::getenv("NUKE_CLOTH_DEBUG") != nullptr;
	if (dbg)
	{
		static int tick = 0;
		if (++tick <= 3 || tick % 60 == 0)
		{
			glm::vec3 lmn(1e9f), lmx(-1e9f);
			for (int v = 0; v < n; ++v)
			{
				const glm::vec3 p(dv[v * 3], dv[v * 3 + 1], dv[v * 3 + 2]);
				lmn = glm::min(lmn, p); lmx = glm::max(lmx, p);
			}
			float hy0 = 1e9f, hy1 = -1e9f, sy1 = -1e9f;
			int hc = 0;
			for (int v = 0; v < n; ++v)
			{
				const int s2 = m_renderToSim[v];
				if (s2 < (int)m_hardVert.size() && m_hardVert[s2])
				{ hy0 = std::min(hy0, dv[v * 3 + 1]); hy1 = std::max(hy1, dv[v * 3 + 1]); ++hc; }
				else
					sy1 = std::max(sy1, dv[v * 3 + 1]);
			}
			std::cout << "[ClothDbg]\t'" << atom->GetName() << "' render local x=["
			          << lmn.x << " .. " << lmx.x << "] y=[" << lmn.y << " .. " << lmx.y
			          << "] hardY=[" << hy0 << " .. " << hy1 << "] hardN=" << hc
			          << " simTopY=" << sy1 << std::endl;
		}
	}
}

// Render write, every FRAME: the sheet lerps between the last two fixed steps by wall-clock
// phase — a 60 Hz sim on a 144 Hz screen snaps visibly otherwise (the body animates per
// frame, the cloth per step).
void Cloth::Update()
{
	if (!enabled || !m_body || !m_clothMesh || m_numSim <= 0) return;
	const float* src = m_simPos.data();
	float anc[3] = { m_curAnchor[0], m_curAnchor[1], m_curAnchor[2] };
	if (m_havePrev && m_prevPos.size() == m_simPos.size() && m_fixDtSec > 1e-4)
	{
		const double now = boost::chrono::duration_cast<boost::chrono::duration<double>>(
			boost::chrono::steady_clock::now().time_since_epoch()).count();
		const float a = glm::clamp((float)((now - m_lastFixSec) / m_fixDtSec), 0.0f, 1.0f);
		m_lerpPos.resize(m_simPos.size());
		for (size_t i = 0; i < m_simPos.size(); ++i)
			m_lerpPos[i] = m_prevPos[i] + (m_simPos[i] - m_prevPos[i]) * a;
		src = m_lerpPos.data();
		for (int c = 0; c < 3; ++c)
			anc[c] = m_prevAnchor[c] + (m_curAnchor[c] - m_prevAnchor[c]) * a;
	}
	WriteRenderMesh(src, anc);
}

void Cloth::FixedUpdate()
{
	if (!enabled || !EnsureBody()) return;
	iPhysics* phys = m_scene;
	static const bool dbgTick = std::getenv("NUKE_CLOTH_DEBUG") != nullptr;
	if (dbgTick)
	{
		static int calls = 0;
		if (++calls <= 3 || calls % 60 == 0)
			std::cout << "[ClothDbg]\ttick #" << calls << " '" << atom->GetName()
			          << "' body=" << m_body << std::endl;
	}

	// 1) The result of THIS step (the world steps physics before component FixedUpdates).
	//    The previous step's positions stay around: the render write (Update) interpolates.
	if (m_simPos.size() == (size_t)m_numSim * 3) { m_prevPos = m_simPos; m_havePrev = true; }
	if (!phys->getSoftBodyVertices(m_body, m_simPos.data(), m_numSim))
	{
		static const bool dbg = std::getenv("NUKE_CLOTH_DEBUG") != nullptr;
		if (dbg)
			std::cout << "[ClothDbg]\t'" << (atom ? atom->GetName() : "?")
			          << "' getSoftBodyVertices FAILED (body " << m_body << ")" << std::endl;
		return;
	}
	{
		const double now = boost::chrono::duration_cast<boost::chrono::duration<double>>(
			boost::chrono::steady_clock::now().time_since_epoch()).count();
		if (m_lastFixSec > 0.0)
		{
			const double dt = now - m_lastFixSec;
			if (dt > 1e-4 && dt < 0.5) m_fixDtSec = dt;   // hitches don't stretch the lerp
		}
		m_lastFixSec = now;
	}

	// 2) The animation drives the NEXT step, in ANCHOR SPACE: joints arrive with the
	//    skeleton's mean position subtracted, so world travel, clip drift and loop teleports
	//    never reach the solver — only the pose relative to the body does.
	if (m_skinnedMode)
	{
		Skeleton* ssk = m_smr ? m_smr->skeleton : nullptr;
		const std::vector<float>* sglobals = (ssk && m_smr->Globals().size() == ssk->bones.size() * 16)
		                                   ? &m_smr->Globals() : nullptr;
		const size_t nj = m_effInvBind.size() / 16;
		if (sglobals && nj > 0)
		{
			const glm::mat4 world = AtomWorld(atom);
			m_joints16.resize(nj * 16);
			glm::vec3 anchor(0.0f);
			for (size_t j = 0; j < nj; ++j)
				anchor += glm::vec3((world * glm::make_mat4(sglobals->data() + j * 16))[3]);
			anchor /= (float)nj;
			for (size_t j = 0; j < nj; ++j)
			{
				glm::mat4 wj = world * glm::make_mat4(sglobals->data() + j * 16);
				wj[3] -= glm::vec4(anchor, 0.0f);
				memcpy(&m_joints16[j * 16], glm::value_ptr(wj), sizeof(float) * 16);
			}
			// A loop teleport moves only the anchor now; don't smear the render lerp across it.
			const glm::vec3 jump = anchor - glm::make_vec3(m_curAnchor);
			if (m_haveAnchor && glm::dot(jump, jump) > 0.09f) m_havePrev = false;
			memcpy(m_prevAnchor, m_curAnchor, sizeof(m_prevAnchor));
			m_curAnchor[0] = anchor.x; m_curAnchor[1] = anchor.y; m_curAnchor[2] = anchor.z;
			m_haveAnchor = true;
			phys->setSoftBodyJoints(m_body, m_joints16.data(), (int)nj, false);
			if (dbgTick)
			{
				static int jt = 0;
				if (++jt <= 3 || jt % 60 == 0)
				{
					const Vector3 com = CenterOfMass();
					// The HEM vertex (lowest bind Y): its skin TARGET by our own formula vs the
					// ACTUAL sim position — a walking target with a frozen actual = solver issue,
					// a frozen target = weights/joints issue.
					int hem = 0;
					float loY = 1e9f;
					for (int s = 0; s < m_numSim; ++s)
					{
						const float y = m_srcMesh->vertexArray[m_simToRender[s] * 3 + 1];
						if (y < loY) { loY = y; hem = s; }
					}
					const int hv = m_simToRender[hem];
					glm::vec3 tgt(0.0f);
					float wsum = 0.0f;
					for (int k = 0; k < 4; ++k)
					{
						const float bw = m_srcMesh->boneWeight[hv * 4 + k];
						if (bw <= 0.0f) continue;
						const int b = m_srcMesh->boneIndex[hv * 4 + k];
						if ((size_t)b * 16 >= m_joints16.size()) continue;
						const glm::vec4 bp(m_srcMesh->vertexArray[hv * 3 + 0], m_srcMesh->vertexArray[hv * 3 + 1],
						                   m_srcMesh->vertexArray[hv * 3 + 2], 1.0f);
						tgt += bw * glm::vec3(glm::make_mat4(&m_joints16[(size_t)b * 16])
						                    * glm::make_mat4(&m_effInvBind[(size_t)b * 16]) * bp);
						wsum += bw;
					}
					if (wsum > 0.0f) tgt /= wsum;
					std::cout << "[ClothDbg]\t'" << atom->GetName() << "' anchor=" << anchor.x << " "
					          << anchor.y << " " << anchor.z << " hemTgt=" << tgt.x << " " << tgt.y
					          << " " << tgt.z << " hemAct=" << m_simPos[hem * 3] << " "
					          << m_simPos[hem * 3 + 1] << " " << m_simPos[hem * 3 + 2] << std::endl;
				}
			}
		}
	}

	// 3) Body capsules ride the pose (kinematic softOnly proxies in the SAME anchor space;
	//    the solver collides the sheet with them together with the skinned constraints).
	if (collision)
		DriveBodyProxies();

	// 4) Free cloth (flags, tablecloths) rides its kinematic pins in world space.
	if (!m_skinnedMode && !m_pinIdx.empty())
	{
		std::vector<float> pinWorld;
		GatherPins(pinWorld);
		phys->setSoftBodyVertices(m_body, m_pinIdx.data(), pinWorld.data(), (int)m_pinIdx.size());
	}

	// 5) Wind: one gusty sample at the sheet's center, as a velocity delta on the free verts.
	//    (An anchor-space sheet sits near the origin — sample at its WORLD position.)
	if (windOn)
	{
		glm::vec3 c(0.0f);
		for (int s = 0; s < m_numSim; ++s)
			c += glm::vec3(m_simPos[s * 3], m_simPos[s * 3 + 1], m_simPos[s * 3 + 2]);
		c /= (float)m_numSim;
		if (m_skinnedMode) c += glm::make_vec3(m_curAnchor);
		const Vector3 wv = Wind::Sample(Vector3(c.x, c.y, c.z));
		const float dt = 1.0f / 60.0f;   // fixed cadence (see World::FixedUpdate)
		const float dv[3] = { (float)wv.x * dt, (float)wv.y * dt, (float)wv.z * dt };
		if (dv[0] != 0.0f || dv[1] != 0.0f || dv[2] != 0.0f)
			phys->addSoftBodyVelocity(m_body, dv);
	}
}

}  // namespace nuke
