// Scalability presets: the tier table, the caps, persistence and hardware autodetect.
#include "API/Model/Quality.h"
#include "API/Model/Cvar.h"
#include "API/Model/Events.h"
#include "API/Model/Game.h"
#include "API/Model/Jobs.h"
#include "API/Model/Log.h"
#include "interface/AppInstance.h"
#include "render/irender.h"
#include "config.h"
#include <boost/thread/mutex.hpp>
#include <boost/thread.hpp>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>

namespace nuke {

namespace {
struct Feature { const char* name; const char* desc; };
const Feature kFeatures[] = {
	{ "raytracing",   "Ray tracing on the next launch (window.rayTracing): Low/Medium off, High/Ultra on" },
	{ "reflections",  "RT reflections: Low/Medium = screen-space only, High = 1 bounce, Ultra = as configured" },
	{ "shadows",      "Shadow map cap 1024 / 2048 / 4096 / authored; shadow distance x0.5 / x0.75 / x1 / x1" },
	{ "ao",           "Ambient occlusion cap: SSAO / HBAO / VBAO / any (RT-AO)" },
	{ "gi",           "Dynamic GI probes (DDGI): Low off, else as authored" },
	{ "ssgi",         "Screen-space GI cap: off / low / medium / high" },
	{ "volumetrics",  "Volumetric fog cap: low / low / medium / high" },
	{ "clouds",       "Cloud quality cap: low / medium / high / high" },
	{ "water",        "Water FX: Low = no ripples, wake foam, hull flow, tessellation; Medium = no hull flow / tessellation; else all" },
	{ "foliage",      "Foliage density x0.35 / x0.6 / x0.85 / x1" },
	{ "particles",    "Particle counts x0.35 / x0.6 / x0.85 / x1" },
	{ "tessellation", "Displacement tessellation: off / x0.5 / x1 / x1" },
	{ "postfx",       "Post effects: Low = no motion blur, no depth of field; Medium = no motion blur; else all" },
	{ "textures",     "Texture streaming budget 1024 / 2048 / 4096 MB / unlimited (window.textureStreamMB)" },
	{ "upscale",      "Upscaler quality when upscaling is on: Performance / Balanced / Quality / Native" },
};
const int kFeatureCount = (int)(sizeof(kFeatures) / sizeof(kFeatures[0]));
const char* kPresets[] = { "Low", "Medium", "High", "Ultra" };

boost::mutex g_mx;
int  g_preset = 2;                    // High until config / autodetect say otherwise
int  g_tier[kFeatureCount];           // effective tiers
bool g_over[kFeatureCount];           // overridden?
bool g_loaded = false;
int  g_version = 1;
std::string g_adapter; double g_adapterMB = 0.0; bool g_adapterRT = false, g_adapterDiscrete = false, g_adapterKnown = false;

int FeatureIndex(const std::string& name)
{
	for (int i = 0; i < kFeatureCount; ++i) if (name == kFeatures[i].name) return i;
	return -1;
}
int PresetIndex(const std::string& name)
{
	std::string l = name; for (char& c : l) c = (char)tolower((unsigned char)c);
	for (int i = 0; i < 4; ++i) { std::string p = kPresets[i]; for (char& c : p) c = (char)tolower((unsigned char)c); if (p == l) return i; }
	return -1;
}

// Config ["quality"] -> the tables (once); nothing there = High until Boot autodetects.
void LoadLocked()
{
	if (g_loaded) return;
	g_loaded = true;
	for (int i = 0; i < kFeatureCount; ++i) { g_tier[i] = g_preset; g_over[i] = false; }
	Config* c = Config::getSingleton();
	if (!c || !c->quality.set) return;
	const int p = PresetIndex(c->quality.preset);
	if (p >= 0) { g_preset = p; for (int i = 0; i < kFeatureCount; ++i) g_tier[i] = p; }
	for (auto& kv : c->quality.overrides)
	{
		const int fi = FeatureIndex(kv.first);
		if (fi < 0) continue;
		g_tier[fi] = std::max(0, std::min(3, kv.second)); g_over[fi] = true;
	}
}

void PersistLocked()
{
	Config* c = Config::getSingleton();
	if (!c) return;
	c->quality.set = true;
	c->quality.preset = kPresets[g_preset];
	c->quality.overrides.clear();
	for (int i = 0; i < kFeatureCount; ++i) if (g_over[i]) c->quality.overrides[kFeatures[i].name] = g_tier[i];
	c->quality.adapter = g_adapter;
}

void SaveConfig()
{
	Config* c = Config::getSingleton();
	AppInstance* app = AppInstance::GetSingleton();
	if (c && app && !app->isEditor()) c->saveWindow();   // the game owns its config; the editor keeps the session
}

// The knobs that are SET rather than capped: they live in their own config keys and apply live.
void ApplyLiveKnobs(int textures, int upscale, int raytracing)
{
	Config* c = Config::getSingleton();
	if (!c) return;
	static const int kStreamMB[4] = { 1024, 2048, 4096, 0 };
	if (c->window.textureStreamMB != kStreamMB[textures]) Game::SetTextureStreaming(kStreamMB[textures]);
	if (c->upscale.set && c->upscale.mode != (int)UpscaleMode::Off)
	{
		static const UpscaleQuality kUq[4] = { UpscaleQuality::Performance, UpscaleQuality::Balanced, UpscaleQuality::Quality, UpscaleQuality::Native };
		if (Game::GetUpscaleQuality() != kUq[upscale]) Game::SetUpscaleQuality(kUq[upscale]);
	}
	const bool rtWanted = raytracing >= 2;
	if (c->window.rayTracing != rtWanted)
	{
		c->window.rayTracing = rtWanted;
		std::cout << "[Quality]\t\tray tracing " << (rtWanted ? "on" : "off") << " from the next launch (window.rayTracing)" << std::endl;
	}
	AppInstance* app = AppInstance::GetSingleton();
	if (iRender* r = app ? app->render : nullptr)
	{
		r->setRTReflection(c->rt.intensity, c->rt.maxDist, Quality::CapRTBounces(c->rt.bounces), c->rt.roughCutoff);
		r->setTessellationScale(Quality::TessellationScale());
	}
}

void Changed(const std::string& what)
{
	int tex, up, rt;
	{ boost::mutex::scoped_lock l(g_mx); ++g_version; tex = g_tier[13]; up = g_tier[14]; rt = g_tier[0]; PersistLocked(); }
	ApplyLiveKnobs(tex, up, rt);
	SaveConfig();
	Events::EmitEngine("quality.changed", what);
}
}  // namespace

// ---- the reflected surface -------------------------------------------------------------------------

bool Quality::SetPreset(const std::string& name)
{
	const int p = PresetIndex(name);
	if (p < 0) { Log::Write(LOG_WARN, "Quality", "unknown preset '" + name + "' (Low / Medium / High / Ultra)"); return false; }
	{
		boost::mutex::scoped_lock l(g_mx); LoadLocked();
		g_preset = p;
		for (int i = 0; i < kFeatureCount; ++i) { g_tier[i] = p; g_over[i] = false; }
	}
	std::cout << "[Quality]\t\tpreset " << kPresets[p] << std::endl;
	Changed("preset");
	return true;
}

std::string Quality::Preset()
{
	boost::mutex::scoped_lock l(g_mx); LoadLocked();
	for (int i = 0; i < kFeatureCount; ++i) if (g_over[i]) return "Custom";
	return kPresets[g_preset];
}

bool Quality::Set(const std::string& feature, int tier)
{
	const int fi = FeatureIndex(feature);
	if (fi < 0) { Log::Write(LOG_WARN, "Quality", "unknown feature '" + feature + "' (" + Features() + ")"); return false; }
	tier = std::max(0, std::min(3, tier));
	{
		boost::mutex::scoped_lock l(g_mx); LoadLocked();
		g_tier[fi] = tier; g_over[fi] = tier != g_preset;
	}
	Changed(feature);
	return true;
}

int Quality::Get(const std::string& feature)
{
	const int fi = FeatureIndex(feature);
	if (fi < 0) return -1;
	boost::mutex::scoped_lock l(g_mx); LoadLocked();
	return g_tier[fi];
}

bool Quality::Reset(const std::string& feature)
{
	const int fi = FeatureIndex(feature);
	if (fi < 0) return false;
	{
		boost::mutex::scoped_lock l(g_mx); LoadLocked();
		if (!g_over[fi]) return true;
		g_tier[fi] = g_preset; g_over[fi] = false;
	}
	Changed(feature);
	return true;
}

std::string Quality::Features()
{
	std::string s;
	for (int i = 0; i < kFeatureCount; ++i) { if (i) s += ","; s += kFeatures[i].name; }
	return s;
}

std::string Quality::Describe(const std::string& feature)
{
	const int fi = FeatureIndex(feature);
	if (fi < 0) return "unknown feature: " + feature;
	int t; bool o;
	{ boost::mutex::scoped_lock l(g_mx); LoadLocked(); t = g_tier[fi]; o = g_over[fi]; }
	return feature + " = " + std::to_string(t) + " (" + kPresets[t] + (o ? ", override" : "") + "): " + kFeatures[fi].desc;
}

std::string Quality::AdapterName()    { boost::mutex::scoped_lock l(g_mx); return g_adapter; }
double      Quality::AdapterMemoryMB(){ boost::mutex::scoped_lock l(g_mx); return g_adapterMB; }
bool        Quality::AdapterRayTracing(){ boost::mutex::scoped_lock l(g_mx); return g_adapterRT; }

std::string Quality::Autodetect()
{
	// VRAM + ray tracing + discrete decide; a small CPU caps the result.
	double mb; bool rt, discrete, known;
	{ boost::mutex::scoped_lock l(g_mx); mb = g_adapterMB; rt = g_adapterRT; discrete = g_adapterDiscrete; known = g_adapterKnown; }
	int p = 2;
	if (known)
	{
		if (mb >= 11000.0 && rt)            p = 3;
		else if (mb >= 7000.0)              p = 2;
		else if (mb >= 3500.0 || discrete)  p = 1;
		else                                p = 0;
	}
	const int cores = (int)boost::thread::hardware_concurrency();
	if (cores > 0 && cores < 4) p = std::min(p, 1);
	std::cout << "[Quality]\t\tautodetect: " << (known ? g_adapter : std::string("unknown adapter")) << " " << (int)mb << " MB"
	          << (rt ? " RT" : "") << (discrete ? " discrete" : "") << ", " << cores << " cores -> " << kPresets[p] << std::endl;
	SetPreset(kPresets[p]);
	return kPresets[p];
}

// ---- native ---------------------------------------------------------------------------------------

void Quality::Boot(iRender* r)
{
	NukeAdapterInfo ai;
	if (r && r->getAdapterInfo(ai))
	{
		boost::mutex::scoped_lock l(g_mx);
		g_adapter = ai.name; g_adapterMB = ai.memoryMB; g_adapterRT = ai.rayTracing; g_adapterDiscrete = ai.discrete; g_adapterKnown = true;
	}
	Config* c = Config::getSingleton();
	bool need = true;
	{
		boost::mutex::scoped_lock l(g_mx); LoadLocked();
		// A config choice stands - unless it was autodetected for another GPU.
		if (c && c->quality.set && (c->quality.adapter.empty() || c->quality.adapter == g_adapter || !g_adapterKnown)) need = false;
	}
	if (need) Autodetect();
	else
	{
		int tex, up, rt;
		{ boost::mutex::scoped_lock l(g_mx); tex = g_tier[13]; up = g_tier[14]; rt = g_tier[0]; }
		ApplyLiveKnobs(tex, up, rt);
		std::cout << "[Quality]\t\tpreset " << Preset() << " (config)" << std::endl;
	}
	BindCvars();
}

int Quality::Version() { boost::mutex::scoped_lock l(g_mx); return g_version; }

int Quality::Tier(const char* feature)
{
	const int fi = FeatureIndex(feature);
	if (fi < 0) return 3;
	boost::mutex::scoped_lock l(g_mx); LoadLocked();
	return g_tier[fi];
}

const char* Quality::TierName(int tier) { return kPresets[std::max(0, std::min(3, tier))]; }

int   Quality::CapShadowRes(int a)     { static const int cap[4] = { 1024, 2048, 4096, 1 << 20 }; return std::min(a, cap[Tier("shadows")]); }
float Quality::ShadowDistanceScale()   { static const float s[4] = { 0.5f, 0.75f, 1.0f, 1.0f }; return s[Tier("shadows")]; }
int   Quality::CapAO(int a)            { static const int cap[4] = { 1, 2, 4, 5 }; return std::min(a, cap[Tier("ao")]); }
int   Quality::CapSSGI(int a)          { return std::min(a, Tier("ssgi")); }
bool  Quality::GIEnabled(bool a)       { return a && Tier("gi") >= 1; }
int   Quality::CapVolumetrics(int a)   { static const int cap[4] = { 1, 1, 2, 3 }; return std::min(a, cap[Tier("volumetrics")]); }
int   Quality::CapClouds(int a)        { static const int cap[4] = { 0, 1, 2, 2 }; return std::min(a, cap[Tier("clouds")]); }
int   Quality::CapRTBounces(int a)     { const int t = Tier("reflections"); return t >= 3 ? a : std::min(a, 1); }
float Quality::FoliageScale()          { static const float s[4] = { 0.35f, 0.6f, 0.85f, 1.0f }; return s[Tier("foliage")]; }
float Quality::ParticleScale()         { static const float s[4] = { 0.35f, 0.6f, 0.85f, 1.0f }; return s[Tier("particles")]; }
float Quality::TessellationScale()     { static const float s[4] = { 0.0f, 0.5f, 1.0f, 1.0f }; return s[Tier("tessellation")]; }
bool  Quality::RayTracingWanted()      { return Tier("raytracing") >= 2; }

bool Quality::KeepPostStage(const char* name)
{
	if (!name) return true;
	if (std::strcmp(name, "rtreflect") == 0)  return Tier("reflections") >= 2;
	if (std::strcmp(name, "motionblur") == 0) return Tier("postfx") >= 2;
	if (std::strcmp(name, "dof") == 0)        return Tier("postfx") >= 1;
	return true;
}

void Quality::BindCvars()
{
	static bool bound = false;
	if (bound) return;
	bound = true;
	Cvars::Bind("q.preset", CvarType::String, "Quality preset: Low / Medium / High / Ultra (Custom = overrides)", CvarNone,
	            [] { return Preset(); }, [](const std::string& v) { return SetPreset(v); });
	for (int i = 0; i < kFeatureCount; ++i)
	{
		const std::string name = kFeatures[i].name;
		Cvars::Bind("q." + name, CvarType::Int, std::string("0 Low .. 3 Ultra - ") + kFeatures[i].desc, CvarNone,
		            [name] { return std::to_string(Get(name)); }, [name](const std::string& v) { return Set(name, atoi(v.c_str())); });
	}
}

}  // namespace nuke
