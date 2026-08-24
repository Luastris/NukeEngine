// World Partition streaming (see WorldStream.h). Also implements World::BakeStreamHlod —
// the split save's per-cell HLOD proxy bake (merged, grid-decimated, shading pre-baked into
// vertex color).
#include "API/Model/WorldStream.h"
#include "API/Model/World.h"
#include "API/Model/JsonDoc.h"
#include "API/Model/Atom.h"
#include "API/Model/Camera.h"
#include "API/Model/Mesh.h"
#include "API/Model/Material.h"
#include "API/Model/Shader.h"
#include "API/Model/Prefab.h"
#include "API/Model/Jobs.h"
#include "API/Model/resdb.h"
#include "interface/NUKEEInteface.h"
#include "render/irender.h"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

using json = nlohmann::json;

namespace nuke {

static constexpr double kUnloadFactor = 1.3;   // unload radius = streamRange * this (hysteresis)
static constexpr int    kParkBudget   = 8;     // roots parked per tick
static constexpr int    kRestoreBudget = 8;    // parked roots restored per tick
static constexpr int    kColdBudget   = 4;     // cold-file roots instantiated per tick

// Vertex-colored unlit proxy shader: shading is pre-baked into the colors at save time.
static const char* kHlodVS = R"NUKE(
// NUKE_VCOLOR: consumes the mesh color stream (baked proxy shading).
cbuffer CB { float4x4 g_WVP; float4x4 g_World; };
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; float4 col : ATTRIB3; };
struct PSIn { float4 pos : SV_POSITION; float4 vcol : TEXCOORD0; };
void main(in VSIn i, out PSIn o)
{
    o.pos  = mul(g_WVP, float4(i.pos, 1.0));
    o.vcol = i.col;
}
)NUKE";
static const char* kHlodPS = R"NUKE(
cbuffer MatCB {
#include "matcb_std.hlsli"
};
struct PSIn { float4 pos : SV_POSITION; float4 vcol : TEXCOORD0; };
float4 main(PSIn i) : SV_TARGET
{
    return float4(i.vcol.rgb, 1.0);
}
)NUKE";

// ---- membership ---------------------------------------------------------------------------------

WorldStream::CellKey WorldStream::CellOf(double x, double z, float cellSize)
{
	const double cs = std::max(8.0f, cellSize);
	return CellKey{ (int)std::floor(x / cs), (int)std::floor(z / cs) };
}

bool WorldStream::Spatial(Atom* root)
{
	if (!root || root->alwaysLoaded || root->persistent || root->folder) return false;
	if (root->name == "Editor Camera") return false;   // editor infra
	// Components with a world-spanning footprint (terrain) pin the atom: root-cell membership
	// would unload ground that is physically under a far-away player.
	for (Component* c : root->components)
		if (c && c->StreamGlobal()) return false;
	return true;
}

bool WorldStream::Active(World* w)
{
	if (!w || !w->settings.streamEnabled || w->auxiliary) return false;
	AppInstance* app = AppInstance::GetSingleton();
	if (app->isEditor() && app->playState == 0) return false;   // edit mode: everything loaded
	return true;
}

// ---- lifecycle ----------------------------------------------------------------------------------

WorldStream::~WorldStream() { Reset(); }

void WorldStream::Setup(const std::vector<CellKey>& fileCells, const std::string& dir)
{
	Reset();
	cellsDir = dir;
	for (const CellKey& k : fileCells) cells[k].fromFile = true;
}

void WorldStream::Reset()
{
	for (auto& kv : cells) FreeHlodMesh(kv.second);
	cells.clear();
	cellsDir.clear();
	coldReady.clear();
	hlodTried = false;
	++coldGen;   // orphan in-flight reads
	// hlodMat survives resets (shader/material are session-wide).
}

int WorldStream::LoadedCount() const
{
	int n = 0;
	for (const auto& kv : cells)
		if (kv.second.parked.empty() && (!kv.second.fromFile || kv.second.coldLoaded)) ++n;
	return n;
}

void WorldStream::DebugCells(std::vector<CellInfo>& out)
{
	AppInstance* app = AppInstance::GetSingleton();
	out.clear();
	out.reserve(cells.size() + loadedSet.size());
	for (auto& kv : cells)
	{
		Cell& c = kv.second;
		CellInfo ci;
		ci.key        = kv.first;
		ci.loaded     = loadedSet.count(kv.first) != 0;
		ci.fromFile   = c.fromFile;
		ci.coldLoaded = c.coldLoaded;
		ci.loading    = c.loading;
		ci.hlodDraw   = c.hlod.draw;
		ci.parked     = (int)c.parked.size();
		for (const std::string& s : c.parked) ci.parkedBytes += s.size();
		// Disk size once per session (raw project path; inside a pak the stat just yields 0).
		if (c.fileBytes == ~0ull)
		{
			c.fileBytes = 0;
			if (c.fromFile && !cellsDir.empty())
			{
				char nameBuf[64];
				std::snprintf(nameBuf, sizeof(nameBuf), "%d_%d.nuworld", kv.first.x, kv.first.z);
				boost::system::error_code ec;
				const uint64_t sz = (uint64_t)boost::filesystem::file_size(
					boost::filesystem::path(app->ResolveContent(cellsDir + "/" + nameBuf)), ec);
				if (!ec) c.fileBytes = sz;
			}
		}
		ci.fileBytes = c.fileBytes;
		out.push_back(ci);
	}
	// Active-set cells with nothing parked and no file never enter the map — still worth showing.
	for (const CellKey& k : loadedSet)
		if (!cells.count(k))
		{
			CellInfo ci;
			ci.key = k;
			ci.loaded = true;
			out.push_back(ci);
		}
}

// ---- streaming tick -----------------------------------------------------------------------------

// Streaming anchors in WORLD space: the game's main camera and (in the editor) the atom named
// "Editor Camera". Positions only.
static void StreamAnchors(World* w, std::vector<Vector3>& out)
{
	out.clear();
	if (Camera* cam = w->GetMainCamera())
		if (cam->transform) out.push_back(cam->transform->globalPosition());
	if (Atom* ed = w->Get("Editor Camera"))
		if (Camera* cam = ed->GetComponent<Camera>())
			if (cam->transform) out.push_back(cam->transform->globalPosition());
}

// XZ box distance from the nearest anchor to a cell.
static double CellDistance(const WorldStream::CellKey& k, float cellSize,
                           const std::vector<Vector3>& anchors)
{
	const double cs = std::max(8.0f, cellSize);
	const double x0 = k.x * cs, z0 = k.z * cs;
	double best = 1e30;
	for (const Vector3& a : anchors)
	{
		const double dx = std::max({ x0 - a.x, a.x - (x0 + cs), 0.0 });
		const double dz = std::max({ z0 - a.z, a.z - (z0 + cs), 0.0 });
		best = std::min(best, std::sqrt(dx * dx + dz * dz));
	}
	return best;
}

void WorldStream::Tick(World* w)
{
	AppInstance* app = AppInstance::GetSingleton();
	std::vector<Vector3> anchors;
	StreamAnchors(w, anchors);
	if (anchors.empty()) return;   // headless/loading: keep the world as-is
	const float cs = w->settings.streamCellSize;
	const double loadR = w->settings.streamRange;
	const double unloadR = loadR * kUnloadFactor;

	// HLOD bin: probe once per session (missing file = no proxies, no retries).
	if (!hlodTried && !cellsDir.empty())
	{
		hlodTried = true;
		LoadHlod(cellsDir + "/hlod.bin");
	}

	// 1) Park pass: live spatial roots whose cell fell out of the unload ring. Snapshot the
	// root list first — parking mutates the hierarchy.
	{
		std::vector<Atom*> roots;
		for (Atom* a : w->GetHierarchy()) roots.push_back(a);
		int budget = kParkBudget;
		for (Atom* a : roots)
		{
			if (budget <= 0) break;
			if (!Spatial(a)) continue;
			const Vector3 p = a->GetTransform().globalPosition();
			const CellKey key = CellOf(p.x, p.z, cs);
			Cell& c = cells[key];   // dynamic discovery: spawned/moved atoms register their cell
			if (CellDistance(key, cs, anchors) <= unloadR) continue;
			c.parked.push_back(SaveAtomToString(a));
			w->RemoveAtomById((long)a->id.id);
			--budget;
		}
	}

	// 2) Restore pass: parked subtrees whose cell re-entered the load ring.
	{
		int budget = kRestoreBudget;
		for (auto& kv : cells)
		{
			if (budget <= 0) break;
			Cell& c = kv.second;
			if (c.parked.empty()) continue;
			if (CellDistance(kv.first, cs, anchors) > loadR) continue;
			while (budget > 0 && !c.parked.empty())
			{
				Atom* a = LoadAtomFromString(c.parked.back());
				c.parked.pop_back();
				if (a) w->Add(a);
				--budget;
			}
			if (c.parked.empty()) FreeHlodMesh(c);
		}
	}

	// 3) Cold loads: cells listed in the index whose file was never pulled this session.
	for (auto& kv : cells)
	{
		Cell& c = kv.second;
		if (!c.fromFile || c.coldLoaded || c.loading) continue;
		if (CellDistance(kv.first, cs, anchors) > loadR) continue;
		c.loading = true;
		char nameBuf[64];
		std::snprintf(nameBuf, sizeof(nameBuf), "%d_%d.nuworld", kv.first.x, kv.first.z);
		const std::string rel = cellsDir + "/" + nameBuf;
		const CellKey key = kv.first;
		const uint32_t my = coldGen;
		auto data = std::make_shared<std::string>();
		WorldStream* self = this;
		Jobs::Schedule([data, rel]()
		{
			if (Jobs::Stopping()) return;
			std::string s;
			if (AppInstance::GetSingleton()->ReadContent(rel, s)) data->swap(s);
		})
		.Then([self, key, my, data]()
		{
			// Liveness: the world (and its stream) may have been replaced while the read ran.
			AppInstance* app2 = AppInstance::GetSingleton();
			if (!app2->currentWorld || app2->currentWorld->stream != self) return;
			if (self->coldGen != my) return;
			self->coldReady.push_back(ColdIn{ key, data });
		});
	}

	// 4) Apply landed cold reads (budgeted instantiation; a big cell spreads over frames).
	{
		int budget = kColdBudget;
		while (budget > 0 && !coldReady.empty())
		{
			ColdIn& in = coldReady.front();
			auto it = cells.find(in.key);
			if (it == cells.end()) { coldReady.erase(coldReady.begin()); continue; }
			Cell& c = it->second;
			if (!in.data || in.data->empty())
			{
				std::cout << "[World]\t\t\t" << "stream: cell " << in.key.x << "_" << in.key.z
				          << " file missing/empty" << std::endl;
				c.loading = false; c.coldLoaded = true;   // don't retry a broken file every tick
				coldReady.erase(coldReady.begin());
				continue;
			}
			json cj = ParseDoc(*in.data);
			if (cj.is_discarded() || !cj.contains("atoms") || !cj["atoms"].is_array())
			{
				std::cout << "[World]\t\t\t" << "stream: cell " << in.key.x << "_" << in.key.z
				          << " is not valid JSON" << std::endl;
				c.loading = false; c.coldLoaded = true;
				coldReady.erase(coldReady.begin());
				continue;
			}
			// Instantiate up to the budget; keep the rest for the next tick by re-serializing
			// the remainder into parked strings (uniform path, no partial-doc state).
			json& arr = cj["atoms"];
			int done = 0;
			for (json& aj : arr)
			{
				if (done >= budget) break;
				w->AddAtomFromJson(aj);
				++done;
			}
			budget -= done;
			for (size_t i = (size_t)done; i < arr.size(); ++i) c.parked.push_back(arr[i].dump());
			c.loading = false;
			c.coldLoaded = true;
			FreeHlodMesh(c);
			w->FinalizeIncrementalLoad();
			coldReady.erase(coldReady.begin());
		}
	}

	// 5) Proxy visibility for Render: unloaded cells inside the HLOD range.
	const double hlodR = w->settings.streamHlodRange;
	for (auto& kv : cells)
	{
		Cell& c = kv.second;
		const bool unloaded = !c.parked.empty() || (c.fromFile && !c.coldLoaded);
		c.hlod.draw = unloaded && !c.hlod.verts.empty()
		           && (hlodR <= 0.0 || CellDistance(kv.first, cs, anchors) <= hlodR);
	}
	(void)app;
}

// ---- resident atoms (snapshot completeness) -----------------------------------------------------

void WorldStream::AppendResident(std::vector<std::string>& out)
{
	AppInstance* app = AppInstance::GetSingleton();
	for (auto& kv : cells)
	{
		const Cell& c = kv.second;
		for (const std::string& s : c.parked) out.push_back(s);
		if (c.fromFile && !c.coldLoaded)
		{
			// Never loaded this session: the file IS the cell's state.
			char nameBuf[64];
			std::snprintf(nameBuf, sizeof(nameBuf), "%d_%d.nuworld", kv.first.x, kv.first.z);
			std::string data;
			if (!app->ReadContent(cellsDir + "/" + nameBuf, data)) continue;
			json cj = ParseDoc(data);
			if (cj.is_discarded() || !cj.contains("atoms")) continue;
			for (const json& aj : cj["atoms"]) out.push_back(aj.dump());
		}
	}
}

// ---- HLOD proxies -------------------------------------------------------------------------------

static constexpr char kHlodMagic[8] = { 'N', 'U', 'H', 'L', 'O', 'D', '0', '1' };

bool WorldStream::LoadHlod(const std::string& contentRelPath)
{
	std::string data;
	if (!AppInstance::GetSingleton()->ReadContent(contentRelPath, data)) return false;
	if (data.size() < 12 || std::memcmp(data.data(), kHlodMagic, 8) != 0) return false;
	const char* p = data.data() + 8;
	const char* end = data.data() + data.size();
	auto rd = [&](void* dst, size_t n) -> bool
	{ if ((size_t)(end - p) < n) return false; std::memcpy(dst, p, n); p += n; return true; };
	uint32_t count = 0;
	if (!rd(&count, 4)) return false;
	int loaded = 0;
	for (uint32_t i = 0; i < count; ++i)
	{
		int32_t x = 0, z = 0; uint32_t nv = 0;
		if (!rd(&x, 4) || !rd(&z, 4) || !rd(&nv, 4)) return false;
		Cell& c = cells[CellKey{ x, z }];
		c.hlod.verts.resize((size_t)nv * 3);
		c.hlod.colors.resize((size_t)nv * 4);
		if (!rd(c.hlod.verts.data(), c.hlod.verts.size() * 4)) return false;
		if (!rd(c.hlod.colors.data(), c.hlod.colors.size() * 4)) return false;
		++loaded;
	}
	std::cout << "[World]\t\t\t" << "stream: HLOD proxies for " << loaded << " cells" << std::endl;
	return true;
}

void WorldStream::EnsureHlodMat()
{
	if (hlodMat) return;
	// The proxy shader registers as an ordinary Shader asset (same path as module shaders).
	if (!ResDB::getSingleton()->GetShader("hlodproxy"))
	{
		Shader* s = Shader::FromSources("hlodproxy", kHlodVS, kHlodPS);
		if (s)
		{
			s->guid = "hlodproxy";
			ResDB::getSingleton()->RegisterShader(s);
		}
	}
	hlodMat = new Material();
	hlodMat->matName = "hlod-proxy";
	hlodMat->shaderGuid = "hlodproxy";
	hlodMat->castShadows = false;
	hlodMat->Resolve();
}

void WorldStream::EnsureHlodMesh(Cell& c)
{
	if (c.hlod.mesh || c.hlod.verts.empty()) return;
	Mesh* m = new Mesh();
	std::snprintf(m->name, sizeof(m->name), "hlod-cell");
	const int nv = (int)(c.hlod.verts.size() / 3);
	m->vertexArray = new float[c.hlod.verts.size()];
	std::memcpy(m->vertexArray, c.hlod.verts.data(), c.hlod.verts.size() * sizeof(float));
	m->normalArray = new float[c.hlod.verts.size()];
	for (int i = 0; i < nv; ++i)
	{ m->normalArray[i * 3] = 0; m->normalArray[i * 3 + 1] = 1; m->normalArray[i * 3 + 2] = 0; }
	m->colorArray = new float[c.hlod.colors.size()];
	std::memcpy(m->colorArray, c.hlod.colors.data(), c.hlod.colors.size() * sizeof(float));
	m->uvArray = nullptr;
	m->numVerts = nv;
	m->numIndices = 0;
	m->boundsValid = false;
	c.hlod.mesh = m;
}

void WorldStream::FreeHlodMesh(Cell& c)
{
	if (!c.hlod.mesh) return;
	if (iRender* r = AppInstance::GetSingleton()->render) r->invalidateMesh(c.hlod.mesh);
	delete c.hlod.mesh;
	c.hlod.mesh = nullptr;
}

void WorldStream::Render(World* w, iRender* r)
{
	if (!r) return;
	(void)w;
	static const float pos[3] = { 0, 0, 0 };
	static const float quat[4] = { 0, 0, 0, 1 };
	static const float scale[3] = { 1, 1, 1 };
	for (auto& kv : cells)
	{
		Cell& c = kv.second;
		if (!c.hlod.draw) continue;
		EnsureHlodMat();
		EnsureHlodMesh(c);
		if (c.hlod.mesh) r->renderObject(c.hlod.mesh, hlodMat, pos, quat, scale);
	}
}

// ---- the bake (split save) ----------------------------------------------------------------------

namespace {
// World-space TRS chain composed while walking the atom JSON (world = parentWorld * local).
struct Xf
{
	Vector3 pos{ 0, 0, 0 };
	Quaternion rot = Quaternion::Identity();
	Vector3 scale{ 1, 1, 1 };
	Vector3 Apply(const Vector3& v) const
	{
		const Vector3 s(v.x * scale.x, v.y * scale.y, v.z * scale.z);
		const Vector3 rv = rot.Rotate(s);
		return Vector3(rv.x + pos.x, rv.y + pos.y, rv.z + pos.z);
	}
	Vector3 ApplyDir(const Vector3& v) const { return rot.Rotate(v); }
};
Xf ComposeXf(const Xf& parent, const json& t)
{
	Xf l;
	if (t.is_object())
	{
		if (t.contains("position") && t["position"].is_array() && t["position"].size() == 3)
			l.pos = Vector3(t["position"][0], t["position"][1], t["position"][2]);
		if (t.contains("rotation") && t["rotation"].is_array() && t["rotation"].size() == 4)
			l.rot = Quaternion(t["rotation"][0], t["rotation"][1], t["rotation"][2], t["rotation"][3]);
		if (t.contains("scale") && t["scale"].is_array() && t["scale"].size() == 3)
			l.scale = Vector3(t["scale"][0], t["scale"][1], t["scale"][2]);
	}
	Xf w;
	const Vector3 sp(l.pos.x * parent.scale.x, l.pos.y * parent.scale.y, l.pos.z * parent.scale.z);
	const Vector3 rp = parent.rot.Rotate(sp);
	w.pos = Vector3(rp.x + parent.pos.x, rp.y + parent.pos.y, rp.z + parent.pos.z);
	Quaternion pr = parent.rot;   // operator* is non-const
	w.rot = pr * l.rot;
	w.scale = Vector3(parent.scale.x * l.scale.x, parent.scale.y * l.scale.y, parent.scale.z * l.scale.z);
	return w;
}

struct BakeAcc
{
	std::vector<float> verts;    // 3/vert (triangle soup, world space)
	std::vector<float> colors;   // 3/vert (lit color)
};

// Append one MeshRenderer's triangles: world-space positions, color = material tint with a
// fixed-sun lambert pre-baked (the proxy shader is unlit).
void BakeMesh(BakeAcc& acc, Mesh* mesh, const Xf& xf, const Vector3& tint)
{
	if (!mesh || !mesh->vertexArray || mesh->numVerts < 3) return;
	const Vector3 sun = Vector3(0.35, 0.85, 0.4);
	const double sunLen = std::sqrt(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
	const int triCount = mesh->TriCount();   // LOD0 only — never the appended LOD shells
	for (int tri = 0; tri < triCount; ++tri)
	{
		Vector3 wp[3];
		for (int k = 0; k < 3; ++k)
		{
			const uint32_t vi = mesh->TriIndex(tri, k);
			if ((int)vi >= mesh->numVerts) return;   // malformed
			const float* v = mesh->vertexArray + (size_t)vi * 3;
			wp[k] = xf.Apply(Vector3(v[0], v[1], v[2]));
		}
		// Face normal for the baked lambert.
		const Vector3 e1(wp[1].x - wp[0].x, wp[1].y - wp[0].y, wp[1].z - wp[0].z);
		const Vector3 e2(wp[2].x - wp[0].x, wp[2].y - wp[0].y, wp[2].z - wp[0].z);
		Vector3 n(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
		const double nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		if (nl > 1e-12) { n.x /= nl; n.y /= nl; n.z /= nl; }
		const double ndl = std::max(0.0, (n.x * sun.x + n.y * sun.y + n.z * sun.z) / sunLen);
		const double lit = 0.45 + 0.55 * ndl;
		for (int k = 0; k < 3; ++k)
		{
			acc.verts.push_back((float)wp[k].x);
			acc.verts.push_back((float)wp[k].y);
			acc.verts.push_back((float)wp[k].z);
			acc.colors.push_back((float)(tint.x * lit));
			acc.colors.push_back((float)(tint.y * lit));
			acc.colors.push_back((float)(tint.z * lit));
		}
	}
}

void BakeAtomJson(BakeAcc& acc, const json& a, const Xf& parent)
{
	if (!a.is_object()) return;
	const Xf xf = ComposeXf(parent, a.contains("transform") ? a["transform"] : json());
	if (a.contains("components") && a["components"].is_array())
		for (const json& cj : a["components"])
		{
			if (cj.value("type", std::string()) != "MeshRenderer") continue;
			if (!cj.value("enabled", true)) continue;
			const json props = cj.contains("props") ? cj["props"] : json::object();
			Mesh* mesh = ResDB::getSingleton()->GetMesh(props.value("meshGuid", std::string()));
			Vector3 tint(1, 1, 1);
			if (cj.contains("material") && cj["material"].is_object()
			    && cj["material"].contains("color") && cj["material"]["color"].is_array()
			    && cj["material"]["color"].size() == 4)
				tint = Vector3(cj["material"]["color"][0], cj["material"]["color"][1], cj["material"]["color"][2]);
			else if (Material* m = ResDB::getSingleton()->GetMaterial(props.value("matGuid", std::string())))
				tint = Vector3(m->color.r, m->color.g, m->color.b);
			BakeMesh(acc, mesh, xf, tint);
		}
	if (a.contains("children") && a["children"].is_array())
		for (const json& ch : a["children"]) BakeAtomJson(acc, ch, xf);
}

// Grid vertex clustering: triangles whose corners land in 3 distinct clusters survive with
// cluster-averaged positions/colors — a cheap, robust decimation for far proxies.
void Decimate(const BakeAcc& in, double cluster, std::vector<float>& verts, std::vector<float>& cols)
{
	struct CK { int64_t x, y, z;
		bool operator<(const CK& o) const
		{ return x != o.x ? x < o.x : (y != o.y ? y < o.y : z < o.z); }
		bool operator==(const CK& o) const { return x == o.x && y == o.y && z == o.z; } };
	std::map<CK, std::pair<Vector3, Vector3>> sum;   // pos sum, color sum
	std::map<CK, int> cnt;
	auto keyOf = [&](const float* p) -> CK
	{ return CK{ (int64_t)std::floor(p[0] / cluster), (int64_t)std::floor(p[1] / cluster),
	             (int64_t)std::floor(p[2] / cluster) }; };
	const size_t nv = in.verts.size() / 3;
	for (size_t i = 0; i < nv; ++i)
	{
		const CK k = keyOf(&in.verts[i * 3]);
		auto& s = sum[k];
		s.first.x += in.verts[i * 3]; s.first.y += in.verts[i * 3 + 1]; s.first.z += in.verts[i * 3 + 2];
		s.second.x += in.colors[i * 3]; s.second.y += in.colors[i * 3 + 1]; s.second.z += in.colors[i * 3 + 2];
		cnt[k]++;
	}
	for (size_t tri = 0; tri * 3 + 2 < nv; ++tri)
	{
		CK k[3];
		for (int c = 0; c < 3; ++c) k[c] = keyOf(&in.verts[(tri * 3 + c) * 3]);
		if (k[0] == k[1] || k[1] == k[2] || k[0] == k[2]) continue;   // collapsed
		for (int c = 0; c < 3; ++c)
		{
			const auto& s = sum[k[c]];
			const double n = (double)cnt[k[c]];
			verts.push_back((float)(s.first.x / n));
			verts.push_back((float)(s.first.y / n));
			verts.push_back((float)(s.first.z / n));
			cols.push_back((float)(s.second.x / n));
			cols.push_back((float)(s.second.y / n));
			cols.push_back((float)(s.second.z / n));
			cols.push_back(1.0f);
		}
	}
}
}  // namespace

void World::BakeStreamHlod(const std::map<std::pair<int, int>, nlohmann::json>& cellAtoms,
                           const std::string& binPath)
{
	const double cluster = std::max(0.5, (double)settings.streamCellSize / 48.0);
	std::string blob;
	blob.append(kHlodMagic, 8);
	uint32_t count = 0;
	blob.append(4, '\0');   // patched below
	size_t totalVerts = 0;
	for (const auto& kv : cellAtoms)
	{
		BakeAcc acc;
		Xf root;
		if (kv.second.is_array())
			for (const json& a : kv.second) BakeAtomJson(acc, a, root);
		std::vector<float> verts, cols;
		Decimate(acc, cluster, verts, cols);
		if (verts.empty()) continue;
		const int32_t x = kv.first.first, z = kv.first.second;
		const uint32_t nv = (uint32_t)(verts.size() / 3);
		blob.append((const char*)&x, 4);
		blob.append((const char*)&z, 4);
		blob.append((const char*)&nv, 4);
		blob.append((const char*)verts.data(), verts.size() * 4);
		blob.append((const char*)cols.data(), cols.size() * 4);
		++count;
		totalVerts += nv;
	}
	std::memcpy(&blob[8], &count, 4);
	boost::filesystem::ofstream f(boost::filesystem::path(binPath), std::ios::binary | std::ios::trunc);
	if (f) f.write(blob.data(), (std::streamsize)blob.size());
	std::cout << "[World]\t\t\t" << "HLOD baked: " << count << " cells, " << totalVerts
	          << " proxy verts -> " << binPath << std::endl;
}

}  // namespace nuke
