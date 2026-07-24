#include "API/Model/Wind.h"
#include "API/Model/Noise.h"
#include "API/Model/Atom.h"
#include <nlohmann/json.hpp>
#include <boost/thread/mutex.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <algorithm>
#include <cmath>

namespace nuke {

// ---- global wind state (world state — serialized in the .nuworld "wind" block) ------------
static float  gDir[3] = { 1, 0, 0 };
static float  gStrength = 0.0f;        // default: no wind
static float  gGustAmount = 0.0f, gGustFreq = 0.5f;
static float  gTurbAmount = 0.0f, gTurbScale = 20.0f;
static double gTime = 0.0;   // Wind's own smooth clock (Advance: scaled game delta per frame)

// WindZone registry. Guarded: Sample may be called from the fixed thread (future cloth) while
// zones register/unregister on the game thread.
static boost::mutex gZoneLock;
static std::vector<WindZone*> gZones;

// ---- WindZone component -------------------------------------------------------------------

WindZone::WindZone() : Component("WindZone") {}

void WindZone::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
	Wind::Register(this);
}

void WindZone::Destroy() { Wind::Unregister(this); }
void WindZone::Update() {}
void WindZone::FixedUpdate() {}
void WindZone::Pause() {}
void WindZone::Reset() {}

void Wind::Register(WindZone* z)
{
	boost::mutex::scoped_lock l(gZoneLock);
	if (std::find(gZones.begin(), gZones.end(), z) == gZones.end()) gZones.push_back(z);
}

void Wind::Advance(double dt) { if (dt > 0.0) gTime += dt; }

void Wind::Unregister(WindZone* z)
{
	boost::mutex::scoped_lock l(gZoneLock);
	gZones.erase(std::remove(gZones.begin(), gZones.end(), z), gZones.end());
}

// ---- reflected parameters -----------------------------------------------------------------

void Wind::SetDirection(const Vector3& dir)
{
	glm::vec3 d((float)dir.x, (float)dir.y, (float)dir.z);
	float len = glm::length(d);
	if (len < 1e-6f) return;   // a zero direction is meaningless — keep the current one
	d /= len;
	gDir[0] = d.x; gDir[1] = d.y; gDir[2] = d.z;
}
Vector3 Wind::Direction() { return Vector3(gDir[0], gDir[1], gDir[2]); }

void   Wind::SetStrength(double s) { gStrength = s < 0.0 ? 0.f : (float)s; }
double Wind::Strength()            { return gStrength; }

void Wind::SetGusts(double amount, double frequency)
{
	gGustAmount = (float)std::max(0.0, std::min(1.0, amount));
	gGustFreq   = (float)std::max(0.0, frequency);
}
double Wind::GustAmount()    { return gGustAmount; }
double Wind::GustFrequency() { return gGustFreq; }

void Wind::SetTurbulence(double amount, double scale)
{
	gTurbAmount = (float)std::max(0.0, std::min(1.0, amount));
	gTurbScale  = (float)std::max(0.01, scale);
}
double Wind::TurbulenceAmount() { return gTurbAmount; }
double Wind::TurbulenceScale()  { return gTurbScale; }

// Gusted strength multiplier right now (shared by Sample and the renderer push): a slow
// Perlin swell over GAME time — pauses when the world is frozen, like everything gameplay.
static float GustedStrength(double t)
{
	if (gGustAmount <= 0.f || gGustFreq <= 0.f) return gStrength;
	float swell = (float)Noise::Perlin2(7.0, t * gGustFreq, 0.0);         // ~[-1, 1]
	float m = 1.0f + gGustAmount * swell;
	return gStrength * (m < 0.f ? 0.f : m);
}

Vector3 Wind::Sample(const Vector3& worldPos)
{
	const double t = gTime;
	glm::vec3 v = glm::vec3(gDir[0], gDir[1], gDir[2]) * GustedStrength(t);

	// Spatial turbulence: deflect a fraction of the strength by 3D noise drifting over time.
	if (gTurbAmount > 0.f && gStrength > 0.f)
	{
		const double s = 1.0 / gTurbScale;
		const double x = worldPos.x * s, y = worldPos.y * s, z = worldPos.z * s, dr = t * 0.25;
		glm::vec3 n((float)Noise::Perlin3(11.0, x + dr, y, z),
		            (float)Noise::Perlin3(23.0, x, y + dr, z),
		            (float)Noise::Perlin3(37.0, x, y, z + dr));
		v += n * (gTurbAmount * gStrength);
	}

	// Zones: additive local volumes.
	boost::mutex::scoped_lock l(gZoneLock);
	for (WindZone* zn : gZones)
	{
		if (!zn || !zn->enabled || !zn->transform) continue;
		Transform& tr = *zn->transform;
		Vector3 cP = tr.globalPosition();
		glm::vec3 center((float)cP.x, (float)cP.y, (float)cP.z);
		glm::vec3 p((float)worldPos.x, (float)worldPos.y, (float)worldPos.z);
		float edge01;   // 0 at the center, 1 at the shape edge; outside -> skip
		if (zn->shape == 0)
		{
			const float r = zn->radius > 0.01f ? zn->radius : 0.01f;
			const float d = glm::length(p - center);
			if (d > r) continue;
			edge01 = d / r;
		}
		else
		{
			Quaternion q = tr.globalRotation();
			glm::quat rot((float)q.w, (float)q.x, (float)q.y, (float)q.z);
			glm::vec3 lp = glm::inverse(rot) * (p - center);   // box is atom-oriented
			glm::vec3 he((float)zn->halfExtents.x, (float)zn->halfExtents.y, (float)zn->halfExtents.z);
			he = glm::max(glm::abs(he), glm::vec3(0.01f));
			glm::vec3 a = glm::abs(lp) / he;
			edge01 = std::max({ a.x, a.y, a.z });
			if (edge01 > 1.f) continue;
		}
		const float w = 1.0f - zn->falloff * edge01;   // falloff 0 = hard, 1 = linear fade
		glm::vec3 zdir;
		if (zn->mode == 0)   // directional: along the atom's forward
		{
			Vector3 f = tr.direction();
			zdir = glm::vec3((float)f.x, (float)f.y, (float)f.z);
		}
		else                 // radial: outward from the center (negative strength = suction)
		{
			glm::vec3 out = p - center;
			float len = glm::length(out);
			zdir = len > 1e-5f ? out / len : glm::vec3(0, 1, 0);
		}
		v += zdir * (zn->strength * w);
	}
	return Vector3(v.x, v.y, v.z);
}

// ---- serialization + renderer push --------------------------------------------------------

void Wind::SaveJson(nlohmann::json& j)
{
	// Written only when any parameter is non-default — a windless world stays clean.
	if (gStrength <= 0.f && gGustAmount <= 0.f && gTurbAmount <= 0.f) return;
	j["wind"] = { { "dir", { gDir[0], gDir[1], gDir[2] } }, { "strength", gStrength },
	              { "gustAmount", gGustAmount }, { "gustFreq", gGustFreq },
	              { "turbAmount", gTurbAmount }, { "turbScale", gTurbScale } };
}

void Wind::LoadJson(const nlohmann::json& j)
{
	if (!j.contains("wind") || !j["wind"].is_object()) { ResetDefaults(); return; }
	const nlohmann::json& w = j["wind"];
	if (w.contains("dir") && w["dir"].is_array() && w["dir"].size() == 3)
		for (int i = 0; i < 3; ++i) gDir[i] = w["dir"][i].get<float>();
	gStrength   = w.value("strength", 0.0f);
	gGustAmount = w.value("gustAmount", 0.0f);
	gGustFreq   = w.value("gustFreq", 0.5f);
	gTurbAmount = w.value("turbAmount", 0.0f);
	gTurbScale  = w.value("turbScale", 20.0f);
}

void Wind::ResetDefaults()
{
	gDir[0] = 1; gDir[1] = 0; gDir[2] = 0;
	gStrength = 0.f; gGustAmount = 0.f; gGustFreq = 0.5f;
	gTurbAmount = 0.f; gTurbScale = 20.0f;
}

void Wind::ShaderParams(float outDirStrength[4], float outParams[4])
{
	const double t = gTime;
	outDirStrength[0] = gDir[0]; outDirStrength[1] = gDir[1]; outDirStrength[2] = gDir[2];
	outDirStrength[3] = GustedStrength(t);            // CURRENT gusted strength (animated)
	outParams[0] = gTurbAmount;
	outParams[1] = gTurbScale > 0.f ? 1.0f / gTurbScale : 0.f;   // shaders want the reciprocal
	outParams[2] = (float)t;                          // phase source for vertex sway
	outParams[3] = gGustFreq;
}

}  // namespace nuke
