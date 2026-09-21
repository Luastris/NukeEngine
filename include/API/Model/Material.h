#pragma once
#ifndef NUKEE_MATERIAL_H
#define NUKEE_MATERIAL_H
#include "NukeAPI.h"
#include <iostream>
#include <string>
#include <map>
#include <array>
#include "Texture.h"
#include "Shader.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <assimp/material.h>

namespace nuke {

using namespace std;

// ---- LiveMaterial sections (all optional; a material with none of them is a plain material) ----
// A material is a full gameplay SURFACE: dynamic condition states (wet/snow/dust/rust...),
// displacement + anti-tiling variation, a sound identity, typed hit reactions and a physics
// tag. Condition VALUES come from the world environment, per-atom overrides and painted
// masks; the material only describes its RESPONSE to them.

// Response to one condition state: overlay texture set + parameter targets, blended in as the
// state value rises past `threshold`.
struct NUKEENGINE_API LiveState
{
	std::string state;                 // condition id: "wet", "snow", "dust", "rust", "mud", ...
	std::string albedoGuid;            // overlay maps (any may be empty)
	std::string normalGuid;
	std::string mrGuid;                // G = roughness, B = metallic
	Color color     = Color(1, 1, 1, 1);   // tint of the overlay (also works with no maps)
	float metallic  = -1.0f;           // target at full state; -1 = keep the base value
	float roughness = -1.0f;           // target at full state; -1 = keep the base value
	float threshold = 0.0f;            // state value where the blend starts
	float feather   = 0.25f;           // blend width past the threshold
	float topOnly   = 0.0f;            // 0 = uniform, 1 = up-facing surfaces only (snow/dust settle)
	float displace  = 0.0f;            // accumulation depth at full state (world units): real relief under tessellation, trails carve it
	// Spatial shaping (terrain today): states never blanket the world uniformly.
	float hMin = 0.0f, hMax = 0.0f;    // world-Y band the state lives in (hMax <= hMin = whole range)
	float hFeather = 8.0f;             // band edge softness (world units) — the snowline fades
	float windward = 0.0f;             // wind-facing bias: >0 = leeward slopes first, <0 = windward; 0 = off
	std::string couple;                // weight rides ANOTHER state's local weight (mud couples to wet)
	Texture* albedo = nullptr;         // runtime-resolved (Resolve())
	Texture* normal = nullptr;
	Texture* mrTex  = nullptr;
};

// Reaction to one typed hit on this surface.
struct NUKEENGINE_API LiveHit
{
	std::string hitType;               // typed hit id: "bullet", "blunt", "slash", "explosion", ...
	std::string prefabGuid;            // prefab spawned at the hit point (particles/debris; content-relative .nuprefab)
	std::string decalGuid;             // decal texture stamped at the hit
	std::string soundGuid;             // impact sound (content-relative audio file)
	// Material event fired AT the hit point (world) — masked reactions/ripples run from real
	// gameplay hits, not just the editor tool. The hit's own parameters ride along in the
	// g_Hit shader prop: (impulse, hit normal xyz).
	std::string eventName;
	float minImpulse = 0.0f;           // reactions below this impulse are skipped
	float lifetime  = 4.0f;            // spawned prefab/decal auto-destroy (seconds; 0 = keep)
	float decalSize = 0.5f;            // decal box half-size (meters)
	Color decalTint = Color(1, 1, 1, 1);
	float decalIntensity = 1.0f;
	int   decalMode = 2;               // 0 = Albedo (on top), 1 = Light Projector, 2 = Stain (lit)
	float decalFade = 0.35f;           // spread-in seconds (blood creep; 0 = instant stamp)
};

// The surface's sound identity (all refs = content-relative audio files).
struct NUKEENGINE_API LiveSound
{
	std::vector<std::string> footsteps;   // step clips, played round-robin with variation
	std::string ambientGuid;              // surface ambient loop
	std::string windGuid;                 // wind-over-surface loop
	float footVolume = 1.0f, ambientVolume = 1.0f, windVolume = 1.0f;
};

// Spatial mask: a named shape the tweens/events use to LOCALIZE effects (ripples from a
// drop land HERE, not everywhere). Every scalar below is tween-animatable as a
// "mask:<name>:<field>" target (scale 0->R + strength 1->0 = an expanding, dying ring).
// Point events move the center. Rendered per pixel by the shader (g_Msk* slots).
struct NUKEENGINE_API LiveMask
{
	std::string name;
	int space = 0;              // 0 = material UV, 1 = world position
	int shape = 0;              // 0 = filled circle, 1 = ring, 2 = stamp texture (star, splat...)
	std::string stampGuid;      // shape = 2: grayscale stamp
	float cx = 0.5f, cy = 0.5f, cz = 0.0f;   // center (UV or world)
	float scale    = 0.25f;     // radius / stamp half-size (UV units or meters)
	float repeat   = 0.0f;      // ring: extra concentric rings per radius (0 = single)
	float rotation = 0.0f;      // stamp rotation, degrees
	float fade     = 0.0f;      // 0..1 radial fade toward the edge
	float softness = 0.25f;     // edge feather, relative to the radius
	float strength = 1.0f;      // master weight (animate for decay)
	Texture* stamp = nullptr;   // runtime-resolved
};

// Named material event: what happens when a trigger fires it. Triggers come from tween
// timelines (global) or from outside — Trigger()/TriggerAt()/TriggerAtWorld() — with or
// without a point (point info is required only for localized reactions: it moves `maskAt`).
// The event carries the point and runs its actions — nothing else. A point fire routes the
// point AUTOMATICALLY into every mask the actions touch: the modulating mask of each started
// tween and the target mask of any "mask:<name>:*" tween/set-param.
struct NUKEENGINE_API LiveEvent
{
	std::string name;
	std::vector<std::string> startTweens;   // tweens (by name) started as one-shot instances
	std::vector<std::string> setParams;     // params set instantly on fire...
	std::vector<float>       setValues;     // ...4 floats per entry
};

// Parameter tween: a value animated from game boot time (STATELESS — value = f(t), so every
// instance and every save/load agrees). `param` targets a built-in ("uv" = UV scroll offset,
// "color", "emissive", "emissiveIntensity", "metallic", "roughness", "specular", surface
// scalars "dispScale"/"dispMid"/"parallax"/"varAmount"/"varScale"/"varHue"), ANY reflected
// material prop by field name (BRDF pack, UV transform, tints, live scalars), or a custom
// MatCB prop name of the material's shader (g_*).
struct NUKEENGINE_API LiveTween
{
	std::string name;                    // display/reference name (events start tweens by it)
	std::string param;                   // target (see above)
	float duration = 1.0f;               // seconds per leg
	int   loop = 1;                      // 0 = once, 1 = loop, 2 = ping-pong
	int   runMode = 0;                   // 0 = auto (runs from game time), 1 = on-event only
	// Modulating mask (by name; empty = whole surface): shader-visible targets then blend
	// per pixel by the mask weight — dispScale changes only where the mask says so.
	std::string mask;
	// Trigger marks on the timeline: crossing `trigT[k]` fires material event `trigEvent[k]`
	// (global). A tween's triggers can never (re)start that same tween — cycle guard.
	std::vector<float>       trigT;
	std::vector<std::string> trigEvent;
	// ONE timeline (0..1, scaled by duration). Non-color targets: per-component cubic-Hermite
	// VALUE curves (t, value, inTan, outTan) — a curve key IS a waypoint and its tangents ARE
	// the easing. Channels beyond the target's dimension stay empty (empty channel = 0).
	std::vector<float> chan[4];
	// Color targets (color / emissive / any reflected Color prop) animate through a visual
	// gradient instead: stops (t, r, g, b, a).
	std::vector<float> grad;
};

// Auto-foliage: any surface (mesh or terrain layer) using this material GROWS this scatter —
// the engine maintains a transient Foliage per entry over the surface (never serialized).
struct NUKEENGINE_API LiveFoliage
{
	std::string meshGuid;              // scattered mesh (.numesh)
	std::string matGuid;               // material for the instances (empty = default)
	float density  = 2.0f;             // instances per square meter
	float scaleMin = 0.8f, scaleMax = 1.3f;
	float maxSlope = 45.0f;            // degrees from horizontal; steeper spots grow nothing
	float align    = 1.0f;             // 0 = straight up, 1 = follow the surface normal
	float windBend = 1.0f;             // global wind sway
	float interBend = 1.0f;            // characters part it
	int   seed     = 1337;
};

// Static material layer: an overlay texture set blended in at a fixed weight through a 2D mask
// map in material UV space (rust patches, moss, grime). Shares the GPU overlay slots with the
// condition states (layers claim slots first).
struct NUKEENGINE_API LiveLayer
{
	std::string albedoGuid;            // overlay maps (any may be empty)
	std::string normalGuid;
	std::string mrGuid;                // G = roughness, B = metallic
	std::string maskGuid;              // R = blend mask in material UV space (empty = uniform)
	Color color     = Color(1, 1, 1, 1);   // tint of the overlay (also works with no maps)
	float metallic  = -1.0f;           // target at full weight; -1 = keep the base value
	float roughness = -1.0f;
	float value     = 1.0f;            // fixed blend weight (before mask/threshold)
	float threshold = 0.0f;            // weight where the blend starts
	float feather   = 0.25f;           // blend width past the threshold
	float topOnly   = 0.0f;            // 0 = uniform, 1 = up-facing surfaces only
	Texture* albedo = nullptr;         // runtime-resolved (Resolve())
	Texture* normal = nullptr;
	Texture* mrTex  = nullptr;
	Texture* mask   = nullptr;
};

// Static surface shape/variation: displacement height + anti-tiling.
struct NUKEENGINE_API LiveSurface
{
	std::string heightGuid;            // R = height
	float parallax  = 0.0f;            // POM depth in UV space (0 = off; typical 0.02..0.1)
	float dispScale = 0.0f;            // world units at height 1 (0 = displacement off; tessellation)
	float dispMid   = 0.5f;            // neutral height level
	float varAmount = 0.0f;            // anti-tiling strength 0..1 (0 = off)
	float varScale  = 4.0f;            // variation cell size in texture repeats
	float varHue    = 0.0f;            // per-cell color variation amount
	Texture* height = nullptr;         // runtime-resolved
};

class NUKEENGINE_API Material
{
    // Base "Object" (not "Component") keeps it OUT of the Add-Component menu.
    NUKE_CLASS(Material, Object)
public:
    char* name = nullptr;          // legacy raw name (kept for old call sites)

    std::string guid;              // asset id (generated on import; "builtin:default" for the default)
    std::string matName;           // display name
    [[nuke::prop(label="Base Color")]] Color color = Color(1, 1, 1, 1);   // base color RGBA

    // Texture asset references (.nutex GUIDs). diffuse = base color.
    [[nuke::prop(asset="texture", label="Base Color Map")]] std::string diffuseGuid;
    [[nuke::prop(asset="texture", label="Normal Map")]]     std::string normalGuid;
    // Specular reflectance map (KHR_materials_specular): tints/scales the dielectric F0 (white = 0.04).
    [[nuke::prop(asset="texture", label="Specular Map")]]   std::string specularGuid;
    [[nuke::prop(asset="texture", label="Metallic-Roughness Map")]] std::string metalRoughGuid; // G=rough,B=metal
    // Substance-style SEPARATE grayscale maps: when the combined map above is empty, these are
    // baked into one at Resolve() so a Substance texture set drops in with nothing lost.
    [[nuke::prop(asset="texture", label="Metallic Map", tip="Separate grayscale metallic (Substance export); combined Metallic-Roughness wins when both are set")]]
    std::string metallicGuid;
    [[nuke::prop(asset="texture", label="Roughness Map", tip="Separate grayscale roughness (Substance export)")]]
    std::string roughnessGuid;
    [[nuke::prop(asset="texture", label="Opacity Map", tip="Grayscale opacity baked into the base color alpha (set Blend to Transparent to see it)")]]
    std::string opacityGuid;
    [[nuke::prop(asset="texture", label="Occlusion Map")]]          std::string occlusionGuid;  // R = AO
    [[nuke::prop(asset="texture", label="Emissive Map")]]           std::string emissiveGuid;
    // Luma wipe: a grayscale mask; pixels whose luma is below the animated threshold stay,
    // the rest dissolve (feathered edge). Animate with a "wipe" tween (bezier-eased).
    [[nuke::prop(asset="texture", label="Wipe Map", tip="Grayscale luma-wipe mask; the Wipe threshold dissolves pixels by this map")]]
    std::string wipeGuid;
    [[nuke::prop(label="Wipe", min=0, max=1, tip="Luma-wipe threshold: 0 = fully visible, 1 = fully dissolved; animate with a \"wipe\" tween")]]
    float wipeThreshold = 0.0f;
    [[nuke::prop(label="Wipe Feather", min=0, max=0.5, tip="Softness of the dissolve edge")]]
    float wipeFeather = 0.05f;
    [[nuke::prop(label="Metallic", min=0, max=1)]]  float metallic  = 0.0f;
    [[nuke::prop(label="Roughness", min=0, max=1)]] float roughness = 0.6f;
    [[nuke::prop(label="Specular", min=0, max=1)]]  float specular = 1.0f;   // KHR specular factor (1 = 0.04 F0)
    [[nuke::prop(label="Emissive")]]  Color emissive = Color(0, 0, 0, 1);
    [[nuke::prop(label="Emissive Intensity")]] float emissiveIntensity = 0.0f;
    // Casts shadows; the shadow pass alpha-dithers by material alpha, so transparent surfaces work too.
    [[nuke::prop(label="Cast Shadows")]] bool castShadows = true;
    [[nuke::prop(label="Receive Shadows", tip="Off: surfaces with this material ignore all shadowing and stay fully lit.")]] bool receiveShadows = true;
    // Opaque writes depth; Transparent/Additive blend, don't write depth, and sort back-to-front.
    // Cutout draws opaque but clips pixels whose base alpha falls below Alpha Cutoff.
    enum Blend : int { Opaque = 0, Transparent = 1, Additive = 2, Cutout = 3 };
    [[nuke::prop(label="Blend", enum="Opaque,Transparent,Additive,Cutout")]] Blend blendMode = Opaque;
    [[nuke::prop(label="Alpha Cutoff", min=0.01, max=1, tip="Cutout blend: pixels with base alpha below this are clipped")]]
    float alphaCutoff = 0.5f;
    // UV transform applied to every material map (tween "uv" scrolls on top of the offset).
    [[nuke::prop(label="UV Tiling", tip="Texture repeats across the 0..1 UV range")]]  Vector2 uvTiling = Vector2(1, 1);
    [[nuke::prop(label="UV Offset")]]                                                  Vector2 uvOffset = Vector2(0, 0);
    [[nuke::prop(label="UV Rotation", min=-360, max=360, tip="Degrees around the UV origin")]] float uvRotation = 0.0f;
    // Detail maps: a second texture set tiled at its own (usually much higher) frequency,
    // overlay-blended so close-ups keep texture where the base map runs out of texels.
    [[nuke::prop(asset="texture", label="Detail Map", tip="High-frequency albedo detail, overlay-blended (gray = neutral)")]]
    std::string detailGuid;
    [[nuke::prop(asset="texture", label="Detail Normal", tip="High-frequency normal detail")]]
    std::string detailNormalGuid;
    [[nuke::prop(label="Detail Tiling", min=0.1, max=256, tip="Detail repeats per base UV tile")]]
    float detailTiling = 8.0f;
    [[nuke::prop(label="Detail Strength", min=0, max=1)]]
    float detailStrength = 0.5f;
    // World-space triplanar projection: albedo blends across the three planes, the remaining
    // maps follow the dominant plane's UV. For cliffs/meshes with poor or missing UVs.
    [[nuke::prop(label="Triplanar", tip="Project maps by world position instead of mesh UVs (UV Tiling = repeats per meter)")]]
    bool triplanar = false;
    // Vertex-color use (meshes with a color stream): tint the base color, or modulate the
    // first four overlay slots' weights by R/G/B/A (painted-in-DCC material blending).
    [[nuke::prop(label="Vertex Color", enum="Off,Tint,Overlay Mask", tip="Tint: multiply base color; Overlay Mask: R/G/B/A drive overlay slots 0-3")]]
    int vcolorMode = 0;
    // ---- BRDF pack (defaults = the plain metallic-roughness model) ----
    [[nuke::prop(label="Clear Coat", min=0, max=1, tip="Lacquer layer: a second glossy highlight on top (car paint, varnish)")]]
    float clearCoat = 0.0f;
    [[nuke::prop(label="Coat Roughness", min=0.01, max=1)]]
    float clearCoatRoughness = 0.1f;
    [[nuke::prop(label="Anisotropy", min=-1, max=1, tip="Stretches the highlight along the surface tangent (brushed metal); negative = across")]]
    float anisotropy = 0.0f;
    [[nuke::prop(asset="texture", label="Flow Map", tip="RG = tangent direction for anisotropy (0.5,0.5 = neutral); brushed circles, hair flow")]]
    std::string flowGuid;
    [[nuke::prop(label="Sheen", min=0, max=1, tip="Soft grazing highlight (velvet, fabric)")]]
    float sheen = 0.0f;
    [[nuke::prop(label="Sheen Tint")]] Color sheenTint = Color(1, 1, 1, 1);
    [[nuke::prop(label="Translucency", min=0, max=1, tip="Light through thin surfaces (leaves, wax, skin-ish)")]]
    float translucency = 0.0f;
    [[nuke::prop(label="Translucency Tint")]] Color translucencyTint = Color(1, 0.9, 0.8, 1);
    [[nuke::prop(label="IOR", min=1, max=3, tip="Index of refraction: 1.33 water, 1.5 glass, 2.4 diamond. Drives the dielectric F0 AND how hard Refraction bends the background.")]]
    float ior = 1.5f;
    [[nuke::prop(label="Refraction", tip="Transparent blend: bend the background through the surface; the bend strength comes from the IOR")]]
    bool refractive = false;
    [[nuke::prop(label="Iridescence", min=0, max=1, tip="Thin-film rainbow (soap bubble, oil slick)")]]
    float iridescence = 0.0f;
    [[nuke::prop(label="Iridescence Thickness", min=0, max=1, tip="Film thickness sweep - shifts the rainbow bands")]]
    float iridescenceThickness = 0.5f;
    // Toon shading (cel bands; anime/MToon-style avatars). The diffuse light response
    // quantizes into a lit/shade band; the shade side tints with Toon Shade.
    [[nuke::prop(label="Toon", min=0, max=1, tip="Cel-shading band threshold; 0 = off (plain PBR)")]]
    float toonBand = 0.0f;
    [[nuke::prop(label="Toon Soft", min=0, max=0.5, tip="Band edge softness")]]
    float toonSoft = 0.05f;
    [[nuke::prop(label="Toon Shade", tip="Tint of the unlit side of the band")]]
    Color toonShade = Color(0.6, 0.55, 0.65, 1.0);
    // C5 shading polish: skin scattering, eye depth, hair-card alpha quality.
    [[nuke::prop(label="Subsurface", min=0, max=1, tip="Skin-style scattering: wrapped soft diffuse, the scatter tint bleeds into the terminator; pair with Translucency for backlit ears/nose")]]
    float subsurface = 0.0f;
    [[nuke::prop(label="Subsurface Tint", tip="Scatter color - reddish for skin (blood under the surface)")]]
    Color subsurfaceTint = Color(0.9, 0.36, 0.3, 1.0);
    [[nuke::prop(label="Iris Depth", min=0, max=0.5, tip="Eye shading: view parallax sinks the iris under the cornea so the eye reads as a sphere with depth; 0 = off")]]
    float irisDepth = 0.0f;
    [[nuke::prop(label="Hashed Alpha", tip="Cutout blend: stochastic alpha test instead of the hard threshold - soft hair-card edges (shadow dither already matches)")]]
    bool hashedAlpha = false;

    Texture* diff = nullptr;       // runtime-resolved textures (via Resolve())
    Texture* norm = nullptr;
    Texture* spec = nullptr;
    Texture* mr   = nullptr;       // metallic-roughness
    Texture* ao   = nullptr;       // occlusion
    Texture* em   = nullptr;       // emissive
    Texture* wipe = nullptr;       // luma-wipe mask
    Texture* detail = nullptr;     // detail albedo
    Texture* detailNrm = nullptr;  // detail normal
    Texture* flow = nullptr;       // anisotropy flow map

    Shader*      shader = nullptr;
    aiMaterial*  aiMat  = nullptr;

    // Shader asset ref; default = engine "world".
    [[nuke::prop(asset="shader", label="Shader")]] std::string shaderGuid = "world";

    // Custom shader-parameter values, keyed by the param name from the shader's MatCB (Shader::props).
    // Only meaningful on a material INSTANCE; the shared asset leaves this empty.
    std::map<std::string, std::array<float, 4>> props;

    // Generic named shader TEXTURES (runtime-only, not serialized): bound by variable name on
    // pipes whose shader declares a matching g_Layer* SRV (terrain palette normal/MR maps etc.).
    std::vector<std::pair<std::string, Texture*>> extraTex;

    // ---- LiveMaterial (optional; see the section structs above) ----
    std::vector<LiveState> liveStates;    // condition responses (wet/snow/dust/rust/...)
    std::vector<LiveLayer> liveLayers;    // static overlay layers (fixed weight + 2D mask map)
    std::vector<LiveHit>   liveHits;      // typed hit reactions
    std::vector<LiveFoliage> liveFoliage; // auto-foliage grown on surfaces using this material
    std::vector<LiveTween>   liveTweens;  // parameter animations (stateless, from game boot time)
    std::vector<LiveMask>    liveMasks;   // spatial masks localizing tween effects (GPU g_Msk*)
    std::vector<LiveEvent>   liveEvents;  // named reactions the triggers fire
    float uvAnim[2] = { 0, 0 };           // runtime: current "uv" tween offset
    void ApplyTweens(double time);        // evaluate liveTweens at `time` into this material's fields
    // --- event/trigger runtime (never serialized) --------------------------------------
    struct TweenRun { int tween = -1; double start = 0.0; double prevCyc = -1.0; };
    std::vector<TweenRun>    liveRuns;         // event-started tween instances
    std::vector<double>      tweenPrevCyc;     // per-auto-tween trigger crossing detection
    std::vector<std::string> liveTwinsWritten; // masked twin props written last tick
    Texture* mskStamp = nullptr;               // first stamp-mask texture (shader g_MskStamp)
    Texture* mask = nullptr;                   // alpha mask over diff (sprite Shape x texture); rays alpha-test with it too
    // Fire a material event (scriptable): global, at a UV point, or at a world point. Point
    // info moves the driven masks' centers; global fires skip that (point OPTIONAL for
    // global reactions, required only for localized ones).
    [[nuke::func]] void Trigger(const std::string& eventName)           { FireEvent(eventName, nullptr, false, -1); }
    // `u`/`v` are MESH-space UVs (exactly what Physics.HitUV yields); mapped through the
    // material's UV transform internally — the masks live in the shader's transformed UV.
    [[nuke::func]] void TriggerAt(const std::string& eventName, double u, double v);
    [[nuke::func]] void TriggerAtWorld(const std::string& eventName, const Vector3& p) { float q[3] = { (float)p.x, (float)p.y, (float)p.z }; FireEvent(eventName, q, true, -1); }
    // Fire with BOTH the world point and the MESH-space uv under it (what a hit knows): every
    // routed mask receives the point in its AUTHORED space, so the effect size never depends
    // on the trigger path. Prefer this whenever the uv is available.
    [[nuke::func]] void TriggerAtHit(const std::string& eventName, const Vector3& p, double u, double v);
    // Set ANY material parameter by key or label (the tween-target resolver: built-ins,
    // liveSurface, sound volumes, mask:<name>:<field>, reflected fields, custom g_*).
    [[nuke::func]] void SetScalar(const std::string& param, double v)   { float q[4] = { (float)v, 0, 0, 0 }; SetParam(param, q); }
    [[nuke::func]] void SetVector(const std::string& param, const Vector3& v, double w) { float q[4] = { (float)v.x, (float)v.y, (float)v.z, (float)w }; SetParam(param, q); }
    // excludeTween: cycle guard — events fired from a tween's timeline can never (re)start it.
    // uvPoint (optional, with a WORLD point): the shader-space uv under the hit for UV masks.
    void FireEvent(const std::string& eventName, const float* point, bool world, int excludeTween,
                   const float* uvPoint = nullptr);
    void MapMeshUV(float& u, float& v) const;   // mesh uv -> the shader's transformed uv
    void SetParam(const std::string& param, const float v[4]);   // tween-target resolver, applied once
    int  MaskIndex(const std::string& name) const;               // -1 = none
private:
    void EvalTween(int index, double localTime, double& prevCyc);
    bool ApplyMaskedTwin(const std::string& param, const float v[4], int maskIndex);
public:
    // Mirror the UV transform / cutout / wipe state into `props` (g_UVT/g_UVT2) for the world
    // shader's MatCB. Called each frame for every material in use; cheap when all-default.
    // Also assigns the GPU overlay slots (liveLayers first, then liveStates, capped at
    // kOverlaySlots) and writes their g_Ov* props (state slots carry the GLOBAL condition
    // value; per-atom overrides/masks are patched per draw via the liveDraw* context below).
    void PushRenderProps();

    // ---- GPU overlay slots (renderer-facing runtime, never serialized) ----
    // The asset carries ANY number of layers/states; PushRenderProps assigns the ACTIVE ones
    // (all layers, then states that are set globally / overridden / painted anywhere) to the
    // GPU slots. The slot count is a resource budget (SRV/CB space), not an authoring cap.
    static const int kOverlaySlots = 8;
    struct OverlayRT
    {
        Texture* albedo = nullptr;    // resolved maps of the slot's layer/state
        Texture* normal = nullptr;
        Texture* mrTex  = nullptr;
        Texture* mask   = nullptr;    // 2D mask map (layers only)
        std::string state;            // condition id; empty = static layer
    };
    int       liveOvCount = 0;
    OverlayRT liveOv[kOverlaySlots];
    // Per-draw condition context: written by Surface::PushDrawContext right before the draw is
    // submitted (the renderer consumes it inside that synchronous call and clears liveDrawSet).
    bool      liveDrawSet = false;
    float     liveDrawValue[kOverlaySlots]    = { -1, -1, -1, -1, -1, -1, -1, -1 };   // effective uniform value; -1 = keep CB
    float     liveDrawMaskChan[kOverlaySlots] = { -1, -1, -1, -1, -1, -1, -1, -1 };   // painted-mask channel; -1 = none
    Texture*  liveDrawMask3D = nullptr;      // nearest SurfaceMask, flipbook-encoded (res*res x res)
    float     liveDrawMaskXform[12] = {};    // world -> mask uvw, 3 rows of (xyz, w)
    float     liveDrawMaskRes = 0.0f;
    LiveSound   liveSound;               // footsteps / ambient / wind
    LiveSurface liveSurface;             // displacement + anti-tiling variation
    [[nuke::prop(label="Physics Tag", tip="Surface identity for gameplay/physics queries (e.g. metal, wood, flesh); empty = untagged")]]
    std::string physTag;
    [[nuke::prop(label="Friction", min=-1, max=2, tip="Surface friction override; -1 = keep the body's value")]]
    float liveFriction = -1.0f;
    [[nuke::prop(label="Bounciness", min=-1, max=1, tip="Surface restitution override; -1 = keep the body's value")]]
    float liveBounce = -1.0f;
    // Flammability + burn-out destruction (the fire system): liveIgnite < 0 = fireproof. The
    // charring visual is the material's OWN "burn" condition state, driven 0..1 while burning.
    float liveIgnite = -1.0f;            // seconds of neighbouring heat before catching fire
    float liveBurn = 20.0f;              // burn duration to burn-out
    float liveSpread = 3.0f;             // heat radius while burning (world units)
    std::string liveFirePrefab;          // looping visual spawned while burning (flames/smoke/light)
    std::string liveDebrisPrefab;        // shatter debris (burn-out + Destruct.Shatter; empty = never shatters)
    int   liveDebrisCount = 4;
    std::string liveInsideMat;           // fracture cut faces draw THIS material (brick core ≠ brick face)
    bool HasLive() const;                // any live section present (drives .numat serialization)

    Material();

    void ImportAiMaterial(aiMaterial* m);   // name + color only (textures handled by the importer)
    void Resolve();                         // bind diff/norm/spec from ResDB by GUID
    // Deep copy for instancing: edits live on the instance and save with the world, never touching
    // the original .numat. Re-resolves its texture/shader pointers from ResDB.
    Material* Clone() const;

    // Native asset format (.numat, JSON): guid + name + color + texture GUIDs.
    bool             SaveToFile(const std::string& path) const;
    static Material* LoadFromFile(const std::string& path);
    static Material* LoadFromString(const std::string& text);   // packed content

    // Per-draw overlay context (appended): bit per slot = this draw's value is an explicit
    // SurfaceState override, so the renderer lifts the from-sky gate (g_OvP flag 32) for it.
    unsigned char liveDrawNoSky = 0;
    // W5 (appended): the deepest active state displacement (state value > 0) - the renderer
    // tessellates the draw so the accumulated layer is real depth (world.ds / terrain DS).
    float liveStateDisp = 0.0f;
};
}  // namespace nuke

#endif // !NUKEE_MATERIAL_H
