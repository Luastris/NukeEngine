#include "API/Model/Surface.h"
#include <set>
#include "API/Model/Atom.h"
#include "API/Model/Transform.h"
#include "API/Model/World.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Material.h"
#include "API/Model/Foliage.h"
#include "API/Model/Audio.h"
#include "API/Model/AudioSource.h"
#include "API/Model/Decal.h"
#include "API/Model/Physics.h"
#include "API/Model/Camera.h"
#include "API/Model/CharacterController.h"
#include "API/Model/Collider.h"
#include "API/Model/Prefab.h"
#include "API/Model/Rigidbody.h"
#include "API/Model/Wind.h"
#include "API/Model/Time.h"
#include "interface/AppInstance.h"   // renderer access: invalidate the rebuilt mask flipbook
#include "API/Model/resdb.h"
#include "reflect/ReflectBind.h"     // terrain layer material via reflection (no module link)
#include <render/irender.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>


namespace nuke {

// ---- global condition table + live component registries -----------------------------------
static std::map<std::string, float>       g_conditions;
static std::set<std::string>              g_condSky;   // conditions that come from the sky (renderer-gated)
static std::map<Atom*, SurfaceState*>     g_states;   // one override per atom (last Init wins)
static std::vector<SurfaceMask*>          g_masks;

// ---- base64 (blob codec, same shape as InstancedMesh's) -----------------------------------
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
		if (rev[c] < 0) continue;
		acc = (acc << 6) | rev[c]; bits += 6;
		if (bits >= 8) { bits -= 8; out.push_back((unsigned char)((acc >> bits) & 0xFF)); }
	}
	return out;
}

// ---- SurfaceState -------------------------------------------------------------------------

void SurfaceState::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	Surface::Register(this);
}

void SurfaceState::Destroy() { Surface::Unregister(this); }

void SurfaceState::SetState(const std::string& state, double value)
{
	for (size_t i = 0; i < states.size(); ++i)
		if (states[i] == state)
		{
			if (i < values.size()) values[i] = (float)value; else values.resize(i + 1, (float)value);
			return;
		}
	states.push_back(state);
	values.resize(states.size(), 0.0f);
	values.back() = (float)value;
}

void SurfaceState::ClearState(const std::string& state)
{
	for (size_t i = 0; i < states.size(); ++i)
		if (states[i] == state)
		{
			states.erase(states.begin() + i);
			if (i < values.size()) values.erase(values.begin() + i);
			return;
		}
}

bool SurfaceState::Value(const std::string& state, float& out) const
{
	for (size_t i = 0; i < states.size(); ++i)
		if (states[i] == state) { out = i < values.size() ? values[i] : 0.0f; return true; }
	return false;
}

// ---- SurfaceMask --------------------------------------------------------------------------

void SurfaceMask::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	Surface::Register(this);
}

void SurfaceMask::Destroy()
{
	Surface::Unregister(this);
	if (gpuTex)
	{
		if (AppInstance* app = AppInstance::GetSingleton())
			if (app->render) app->render->invalidateTexture(gpuTex);
		delete gpuTex;
		gpuTex = nullptr;
	}
}

// Grid -> 2D flipbook (Z slabs side by side): pixel(z*res + x, y) = grid[z][y][x]. The shader
// reverses this mapping and lerps between the two nearest slabs (manual trilinear Z).
Texture* SurfaceMask::GpuTex()
{
	EnsureDecoded();
	const int res = resolution;
	if (!gpuTex)
	{
		gpuTex = new Texture();
		gpuTex->guid  = "mask3d:" + std::to_string((unsigned long long)(uintptr_t)this);
		snprintf(gpuTex->name, sizeof(gpuTex->name), "surface-mask3d");
		gpuTex->usage = Texture::UsageData;
		gpuVersion = version - 1;
	}
	if (gpuVersion != version)
	{
		gpuTex->width = res * res; gpuTex->height = res;
		gpuTex->format = Texture::FMT_RGBA8; gpuTex->mipCount = 1; gpuTex->frameCount = 1;
		gpuTex->pixels.resize((size_t)res * res * res * 4);
		for (int z = 0; z < res; ++z)
			for (int y = 0; y < res; ++y)
				std::memcpy(&gpuTex->pixels[(((size_t)y * res * res) + (size_t)z * res) * 4],
				            &grid[(((size_t)z * res + y) * res) * 4], (size_t)res * 4);
		// The rebuilt pixels must reach the GPU before the next sample.
		if (AppInstance* app = AppInstance::GetSingleton())
			if (app->render) app->render->invalidateTexture(gpuTex);
		gpuVersion = version;
	}
	return gpuTex;
}

void SurfaceMask::EnsureDecoded()
{
	if (resolution < 4) resolution = 4;
	if (resolution > 64) resolution = 64;
	const size_t need = (size_t)resolution * resolution * resolution * 4;
	if (!decoded)
	{
		decoded = true;
		if (!data.empty())
		{
			std::vector<unsigned char> bytes = B64Decode(data);
			if (bytes.size() == need) grid = std::move(bytes);
		}
	}
	if (grid.size() != need) { grid.assign(need, 0); ++version; }   // fresh or resolution changed
}

void SurfaceMask::OnBeforeSave()
{
	EnsureDecoded();
	// All-zero grids save as an empty blob (the mask exists but paints nothing yet).
	bool any = false;
	for (unsigned char b : grid) if (b) { any = true; break; }
	data = any ? B64Encode(grid.data(), grid.size()) : std::string();
}

// World -> continuous cell coordinates (0..res over the local box); false = outside the box.
bool SurfaceMask::WorldToCell(const Vector3& worldPos, float& cx, float& cy, float& cz) const
{
	if (!transform) return false;
	Vector3 gp = transform->globalPosition();
	Quaternion gr = transform->globalRotation();
	Vector3 gs = transform->globalScale();
	Vector3 d(worldPos.x - gp.x, worldPos.y - gp.y, worldPos.z - gp.z);
	Vector3 l = gr.conjugate().Rotate(d);
	if (std::abs(gs.x) > 1e-9) l.x /= gs.x;
	if (std::abs(gs.y) > 1e-9) l.y /= gs.y;
	if (std::abs(gs.z) > 1e-9) l.z /= gs.z;
	const double hx = halfExtents.x, hy = halfExtents.y, hz = halfExtents.z;
	if (hx <= 0 || hy <= 0 || hz <= 0) return false;
	if (l.x < -hx || l.x > hx || l.y < -hy || l.y > hy || l.z < -hz || l.z > hz) return false;
	cx = (float)((l.x + hx) / (2.0 * hx) * resolution);
	cy = (float)((l.y + hy) / (2.0 * hy) * resolution);
	cz = (float)((l.z + hz) / (2.0 * hz) * resolution);
	return true;
}

int SurfaceMask::StateSlot(const std::string& state) const
{
	if (state.empty()) return -1;
	if (state == state0) return 0;
	if (state == state1) return 1;
	if (state == state2) return 2;
	if (state == state3) return 3;
	return -1;
}

void SurfaceMask::Paint(const Vector3& worldPos, double radius, double channel, double amount)
{
	const int ch = (int)channel;
	if (ch < 0 || ch > 3 || radius <= 0.0 || !transform) return;
	EnsureDecoded();
	Vector3 gs = transform->globalScale();
	// Brush radius in CELLS per axis (world radius over the scaled cell size).
	const double sx = std::abs(gs.x) > 1e-9 ? std::abs(gs.x) : 1.0;
	const double sy = std::abs(gs.y) > 1e-9 ? std::abs(gs.y) : 1.0;
	const double sz = std::abs(gs.z) > 1e-9 ? std::abs(gs.z) : 1.0;
	const double rx = radius / (2.0 * halfExtents.x * sx / resolution);
	const double ry = radius / (2.0 * halfExtents.y * sy / resolution);
	const double rz = radius / (2.0 * halfExtents.z * sz / resolution);
	float cx, cy, cz;
	// The brush centre may sit OUTSIDE the box while its sphere still overlaps it: clamp-free
	// mapping first, then walk the overlapped cell range.
	{
		Vector3 gp = transform->globalPosition();
		Quaternion gr = transform->globalRotation();
		Vector3 d(worldPos.x - gp.x, worldPos.y - gp.y, worldPos.z - gp.z);
		Vector3 l = gr.conjugate().Rotate(d);
		l.x /= sx; l.y /= sy; l.z /= sz;
		cx = (float)((l.x + halfExtents.x) / (2.0 * halfExtents.x) * resolution);
		cy = (float)((l.y + halfExtents.y) / (2.0 * halfExtents.y) * resolution);
		cz = (float)((l.z + halfExtents.z) / (2.0 * halfExtents.z) * resolution);
	}
	const int x0 = std::max(0, (int)std::floor(cx - rx)), x1 = std::min(resolution - 1, (int)std::ceil(cx + rx));
	const int y0 = std::max(0, (int)std::floor(cy - ry)), y1 = std::min(resolution - 1, (int)std::ceil(cy + ry));
	const int z0 = std::max(0, (int)std::floor(cz - rz)), z1 = std::min(resolution - 1, (int)std::ceil(cz + rz));
	if (x0 > x1 || y0 > y1 || z0 > z1) return;
	bool touched = false;
	for (int z = z0; z <= z1; ++z)
		for (int y = y0; y <= y1; ++y)
			for (int x = x0; x <= x1; ++x)
			{
				// Normalized distance to the brush centre (per-axis radii = ellipsoid in cells).
				const double dx = (x + 0.5 - cx) / (rx > 1e-9 ? rx : 1e-9);
				const double dy = (y + 0.5 - cy) / (ry > 1e-9 ? ry : 1e-9);
				const double dz = (z + 0.5 - cz) / (rz > 1e-9 ? rz : 1e-9);
				const double d2 = dx * dx + dy * dy + dz * dz;
				if (d2 > 1.0) continue;
				const double fall = 1.0 - std::sqrt(d2);           // linear falloff to the edge
				unsigned char& c = grid[(((size_t)z * resolution + y) * resolution + x) * 4 + ch];
				double v = c / 255.0 + amount * fall;
				if (v < 0.0) v = 0.0; if (v > 1.0) v = 1.0;
				c = (unsigned char)(v * 255.0 + 0.5);
				touched = true;
			}
	if (touched) ++version;
}

void SurfaceMask::Clear()
{
	EnsureDecoded();
	std::fill(grid.begin(), grid.end(), (unsigned char)0);
	++version;
}

double SurfaceMask::SampleChannel(const Vector3& worldPos, double channel)
{
	const int ch = (int)channel;
	if (ch < 0 || ch > 3) return 0.0;
	EnsureDecoded();
	float cx, cy, cz;
	if (!WorldToCell(worldPos, cx, cy, cz)) return 0.0;
	// Trilinear over cell centres, clamped to the grid edge.
	const float fx = std::min(std::max(cx - 0.5f, 0.0f), (float)resolution - 1.001f);
	const float fy = std::min(std::max(cy - 0.5f, 0.0f), (float)resolution - 1.001f);
	const float fz = std::min(std::max(cz - 0.5f, 0.0f), (float)resolution - 1.001f);
	const int x = (int)fx, y = (int)fy, z = (int)fz;
	const float tx = fx - x, ty = fy - y, tz = fz - z;
	auto at = [&](int xi, int yi, int zi) -> float
	{
		xi = std::min(xi, resolution - 1); yi = std::min(yi, resolution - 1); zi = std::min(zi, resolution - 1);
		return grid[(((size_t)zi * resolution + yi) * resolution + xi) * 4 + ch] / 255.0f;
	};
	const float v00 = at(x, y, z)     + (at(x + 1, y, z)     - at(x, y, z))     * tx;
	const float v10 = at(x, y + 1, z) + (at(x + 1, y + 1, z) - at(x, y + 1, z)) * tx;
	const float v01 = at(x, y, z + 1)     + (at(x + 1, y, z + 1)     - at(x, y, z + 1))     * tx;
	const float v11 = at(x, y + 1, z + 1) + (at(x + 1, y + 1, z + 1) - at(x, y + 1, z + 1)) * tx;
	const float v0 = v00 + (v10 - v00) * ty, v1 = v01 + (v11 - v01) * ty;
	return v0 + (v1 - v0) * tz;
}

float SurfaceMask::Sample(const Vector3& worldPos, const std::string& state)
{
	const int slot = StateSlot(state);
	return slot < 0 ? 0.0f : (float)SampleChannel(worldPos, slot);
}

// ---- Surface facade -----------------------------------------------------------------------

void Surface::SetCondition(const std::string& state, double value)
{
	if (state.empty()) return;
	if (value <= 0.0) { g_conditions.erase(state); g_condSky.erase(state); return; }
	g_conditions[state] = (float)std::min(value, 1.0);
}

void Surface::SetConditionSky(const std::string& state, bool fromSky)
{
	if (state.empty()) return;
	if (fromSky) g_condSky.insert(state); else g_condSky.erase(state);
}
bool Surface::ConditionFromSky(const std::string& state) { return g_condSky.count(state) != 0; }

// ---- ground trails --------------------------------------------------------------------------
static double g_trailFill = 0.0;
void   Surface::SetTrailFill(double perSecond) { g_trailFill = std::max(perSecond, 0.0); }
double Surface::TrailFill() { return g_trailFill; }

// Footprints of grounded bodies: a CharacterController standing on ground, or a Collider body
// (box/sphere/capsule: its bounding disc) whose bottom rests within a step of the ground below
// (raycast down, its own body ignored). Nearest to the camera first: the renderer takes what
// its stamp buffer holds. Only while a sky-borne layer exists - nothing to carve otherwise.
static void GatherTrails(bc::list<Atom*>& list, const Vector3& cam, std::vector<std::pair<float, std::array<float, 4>>>& out)
{
	for (Atom* a : list)
	{
		if (!a) continue;
		if (!a->children.empty()) GatherTrails(a->children, cam, out);
		Transform& tf = a->GetTransform();
		const Vector3 P = tf.globalPosition();
		const Vector3 scl = tf.globalScale();
		const float sxz = (float)std::max(std::fabs(scl.x), std::fabs(scl.z)), sy = (float)std::fabs(scl.y);
		float radius = 0.0f, halfH = 0.0f; Vector3 center = P; bool grounded = false;
		if (CharacterController* cc = a->GetComponent<CharacterController>())
		{
			if (!cc->enabled) continue;
			radius = std::max(0.05f, cc->radius * sxz);
			halfH  = std::max(0.2f, cc->height * sy) * 0.5f;
			center = Vector3(P.x + cc->capsuleOffset.x * scl.x, P.y + cc->capsuleOffset.y * scl.y + (cc->pivot == 0 ? halfH : 0.0f), P.z + cc->capsuleOffset.z * scl.z);
			grounded = cc->IsGrounded();
		}
		else if (Collider* col = a->GetComponent<Collider>())
		{
			if (!col->enabled || col->shape == Collider::S_Mesh || !a->GetComponent<Rigidbody>()) continue;
			if (col->shape == Collider::S_Sphere)       { radius = col->radius * sxz; halfH = col->radius * sy; }
			else if (col->shape == Collider::S_Capsule) { radius = col->radius * sxz; halfH = (float)(col->halfHeight * sy + col->radius * sy); }
			else { radius = (float)std::max(std::fabs(col->halfExtents.x * scl.x), std::fabs(col->halfExtents.z * scl.z)); halfH = (float)std::fabs(col->halfExtents.y * scl.y); }
			if (Physics::Available() && Physics::RaycastIgnore(center, Vector3(0, -1, 0), halfH + 0.12, a)) grounded = true;
		}
		else continue;
		if (!grounded || radius <= 0.0f) continue;
		const float dx = (float)(center.x - cam.x), dz = (float)(center.z - cam.z);
		out.push_back({ dx * dx + dz * dz, { (float)center.x, (float)center.z, radius, 1.0f } });
	}
}

void Surface::PushTrails(World* w, iRender* r)
{
	if (!w || !r) return;
	static std::vector<std::pair<float, std::array<float, 4>>> found;
	static std::vector<float> packed;
	found.clear(); packed.clear();
	if (AnyConditionFromSky())
	{
		Vector3 cam;
		if (Camera* c = w->GetMainCamera()) if (c->transform) cam = c->transform->globalPosition();
		GatherTrails(w->GetHierarchy(), cam, found);
		std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		packed.reserve(found.size() * 4);
		for (const auto& f : found) packed.insert(packed.end(), f.second.begin(), f.second.end());
	}
	r->setGroundTrails(packed.empty() ? nullptr : packed.data(), (int)(packed.size() / 4), (float)g_trailFill);
}
bool Surface::AnyConditionFromSky()
{
	for (const std::string& st : g_condSky) if (g_conditions.count(st)) return true;
	return false;
}

// Editor preview conditions live in their OWN map: they override reads but never serialize
// (SaveJson walks g_conditions only), so the simulator can't leak weather into saved worlds.
static std::map<std::string, float> g_condPreview;

double Surface::Condition(const std::string& state)
{
	auto pit = g_condPreview.find(state);
	if (pit != g_condPreview.end()) return pit->second;
	auto it = g_conditions.find(state);
	return it != g_conditions.end() ? it->second : 0.0;
}

void Surface::SetConditionPreview(const std::string& state, double value)
{
	if (state.empty()) return;
	if (value < 0.0) g_condPreview.erase(state);
	else             g_condPreview[state] = (float)std::min(value, 1.0);
}

void Surface::ClearConditionPreviews() { g_condPreview.clear(); }

void Surface::ClearConditions() { g_conditions.clear(); g_condSky.clear(); }

double Surface::ValueAt(Atom* atom, const std::string& state, const Vector3& worldPos)
{
	// Base: nearest-ancestor override, else the global condition.
	float base = (float)Condition(state);
	for (Atom* a = atom; a; a = a->parent)
	{
		auto it = g_states.find(a);
		if (it != g_states.end())
		{
			float v;
			if (it->second->Value(state, v)) { base = v; break; }
		}
	}
	// Masks along the ancestor chain lift the value at the point.
	float mask = 0.0f;
	for (Atom* a = atom; a; a = a->parent)
		for (Component* c : a->components)
			if (c && c->enabled && std::strcmp(c->name, "SurfaceMask") == 0)
				mask = std::max(mask, ((SurfaceMask*)c)->Sample(worldPos, state));
	return std::max(base, mask);
}

const std::map<std::string, float>& Surface::All() { return g_conditions; }

// States referenced by any override or mask channel; globals are checked live in StateInUse.
static std::set<std::string> g_statesInUse;

void Surface::RefreshInUse()
{
	g_statesInUse.clear();
	for (const auto& kv : g_states)
		if (kv.second)
			for (const std::string& s : kv.second->states)
				if (!s.empty()) g_statesInUse.insert(s);
	for (SurfaceMask* m : g_masks)
	{
		if (!m) continue;
		if (!m->state0.empty()) g_statesInUse.insert(m->state0);
		if (!m->state1.empty()) g_statesInUse.insert(m->state1);
		if (!m->state2.empty()) g_statesInUse.insert(m->state2);
		if (!m->state3.empty()) g_statesInUse.insert(m->state3);
	}
}

bool Surface::StateInUse(const std::string& state)
{
	return g_conditions.count(state) != 0 || g_condPreview.count(state) != 0
	    || g_statesInUse.count(state) != 0;
}

void Surface::SaveJson(nlohmann::json& j)
{
	if (g_conditions.empty()) return;
	nlohmann::json s = nlohmann::json::object();
	for (const auto& kv : g_conditions) s[kv.first] = kv.second;
	j["surface"] = s;
	if (!g_condSky.empty()) { nlohmann::json a = nlohmann::json::array(); for (const std::string& st : g_condSky) a.push_back(st); j["surfaceSky"] = a; }
}

void Surface::LoadJson(const nlohmann::json& j)
{
	g_conditions.clear(); g_condSky.clear();
	if (j.contains("surfaceSky") && j["surfaceSky"].is_array())
		for (const auto& v : j["surfaceSky"]) if (v.is_string()) g_condSky.insert(v.get<std::string>());
	if (!j.contains("surface") || !j["surface"].is_object()) return;
	for (auto it = j["surface"].begin(); it != j["surface"].end(); ++it)
		if (it.value().is_number()) g_conditions[it.key()] = (float)it.value().get<double>();
}

void Surface::ResetDefaults() { g_conditions.clear(); g_condSky.clear(); }

void Surface::Register(SurfaceState* s)   { if (s && s->atom) g_states[s->atom] = s; }
void Surface::Unregister(SurfaceState* s)
{
	for (auto it = g_states.begin(); it != g_states.end(); )
		if (it->second == s) it = g_states.erase(it); else ++it;
}
void Surface::Register(SurfaceMask* m)
{
	if (m && std::find(g_masks.begin(), g_masks.end(), m) == g_masks.end()) g_masks.push_back(m);
}
void Surface::Unregister(SurfaceMask* m)
{
	g_masks.erase(std::remove(g_masks.begin(), g_masks.end(), m), g_masks.end());
}
SurfaceState* Surface::StateOn(Atom* atom)
{
	auto it = g_states.find(atom);
	return it != g_states.end() ? it->second : nullptr;
}
const std::vector<SurfaceMask*>& Surface::Masks() { return g_masks; }

void Surface::PushDrawContext(Atom* a, Material* m)
{
	if (!m) return;
	if (m->liveOvCount <= 0 || !a) { m->liveDrawSet = false; return; }
	m->liveDrawSet    = true;
	m->liveDrawNoSky  = 0;
	m->liveDrawMask3D = nullptr;
	m->liveDrawMaskRes = 0.0f;
	// Nearest enabled SurfaceMask along the ancestor chain covers the draw.
	SurfaceMask* mask = nullptr;
	for (Atom* p = a; p && !mask; p = p->parent)
		for (Component* c : p->components)
			if (c && c->enabled && std::strcmp(c->name, "SurfaceMask") == 0) { mask = (SurfaceMask*)c; break; }
	bool anyChan = false;
	// Reset EVERY slot: a state that just went dormant leaves the slot table, and a stale
	// value here would patch the zeroed CB prop right back.
	for (int i = m->liveOvCount; i < Material::kOverlaySlots; ++i)
	{ m->liveDrawValue[i] = -1.0f; m->liveDrawMaskChan[i] = -1.0f; }
	for (int i = 0; i < m->liveOvCount; ++i)
	{
		m->liveDrawValue[i]    = -1.0f;
		m->liveDrawMaskChan[i] = -1.0f;
		const std::string& st = m->liveOv[i].state;
		if (st.empty()) continue;   // static layer: the CB already carries its fixed value
		float v = (float)Condition(st);
		for (Atom* p = a; p; p = p->parent)
		{
			auto it = g_states.find(p);
			if (it != g_states.end()) { float ov; if (it->second->Value(st, ov)) { v = ov; m->liveDrawNoSky |= (unsigned char)(1u << i); break; } }
		}
		m->liveDrawValue[i] = v;
		if (mask)
		{
			m->liveDrawMaskChan[i] = (float)mask->StateSlot(st);
			anyChan |= m->liveDrawMaskChan[i] >= 0.0f;
		}
	}
	if (mask && anyChan && mask->transform)
	{
		// world -> mask uvw rows: uvw_i = (axis_i . (p - t)) / (s_i * 2*he_i) + 0.5, with
		// axis_i = the box's rotated basis vector (projection onto the local axis).
		const Vector3    t = mask->transform->globalPosition();
		const Quaternion q = mask->transform->globalRotation();
		const Vector3    s = mask->transform->globalScale();
		const double he[3] = { mask->halfExtents.x, mask->halfExtents.y, mask->halfExtents.z };
		const double sc[3] = { s.x, s.y, s.z };
		const Vector3 axes[3] = { q.Rotate(Vector3(1, 0, 0)), q.Rotate(Vector3(0, 1, 0)), q.Rotate(Vector3(0, 0, 1)) };
		bool ok = true;
		for (int r = 0; r < 3; ++r)
		{
			const double denom = sc[r] * 2.0 * he[r];
			if (std::abs(denom) < 1e-9) { ok = false; break; }
			const double kx = axes[r].x / denom, ky = axes[r].y / denom, kz = axes[r].z / denom;
			m->liveDrawMaskXform[r * 4 + 0] = (float)kx;
			m->liveDrawMaskXform[r * 4 + 1] = (float)ky;
			m->liveDrawMaskXform[r * 4 + 2] = (float)kz;
			m->liveDrawMaskXform[r * 4 + 3] = (float)(-(kx * t.x + ky * t.y + kz * t.z) + 0.5);
		}
		if (ok)
		{
			m->liveDrawMask3D  = mask->GpuTex();
			m->liveDrawMaskRes = (float)mask->resolution;
		}
	}
}

// ---- surface responses ---------------------------------------------------------------

// The material an atom's surface presents (first MeshRenderer/SkinnedMeshRenderer with one).
static Material* MaterialOf(Atom* a)
{
	if (!a) return nullptr;
	for (Component* c : a->components)
	{
		if (!c) continue;
		if (std::strcmp(c->name, "MeshRenderer") != 0 && std::strcmp(c->name, "SkinnedMeshRenderer") != 0) continue;
		MeshRenderer* mr = (MeshRenderer*)c;
		if (mr->mat) return mr->mat;
	}
	return nullptr;
}

// Prefab/decal spawns queued by Hit(); DriveFoliage drains them outside the physics step and
// expires the spawned atoms' lifetimes.
// Every entry remembers its TARGET world: hits on preview atoms (editor tool) must spawn
// into and expire from the PREVIEW world, never the live one — and vice versa.
struct PendingHit
{
	World* target;
	std::string prefab, decal;
	float decalSize, lifetime;
	Vector3 pos, nrm;
	Color decalTint;
	float decalIntensity;
	int   decalMode;
	float decalFade;   // spread-in seconds (0 = instant)
};
static std::vector<PendingHit> g_pendingHits;
struct SpawnedHit { World* target; long id; double deadline; };
static std::vector<SpawnedHit> g_hitSpawned;

// Terrain-aware material: an atom carrying a "Terrain" component (module class, reached via
// reflection — the engine links no module) answers with the splat LAYER material under the
// world point. Null when the atom is not terrain / the point is air / the layer is unset.
static Material* TerrainLayerMaterial(Atom* a, const Vector3& pos)
{
	Component* t = a ? Reflect_FindComponent(a, "Terrain") : nullptr;
	if (!t) return nullptr;
	TypeInfo* ti = Registry_Find("Terrain");
	const Method* m = ti ? Reflect_FindMethod(ti, "LayerMaterialAt") : nullptr;
	if (!m) return nullptr;
	ReflectValue arg;
	detail::ToRV(pos, arg);
	ReflectValue ret;
	if (!Reflect_Invoke(t, *m, &arg, 1, ret) || ret.type != FT::String || ret.str.empty())
		return nullptr;
	return ResDB::getSingleton()->GetMaterial(ret.str);
}

bool Surface::FootstepOn(Atom* ground, const Vector3& pos, double volume)
{
	Material* m = MaterialOf(ground);
	if (!m) m = TerrainLayerMaterial(ground, pos);
	if (!m || m->liveSound.footsteps.empty()) return false;
	static std::map<const Material*, unsigned> rr;   // round-robin step index per material
	const unsigned n = rr[m]++;
	const std::string& clip = m->liveSound.footsteps[n % m->liveSound.footsteps.size()];
	const double voice = Audio::PlayAt(clip, pos, volume * m->liveSound.footVolume, 1.0, 25.0, 1);
	if (voice != 0.0)   // slight variation so repeated steps don't machine-gun
		Audio::SetPitch(voice, 0.94 + 0.12 * (((n * 2654435761u) >> 8 & 1023u) / 1023.0));
	return voice != 0.0;
}

bool Surface::Footstep(Atom* self, double volume)
{
	if (!self) return false;
	const Vector3 p = self->GetTransform().globalPosition();
	if (!Physics::RaycastIgnore(Vector3(p.x, p.y + 0.3, p.z), Vector3(0, -1, 0), 3.0, self)) return false;
	return FootstepOn(Physics::HitAtom(), p, volume);
}

bool Surface::Hit(Atom* atom, const std::string& hitType, const Vector3& pos,
                  const Vector3& normal, double impulse)
{
	return HitIn(nullptr, atom, hitType, pos, normal, impulse);   // null = the live pump's world
}

bool Surface::HitIn(World* target, Atom* atom, const std::string& hitType, const Vector3& pos,
                    const Vector3& normal, double impulse)
{
	Material* m = MaterialOf(atom);
	bool terra = false;
	if (!m) { m = TerrainLayerMaterial(atom, pos); terra = m != nullptr; }   // terrain: the splat layer under the hit
	// Silent misses made hit setups undebuggable ("fired but nothing happened"): say WHY a hit
	// found no reaction, throttled so contact storms can't flood the console.
	if (!m || m->liveHits.empty())
	{
		static double lastWhine = -10.0;
		const double nowW = Time::getSingleton()->elapsed;
		if (nowW - lastWhine > 2.0)
		{
			lastWhine = nowW;
			std::cout << "[Surface]\tHit '" << hitType << "' on '" << (atom ? atom->GetName() : "?")
			          << "': " << (!m ? "no material resolved at the hit point"
			                          : "material '" + (m->matName.empty() ? m->guid : m->matName) + "' has no Hit reactions")
			          << std::endl;
		}
		return false;
	}
	const LiveHit* best = nullptr;
	for (const LiveHit& h : m->liveHits)
		if (!h.hitType.empty() && h.hitType == hitType) { best = &h; break; }
	if (!best)
		for (const LiveHit& h : m->liveHits)
			if (h.hitType.empty()) { best = &h; break; }
	if (!best || impulse < best->minImpulse) return false;
	if (!best->soundGuid.empty())
	{
		// Preview worlds have no audio listener in their space — a positional voice there
		// sits at the wrong distance and stays silent. Editor hits play FLAT on the
		// Preview bus; live-world hits stay positional.
		if (target) Audio::Play(best->soundGuid, 1.0, false, 2);
		else        Audio::PlayAt(best->soundGuid, pos, 1.0, 1.0, 30.0, 1);
	}
	if (!best->eventName.empty())
	{
		// The hit's own parameters ride into the event: shaders/reactions read g_Hit
		// (impulse, hit normal); the point routes into the event's masks — each in its
		// AUTHORED space (uv probed under the hit, so uv masks keep their authored size).
		m->props["g_Hit"] = { (float)impulse, (float)normal.x, (float)normal.y, (float)normal.z };
		float hu = 0, hv = 0;
		if (Physics::MeshUVAt(atom, pos, normal, hu, hv))
			m->TriggerAtHit(best->eventName, pos, hu, hv);
		else if (terra)
			// Terrain has no mesh uv: its shader reads uv-authored masks as PLANAR WORLD XZ
			// (meters), so the hit's XZ IS the uv point — uv masks land at the impact.
			m->TriggerAtHit(best->eventName, pos, pos.x, pos.z);
		else
			m->TriggerAtWorld(best->eventName, pos);
	}
	if (!best->prefabGuid.empty() || !best->decalGuid.empty())
		g_pendingHits.push_back({ target, best->prefabGuid, best->decalGuid,
		                          best->decalSize, best->lifetime, pos, normal,
		                          best->decalTint, best->decalIntensity, best->decalMode,
		                          best->decalFade });
	return true;
}

std::string Surface::TagAt(Atom* atom)
{
	Material* m = MaterialOf(atom);
	return m ? m->physTag : std::string();
}

void Surface::ContactHit(Atom* a, Atom* b, const float point[3], const float normal[3])
{
	// The closing speed along the contact normal approximates the impact strength (m/s);
	// static-vs-static contacts (no rigidbodies) close at 0 and react only to Min Impulse 0.
	auto vel = [](Atom* x) -> Vector3
	{
		Rigidbody* rb = x ? x->GetComponent<Rigidbody>() : nullptr;
		return rb ? rb->Velocity() : Vector3(0, 0, 0);
	};
	const Vector3 rv = vel(a) - vel(b);
	const double closing = std::fabs(rv.x * normal[0] + rv.y * normal[1] + rv.z * normal[2]);
	const Vector3 p(point[0], point[1], point[2]);
	// The contact normal points A -> B: that IS A's surface normal at the point, and B's is
	// the opposite.
	Hit(a, std::string(), p, Vector3(normal[0], normal[1], normal[2]), closing);
	Hit(b, std::string(), p, Vector3(-normal[0], -normal[1], -normal[2]), closing);
}

// ---- LiveMaterial auto-foliage ------------------------------------------------------------
// A surface whose material carries liveFoliage entries GROWS them: the driver maintains one
// TRANSIENT Foliage component per entry on the atom (surface = the atom's own meshes, the
// Foliage default). Transient components never serialize — the material is the single source.

struct DrivenFoliage
{
	std::vector<Foliage*> comps;
	std::string fp;      // entry fingerprint the comps were built from
	World* world = nullptr;   // owning world: mark-and-sweep is PER world (previews drive too)
	long long atomId = 0;     // stable id: a RECYCLED heap address must never match a dead atom's entry
	bool seen = false;   // mark-and-sweep against destroyed atoms
};
static std::map<Atom*, DrivenFoliage> g_grown;

// Ambient/wind loops grown from a material's sound identity (transient AudioSources).
struct DrivenSound
{
	AudioSource* amb = nullptr;
	AudioSource* wind = nullptr;
	std::string fp;
	World* world = nullptr;
	long long atomId = 0;
	bool seen = false;
};
static std::map<Atom*, DrivenSound> g_sndGrown;

static void DropSound(Atom* a, DrivenSound& d)
{
	for (AudioSource* as : { d.amb, d.wind })
	{
		if (!as) continue;
		a->components.remove(as);
		as->Destroy();
		delete as;
	}
	d.amb = d.wind = nullptr;
	d.fp.clear();
}

static std::string FoliageFp(const std::vector<LiveFoliage>& v)
{
	std::ostringstream o;
	for (const LiveFoliage& f : v)
		o << f.meshGuid << '|' << f.matGuid << '|' << f.density << '|' << f.scaleMin << '|'
		  << f.scaleMax << '|' << f.maxSlope << '|' << f.align << '|' << f.windBend << '|'
		  << f.interBend << '|' << f.seed << ';';
	return o.str();
}

static void DropGrown(Atom* a, DrivenFoliage& d)
{
	for (Foliage* f : d.comps)
	{
		if (!f) continue;
		a->components.remove(f);
		f->Destroy();
		delete f;
	}
	d.comps.clear();
	d.fp.clear();
}

void Surface::DrainHits(World* w)
{
	if (!w) return;
	// spawn queued hit reactions (outside the physics step) and expire their lifetimes.
	const double nowT = Time::getSingleton()->elapsed;
	for (size_t phi = 0; phi < g_pendingHits.size(); )
	{
		const PendingHit& ph = g_pendingHits[phi];
		if (ph.target && ph.target != w) { ++phi; continue; }   // someone else's world drains it
		const Vector3 up = std::fabs(ph.nrm.y) > 0.99 ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
		if (!ph.prefab.empty())
			if (Atom* sp = Prefabs::SpawnIn(w, ph.prefab))
			{
				// +Y along the surface normal: debris/particles erupt away from the surface.
				const Vector3 fwd = Vector3(up.y * ph.nrm.z - up.z * ph.nrm.y,
				                            up.z * ph.nrm.x - up.x * ph.nrm.z,
				                            up.x * ph.nrm.y - up.y * ph.nrm.x);
				sp->GetTransform().SetGlobal(ph.pos, Quaternion::LookRotation(fwd, ph.nrm), Vector3(1, 1, 1));
				if (ph.lifetime > 0.0f) g_hitSpawned.push_back({ w, (long)sp->id.id, nowT + ph.lifetime });
			}
		if (!ph.decal.empty())
		{
			Atom* d = w->CreateAtom("HitDecal");
			Decal* dc = new Decal();
			dc->textureGuid = ph.decal;
			dc->tint       = ph.decalTint;
			dc->intensity  = ph.decalIntensity;
			dc->mode       = (DecalMode)ph.decalMode;
			dc->fadeIn     = ph.decalFade;   // spread-in (blood creep); auto-dissolve before death
			dc->appear     = 1;
			if (ph.lifetime > 0.0f)
			{
				dc->dieTime = nowT + ph.lifetime;
				dc->fadeOut = std::min(0.6f, ph.lifetime * 0.25f);
			}
			d->AddComponent(dc);
			// The decal box projects along its local +Z: aim it INTO the surface.
			d->GetTransform().SetGlobal(ph.pos,
				Quaternion::LookRotation(Vector3(-ph.nrm.x, -ph.nrm.y, -ph.nrm.z), up),
				Vector3(ph.decalSize, ph.decalSize, ph.decalSize));
			if (ph.lifetime > 0.0f) g_hitSpawned.push_back({ w, (long)d->id.id, nowT + ph.lifetime });
		}
		g_pendingHits.erase(g_pendingHits.begin() + phi);
	}
	for (size_t i = 0; i < g_hitSpawned.size(); )
	{
		SpawnedHit& sh = g_hitSpawned[i];
		if (sh.target == w && nowT >= sh.deadline)
		{
			w->QueueDestroy(sh.id);
			g_hitSpawned.erase(g_hitSpawned.begin() + i);
		}
		else if (nowT >= sh.deadline + 120.0)   // its world stopped pumping (closed preview)
			g_hitSpawned.erase(g_hitSpawned.begin() + i);
		else ++i;
	}
}

void Surface::DriveFoliage(World* w)
{
	if (!w) return;
	RefreshInUse();   // active-state set for this frame's overlay slot assignment

	DrainHits(w);

	for (auto& kv : g_grown) if (kv.second.world == w) kv.second.seen = false;
	for (auto& kv : g_sndGrown) if (kv.second.world == w) kv.second.seen = false;

	std::function<void(bc::list<Atom*>&)> walk = [&](bc::list<Atom*>& atoms)
	{
		for (Atom* a : atoms)
		{
			if (!a) continue;
			if (!a->enabled)
			{
				// Disabled subtree: keep any growth (paused, not rendered), never sweep it away.
				// Id mismatch = a DEAD atom's entry on a recycled address: leave it unseen (the
				// sweep forgets it without dereferencing anything).
				auto git = g_grown.find(a);
				if (git != g_grown.end() && git->second.atomId == (long long)a->id.id) git->second.seen = true;
				auto sit = g_sndGrown.find(a);
				if (sit != g_sndGrown.end() && sit->second.atomId == (long long)a->id.id)
				{
					sit->second.seen = true;   // keep, but silence while disabled
					if (sit->second.amb)  sit->second.amb->Stop();
					if (sit->second.wind) sit->second.wind->Stop();
				}
				walk(a->children);
				continue;
			}
			{
				// Every DISTINCT material this atom's MeshRenderers draw (base + per-slot
				// instances): tween-animated ones tick, foliage-bearing ones become sources.
				const double now = Time::getSingleton()->elapsed;
				std::vector<Material*> srcs;
				Material* snd = nullptr;   // first material with a sound identity
				bool meshReady = true;
				for (Component* c : a->components)
				{
					if (!c || !c->enabled || c->transient) continue;
					if (std::strcmp(c->name, "MeshRenderer") != 0 && std::strcmp(c->name, "SkinnedMeshRenderer") != 0) continue;
					MeshRenderer* mr = (MeshRenderer*)c;
					auto consider = [&](Material* m)
					{
						if (!m) return;
						if (!m->liveTweens.empty()) m->ApplyTweens(now);
						m->PushRenderProps();   // UV transform / cutout / wipe -> MatCB props
						if (!snd && (!m->liveSound.ambientGuid.empty() || !m->liveSound.windGuid.empty())) snd = m;
						if (m->liveFoliage.empty()) return;
						for (Material* e : srcs) if (e->guid == m->guid) return;
						srcs.push_back(m);
						if (!mr->mesh) meshReady = false;   // async load: retry until scatterable
					};
					consider(mr->mat);
					for (Material* sm : mr->mats) consider(sm);
				}
				// Terrain atoms: the splat LAYER materials are surface sources too — their
				// auto-foliage grows through the scatter-source seam, their sound identity
				// plays like any surface. The fingerprint carries the module's quiet-edit
				// stamp, so digs reflow the growth once the sculpting settles.
				std::string terraFp;
				if (Component* tc = Reflect_FindComponent(a, "Terrain"))
					if (TypeInfo* ti = Registry_Find("Terrain"))
					{
						for (int li = 0; li < 8; ++li)
						{
							const Field* f = Reflect_FindField(ti, "layer" + std::to_string(li));
							if (!f) continue;
							ReflectValue rv = Reflect_GetField(tc, *f);
							if (rv.type != FT::String || rv.str.empty()) continue;
							Material* lm = ResDB::getSingleton()->GetMaterial(rv.str);
							if (!lm) continue;
							// Layer materials are surfaces without a MeshRenderer: their tweens
							// must still evaluate here, or the terrain module has nothing live
							// to mirror into its splat palette (color/uv/emissive animation).
							if (!lm->liveTweens.empty()) lm->ApplyTweens(now);
							lm->PushRenderProps();
							if (!snd && (!lm->liveSound.ambientGuid.empty() || !lm->liveSound.windGuid.empty())) snd = lm;
							if (lm->liveFoliage.empty()) continue;
							bool dup = false;
							for (Material* e : srcs) if (e->guid == lm->guid) { dup = true; break; }
							if (!dup) srcs.push_back(lm);
						}
						if (const Method* fm = Reflect_FindMethod(ti, "FoliageStamp"))
						{
							ReflectValue ret;
							if (Reflect_Invoke(tc, *fm, nullptr, 0, ret))
							{
								std::ostringstream ts; ts << '~' << ret.num;
								terraFp = ts.str();
							}
						}
					}
				// Ambient/wind loops from the surface's sound identity (transient sources; the
				// wind loop's volume follows the global wind strength each frame).
				{
					auto sit = g_sndGrown.find(a);
					// Recycled heap address: the entry belongs to a DEAD atom (its transient
					// sources died with it) — forget it, NEVER touch the dangling pointers.
					if (sit != g_sndGrown.end() && sit->second.atomId != (long long)a->id.id)
					{ g_sndGrown.erase(sit); sit = g_sndGrown.end(); }
					if (snd)
					{
						DrivenSound& ds = g_sndGrown[a];
						ds.seen = true;
						ds.world = w;
						ds.atomId = (long long)a->id.id;
						const LiveSound& ls = snd->liveSound;
						std::ostringstream fo;
						fo << ls.ambientGuid << '|' << ls.windGuid << '|' << ls.ambientVolume << '|' << ls.windVolume;
						if (fo.str() != ds.fp)
						{
							DropSound(a, ds);
							auto grow = [&](const std::string& clip, float vol) -> AudioSource*
							{
								if (clip.empty()) return nullptr;
								AudioSource* as = new AudioSource();
								as->transient = true;
								as->clip = clip; as->loop = true; as->spatial = true;
								as->playOnStart = true; as->volume = vol;
								as->minDist = 2.0f; as->maxDist = 45.0f;
								a->AddComponent(as);
								return as;
							};
							ds.amb  = grow(ls.ambientGuid, ls.ambientVolume);
							ds.wind = grow(ls.windGuid, 0.0f);
							ds.fp = fo.str();
						}
						if (ds.wind)
							ds.wind->volume = ls.windVolume
							                * (float)std::min(std::max(Wind::Strength() / 8.0, 0.0), 1.0);
					}
					else if (sit != g_sndGrown.end())
					{
						DropSound(a, sit->second);
						g_sndGrown.erase(sit);
					}
				}

				auto git = g_grown.find(a);
				// Same recycled-address guard as the sound entries above.
				if (git != g_grown.end() && git->second.atomId != (long long)a->id.id)
				{ g_grown.erase(git); git = g_grown.end(); }
				if (!srcs.empty() && meshReady)
				{
					DrivenFoliage& d = g_grown[a];
					d.seen = true;
					d.world = w;
					d.atomId = (long long)a->id.id;
					std::string fp = terraFp;
					for (Material* s : srcs) fp += s->guid + '#' + FoliageFp(s->liveFoliage);
					if (fp != d.fp)
					{
						DropGrown(a, d);
						for (Material* s : srcs)
							for (const LiveFoliage& e : s->liveFoliage)
							{
								Foliage* f = new Foliage();
								f->transient = true;
								f->onlyMatGuid = s->guid;   // grow ONLY on this material's sections
								f->meshGuid  = e.meshGuid;
								f->matGuid   = e.matGuid;
								f->density   = e.density;
								f->seed      = e.seed;
								f->scaleMin  = e.scaleMin;
								f->scaleMax  = e.scaleMax;
								f->maxSlope  = e.maxSlope;
								f->alignToNormal   = e.align;
								f->windBend        = e.windBend;
								f->interactionBend = e.interBend;
								a->AddComponent(f);
								f->Rebuild();
								d.comps.push_back(f);
							}
						d.fp = fp;
					}
				}
				else if (git != g_grown.end())
				{
					// Materials lost their foliage (or the mesh went away): retract the growth.
					if (!srcs.empty() && !meshReady) git->second.seen = true;   // just waiting on the mesh
					else { DropGrown(a, git->second); g_grown.erase(git); continue; }
				}
			}
			walk(a->children);
		}
	};
	walk(w->GetHierarchy());

	// Sweep THIS world's entries whose atoms vanished this frame (their components died with
	// the atom — never dereference, just forget). Other worlds' entries are theirs to sweep.
	for (auto it = g_grown.begin(); it != g_grown.end(); )
		if (it->second.world == w && !it->second.seen) it = g_grown.erase(it); else ++it;
	for (auto it = g_sndGrown.begin(); it != g_sndGrown.end(); )
		if (it->second.world == w && !it->second.seen) it = g_sndGrown.erase(it); else ++it;
}

void Surface::ForgetWorld(World* w)
{
	if (!w) return;
	// The world is dying: forget everything grown/queued for it (components die with it).
	for (auto it = g_grown.begin(); it != g_grown.end(); )
		if (it->second.world == w) it = g_grown.erase(it); else ++it;
	for (auto it = g_sndGrown.begin(); it != g_sndGrown.end(); )
		if (it->second.world == w) it = g_sndGrown.erase(it); else ++it;
	for (auto it = g_pendingHits.begin(); it != g_pendingHits.end(); )
		if (it->target == w) it = g_pendingHits.erase(it); else ++it;
	for (auto it = g_hitSpawned.begin(); it != g_hitSpawned.end(); )
		if (it->target == w) it = g_hitSpawned.erase(it); else ++it;
}

}  // namespace nuke
