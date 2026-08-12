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
	float displace  = 0.0f;            // displacement contribution at full state (world units)
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
	float minImpulse = 0.0f;           // reactions below this impulse are skipped
	float lifetime  = 4.0f;            // spawned prefab/decal auto-destroy (seconds; 0 = keep)
	float decalSize = 0.5f;            // decal box half-size (meters)
};

// The surface's sound identity (all refs = content-relative audio files).
struct NUKEENGINE_API LiveSound
{
	std::vector<std::string> footsteps;   // step clips, played round-robin with variation
	std::string ambientGuid;              // surface ambient loop
	std::string windGuid;                 // wind-over-surface loop
	float footVolume = 1.0f, ambientVolume = 1.0f, windVolume = 1.0f;
};

// Parameter tween: a value animated from game boot time (STATELESS — value = f(t), so every
// instance and every save/load agrees). `param` targets a built-in ("uv" = UV scroll offset,
// "color", "emissive", "emissiveIntensity", "metallic", "roughness", "specular") or any custom
// MatCB prop name of the material's shader.
struct NUKEENGINE_API LiveTween
{
	std::string param;                   // target (see above)
	float from[4] = { 0, 0, 0, 0 };
	float to[4]   = { 1, 0, 0, 0 };
	float duration = 1.0f;               // seconds per leg
	int   loop = 1;                      // 0 = once, 1 = loop, 2 = ping-pong
	// Cubic-bezier easing: control points at x = 1/3 and 2/3 with these Y values
	// (0.333/0.667 = linear, 0/1 = ease-in-out-ish, 1/0 = overshoot flavors).
	float bez1 = 0.333f, bez2 = 0.667f;
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

    // ---- LiveMaterial (optional; see the section structs above) ----
    std::vector<LiveState> liveStates;    // condition responses (wet/snow/dust/rust/...)
    std::vector<LiveLayer> liveLayers;    // static overlay layers (fixed weight + 2D mask map)
    std::vector<LiveHit>   liveHits;      // typed hit reactions
    std::vector<LiveFoliage> liveFoliage; // auto-foliage grown on surfaces using this material
    std::vector<LiveTween>   liveTweens;  // parameter animations (stateless, from game boot time)
    float uvAnim[2] = { 0, 0 };           // runtime: current "uv" tween offset
    void ApplyTweens(double time);        // evaluate liveTweens at `time` into this material's fields
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
};
}  // namespace nuke

#endif // !NUKEE_MATERIAL_H
