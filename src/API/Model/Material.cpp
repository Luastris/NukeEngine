#include "API/Model/Material.h"
#include "API/Model/Surface.h"
#include "API/Model/Time.h"   // event-started tween instances stamp their start   // global condition values for the overlay-state slots
#include "API/Model/resdb.h"
#include "interface/AppInstance.h"   // renderer access: invalidate re-baked textures
#include <render/irender.h>
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <map>

namespace nuke {

namespace bfs = boost::filesystem;
using json = nlohmann::json;

Material::Material() {}

void Material::ImportAiMaterial(aiMaterial* m) {
	aiString nm;
	if (m->Get(AI_MATKEY_NAME, nm) == AI_SUCCESS) matName = nm.C_Str();

	aiColor3D col(1.f, 1.f, 1.f);
	if (m->Get(AI_MATKEY_COLOR_DIFFUSE, col) == AI_SUCCESS) { color.r = col.r; color.g = col.g; color.b = col.b; }
	float opacity = 1.f;
	if (m->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) color.a = opacity;

	// PBR scalar factors (glTF / PBR materials).
	float mf = 0.f, rf = 1.f, sf = 1.f;
	if (m->Get(AI_MATKEY_METALLIC_FACTOR, mf)  == AI_SUCCESS) metallic  = mf;
	if (m->Get(AI_MATKEY_ROUGHNESS_FACTOR, rf) == AI_SUCCESS) roughness = rf;
#ifdef AI_MATKEY_SPECULAR_FACTOR
	if (m->Get(AI_MATKEY_SPECULAR_FACTOR, sf)  == AI_SUCCESS) specular  = sf;   // KHR_materials_specular
#endif
	aiColor3D ec(0.f, 0.f, 0.f);
	if (m->Get(AI_MATKEY_COLOR_EMISSIVE, ec) == AI_SUCCESS)
	{
		emissive.r = ec.r; emissive.g = ec.g; emissive.b = ec.b;
		if (ec.r > 0.f || ec.g > 0.f || ec.b > 0.f) emissiveIntensity = 1.0f;
	}

	aiMat = m;   // textures are converted + assigned (as .nutex GUIDs) by the importer
}

Material* Material::Clone() const
{
	Material* m = new Material();
	m->guid        = guid;
	m->matName     = matName;
	m->color = color;
	m->diffuseGuid = diffuseGuid;
	m->normalGuid  = normalGuid;
	m->specularGuid= specularGuid;
	m->metalRoughGuid = metalRoughGuid;
	m->metallicGuid   = metallicGuid;
	m->roughnessGuid  = roughnessGuid;
	m->opacityGuid    = opacityGuid;
	m->occlusionGuid  = occlusionGuid;
	m->emissiveGuid   = emissiveGuid;
	m->wipeGuid       = wipeGuid;
	m->wipeThreshold  = wipeThreshold;
	m->wipeFeather    = wipeFeather;
	m->metallic    = metallic;
	m->roughness   = roughness;
	m->specular    = specular;
	m->emissive = emissive;
	m->emissiveIntensity = emissiveIntensity;
	m->castShadows    = castShadows;
	m->receiveShadows = receiveShadows;
	m->blendMode   = blendMode;
	m->alphaCutoff = alphaCutoff;
	m->uvTiling    = uvTiling;
	m->uvOffset    = uvOffset;
	m->uvRotation  = uvRotation;
	m->detailGuid       = detailGuid;
	m->detailNormalGuid = detailNormalGuid;
	m->detailTiling     = detailTiling;
	m->detailStrength   = detailStrength;
	m->triplanar        = triplanar;
	m->vcolorMode       = vcolorMode;
	m->clearCoat = clearCoat; m->clearCoatRoughness = clearCoatRoughness;
	m->anisotropy = anisotropy; m->flowGuid = flowGuid;
	m->sheen = sheen; m->sheenTint = sheenTint;
	m->translucency = translucency; m->translucencyTint = translucencyTint;
	m->ior = ior; m->refractive = refractive;
	m->iridescence = iridescence; m->iridescenceThickness = iridescenceThickness;
	m->shaderGuid  = shaderGuid;
	m->props       = props;
	m->liveStates  = liveStates;
	m->liveLayers  = liveLayers;
	m->liveHits    = liveHits;
	m->liveFoliage = liveFoliage;
	m->liveTweens  = liveTweens;
	m->liveMasks   = liveMasks;
	m->liveEvents  = liveEvents;
	m->liveSound   = liveSound;
	m->liveSurface = liveSurface;
	m->physTag      = physTag;
	m->liveFriction = liveFriction;
	m->liveBounce   = liveBounce;
	m->Resolve();          // bind diff/norm/spec/mr/ao/em/shader pointers from ResDB
	m->PushRenderProps();  // static UV/cutout/wipe state reaches previews and non-world users too
	return m;
}

bool Material::HasLive() const
{
	return !liveStates.empty() || !liveLayers.empty() || !liveHits.empty() || !liveFoliage.empty() || !liveTweens.empty() || !physTag.empty()
	    || !liveMasks.empty() || !liveEvents.empty()
	    || !liveSound.footsteps.empty() || !liveSound.ambientGuid.empty() || !liveSound.windGuid.empty()
	    || !liveSurface.heightGuid.empty() || liveSurface.dispScale != 0.0f || liveSurface.varAmount != 0.0f
	    || liveFriction >= 0.0f || liveBounce >= 0.0f;
}

// ---- Substance-style separate map baking ---------------------------------------------------
// Separate grayscale Metallic/Roughness (and Opacity) maps are baked into the formats the
// pipeline consumes (combined G=rough/B=metal; opacity into base-color alpha) once per
// material, cached by the source pixel buffers so texture hot-reloads re-bake.
struct BakedTex
{
	Texture* tex = nullptr;
	const void* s1 = nullptr;   // source pixel-buffer stamps (data() moves on hot-reload)
	const void* s2 = nullptr;
};
static std::map<std::string, BakedTex> g_bakes;   // "mr:<guid>" / "op:<guid>" -> baked texture

// Nearest-sample `src` (RGBA8, sw*sh) channel `ch` at normalized (u,v); 255 when absent.
static unsigned char SampleCh(const std::vector<unsigned char>& src, int sw, int sh, int ch,
                              float u, float v)
{
	if (src.empty() || sw <= 0 || sh <= 0) return 255;
	int x = (int)(u * sw); if (x >= sw) x = sw - 1;
	int y = (int)(v * sh); if (y >= sh) y = sh - 1;
	return src[((size_t)y * sw + x) * 4 + ch];
}

static Texture* BakeCombinedMR(Material* m, ResDB* db)
{
	Texture* tm = m->metallicGuid.empty()  ? nullptr : db->GetTexture(m->metallicGuid);
	Texture* tr = m->roughnessGuid.empty() ? nullptr : db->GetTexture(m->roughnessGuid);
	if (!tm && !tr) return nullptr;
	BakedTex& b = g_bakes["mr:" + m->guid];
	const void* s1 = tm ? (const void*)tm->pixels.data() : nullptr;
	const void* s2 = tr ? (const void*)tr->pixels.data() : nullptr;
	if (b.tex && b.s1 == s1 && b.s2 == s2) return b.tex;
	const std::vector<unsigned char> pm = tm ? tm->DecodeRGBA() : std::vector<unsigned char>();
	const std::vector<unsigned char> pr = tr ? tr->DecodeRGBA() : std::vector<unsigned char>();
	const int w = std::max(tm ? tm->width : 1, tr ? tr->width : 1);
	const int h = std::max(tm ? tm->height : 1, tr ? tr->height : 1);
	Texture* t = b.tex;
	if (!t)
	{
		t = new Texture();
		t->guid = "baked:mr:" + m->guid;
		snprintf(t->name, sizeof(t->name), "baked-mr");
		t->usage = Texture::UsageData;
	}
	t->width = w; t->height = h;
	t->format = Texture::FMT_RGBA8; t->mipCount = 1; t->frameCount = 1;
	t->pixels.resize((size_t)w * h * 4);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
			unsigned char* px = &t->pixels[((size_t)y * w + x) * 4];
			px[0] = 255;                                                              // R unused (AO has its own map)
			px[1] = tr ? SampleCh(pr, tr->width, tr->height, 0, u, v) : 255;          // G = roughness (scalar factor rules when absent)
			px[2] = tm ? SampleCh(pm, tm->width, tm->height, 0, u, v) : 0;            // B = metallic (absent = dielectric)
			px[3] = 255;
		}
	// A re-bake must reach the GPU: drop the cached texture before it is sampled again.
	if (b.tex)
		if (AppInstance* app = AppInstance::GetSingleton())
			if (app->render) app->render->invalidateTexture(t);
	b = { t, s1, s2 };
	return t;
}

static Texture* BakeOpacityDiffuse(Material* m, ResDB* db)
{
	Texture* to = db->GetTexture(m->opacityGuid);
	if (!to) return nullptr;
	Texture* td = m->diffuseGuid.empty() ? nullptr : db->GetTexture(m->diffuseGuid);
	BakedTex& b = g_bakes["op:" + m->guid];
	const void* s1 = to->pixels.data();
	const void* s2 = td ? (const void*)td->pixels.data() : nullptr;
	if (b.tex && b.s1 == s1 && b.s2 == s2) return b.tex;
	const std::vector<unsigned char> po = to->DecodeRGBA();
	const std::vector<unsigned char> pd = td ? td->DecodeRGBA() : std::vector<unsigned char>();
	const int w = std::max(to->width, td ? td->width : 1);
	const int h = std::max(to->height, td ? td->height : 1);
	Texture* t = b.tex;
	if (!t)
	{
		t = new Texture();
		t->guid = "baked:op:" + m->guid;
		snprintf(t->name, sizeof(t->name), "baked-opacity");
		t->usage = td ? td->usage : Texture::UsageColor;
	}
	t->width = w; t->height = h;
	t->format = Texture::FMT_RGBA8; t->mipCount = 1; t->frameCount = 1;
	t->pixels.resize((size_t)w * h * 4);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
		{
			const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
			unsigned char* px = &t->pixels[((size_t)y * w + x) * 4];
			px[0] = td ? SampleCh(pd, td->width, td->height, 0, u, v) : 255;
			px[1] = td ? SampleCh(pd, td->width, td->height, 1, u, v) : 255;
			px[2] = td ? SampleCh(pd, td->width, td->height, 2, u, v) : 255;
			const unsigned op = SampleCh(po, to->width, to->height, 0, u, v);
			const unsigned da = td ? SampleCh(pd, td->width, td->height, 3, u, v) : 255;
			px[3] = (unsigned char)(op * da / 255);   // opacity multiplies the base alpha
		}
	if (b.tex)
		if (AppInstance* app = AppInstance::GetSingleton())
			if (app->render) app->render->invalidateTexture(t);
	b = { t, s1, s2 };
	return t;
}

void Material::Resolve()
{
	ResDB* db = ResDB::getSingleton();
	// Re-bind when the pointer is missing OR no longer matches the guid (instance overrides).
	if (diffuseGuid.empty())                              diff = nullptr;
	else if (!diff   || diff->guid   != diffuseGuid)     diff   = db->GetTexture(diffuseGuid);
	if (normalGuid.empty())                              norm = nullptr;
	else if (!norm   || norm->guid   != normalGuid)     norm   = db->GetTexture(normalGuid);
	if (specularGuid.empty())                            spec = nullptr;
	else if (!spec   || spec->guid   != specularGuid)   spec   = db->GetTexture(specularGuid);
	if (metalRoughGuid.empty())                          mr = nullptr;
	else if (!mr     || mr->guid     != metalRoughGuid) mr     = db->GetTexture(metalRoughGuid);
	if (occlusionGuid.empty())                           ao = nullptr;
	else if (!ao     || ao->guid     != occlusionGuid)  ao     = db->GetTexture(occlusionGuid);
	if (emissiveGuid.empty())                            em = nullptr;
	else if (!em     || em->guid     != emissiveGuid)   em     = db->GetTexture(emissiveGuid);
	if (wipeGuid.empty())                                wipe = nullptr;
	else if (!wipe   || wipe->guid   != wipeGuid)       wipe   = db->GetTexture(wipeGuid);
	if (detailGuid.empty())                              detail = nullptr;
	else if (!detail || detail->guid != detailGuid)     detail = db->GetTexture(detailGuid);
	if (detailNormalGuid.empty())                            detailNrm = nullptr;
	else if (!detailNrm || detailNrm->guid != detailNormalGuid) detailNrm = db->GetTexture(detailNormalGuid);
	if (flowGuid.empty())                            flow = nullptr;
	else if (!flow || flow->guid != flowGuid)        flow = db->GetTexture(flowGuid);
	mskStamp = nullptr;
	for (LiveMask& mk : liveMasks)
	{
		if (mk.stampGuid.empty())                          mk.stamp = nullptr;
		else if (!mk.stamp || mk.stamp->guid != mk.stampGuid) mk.stamp = db->GetTexture(mk.stampGuid);
		if (!mskStamp && mk.stamp) mskStamp = mk.stamp;   // the shader's single g_MskStamp slot
	}
	if (shaderGuid.empty())                              shader = nullptr;
	else if (!shader || shader->guid != shaderGuid)     shader = db->GetShader(shaderGuid);
	for (LiveState& s : liveStates)
	{
		s.albedo = s.albedoGuid.empty() ? nullptr : db->GetTexture(s.albedoGuid);
		s.normal = s.normalGuid.empty() ? nullptr : db->GetTexture(s.normalGuid);
		s.mrTex  = s.mrGuid.empty()     ? nullptr : db->GetTexture(s.mrGuid);
	}
	for (LiveLayer& ly : liveLayers)
	{
		ly.albedo = ly.albedoGuid.empty() ? nullptr : db->GetTexture(ly.albedoGuid);
		ly.normal = ly.normalGuid.empty() ? nullptr : db->GetTexture(ly.normalGuid);
		ly.mrTex  = ly.mrGuid.empty()     ? nullptr : db->GetTexture(ly.mrGuid);
		ly.mask   = ly.maskGuid.empty()   ? nullptr : db->GetTexture(ly.maskGuid);
	}
	liveSurface.height = liveSurface.heightGuid.empty() ? nullptr : db->GetTexture(liveSurface.heightGuid);
	// Substance-style separate maps: bake into the consumed formats (combined map wins if set).
	if (metalRoughGuid.empty() && (!metallicGuid.empty() || !roughnessGuid.empty()))
		mr = BakeCombinedMR(this, db);
	if (!opacityGuid.empty())
		if (Texture* od = BakeOpacityDiffuse(this, db)) diff = od;
}

// Case-insensitive equality for tween-target matching: `param` may arrive as the storage
// key ("dispScale") or as the inspector label ("Disp Scale") — both must resolve.
static bool IEq(const std::string& a, const char* b)
{
	size_t i = 0;
	for (; i < a.size() && b[i]; ++i)
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
	return i == a.size() && !b[i];
}
static bool IEq(const std::string& a, const std::string& b) { return IEq(a, b.c_str()); }

// One channel of a tween timeline: cubic-Hermite VALUE keys (t, value, inTan, outTan) — a
// key IS a waypoint and its tangents ARE the easing. Empty channel evaluates to 0.
static float EvalChan(const std::vector<float>& c, float u)
{
	const size_t n = c.size() / 4;
	if (!n) return 0.0f;
	if (n == 1 || u <= c[0]) return c[1];
	if (u >= c[(n - 1) * 4]) return c[(n - 1) * 4 + 1];
	size_t k = 1; while (k < n && u > c[k * 4]) ++k;
	const float t0 = c[(k - 1) * 4], v0 = c[(k - 1) * 4 + 1];
	const float t1 = c[k * 4],       v1 = c[k * 4 + 1];
	const float h = t1 - t0; if (h < 1e-6f) return v1;
	const float m0 = c[(k - 1) * 4 + 3] * h, m1 = c[k * 4 + 2] * h;
	const float x = (u - t0) / h, x2 = x * x, x3 = x2 * x;
	return (2 * x3 - 3 * x2 + 1) * v0 + (x3 - 2 * x2 + x) * m0 + (-2 * x3 + 3 * x2) * v1 + (x3 - x2) * m1;
}

// Color gradient stops (t, r, g, b, a): piecewise-linear, flat before/after the end stops —
// exactly what the editor's gradient bar shows.
static void SampleGrad(const std::vector<float>& s, float e, float out[4])
{
	const size_t n = s.size() / 5;
	out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f;
	if (!n) return;
	if (e <= s[0]) { for (int i = 0; i < 4; ++i) out[i] = s[1 + i]; return; }
	if (e >= s[(n - 1) * 5]) { for (int i = 0; i < 4; ++i) out[i] = s[(n - 1) * 5 + 1 + i]; return; }
	size_t k = 1; while (k < n && e > s[k * 5]) ++k;
	const float t0 = s[(k - 1) * 5], t1 = s[k * 5];
	const float x = (t1 - t0) > 1e-6f ? (e - t0) / (t1 - t0) : 0.0f;
	for (int i = 0; i < 4; ++i)
		out[i] = s[(k - 1) * 5 + 1 + i] + (s[k * 5 + 1 + i] - s[(k - 1) * 5 + 1 + i]) * x;
}

// Mesh-space uv -> the shader's TRANSFORMED uv (ApplyUVT: tiling -> offset + tween scroll ->
// rotation) — the masks live in that space; mirrors the shader math exactly.
void Material::MapMeshUV(float& u, float& v) const
{
	const bool idTile = std::fabs((float)uvTiling.x) + std::fabs((float)uvTiling.y) < 1e-6f;
	float su = u * (idTile ? 1.0f : (float)uvTiling.x) + (float)uvOffset.x + uvAnim[0];
	float sv = v * (idTile ? 1.0f : (float)uvTiling.y) + (float)uvOffset.y + uvAnim[1];
	if (std::fabs(uvRotation) > 1e-6f)
	{
		const float rr = uvRotation * 0.01745329252f, sr = std::sin(rr), cr = std::cos(rr);
		const float ru = su * cr - sv * sr, rv = su * sr + sv * cr;
		su = ru; sv = rv;
	}
	u = su; v = sv;
}

void Material::TriggerAt(const std::string& eventName, double u, double v)
{
	float su = (float)u, sv = (float)v;
	MapMeshUV(su, sv);
	float p[3] = { su, sv, 0 };
	FireEvent(eventName, p, false, -1);
}

void Material::TriggerAtHit(const std::string& eventName, const Vector3& p, double u, double v)
{
	float su = (float)u, sv = (float)v;
	MapMeshUV(su, sv);
	const float w[3] = { (float)p.x, (float)p.y, (float)p.z };
	const float t[2] = { su, sv };
	FireEvent(eventName, w, true, -1, t);
}

int Material::MaskIndex(const std::string& n) const
{
	if (n.empty()) return -1;
	for (int i = 0; i < (int)liveMasks.size(); ++i)
		if (IEq(n, liveMasks[i].name)) return i;
	return -1;
}

// Masked tween application: the BASE param stays untouched; the tweened value goes into a
// TWIN prop whose .w carries maskSlot+1 — the shader lerps base -> twin per pixel by the
// mask weight. Built-in twins: g_DispT (pom, dispScale, dispMid), g_ColorT (base rgb),
// g_EmisT (final emissive rgb), g_ParamsT (metallic, roughness, wipe). A custom "g_X"
// target writes "g_XT" — any shader supports masking by declaring the twin and calling
// NukeMasked() (nuke_material.hlsli). Twins merge within one tick (two masked tweens may
// drive different components of one twin; the LAST mask index wins the slot).
bool Material::ApplyMaskedTwin(const std::string& p, const float v[4], int mi)
{
	const float mw = (float)(mi + 1);
	auto twin = [&](const char* name, int comp, float val, float b0, float b1, float b2)
	{
		auto it = props.find(name);
		const bool fresh = it == props.end() || it->second[3] <= 0.0f;
		std::array<float, 4>& d = props[name];
		if (fresh) { d[0] = b0; d[1] = b1; d[2] = b2; }
		d[comp] = val; d[3] = mw;
		liveTwinsWritten.push_back(name);
	};
	if      (IEq(p, "dispScale") || IEq(p, "Disp Scale"))
		twin("g_DispT", 1, v[0], liveSurface.parallax, liveSurface.dispScale, liveSurface.dispMid);
	else if (IEq(p, "parallax") || IEq(p, "Parallax"))
		twin("g_DispT", 0, v[0], liveSurface.parallax, liveSurface.dispScale, liveSurface.dispMid);
	else if (IEq(p, "dispMid") || IEq(p, "Disp Mid"))
		twin("g_DispT", 2, v[0], liveSurface.parallax, liveSurface.dispScale, liveSurface.dispMid);
	else if (IEq(p, "color") || IEq(p, "Base Color"))
	{
		props["g_ColorT"] = { v[0], v[1], v[2], mw };
		liveTwinsWritten.push_back("g_ColorT");
	}
	else if (IEq(p, "emissive") || IEq(p, "Emissive"))
	{
		props["g_EmisT"] = { v[0] * emissiveIntensity, v[1] * emissiveIntensity, v[2] * emissiveIntensity, mw };
		liveTwinsWritten.push_back("g_EmisT");
	}
	else if (IEq(p, "emissiveIntensity") || IEq(p, "Emissive Intensity"))
	{
		props["g_EmisT"] = { (float)emissive.r * v[0], (float)emissive.g * v[0], (float)emissive.b * v[0], mw };
		liveTwinsWritten.push_back("g_EmisT");
	}
	else if (IEq(p, "metallic") || IEq(p, "Metallic"))
		twin("g_ParamsT", 0, v[0], metallic, roughness, wipeThreshold);
	else if (IEq(p, "roughness") || IEq(p, "Roughness"))
		twin("g_ParamsT", 1, v[0], metallic, roughness, wipeThreshold);
	else if (IEq(p, "wipe") || IEq(p, "Wipe"))
		twin("g_ParamsT", 2, v[0], metallic, roughness, wipeThreshold);
	else if (p.rfind("g_", 0) == 0)
	{
		const std::string tn = p + "T";
		props[tn] = { v[0], v[1], v[2], mw };
		liveTwinsWritten.push_back(tn);
	}
	else return false;   // no per-pixel meaning -> caller applies unmasked
	return true;
}

void Material::SetParam(const std::string& param, const float v[4])
{
	// mask fields: "mask:<name>:<field>" — the masks ARE animatable props
	if (param.rfind("mask:", 0) == 0)
	{
		const size_t c2 = param.find(':', 5);
		if (c2 == std::string::npos) return;
		const std::string mn = param.substr(5, c2 - 5), mf = param.substr(c2 + 1);
		for (LiveMask& mk : liveMasks)
		{
			if (!IEq(mn, mk.name)) continue;
			if      (IEq(mf, "scale"))    mk.scale    = v[0];
			else if (IEq(mf, "repeat"))   mk.repeat   = v[0];
			else if (IEq(mf, "rotation")) mk.rotation = v[0];
			else if (IEq(mf, "fade"))     mk.fade     = v[0];
			else if (IEq(mf, "softness")) mk.softness = v[0];
			else if (IEq(mf, "strength")) mk.strength = v[0];
			else if (IEq(mf, "cx"))       mk.cx       = v[0];
			else if (IEq(mf, "cy"))       mk.cy       = v[0];
			else if (IEq(mf, "cz"))       mk.cz       = v[0];
			return;
		}
		return;
	}
	auto is = [&](const char* key, const char* lbl) { return IEq(param, key) || IEq(param, lbl); };
	if      (is("uv", "UV Scroll"))                         { uvAnim[0] = v[0]; uvAnim[1] = v[1]; }
	else if (is("color", "Base Color"))                     color    = Color(v[0], v[1], v[2], v[3]);
	else if (is("emissive", "Emissive"))                    emissive = Color(v[0], v[1], v[2], v[3]);
	else if (is("emissiveIntensity", "Emissive Intensity")) emissiveIntensity = v[0];
	else if (is("wipe", "Wipe"))                            wipeThreshold = v[0];
	else if (is("metallic", "Metallic"))                    metallic  = v[0];
	else if (is("roughness", "Roughness"))                  roughness = v[0];
	else if (is("specular", "Specular"))                    specular  = v[0];
	else if (is("parallax", "Parallax"))                    liveSurface.parallax  = v[0];
	else if (is("dispScale", "Disp Scale"))                 liveSurface.dispScale = v[0];
	else if (is("dispMid", "Disp Mid"))                     liveSurface.dispMid   = v[0];
	else if (is("varAmount", "Variation"))                  liveSurface.varAmount = v[0];
	else if (is("varScale", "Var Cell"))                    liveSurface.varScale  = v[0];
	else if (is("varHue", "Var Hue"))                       liveSurface.varHue    = v[0];
	else if (is("footVolume", "Step Volume"))               liveSound.footVolume    = v[0];
	else if (is("ambientVolume", "Ambient Volume"))         liveSound.ambientVolume = v[0];
	else if (is("windVolume", "Wind Volume"))               liveSound.windVolume    = v[0];
	else
	{
		// Any reflected material prop, matched by field name OR inspector label; names
		// matching nothing land in the shader MatCB props map (g_*).
		bool wrote = false;
		if (TypeInfo* ti = GetType())
			for (const Field& f : ti->fields)
			{
				if (!IEq(param, f.name) && !(!f.label.empty() && IEq(param, f.label))) continue;
				void* p = f.addr(this);
				switch (f.type)
				{
					case FT::Float:  *(float*)p  = v[0];                             wrote = true; break;
					case FT::Double: *(double*)p = v[0];                             wrote = true; break;
					case FT::Vec2:   *(Vector2*)p = Vector2(v[0], v[1]);             wrote = true; break;
					case FT::Vec3:   *(Vector3*)p = Vector3(v[0], v[1], v[2]);       wrote = true; break;
					case FT::Vec4:   *(Vector4*)p = Vector4(v[0], v[1], v[2], v[3]); wrote = true; break;
					case FT::Color:  *(Color*)p  = Color(v[0], v[1], v[2], v[3]);    wrote = true; break;
					default: break;
				}
				break;
			}
		if (!wrote) props[param] = { v[0], v[1], v[2], v[3] };
	}
}

void Material::EvalTween(int index, double time, double& prevCyc)
{
	const LiveTween& tw = liveTweens[index];
	if (tw.duration <= 1e-4f || tw.param.empty()) { prevCyc = -1.0; return; }
	const double cyc = std::max(time, 0.0) / tw.duration;
	float u;
	if (tw.loop == 0)      u = (float)std::min(cyc, 1.0);
	else if (tw.loop == 2) { const double pp = std::fmod(cyc, 2.0); u = (float)(pp <= 1.0 ? pp : 2.0 - pp); }
	else                   u = (float)std::fmod(cyc, 1.0);
	// timeline trigger marks: fire every crossing since the previous evaluation
	if (!tw.trigT.empty() && prevCyc >= 0.0 && cyc > prevCyc)
		for (size_t k = 0; k < tw.trigT.size() && k < tw.trigEvent.size(); ++k)
		{
			const double T = std::min(std::max((double)tw.trigT[k], 0.0), 1.0);
			bool crossed;
			if (tw.loop == 0)      crossed = prevCyc < T && cyc >= T;
			else if (tw.loop == 1) crossed = std::floor(cyc - T) > std::floor(prevCyc - T);
			else                   crossed = std::floor((cyc - T) * 0.5) > std::floor((prevCyc - T) * 0.5)
			                              || std::floor((cyc - (2.0 - T)) * 0.5) > std::floor((prevCyc - (2.0 - T)) * 0.5);
			if (crossed) FireEvent(tw.trigEvent[k], nullptr, false, index);
		}
	prevCyc = cyc;
	float v[4];
	if (!tw.grad.empty()) SampleGrad(tw.grad, u, v);
	else for (int i = 0; i < 4; ++i) v[i] = EvalChan(tw.chan[i], u);
	// masked tween -> per-pixel twin props; everything else applies directly
	const int mi = tw.param.rfind("mask:", 0) == 0 ? -1 : MaskIndex(tw.mask);
	if (mi >= 0 && ApplyMaskedTwin(tw.param, v, mi)) return;
	SetParam(tw.param, v);
}

void Material::FireEvent(const std::string& eventName, const float* point, bool world, int excludeTween,
                         const float* uvPoint)
{
	static int depth = 0;                 // runaway chain guard (event -> tween -> event ...)
	if (depth > 8 || eventName.empty()) return;
	++depth;
	for (const LiveEvent& e : liveEvents)
	{
		if (!IEq(eventName, e.name)) continue;
		// A point fire routes the point into every mask this event's actions touch: the
		// modulating mask of each started tween and the target of any "mask:<name>:*"
		// tween/set-param. Global fires (no point) simply skip this.
		if (point)
		{
			auto touch = [&](const std::string& maskName)
			{
				if (maskName.empty()) return;
				for (LiveMask& mk : liveMasks)
					if (IEq(maskName, mk.name))
					{
						// The mask's AUTHORED space wins — the point converts, the space never
						// flips (a flip re-reads `scale` in different units and the SAME effect
						// changes size between trigger paths).
						if (mk.space == 0)
						{
							const float* uv = world ? uvPoint : point;   // hits carry both
							if (uv) { mk.cx = uv[0]; mk.cy = uv[1]; mk.cz = 0.0f; }
						}
						else if (world)
						{
							mk.cx = point[0]; mk.cy = point[1]; mk.cz = point[2];
						}
						break;
					}
			};
			auto maskOfParam = [](const std::string& p) -> std::string
			{
				if (p.rfind("mask:", 0) != 0) return std::string();
				const size_t c2 = p.find(':', 5);
				return c2 == std::string::npos ? std::string() : p.substr(5, c2 - 5);
			};
			for (const std::string& tn : e.startTweens)
				for (const LiveTween& tw : liveTweens)
				{
					const std::string& nm = tw.name.empty() ? tw.param : tw.name;
					if (!IEq(tn, nm)) continue;
					touch(tw.mask);
					touch(maskOfParam(tw.param));
					break;
				}
			for (const std::string& sp : e.setParams)
				touch(maskOfParam(sp));
		}
		for (size_t s = 0; s < e.setParams.size(); ++s)
		{
			float sv[4] = { 0, 0, 0, 0 };
			for (size_t q = 0; q < 4 && s * 4 + q < e.setValues.size(); ++q) sv[q] = e.setValues[s * 4 + q];
			SetParam(e.setParams[s], sv);
		}
		const double now = Time::getSingleton()->elapsed;
		for (const std::string& tn : e.startTweens)
			for (int ti = 0; ti < (int)liveTweens.size(); ++ti)
			{
				const std::string& nm = liveTweens[ti].name.empty() ? liveTweens[ti].param : liveTweens[ti].name;
				if (ti == excludeTween || !IEq(tn, nm)) continue;
				bool has = false;   // restart the live instance instead of stacking a second
				for (TweenRun& r : liveRuns)
					if (r.tween == ti) { r.start = now; r.prevCyc = -1.0; has = true; break; }
				if (!has) liveRuns.push_back({ ti, now, -1.0 });
				break;
			}
		break;
	}
	--depth;
}

void Material::ApplyTweens(double time)
{
	// Masked twin props go stale unless rewritten every tick: switch them off first.
	for (const std::string& tn : liveTwinsWritten)
	{
		auto it = props.find(tn);
		if (it != props.end()) it->second[3] = 0.0f;
	}
	liveTwinsWritten.clear();
	if (tweenPrevCyc.size() != liveTweens.size()) tweenPrevCyc.assign(liveTweens.size(), -1.0);
	for (int ti = 0; ti < (int)liveTweens.size(); ++ti)
		if (liveTweens[ti].runMode == 0)
			EvalTween(ti, time, tweenPrevCyc[ti]);
	for (size_t r = 0; r < liveRuns.size();)
	{
		TweenRun& run = liveRuns[r];
		if (run.tween < 0 || run.tween >= (int)liveTweens.size()) { liveRuns.erase(liveRuns.begin() + r); continue; }
		const LiveTween& tw = liveTweens[run.tween];
		const double local = time - run.start;
		EvalTween(run.tween, local, run.prevCyc);
		if (tw.loop == 0 && tw.duration > 1e-4f && local > tw.duration + 0.25)
		{ liveRuns.erase(liveRuns.begin() + r); continue; }   // once-instances retire past the end
		++r;
	}
}

void Material::PushRenderProps()
{
	// Spatial masks -> g_Msk* GPU slots (6; strength 0 = slot off). Pushed every frame so
	// tween-animated mask params (expanding rings) reach the shader live.
	if (!liveMasks.empty() || props.count("g_MskA0"))
	{
		char nm[16];
		for (int i = 0; i < 6; ++i)
		{
			const LiveMask* mk = i < (int)liveMasks.size() ? &liveMasks[i] : nullptr;
			snprintf(nm, sizeof(nm), "g_MskA%d", i);
			props[nm] = mk ? std::array<float, 4>{ mk->cx, mk->cy, mk->cz, (float)(mk->shape + mk->space * 4) }
			               : std::array<float, 4>{ 0, 0, 0, 0 };
			snprintf(nm, sizeof(nm), "g_MskB%d", i);
			props[nm] = mk ? std::array<float, 4>{ mk->scale, mk->repeat, mk->rotation * 0.01745329252f, mk->fade }
			               : std::array<float, 4>{ 0, 0, 0, 0 };
			snprintf(nm, sizeof(nm), "g_MskC%d", i);
			props[nm] = mk ? std::array<float, 4>{ mk->softness, mk->strength, mk->stamp ? 1.0f : 0.0f, 0 }
			               : std::array<float, 4>{ 0, 0, 0, 0 };
		}
	}

	// g_UVT = (tiling.xy, offset.xy + tween scroll); zero tiling reads as identity in the shader,
	// so materials that never push stay correct. Write only when live to keep the map small.
	const bool uvOn = uvTiling.x != 1.0 || uvTiling.y != 1.0 || uvOffset.x != 0.0 || uvOffset.y != 0.0
	               || uvRotation != 0.0f || uvAnim[0] != 0.0f || uvAnim[1] != 0.0f;
	const bool wipeOn = wipe != nullptr && wipeThreshold > 0.0f;
	const bool cutOn  = blendMode == Cutout;
	if (uvOn || props.count("g_UVT"))
		props["g_UVT"] = { (float)uvTiling.x, (float)uvTiling.y,
		                   (float)uvOffset.x + uvAnim[0], (float)uvOffset.y + uvAnim[1] };
	// g_UVT2 = (rotation rad, alpha cutoff (0 = off), wipe threshold (0 = off), wipe feather).
	if (uvOn || wipeOn || cutOn || props.count("g_UVT2"))
		props["g_UVT2"] = { uvRotation * 0.01745329f, cutOn ? alphaCutoff : 0.0f,
		                    wipeOn ? wipeThreshold : 0.0f, wipeFeather };
	// g_Disp = (POM depth uv-space, tess displacement world units, mid level, reserved).
	const bool dispOn = liveSurface.height != nullptr
	                 && (liveSurface.parallax > 0.0f || liveSurface.dispScale != 0.0f);
	if (dispOn || props.count("g_Disp"))
		props["g_Disp"] = { dispOn ? liveSurface.parallax : 0.0f,
		                    dispOn ? liveSurface.dispScale : 0.0f, liveSurface.dispMid, 0.0f };

	// g_Det = (detail tiling, strength, flags 1=albedo 2=normal 4=flipG, 0);
	// g_Var = (anti-tiling amount, cell scale, hue variation, flags 1=triplanar 2=vcolTint 4=vcolMask).
	const bool detOn = (detail || detailNrm) && detailStrength > 0.0f;
	if (detOn || props.count("g_Det"))
		props["g_Det"] = { detailTiling, detOn ? detailStrength : 0.0f,
		                   (detail ? 1.0f : 0.0f) + (detailNrm ? 2.0f : 0.0f)
		                 + ((detailNrm && detailNrm->invertGreen) ? 4.0f : 0.0f), 0.0f };
	const float varFlags = (triplanar ? 1.0f : 0.0f) + (vcolorMode == 1 ? 2.0f : 0.0f)
	                     + (vcolorMode == 2 ? 4.0f : 0.0f);
	const bool varOn = liveSurface.varAmount > 0.0f || varFlags != 0.0f;
	if (varOn || props.count("g_Var"))
		props["g_Var"] = { liveSurface.varAmount, liveSurface.varScale > 1e-3f ? liveSurface.varScale : 4.0f,
		                   liveSurface.varHue, varFlags };

	// BRDF pack -> g_Brdf1..4 (all defaults = the plain metallic-roughness model).
	const bool brdfOn = clearCoat > 0.0f || anisotropy != 0.0f || sheen > 0.0f
	                 || translucency > 0.0f || iridescence > 0.0f || ior != 1.5f
	                 || refractive;
	if (brdfOn || props.count("g_Brdf1"))
	{
		props["g_Brdf1"] = { clearCoat, clearCoatRoughness, anisotropy, sheen };
		props["g_Brdf2"] = { translucency, ior, iridescence, iridescenceThickness };
		props["g_Brdf3"] = { (float)sheenTint.r, (float)sheenTint.g, (float)sheenTint.b, flow ? 1.0f : 0.0f };
		props["g_Brdf4"] = { (float)translucencyTint.r, (float)translucencyTint.g, (float)translucencyTint.b,
		                     (blendMode == Transparent && refractive) ? 1.0f : 0.0f };
	}

	// GPU overlay slots: static layers claim slots first (always on), then the ACTIVE condition
	// states — set globally, overridden on some atom or painted by some mask (the asset itself
	// may carry any number; the slots are a per-draw resource budget, not an authoring cap).
	// g_Ov  = (value, threshold, feather, topOnly) — state slots carry the GLOBAL condition
	//         value here; per-atom overrides and the painted mask are patched per draw.
	// g_OvT = tint rgba; g_OvP = (metallic target, roughness target, mask3D channel, flags).
	// flags: 1 = albedo map, 2 = normal map, 4 = MR map, 8 = 2D mask map, 16 = flip normal green.
	static const auto kOvV = []{ std::array<std::string, kOverlaySlots> a; for (int i = 0; i < kOverlaySlots; ++i) a[i] = "g_Ov"  + std::to_string(i); return a; }();
	static const auto kOvT = []{ std::array<std::string, kOverlaySlots> a; for (int i = 0; i < kOverlaySlots; ++i) a[i] = "g_OvT" + std::to_string(i); return a; }();
	static const auto kOvP = []{ std::array<std::string, kOverlaySlots> a; for (int i = 0; i < kOverlaySlots; ++i) a[i] = "g_OvP" + std::to_string(i); return a; }();
	liveOvCount = 0;
	auto slotProps = [&](int i, Texture* alb, Texture* nrm, Texture* mrT, Texture* mask,
	                     const Color& tint, float metal, float rough,
	                     float value, float threshold, float feather, float topOnly)
	{
		const float flags = (alb ? 1.0f : 0.0f) + (nrm ? 2.0f : 0.0f) + (mrT ? 4.0f : 0.0f)
		                  + (mask ? 8.0f : 0.0f) + ((nrm && nrm->invertGreen) ? 16.0f : 0.0f);
		props[kOvV[i]] = { value, threshold, feather, topOnly };
		props[kOvT[i]] = { (float)tint.r, (float)tint.g, (float)tint.b, (float)tint.a };
		props[kOvP[i]] = { metal, rough, -1.0f, flags };
	};
	for (const LiveLayer& ly : liveLayers)
	{
		if (liveOvCount >= kOverlaySlots) break;
		const int i = liveOvCount++;
		liveOv[i] = { ly.albedo, ly.normal, ly.mrTex, ly.mask, std::string() };
		slotProps(i, ly.albedo, ly.normal, ly.mrTex, ly.mask, ly.color, ly.metallic, ly.roughness,
		          ly.value, ly.threshold, ly.feather, ly.topOnly);
	}
	for (const LiveState& s : liveStates)
	{
		if (liveOvCount >= kOverlaySlots) break;
		// Only states that can matter this frame take a slot: set globally, overridden on some
		// atom or painted by some mask. Dormant states cost nothing.
		if (s.state.empty() || !Surface::StateInUse(s.state)) continue;
		const int i = liveOvCount++;
		liveOv[i] = { s.albedo, s.normal, s.mrTex, nullptr, s.state };
		slotProps(i, s.albedo, s.normal, s.mrTex, nullptr, s.color, s.metallic, s.roughness,
		          (float)Surface::Condition(s.state), s.threshold, s.feather, s.topOnly);
	}
	// Unused slots that once had props must go inert (a removed layer may not linger).
	for (int i = liveOvCount; i < kOverlaySlots; ++i)
		if (props.count(kOvV[i]))
		{
			props[kOvV[i]] = { 0, 0, 0, 0 };
			props[kOvP[i]] = { -1, -1, -1, 0 };
			liveOv[i] = {};
		}
}

bool Material::SaveToFile(const std::string& path) const
{
	json j;
	j["guid"]     = guid;
	j["name"]     = matName;
	j["shader"]   = shaderGuid;
	j["color"]    = { color.r, color.g, color.b, color.a };
	j["diffuse"]  = diffuseGuid;
	j["normal"]   = normalGuid;
	j["specular"] = specularGuid;
	j["metalRough"] = metalRoughGuid;
	if (!metallicGuid.empty())  j["metallicMap"]  = metallicGuid;
	if (!roughnessGuid.empty()) j["roughnessMap"] = roughnessGuid;
	if (!opacityGuid.empty())   j["opacityMap"]   = opacityGuid;
	j["occlusion"]  = occlusionGuid;
	j["emissiveMap"]= emissiveGuid;
	if (!wipeGuid.empty())
	{
		j["wipeMap"]     = wipeGuid;
		j["wipe"]        = wipeThreshold;
		j["wipeFeather"] = wipeFeather;
	}
	j["metallic"]   = metallic;
	j["roughness"]  = roughness;
	j["specularFactor"] = specular;
	j["emissive"]   = { emissive.r, emissive.g, emissive.b };
	j["emissiveIntensity"] = emissiveIntensity;
	j["castShadows"] = castShadows;
	j["receiveShadows"] = receiveShadows;
	j["blendMode"]   = blendMode;
	if (blendMode == Cutout) j["alphaCutoff"] = alphaCutoff;
	if (uvTiling.x != 1.0 || uvTiling.y != 1.0 || uvOffset.x != 0.0 || uvOffset.y != 0.0 || uvRotation != 0.0f)
	{
		j["uvTiling"]   = { uvTiling.x, uvTiling.y };
		j["uvOffset"]   = { uvOffset.x, uvOffset.y };
		j["uvRotation"] = uvRotation;
	}
	if (!detailGuid.empty())       j["detail"] = detailGuid;
	if (!detailNormalGuid.empty()) j["detailNormal"] = detailNormalGuid;
	if (!detailGuid.empty() || !detailNormalGuid.empty())
	{
		j["detailTiling"]   = detailTiling;
		j["detailStrength"] = detailStrength;
	}
	if (triplanar)       j["triplanar"] = true;
	if (vcolorMode != 0) j["vcolorMode"] = vcolorMode;
	if (clearCoat > 0.0f || anisotropy != 0.0f || sheen > 0.0f || translucency > 0.0f
	    || iridescence > 0.0f || ior != 1.5f || refractive || !flowGuid.empty())
		j["brdf"] = { {"clearCoat", clearCoat}, {"coatRoughness", clearCoatRoughness},
		              {"anisotropy", anisotropy}, {"flow", flowGuid},
		              {"sheen", sheen}, {"sheenTint", {sheenTint.r, sheenTint.g, sheenTint.b}},
		              {"translucency", translucency},
		              {"translucencyTint", {translucencyTint.r, translucencyTint.g, translucencyTint.b}},
		              {"ior", ior}, {"refractive", refractive},
		              {"iridescence", iridescence}, {"iridescenceThickness", iridescenceThickness} };
	// LiveMaterial sections: written only when present, so plain materials stay clean.
	if (HasLive())
	{
		json l;
		for (const LiveState& s : liveStates)
			l["states"].push_back({ {"state", s.state}, {"albedo", s.albedoGuid}, {"normal", s.normalGuid},
			                        {"mr", s.mrGuid}, {"color", {s.color.r, s.color.g, s.color.b, s.color.a}},
			                        {"metallic", s.metallic}, {"roughness", s.roughness},
			                        {"threshold", s.threshold}, {"feather", s.feather},
			                        {"topOnly", s.topOnly}, {"displace", s.displace} });
		for (const LiveLayer& ly : liveLayers)
			l["layers"].push_back({ {"albedo", ly.albedoGuid}, {"normal", ly.normalGuid},
			                        {"mr", ly.mrGuid}, {"mask", ly.maskGuid},
			                        {"color", {ly.color.r, ly.color.g, ly.color.b, ly.color.a}},
			                        {"metallic", ly.metallic}, {"roughness", ly.roughness},
			                        {"value", ly.value}, {"threshold", ly.threshold},
			                        {"feather", ly.feather}, {"topOnly", ly.topOnly} });
		for (const LiveHit& h : liveHits)
			l["hits"].push_back({ {"type", h.hitType}, {"prefab", h.prefabGuid}, {"decal", h.decalGuid},
			                      {"sound", h.soundGuid}, {"event", h.eventName}, {"minImpulse", h.minImpulse},
			                      {"lifetime", h.lifetime}, {"decalSize", h.decalSize},
			                      {"decalTint", {h.decalTint.r, h.decalTint.g, h.decalTint.b, h.decalTint.a}},
			                      {"decalIntensity", h.decalIntensity}, {"decalMode", h.decalMode},
			                      {"decalFade", h.decalFade} });
		for (const LiveFoliage& f : liveFoliage)
			l["foliage"].push_back({ {"mesh", f.meshGuid}, {"material", f.matGuid}, {"density", f.density},
			                         {"scaleMin", f.scaleMin}, {"scaleMax", f.scaleMax}, {"maxSlope", f.maxSlope},
			                         {"align", f.align}, {"windBend", f.windBend}, {"interBend", f.interBend},
			                         {"seed", f.seed} });
		for (const LiveTween& tw : liveTweens)
		{
			json tj = { {"param", tw.param}, {"duration", tw.duration}, {"loop", tw.loop} };
			if (!tw.name.empty()) tj["name"] = tw.name;
			if (tw.runMode)       tj["runMode"] = tw.runMode;
			if (!tw.mask.empty()) tj["mask"] = tw.mask;
			if (!tw.trigT.empty()) { tj["trigT"] = tw.trigT; tj["trigEvent"] = tw.trigEvent; }
			if (!tw.grad.empty()) tj["grad"] = tw.grad;
			for (int c = 0; c < 4; ++c)
				if (!tw.chan[c].empty()) tj["chan" + std::to_string(c)] = tw.chan[c];
			l["tweens"].push_back(std::move(tj));
		}
		for (const LiveMask& mk : liveMasks)
			l["masks"].push_back({ {"name", mk.name}, {"space", mk.space}, {"shape", mk.shape},
			                       {"stamp", mk.stampGuid}, {"cx", mk.cx}, {"cy", mk.cy}, {"cz", mk.cz},
			                       {"scale", mk.scale}, {"repeat", mk.repeat}, {"rotation", mk.rotation},
			                       {"fade", mk.fade}, {"softness", mk.softness}, {"strength", mk.strength} });
		for (const LiveEvent& e : liveEvents)
			l["events"].push_back({ {"name", e.name}, {"startTweens", e.startTweens},
			                        {"setParams", e.setParams}, {"setValues", e.setValues} });
		if (!liveSound.footsteps.empty() || !liveSound.ambientGuid.empty() || !liveSound.windGuid.empty())
			l["sound"] = { {"footsteps", liveSound.footsteps}, {"ambient", liveSound.ambientGuid},
			               {"wind", liveSound.windGuid}, {"footVolume", liveSound.footVolume},
			               {"ambientVolume", liveSound.ambientVolume}, {"windVolume", liveSound.windVolume} };
		if (!liveSurface.heightGuid.empty() || liveSurface.dispScale != 0.0f || liveSurface.varAmount != 0.0f)
			l["surface"] = { {"height", liveSurface.heightGuid}, {"parallax", liveSurface.parallax},
			                 {"dispScale", liveSurface.dispScale},
			                 {"dispMid", liveSurface.dispMid}, {"varAmount", liveSurface.varAmount},
			                 {"varScale", liveSurface.varScale}, {"varHue", liveSurface.varHue} };
		if (!physTag.empty())       l["physTag"] = physTag;
		if (liveFriction >= 0.0f)   l["friction"] = liveFriction;
		if (liveBounce >= 0.0f)     l["bounce"] = liveBounce;
		j["live"] = l;
	}
	boost::filesystem::path p(path);
	boost::filesystem::ofstream f(p);
	if (!f) return false;
	f << j.dump(2);
	return (bool)f;
}

Material* Material::LoadFromFile(const std::string& path)
{
	boost::filesystem::ifstream f{boost::filesystem::path(path)};   // brace-init: vexing parse
	if (!f) return nullptr;
	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return LoadFromString(text);
}

Material* Material::LoadFromString(const std::string& text)
{
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded()) return nullptr;

	Material* m = new Material();
	m->guid         = j.value("guid", std::string());
	m->matName      = j.value("name", std::string());
	m->shaderGuid   = j.value("shader", std::string("world"));
	m->diffuseGuid  = j.value("diffuse", std::string());
	m->normalGuid   = j.value("normal", std::string());
	m->specularGuid = j.value("specular", std::string());
	m->metalRoughGuid = j.value("metalRough", std::string());
	m->metallicGuid   = j.value("metallicMap", std::string());
	m->roughnessGuid  = j.value("roughnessMap", std::string());
	m->opacityGuid    = j.value("opacityMap", std::string());
	m->occlusionGuid  = j.value("occlusion", std::string());
	m->emissiveGuid   = j.value("emissiveMap", std::string());
	m->wipeGuid       = j.value("wipeMap", std::string());
	m->wipeThreshold  = j.value("wipe", 0.0f);
	m->wipeFeather    = j.value("wipeFeather", 0.05f);
	m->metallic     = j.value("metallic", 0.0f);
	m->roughness    = j.value("roughness", 0.6f);
	m->specular     = j.value("specularFactor", 1.0f);
	m->emissiveIntensity = j.value("emissiveIntensity", 0.0f);
	m->castShadows = j.value("castShadows", true);
	m->receiveShadows = j.value("receiveShadows", true);
	m->blendMode   = (Material::Blend)j.value("blendMode", 0);
	m->alphaCutoff = j.value("alphaCutoff", 0.5f);
	m->uvRotation  = j.value("uvRotation", 0.0f);
	m->detailGuid       = j.value("detail", std::string());
	m->detailNormalGuid = j.value("detailNormal", std::string());
	m->detailTiling     = j.value("detailTiling", 8.0f);
	m->detailStrength   = j.value("detailStrength", 0.5f);
	m->triplanar        = j.value("triplanar", false);
	m->vcolorMode       = j.value("vcolorMode", 0);
	if (j.contains("brdf") && j["brdf"].is_object())
	{
		const json& b = j["brdf"];
		m->clearCoat          = b.value("clearCoat", 0.0f);
		m->clearCoatRoughness = b.value("coatRoughness", 0.1f);
		m->anisotropy         = b.value("anisotropy", 0.0f);
		m->flowGuid           = b.value("flow", std::string());
		m->sheen              = b.value("sheen", 0.0f);
		m->translucency       = b.value("translucency", 0.0f);
		m->ior                = b.value("ior", 1.5f);
		m->refractive         = b.value("refractive", b.value("refraction", 0.0f) > 0.0f);
		m->iridescence        = b.value("iridescence", 0.0f);
		m->iridescenceThickness = b.value("iridescenceThickness", 0.5f);
		if (b.contains("sheenTint") && b["sheenTint"].is_array() && b["sheenTint"].size() == 3)
		{ m->sheenTint.r = b["sheenTint"][0]; m->sheenTint.g = b["sheenTint"][1]; m->sheenTint.b = b["sheenTint"][2]; }
		if (b.contains("translucencyTint") && b["translucencyTint"].is_array() && b["translucencyTint"].size() == 3)
		{ m->translucencyTint.r = b["translucencyTint"][0]; m->translucencyTint.g = b["translucencyTint"][1]; m->translucencyTint.b = b["translucencyTint"][2]; }
	}
	if (j.contains("uvTiling") && j["uvTiling"].is_array() && j["uvTiling"].size() == 2)
	{ m->uvTiling.x = j["uvTiling"][0]; m->uvTiling.y = j["uvTiling"][1]; }
	if (j.contains("uvOffset") && j["uvOffset"].is_array() && j["uvOffset"].size() == 2)
	{ m->uvOffset.x = j["uvOffset"][0]; m->uvOffset.y = j["uvOffset"][1]; }
	if (j.contains("color") && j["color"].is_array() && j["color"].size() == 4)
	{
		m->color.r = j["color"][0]; m->color.g = j["color"][1];
		m->color.b = j["color"][2]; m->color.a = j["color"][3];
	}
	if (j.contains("emissive") && j["emissive"].is_array() && j["emissive"].size() == 3)
	{
		m->emissive.r = j["emissive"][0]; m->emissive.g = j["emissive"][1]; m->emissive.b = j["emissive"][2];
	}
	if (j.contains("live") && j["live"].is_object())
	{
		const json& l = j["live"];
		if (l.contains("states"))
			for (const json& s : l["states"])
			{
				LiveState st;
				st.state      = s.value("state", std::string());
				st.albedoGuid = s.value("albedo", std::string());
				st.normalGuid = s.value("normal", std::string());
				st.mrGuid     = s.value("mr", std::string());
				st.metallic   = s.value("metallic", -1.0f);
				st.roughness  = s.value("roughness", -1.0f);
				st.threshold  = s.value("threshold", 0.0f);
				st.feather    = s.value("feather", 0.25f);
				st.topOnly    = s.value("topOnly", 0.0f);
				st.displace   = s.value("displace", 0.0f);
				if (s.contains("color") && s["color"].is_array() && s["color"].size() == 4)
				{
					st.color.r = s["color"][0]; st.color.g = s["color"][1];
					st.color.b = s["color"][2]; st.color.a = s["color"][3];
				}
				m->liveStates.push_back(std::move(st));
			}
		if (l.contains("layers"))
			for (const json& s : l["layers"])
			{
				LiveLayer ly;
				ly.albedoGuid = s.value("albedo", std::string());
				ly.normalGuid = s.value("normal", std::string());
				ly.mrGuid     = s.value("mr", std::string());
				ly.maskGuid   = s.value("mask", std::string());
				ly.metallic   = s.value("metallic", -1.0f);
				ly.roughness  = s.value("roughness", -1.0f);
				ly.value      = s.value("value", 1.0f);
				ly.threshold  = s.value("threshold", 0.0f);
				ly.feather    = s.value("feather", 0.25f);
				ly.topOnly    = s.value("topOnly", 0.0f);
				if (s.contains("color") && s["color"].is_array() && s["color"].size() == 4)
				{
					ly.color.r = s["color"][0]; ly.color.g = s["color"][1];
					ly.color.b = s["color"][2]; ly.color.a = s["color"][3];
				}
				m->liveLayers.push_back(std::move(ly));
			}
		if (l.contains("hits"))
			for (const json& h : l["hits"])
			{
				LiveHit lh;
				lh.hitType    = h.value("type", std::string());
				lh.prefabGuid = h.value("prefab", std::string());
				lh.decalGuid  = h.value("decal", std::string());
				lh.soundGuid  = h.value("sound", std::string());
				lh.eventName  = h.value("event", std::string());
				lh.minImpulse = h.value("minImpulse", 0.0f);
				lh.lifetime   = h.value("lifetime", 4.0f);
				lh.decalSize  = h.value("decalSize", 0.5f);
				lh.decalIntensity = h.value("decalIntensity", 1.0f);
				lh.decalMode      = h.value("decalMode", 2);
				lh.decalFade      = h.value("decalFade", 0.35f);
				if (h.contains("decalTint") && h["decalTint"].is_array() && h["decalTint"].size() == 4)
				{
					lh.decalTint.r = h["decalTint"][0]; lh.decalTint.g = h["decalTint"][1];
					lh.decalTint.b = h["decalTint"][2]; lh.decalTint.a = h["decalTint"][3];
				}
				m->liveHits.push_back(std::move(lh));
			}
		if (l.contains("foliage"))
			for (const json& f : l["foliage"])
			{
				LiveFoliage lf;
				lf.meshGuid  = f.value("mesh", std::string());
				lf.matGuid   = f.value("material", std::string());
				lf.density   = f.value("density", 2.0f);
				lf.scaleMin  = f.value("scaleMin", 0.8f);
				lf.scaleMax  = f.value("scaleMax", 1.3f);
				lf.maxSlope  = f.value("maxSlope", 45.0f);
				lf.align     = f.value("align", 1.0f);
				lf.windBend  = f.value("windBend", 1.0f);
				lf.interBend = f.value("interBend", 1.0f);
				lf.seed      = f.value("seed", 1337);
				m->liveFoliage.push_back(std::move(lf));
			}
		if (l.contains("tweens"))
			for (const json& t : l["tweens"])
			{
				LiveTween tw;
				tw.param    = t.value("param", std::string());
				tw.duration = t.value("duration", 1.0f);
				tw.loop     = t.value("loop", 1);
				tw.name    = t.value("name", std::string());
				tw.runMode = t.value("runMode", 0);
				tw.mask    = t.value("mask", std::string());
				if (t.contains("trigT") && t["trigT"].is_array())
					tw.trigT = t["trigT"].get<std::vector<float>>();
				if (t.contains("trigEvent") && t["trigEvent"].is_array())
					tw.trigEvent = t["trigEvent"].get<std::vector<std::string>>();
				for (int c = 0; c < 4; ++c)
				{
					const std::string ck = "chan" + std::to_string(c);
					if (t.contains(ck) && t[ck].is_array())
						tw.chan[c] = t[ck].get<std::vector<float>>();
				}
				if (t.contains("grad") && t["grad"].is_array())
					tw.grad = t["grad"].get<std::vector<float>>();
				if (tw.grad.empty() && tw.chan[0].empty() && tw.chan[1].empty()
				 && tw.chan[2].empty() && tw.chan[3].empty())
				{
					// Legacy tweens: "stops" (t,x,y,z,w) or a from/to pair -> one timeline.
					std::vector<float> st;
					if (t.contains("stops") && t["stops"].is_array() && t["stops"].size() % 5 == 0)
						st = t["stops"].get<std::vector<float>>();
					if (st.empty())
					{
						float f[4] = { 0, 0, 0, 0 }, o[4] = { 1, 0, 0, 0 };
						if (t.contains("from") && t["from"].is_array() && t["from"].size() == 4)
							for (int i = 0; i < 4; ++i) f[i] = t["from"][i];
						if (t.contains("to") && t["to"].is_array() && t["to"].size() == 4)
							for (int i = 0; i < 4; ++i) o[i] = t["to"][i];
						st = { 0, f[0], f[1], f[2], f[3],  1, o[0], o[1], o[2], o[3] };
					}
					if (tw.param == "color" || tw.param == "emissive")
						tw.grad = st;   // same layout: (t, r, g, b, a)
					else
					{
						const size_t ns = st.size() / 5;
						for (int c = 0; c < 4; ++c)
							for (size_t s = 0; s < ns; ++s)
							{
								// auto tangents from neighbour slopes keep the linear shape
								const size_t qa = s ? s - 1 : 0, qb = s + 1 < ns ? s + 1 : ns - 1;
								const float dt = st[qb * 5] - st[qa * 5];
								const float mm = dt > 1e-6f ? (st[qb * 5 + 1 + c] - st[qa * 5 + 1 + c]) / dt : 0.f;
								tw.chan[c].insert(tw.chan[c].end(), { st[s * 5], st[s * 5 + 1 + c], mm, mm });
							}
					}
				}
				m->liveTweens.push_back(std::move(tw));
			}
		if (l.contains("masks"))
			for (const json& mj : l["masks"])
			{
				LiveMask mk;
				mk.name      = mj.value("name", std::string());
				mk.space     = mj.value("space", 0);
				mk.shape     = mj.value("shape", 0);
				mk.stampGuid = mj.value("stamp", std::string());
				mk.cx = mj.value("cx", 0.5f); mk.cy = mj.value("cy", 0.5f); mk.cz = mj.value("cz", 0.0f);
				mk.scale    = mj.value("scale", 0.25f);
				mk.repeat   = mj.value("repeat", 0.0f);
				mk.rotation = mj.value("rotation", 0.0f);
				mk.fade     = mj.value("fade", 0.0f);
				mk.softness = mj.value("softness", 0.25f);
				mk.strength = mj.value("strength", 1.0f);
				m->liveMasks.push_back(std::move(mk));
			}
		if (l.contains("events"))
			for (const json& ej : l["events"])
			{
				LiveEvent e;
				e.name = ej.value("name", std::string());
				if (ej.contains("startTweens") && ej["startTweens"].is_array())
					e.startTweens = ej["startTweens"].get<std::vector<std::string>>();
				if (ej.contains("setParams") && ej["setParams"].is_array())
					e.setParams = ej["setParams"].get<std::vector<std::string>>();
				if (ej.contains("setValues") && ej["setValues"].is_array())
					e.setValues = ej["setValues"].get<std::vector<float>>();
				m->liveEvents.push_back(std::move(e));
			}
		if (l.contains("sound") && l["sound"].is_object())
		{
			const json& s = l["sound"];
			if (s.contains("footsteps"))
				for (const json& fs : s["footsteps"]) m->liveSound.footsteps.push_back(fs.get<std::string>());
			m->liveSound.ambientGuid   = s.value("ambient", std::string());
			m->liveSound.windGuid      = s.value("wind", std::string());
			m->liveSound.footVolume    = s.value("footVolume", 1.0f);
			m->liveSound.ambientVolume = s.value("ambientVolume", 1.0f);
			m->liveSound.windVolume    = s.value("windVolume", 1.0f);
		}
		if (l.contains("surface") && l["surface"].is_object())
		{
			const json& s = l["surface"];
			m->liveSurface.heightGuid = s.value("height", std::string());
			m->liveSurface.parallax   = s.value("parallax", 0.0f);
			m->liveSurface.dispScale  = s.value("dispScale", 0.0f);
			m->liveSurface.dispMid    = s.value("dispMid", 0.5f);
			m->liveSurface.varAmount  = s.value("varAmount", 0.0f);
			m->liveSurface.varScale   = s.value("varScale", 4.0f);
			m->liveSurface.varHue     = s.value("varHue", 0.0f);
		}
		m->physTag      = l.value("physTag", std::string());
		m->liveFriction = l.value("friction", -1.0f);
		m->liveBounce   = l.value("bounce", -1.0f);
	}
	return m;
}
}  // namespace nuke
