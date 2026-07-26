#include "API/Model/InstancedMesh.h"
#include "API/Model/resdb.h"
#include "reflect/ReflectBind.h"   // Reflect_DropObject (owned material instance)
#include "interface/AppInstance.h" // renderer access: invalidateMesh on RT chunk release
#include <render/irender.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <map>
#include <tuple>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace nuke {

// ---- base64 (the `data` blob codec; local — no engine-wide dependency) --------------------
static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string B64Encode(const unsigned char* p, size_t n)
{
	std::string out; out.reserve(((n + 2) / 3) * 4);
	for (size_t i = 0; i < n; i += 3)
	{
		unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] : 0) << 8 | (i + 2 < n ? p[i + 2] : 0);
		out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
		out += (i + 1 < n) ? kB64[(v >> 6) & 63] : '=';
		out += (i + 2 < n) ? kB64[v & 63] : '=';
	}
	return out;
}
static std::vector<unsigned char> B64Decode(const std::string& s)
{
	static int rev[256]; static bool init = false;
	if (!init) { init = true; for (int i = 0; i < 256; ++i) rev[i] = -1; for (int i = 0; i < 64; ++i) rev[(unsigned char)kB64[i]] = i; }
	std::vector<unsigned char> out; out.reserve(s.size() / 4 * 3);
	int acc = 0, bits = 0;
	for (unsigned char c : s)
	{
		if (rev[c] < 0) continue;   // '=' / whitespace
		acc = (acc << 6) | rev[c]; bits += 6;
		if (bits >= 8) { bits -= 8; out.push_back((unsigned char)((acc >> bits) & 0xFF)); }
	}
	return out;
}

InstancedMesh::InstancedMesh() : Component("InstancedMesh") {}

void InstancedMesh::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	if (!mesh && !meshGuid.empty()) mesh = ResDB::getSingleton()->GetMesh(meshGuid);
	if (!mat && !matGuid.empty())
	{
		Material* asset = ResDB::getSingleton()->GetMaterial(matGuid);
		if (asset) mat = asset->Clone();
	}
}

void InstancedMesh::Destroy()
{
	if (mat) { Reflect_DropObject(mat); delete mat; mat = nullptr; }
	if (gpuBuf && gpuOwner) { gpuOwner->destroyInstanceBuffer(gpuBuf); gpuBuf = 0; gpuOwner = nullptr; }
	ReleaseRTChunks();
}

void InstancedMesh::ReleaseRTChunks()
{
	if (rtChunkMeshes.empty()) return;
	// The renderer caches GPU buffers + BLASes keyed by Mesh* — drop those entries BEFORE
	// deleting, or a later allocation at the same address would be served stale GPU data.
	iRender* r = AppInstance::GetSingleton() ? AppInstance::GetSingleton()->render : nullptr;
	for (Mesh* m : rtChunkMeshes)
	{
		if (!m) continue;
		if (r) r->invalidateMesh(m);
		delete[] m->vertexArray; delete[] m->normalArray; delete[] m->uvArray;
		delete[] m->rtBendArray; delete[] m->rtPivotArray;
		m->vertexArray = m->normalArray = m->uvArray = nullptr;
		m->rtBendArray = m->rtPivotArray = nullptr;
		delete m;
	}
	rtChunkMeshes.clear();
}

void InstancedMesh::Update() {}
void InstancedMesh::FixedUpdate() {}
void InstancedMesh::Pause() {}
void InstancedMesh::Reset() { mesh = nullptr; }

// The serialized store is the base64 blob: 17 floats per instance (pos3 quat4 scale3 color4
// custom4 = the Inst layout verbatim, little-endian). Reflection saves `data` as an ordinary
// string prop — thousands of instances stay ONE compact JSON value.
void InstancedMesh::OnBeforeSave()
{
	EnsureDecoded();
	static_assert(sizeof(Inst) == 18 * sizeof(float), "Inst must stay tightly packed (the blob is its raw bytes)");
	data = instances.empty() ? std::string()
	                         : B64Encode((const unsigned char*)instances.data(), instances.size() * sizeof(Inst));
}

void InstancedMesh::EnsureDecoded()
{
	if (decoded) return;
	decoded = true;
	if (data.empty() || !instances.empty()) return;   // fresh component or already authored live
	std::vector<unsigned char> bytes = B64Decode(data);
	const size_t n = bytes.size() / sizeof(Inst);
	if (n == 0) return;
	instances.resize(n);
	memcpy(instances.data(), bytes.data(), n * sizeof(Inst));
	dirty = true;
}

// ---- reflected instance API ---------------------------------------------------------------

int InstancedMesh::AddInstance(const Vector3& pos, const Vector3& eulerDeg, const Vector3& scale)
{
	EnsureDecoded();
	Inst in{};
	in.pos[0] = (float)pos.x; in.pos[1] = (float)pos.y; in.pos[2] = (float)pos.z;
	glm::quat q(glm::radians(glm::vec3((float)eulerDeg.x, (float)eulerDeg.y, (float)eulerDeg.z)));
	in.quat[0] = q.x; in.quat[1] = q.y; in.quat[2] = q.z; in.quat[3] = q.w;
	in.scale[0] = (float)scale.x; in.scale[1] = (float)scale.y; in.scale[2] = (float)scale.z;
	in.color[0] = in.color[1] = in.color[2] = in.color[3] = 1.0f;
	in.custom[0] = in.custom[1] = in.custom[2] = in.custom[3] = 0.0f;
	instances.push_back(in);
	MarkDirty();
	return (int)instances.size() - 1;
}

void InstancedMesh::SetInstancePos(int index, const Vector3& pos)
{
	EnsureDecoded();
	if (index < 0 || index >= (int)instances.size()) return;
	instances[index].pos[0] = (float)pos.x; instances[index].pos[1] = (float)pos.y; instances[index].pos[2] = (float)pos.z;
	MarkDirty();
}

void InstancedMesh::SetInstanceTint(int index, double r, double g, double b, double a)
{
	EnsureDecoded();
	if (index < 0 || index >= (int)instances.size()) return;
	Inst& in = instances[index];
	in.color[0] = (float)r; in.color[1] = (float)g; in.color[2] = (float)b; in.color[3] = (float)a;
	MarkDirty();
}

void InstancedMesh::SetInstanceCustom(int index, double x, double y, double z, double w)
{
	EnsureDecoded();
	if (index < 0 || index >= (int)instances.size()) return;
	Inst& in = instances[index];
	in.custom[0] = (float)x; in.custom[1] = (float)y; in.custom[2] = (float)z; in.custom[3] = (float)w;
	MarkDirty();
}

void InstancedMesh::RemoveInstance(int index)
{
	EnsureDecoded();
	if (index < 0 || index >= (int)instances.size()) return;
	instances.erase(instances.begin() + index);
	MarkDirty();
}

void InstancedMesh::ClearInstances()
{
	EnsureDecoded();
	instances.clear();
	data.clear();
	MarkDirty();
}

int InstancedMesh::InstanceCount() { EnsureDecoded(); return (int)instances.size(); }

// ---- render sync --------------------------------------------------------------------------

// Rebuild chunks + upload the packed instance records when anything changed: the instance set
// (dirty) or the ATOM's world transform (instances are local to the atom). Instances upload
// GROUPED BY SPATIAL CELL so a chunk is one contiguous [first, count) range; the authoring
// order (script indices) is never reordered — only the upload order is.
bool InstancedMesh::EnsureRenderReady(iRender* r)
{
	if (!r) return false;
	// HOT-APPLY asset resolution: re-resolve on any prop change (assign/replace/reset) —
	// a latched first state must never survive an inspector edit. The material is an OWNED
	// clone, so a swap tears the old instance down properly.
	if (meshGuid != meshGuidRes)
	{
		mesh = meshGuid.empty() ? nullptr : ResDB::getSingleton()->GetMesh(meshGuid);
		if (meshGuid.empty() || mesh) { meshGuidRes = meshGuid; dirty = true; }
	}
	if (matGuid != matGuidRes)
	{
		Material* asset = matGuid.empty() ? nullptr : ResDB::getSingleton()->GetMaterial(matGuid);
		if (matGuid.empty() || asset)
		{
			if (mat) { Reflect_DropObject(mat); delete mat; mat = nullptr; }
			if (asset) mat = asset->Clone();
			matGuidRes = matGuid;
		}
	}
	EnsureDecoded();
	if (mat) mat->receiveShadows = receiveShadows;   // component checkbox drives its OWNED clone
	if (cellSize != cellSizeRes) { cellSizeRes = cellSize; dirty = true; }   // live re-chunk on edit
	if (!mesh || instances.empty()) return false;

	// Atom world matrix (glm column-major; v' = M*v). Compare against the snapshot — an atom
	// that moved (platform carrying its scatter) re-uploads with the new composition.
	Transform& t = atom->GetTransform();
	Vector3 P = t.globalPosition(); Quaternion Q = t.globalRotation(); Vector3 S = t.globalScale();
	if (!ScaleWithAtom()) S = Vector3(1, 1, 1);   // scatter layers ignore the atom's scale (7.4)
	glm::mat4 aw = glm::translate(glm::mat4(1.0f), glm::vec3((float)P.x, (float)P.y, (float)P.z))
	             * glm::mat4_cast(glm::quat((float)Q.w, (float)Q.x, (float)Q.y, (float)Q.z))
	             * glm::scale(glm::mat4(1.0f), glm::vec3((float)S.x, (float)S.y, (float)S.z));
	bool moved = !hasLastWorld || memcmp(lastWorld, &aw[0][0], sizeof(lastWorld)) != 0;
	if (moved) { memcpy(lastWorld, &aw[0][0], sizeof(lastWorld)); hasLastWorld = true; }

	if (gpuBuf && gpuOwner != r) { gpuBuf = 0; gpuOwner = nullptr; dirty = true; }   // renderer changed (restart)
	if (!dirty && !moved && gpuBuf) return true;

	// Mesh-local bounds -> a conservative world radius per instance (chunk AABB expansion).
	mesh->EnsureBounds();
	float meshR = 1.0f;
	if (mesh->boundsValid)
	{
		const float* mn = mesh->aabbMin; const float* mx = mesh->aabbMax;
		float ex = 0.5f * (mx[0] - mn[0]), ey = 0.5f * (mx[1] - mn[1]), ez = 0.5f * (mx[2] - mn[2]);
		float cx = 0.5f * (mx[0] + mn[0]), cy = 0.5f * (mx[1] + mn[1]), cz = 0.5f * (mx[2] + mn[2]);
		meshR = sqrtf(ex * ex + ey * ey + ez * ez) + sqrtf(cx * cx + cy * cy + cz * cz);
	}
	const float atomScaleMax = (float)std::max({ fabs(S.x), fabs(S.y), fabs(S.z) });

	// Group instance indices by spatial cell (world position of the instance).
	const float cs = cellSize > 0.5f ? cellSize : 16.0f;
	std::map<std::tuple<int, int, int>, std::vector<int>> cells;
	std::vector<glm::vec3> wposCache(instances.size());
	for (int i = 0; i < (int)instances.size(); ++i)
	{
		const Inst& in = instances[i];
		glm::vec4 wp = aw * glm::vec4(in.pos[0], in.pos[1], in.pos[2], 1.0f);
		wposCache[i] = glm::vec3(wp);
		cells[{ (int)floorf(wp.x / cs), (int)floorf(wp.y / cs), (int)floorf(wp.z / cs) }].push_back(i);
	}

	// RT merged chunks (see header): bake each cell's instances into ONE Mesh in ATOM-LOCAL
	// space — a single TLAS entry per chunk. Built alongside the raster chunks so the two
	// can never drift; pooled Mesh objects are refilled in place (version bump re-uploads
	// GPU buffers and the BLAS lazily). Skipped entirely when RT is off for this set.
	// Merged chunks serve RT shadows AND reflections; the renderer bends them with the
	// NukeBend compute + BLAS refit every frame, so the rays hit SWAYING blades.
	const bool rtWanted = r->rtAvailable() && (castShadows || inReflections) && rtMaxInstances > 0 &&
	                      mesh->vertexArray && mesh->normalArray && mesh->uvArray;
	if (rtWanted)
		while (rtChunkMeshes.size() < cells.size())
		{
			Mesh* m = new Mesh();
			snprintf(m->name, sizeof(m->name), "rtchunk"); m->numVerts = 0;
			m->vertexArray = m->normalArray = m->uvArray = nullptr;
			rtChunkMeshes.push_back(m);
		}
	size_t rtCi = 0;

	// Pack records cell by cell; each instance's rows = HLSL rows of (atomWorld * local).
	std::vector<NukeInstanceData> packed;
	packed.reserve(instances.size());
	chunks.clear();
	for (auto& kv : cells)
	{
		if (rtWanted)
		{
			Mesh* rm = rtChunkMeshes[rtCi++];
			const int mv = mesh->numVerts, nv = (int)kv.second.size() * mv;
			if (rm->numVerts != nv)
			{
				delete[] rm->vertexArray; delete[] rm->normalArray; delete[] rm->uvArray;
				delete[] rm->rtBendArray; delete[] rm->rtPivotArray;
				rm->vertexArray  = new float[(size_t)nv * 3];
				rm->normalArray  = new float[(size_t)nv * 3];
				rm->uvArray      = new float[(size_t)nv * 2];
				rm->rtBendArray  = new float[(size_t)nv * 4];
				rm->rtPivotArray = new float[(size_t)nv * 4];
				rm->numVerts     = nv;
			}
			float* vp = rm->vertexArray; float* np = rm->normalArray; float* up = rm->uvArray;
			float* bp = rm->rtBendArray; float* pv2 = rm->rtPivotArray;
			for (int i : kv.second)
			{
				const Inst& in = instances[i];
				glm::quat q(in.quat[3], in.quat[0], in.quat[1], in.quat[2]);
				glm::vec3 tr(in.pos[0], in.pos[1], in.pos[2]), sc(in.scale[0], in.scale[1], in.scale[2]);
				for (int v = 0; v < mv; ++v)
				{
					glm::vec3 p(mesh->vertexArray[v * 3], mesh->vertexArray[v * 3 + 1], mesh->vertexArray[v * 3 + 2]);
					const float localY = p.y;   // blade-local height BEFORE the transform (bend gate)
					p = tr + q * (p * sc);
					*vp++ = p.x; *vp++ = p.y; *vp++ = p.z;
					glm::vec3 n(mesh->normalArray[v * 3], mesh->normalArray[v * 3 + 1], mesh->normalArray[v * 3 + 2]);
					n = q * n;   // rotation only (scale skew ignored — foliage scales ~uniformly)
					*np++ = n.x; *np++ = n.y; *np++ = n.z;
					*up++ = mesh->uvArray[v * 2]; *up++ = mesh->uvArray[v * 2 + 1];
					// Bend streams for the RT compute (bend.cs): gates from the instance custom
					// (z/w), pivot = the instance origin — the same phase hash the raster VS uses.
					*bp++ = localY; *bp++ = in.custom[2]; *bp++ = in.custom[3]; *bp++ = 0.0f;
					*pv2++ = tr.x; *pv2++ = tr.y; *pv2++ = tr.z; *pv2++ = 0.0f;
				}
			}
			rm->boundsValid = false;
			++rm->version;   // renderer re-uploads the buffers + rebuilds the BLAS on mismatch
		}

		Chunk c; c.first = (int)packed.size(); c.count = (int)kv.second.size();
		c.mn[0] = c.mn[1] = c.mn[2] = 1e30f; c.mx[0] = c.mx[1] = c.mx[2] = -1e30f;
		for (int i : kv.second)
		{
			const Inst& in = instances[i];
			glm::mat4 lw = glm::translate(glm::mat4(1.0f), glm::vec3(in.pos[0], in.pos[1], in.pos[2]))
			             * glm::mat4_cast(glm::quat(in.quat[3], in.quat[0], in.quat[1], in.quat[2]))
			             * glm::scale(glm::mat4(1.0f), glm::vec3(in.scale[0], in.scale[1], in.scale[2]));
			glm::mat4 w = aw * lw;
			NukeInstanceData rec;
			// HLSL row i = (w[0][i], w[1][i], w[2][i], w[3][i]) — columns of the glm matrix.
			for (int k = 0; k < 4; ++k) rec.row0[k] = w[k][0];
			for (int k = 0; k < 4; ++k) rec.row1[k] = w[k][1];
			for (int k = 0; k < 4; ++k) rec.row2[k] = w[k][2];
			memcpy(rec.color, in.color, sizeof(rec.color));
			memcpy(rec.custom, in.custom, sizeof(rec.custom));
			packed.push_back(rec);
			// Chunk AABB: instance world position padded by the mesh radius at this scale.
			const float sMax = std::max({ fabs(in.scale[0]), fabs(in.scale[1]), fabs(in.scale[2]) }) * atomScaleMax;
			const float pad = meshR * (sMax > 0.f ? sMax : 1.f);
			const glm::vec3& wp = wposCache[i];
			c.mn[0] = std::min(c.mn[0], wp.x - pad); c.mx[0] = std::max(c.mx[0], wp.x + pad);
			c.mn[1] = std::min(c.mn[1], wp.y - pad); c.mx[1] = std::max(c.mx[1], wp.y + pad);
			c.mn[2] = std::min(c.mn[2], wp.z - pad); c.mx[2] = std::max(c.mx[2], wp.z + pad);
		}
		chunks.push_back(c);
	}

	// Pool meshes beyond the live chunk count: zero them out and drop their renderer caches so
	// the RT gather skips them (kept allocated for reuse; freed for real in ReleaseRTChunks).
	for (size_t k = rtCi; k < rtChunkMeshes.size(); ++k)
		if (rtChunkMeshes[k] && rtChunkMeshes[k]->numVerts) { rtChunkMeshes[k]->numVerts = 0; r->invalidateMesh(rtChunkMeshes[k]); }

	if (!gpuBuf) { gpuBuf = r->createInstanceBuffer(); gpuOwner = r; }
	if (!gpuBuf) return false;
	r->updateInstanceBuffer(gpuBuf, packed.data(), (int)packed.size());
	dirty = false;
	return true;
}

}  // namespace nuke
