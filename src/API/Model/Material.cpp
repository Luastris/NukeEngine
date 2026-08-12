#include "API/Model/Material.h"
#include "API/Model/Surface.h"   // global condition values for the overlay-state slots
#include "API/Model/resdb.h"
#include "interface/AppInstance.h"   // renderer access: invalidate re-baked textures
#include <render/irender.h>
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <array>
#include <cmath>
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
	m->shaderGuid  = shaderGuid;
	m->props       = props;
	m->liveStates  = liveStates;
	m->liveLayers  = liveLayers;
	m->liveHits    = liveHits;
	m->liveFoliage = liveFoliage;
	m->liveTweens  = liveTweens;
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

// Cubic-bezier easing with control points at x = 1/3 and 2/3 (so x(u) == u exactly) and the
// tween's Y values — evaluate y(u) directly.
static float TweenEase(float u, float y1, float y2)
{
	const float iu = 1.0f - u;
	return 3.0f * iu * iu * u * y1 + 3.0f * iu * u * u * y2 + u * u * u;
}

void Material::ApplyTweens(double time)
{
	for (const LiveTween& tw : liveTweens)
	{
		if (tw.duration <= 1e-4f) continue;
		float u;
		const double cyc = time / tw.duration;
		if (tw.loop == 0)      u = (float)std::min(cyc, 1.0);
		else if (tw.loop == 2) { const double p = std::fmod(cyc, 2.0); u = (float)(p <= 1.0 ? p : 2.0 - p); }
		else                   u = (float)std::fmod(cyc, 1.0);
		const float e = TweenEase(u, tw.bez1, tw.bez2);
		float v[4];
		for (int i = 0; i < 4; ++i) v[i] = tw.from[i] + (tw.to[i] - tw.from[i]) * e;
		if      (tw.param == "uv")        { uvAnim[0] = v[0]; uvAnim[1] = v[1]; }
		else if (tw.param == "color")     color    = Color(v[0], v[1], v[2], v[3]);
		else if (tw.param == "emissive")  emissive = Color(v[0], v[1], v[2], 1.0);
		else if (tw.param == "emissiveIntensity") emissiveIntensity = v[0];
		else if (tw.param == "wipe")      wipeThreshold = v[0];
		else if (tw.param == "metallic")  metallic  = v[0];
		else if (tw.param == "roughness") roughness = v[0];
		else if (tw.param == "specular")  specular  = v[0];
		else if (!tw.param.empty())       props[tw.param] = { v[0], v[1], v[2], v[3] };   // custom MatCB prop
	}
}

void Material::PushRenderProps()
{
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
			                      {"sound", h.soundGuid}, {"minImpulse", h.minImpulse},
			                      {"lifetime", h.lifetime}, {"decalSize", h.decalSize} });
		for (const LiveFoliage& f : liveFoliage)
			l["foliage"].push_back({ {"mesh", f.meshGuid}, {"material", f.matGuid}, {"density", f.density},
			                         {"scaleMin", f.scaleMin}, {"scaleMax", f.scaleMax}, {"maxSlope", f.maxSlope},
			                         {"align", f.align}, {"windBend", f.windBend}, {"interBend", f.interBend},
			                         {"seed", f.seed} });
		for (const LiveTween& tw : liveTweens)
			l["tweens"].push_back({ {"param", tw.param},
			                        {"from", {tw.from[0], tw.from[1], tw.from[2], tw.from[3]}},
			                        {"to",   {tw.to[0],   tw.to[1],   tw.to[2],   tw.to[3]}},
			                        {"duration", tw.duration}, {"loop", tw.loop},
			                        {"bez1", tw.bez1}, {"bez2", tw.bez2} });
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
				lh.minImpulse = h.value("minImpulse", 0.0f);
				lh.lifetime   = h.value("lifetime", 4.0f);
				lh.decalSize  = h.value("decalSize", 0.5f);
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
				tw.bez1     = t.value("bez1", 0.333f);
				tw.bez2     = t.value("bez2", 0.667f);
				if (t.contains("from") && t["from"].is_array() && t["from"].size() == 4)
					for (int i = 0; i < 4; ++i) tw.from[i] = t["from"][i];
				if (t.contains("to") && t["to"].is_array() && t["to"].size() == 4)
					for (int i = 0; i < 4; ++i) tw.to[i] = t["to"][i];
				m->liveTweens.push_back(std::move(tw));
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
