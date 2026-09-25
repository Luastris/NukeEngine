#pragma once
#ifndef NUKEE_INTERFACE_H
#define NUKEE_INTERFACE_H
#include <boost/config.hpp>   // BOOST_SYMBOL_EXPORT
#include <cstdint>

#include <string>
#include <utility>   // std::pair (shipExtras dist copies)
#include <vector>
#include "AppInstance.h"

// ---- Module ABI level ---------------------------------------------------------------------
// Stamps every module DLL with the NUKEModule vtable level it was compiled against; the loader
// reads it at discovery (ModuleAbi in Modular.h) and hosts MUST guard calls to appended virtuals
// with `ModuleAbi(m) >= <level of the virtual>` — an older DLL has a shorter vtable. Unstamped
// DLLs report level 1. Bump when appending a virtual, and tag the virtual with its level:
//   1 — provides/phase/queryService/cookContent/sharedService/shipExtras
//   2 — editorTool
//   3 — companionOf
//   4 — (reserved; the iScript::ModuleDeps/PlatformOf growth is gated by nuke_engine_abi 12 —
//        service vtables have no per-call guard, so the discovery gate is the one that counts)
//   5 — cookTransform/projectSettings
// One-definition-per-image data export: dllexport+selectany on MSVC, default-visibility weak
// everywhere else (Mach-O/ELF) — every module stamps its own copy, the loader reads it per image.
#ifdef _WIN32
  #define NUKE_ABI_STAMP __declspec(dllexport) __declspec(selectany)
#else
  #define NUKE_ABI_STAMP __attribute__((visibility("default"), weak))
#endif

#define NUKE_MODULE_ABI 5

// One project-level setting a module contributes (data only — the editor renders the widget).
struct NukeModuleSetting
{
	std::string key;            // "moduleSettings" key in the .nuproj, e.g. "audioOggQ"
	std::string label;          // Project Settings row label
	std::string tip;            // tooltip ("" = none)
	int         type = 0;       // 0 bool, 1 int, 2 float, 3 string
	double      defVal = 0.0;   // bool/int/float default
	std::string defStr;         // string default
	double      minV = 0.0, maxV = 0.0;   // int/float range (min == max = unbounded)
};
extern "C" { NUKE_ABI_STAMP int nuke_module_abi = NUKE_MODULE_ABI; }
// Build flavor of THIS binary. Same-source Debug and Release builds share the engine ABI
// stamp, but mixing them corrupts memory (CRT/iterator-debug/layout differences) — module
// discovery reads this out of the FILE and refuses a mismatch before any code runs.
#ifdef _DEBUG
extern "C" { NUKE_ABI_STAMP int nuke_build_debug = 1; }
#else
extern "C" { NUKE_ABI_STAMP int nuke_build_debug = 0; }
#endif

// ---- Engine BINARY-COMPATIBILITY generation -------------------------------------------------
// Tracks the whole engine ABI a module was compiled against (exported class layouts, signatures);
// the loader REFUSES a module whose stamp differs. Bump on any break. Unstamped = 1.
//   1 — pre-stamp builds
//   2 — Atom gained `enabled`; NukeWindow.vsync moved to the tail
//   3 — Mesh v4 layout (indexArray/streams/sections/LODs); MeshRenderer gained matGuids/mats
//   4 — Mesh gained skelGuid (v5); SkinnedMeshRenderer/Skeleton/SocketAttachment added
//   5 — Mesh gained morph targets (v6); iRender gained gpuSkin/setSkinPalette
//   6 — Anim runtime v2: AnimClip v3 (notifies/curves/prop tracks), Animator controller mode
//       (smGuid/rootMotion/graph), Camera shake, ResDB AnimSM/BlendSpace registries
//   7 — Animator drives every subtree SkinnedMeshRenderer (smrs vector; modular characters)
//   8 — AnimClip gained skelGuid (v4; chain retargeting across skeletons)
//   9 — iPhysics gained SwingTwist ragdoll joints (vtable append); Ragdoll/.nurag added
//  10 — AnimSM::State gained node coords (nx/ny); Animator gained the editor preview seam
//       (previewSm/previewBlend/muteNotifies)
//  11 — Mesh gained import-time materials (defaultMats, .numesh v7); reflection gained the
//       script-class registry (Reflect_RegisterScriptClass / Reflect_ScriptClasses)
//  12 — iScript gained ModuleDeps/PlatformOf (mod dependency + platform queries). Service
//       vtables have NO per-call guard, so growing one is a layout break like any other.
//  13 — WindowDesc/NukeWindow gained clickThrough + hideFromCapture (overlay window flags)
//  14 — reflection Field gained `net` ([[nuke::prop(net)]] replicated-field tag for NukeNet)
//  15 — Atom gained `folder`; AppInstance gained selectedExtra + editor snap settings
//  16 — AppInstance gained the viewport TOOL FEED block (cursor ray/stroke for module tools)
//  17 — Material gained the LiveMaterial sections (LiveState/twins/responses)
//  18 — Component gained `transient` (derived, never serialized); Material BRDF prop block
//  19 — Atom gained `alwaysLoaded` (world streaming pin)
//  20 — AppInstance gained the game pointer-consume append (player input vs editor UI)
//  21 — Component vtable gained StreamGlobal() (footprint components never stream out)
//  22 — iPhysics gained cookMeshShape/freeCookedBlob/createBodyFromCooked/activateBodies
//  23 — Mesh gained `pooled`; iRender gained renderObjectRange + getFrustum (cluster cull)
//  24 — Shader gained hs/ds sources; iRender gained createShaderPipelineTess
//  25 — iRender gained the Hi-Z occlusion seams (setOcclusionId/endOpaque/setOcclusionCulling/
//       getOcclusionStats); World::Settings gained occlusionCull; AppInstance gained freezeCulling
//  26 — AppInstance gained the streaming-boot start-zone block (activationStartZone* +
//       WorldStartZoneReady); StatusBar::Entry gained priority/seq/expiresAt
//  27 — Fast loading 4: Texture gained the pak-resident source (pakSource shared_ptr) and
//       Config the io.* block (directStorage/stagingMB/gpuDecompression); Package::Entry
//       carries the block table (layout + blocks) — every module that includes them relinks.
//  28 — NukeWindow gained fpsLimit (its growth shifts every Config member after
//       `window`) and Config gained jobCoreBudget — old binaries would read garbage offsets
//       (a stale dist player hung allocating 20 GB before the first frame).
//  29 — custom cursors: iRender gained setCursorImage (appended vtable slot; the renderer
//       must implement the same layout the engine calls). Decal target filter appended
//       drawDecalMesh in the same batch.
//  30 — Texture gained the dynamic block (dynamic + dynamicVersion — sizeof grew),
//       iAudio gained the PCM stream API (openStream/pushStream/streamQueued/closeStream).
//  31 — iRender gained setDebugView (appended vtable slot; mesh-cost view).
//  32 — Material gained liveInsideMat (fracture cut faces; sizeof grew — a stale-module
//       player crashed silently at boot), FireState gained ignitePos + Fire::IgnitePoint.
//  33 — SkinnedMeshRenderer gained morphMapGuid (ARKit-52 morph aliases; sizeof grew,
//       members after it shifted), Mesh::ImportAIMeshes gained the srcLodOf parameter.
//  34 — Material gained the toon block (toonBand/toonSoft/toonShade; members after the
//       BRDF pack shifted) for cel shading / VRM MToon (VR1).
//  35 — iPhysics gained createScene/destroyScene (appended vtable slots; isolated physics
//       scenes — the prefab editor's simulate runs its subtree in a private sandbox).
//  36 — SpringBones gained lastSolveFrame/lastSolveByAnim + windOn (sizeof grew) and a real
//       Update: spring chains self-drive every frame without an Animator (they are physics)
//       and react to the global wind/WindZones (opt-out per chain).
//  37 — Animator gained the applied* live-prop tracking block (sizeof grew): reflected
//       field writes (clip/loop/speed/bone map/controller) now apply while running.
//  38 — SkinnedMeshRenderer gained the per-mesh inverse-bind override cache (sizeof grew):
//       a mesh baked in its own pose (heterogeneous outfit-variant exports) embeds its own
//       binds and the skinning palette prefers them over the skeleton's canonical set.
//  39 — Animator gained fitLimbs (sizeof grew): penetrating hands become one-frame chain-IK
//       goals against the ragdoll capsules (proportion-mismatched clips).
//  40 — C3 cloth: iPhysics gained the soft-body block (createSoftBody/destroySoftBody/
//       set-/getSoftBodyVertices/addSoftBodyVelocity — appended vtable slots) and
//       SkinnedMeshRenderer gained externalMesh (sizeof grew): a Cloth component owns the
//       renderer's output mesh while the pose pipeline keeps feeding pins and sockets.
//  41 — NukeBodyDesc gained softOnly (cloth body proxies: kinematic capsules that collide
//       ONLY with soft bodies) and Cloth gained the proxy/interpolation block (sizeof grew):
//       ragdoll capsules ride the pose inside the solver, and the render sheet interpolates
//       between fixed steps.
//  42 — NukeSoftBodyDesc gained localSpace (anchor-space cloth: fitted clothes simulate
//       around the skeleton's anchor so clip drift/loop teleports never reach the solver;
//       own collision layer — proxies only). Cloth's anchor block grew its sizeof.
//  43 — Material gained the shading polish block (subsurface / irisDepth / hashedAlpha,
//       sizeof grew); iRender gained claimScreenOverlay (appended).
//  44 — Component::LateUpdate appended to the vtable (second traversal of the world tick);
//       iRender gained setAmbientOcclusion (appended); NukeCameraDesc gained cameraId (sizeof
//       grew): TAA / AO history and occlusion views are keyed per camera, not per target.
//       Same cycle, appended: iRender setGIVolumes/updateGIVolumes/giCapture* (DDGI), setScreenGI,
//       setVolumetrics (froxel fog), setFogVolumes / setSpriteVolumeLight / drawSpriteRunSixWay
//       (VL2); NukeGIVolumeDesc / NukeVolumetricsDesc / NukeFogVolumeDesc are new structs;
//       NukeVolumetricsDesc gained shaftDensity, then sunShaftIntensity/sunShaftLength;
//       NukeFogVolumeDesc gained shaftDensity, then the fluid block (sizeof grew); NukeFogDisplacerDesc
//       is new (bodies moving through fluid fog); iRender gained setFogDisplacers (appended).
//       VL3: NukeCloudsDesc is new, iRender gained setClouds + cloudsState (appended); Environment
//       gained the cloud block and ReflectionProbe cloudsSeen (sizeof grew). NukeSky gained
//       sunSize/sunGlow, Environment sunSize/sunGlow (sizeof grew). Physical atmosphere: NukeSky
//       gained the medium block (planetRadius .. aerialStrength), Environment mode Physical + the
//       atmosphere block (sizeof grew). NukeSky gained eclipse, Environment moonLight/eclipse (sizeof grew).
//       iRender gained addRTInstanceTinted (appended): per-instance colour for RT instances.
//       Mesh gained rtSprite (appended, sizeof grew): ray-facing procedural sprites in the TLAS.
//       iRender gained setSpriteMask (appended); Material gained mask (appended, sizeof grew).
//       iRender gained setLensRain (appended): the lens film moved into the renderer (water injects, weather rains).
//       Material gained liveDrawNoSky (appended, sizeof grew); Surface conditions gained a from-sky flag
//       (g_OvP flag 32): the renderer gates such states by its sky-occlusion capture (roofs shelter).
//       Material gained liveStateDisp (appended); iRender gained setGroundTrails (appended): W5 accumulation
//       depth (g_OvD0/1 in the std MatCB block) + the renderer's trail carve map.
//       NukeWaterSurface gained tint (appended): the water's Tint Strength; the renderer's sky map (skymap.hlsli)
//       is the sky for reflections / IBL (FrameCB g_Misc.z = live; native Frame gained skyMapSRV, appended).
//       NukeWaterSurface gained underFog + underWobble (appended): per-effect underwater switches.
//       R3/E2: new component PostFXVolume (API/Model/PostFXVolume.h; no layout change elsewhere); the renderer
//       gained the dof / motionblur / exposure built-in post stages (chain names, no interface change).
//       (44 should have ended with the materials / LateUpdate work; DDGI, volumetrics, atmosphere, weather, water
//       and R3/E2 rode on it by my mistake - one number per FEATURE from here on.)
//   45: 4.2 upscalers (2026-09-24). iRender gained beginGBufferCoverage / endGBufferCoverage (appended):
//       the transparent / additive draws into the prepass coverage = the reactive mask of the temporal
//       upscalers. The "upscale" built-in post stage (DLSS via NGX, FSR 3.1/4 via the FidelityFX API, XeSS,
//       FSR 1); render scale = internal vs output size in the camera pass; FrameCB gained g_MipBias (appended).
//       Same feature, same number: frame generation (DLSS-G / FSR FG / XeSS-FG, D3D12 + Vulkan) and the typed
//       API - Config gained `upscale` (NukeUpscale, appended), iRender gained getUpscaleStatus (appended),
//       config.h gained the UpscaleMode / UpscaleQuality / FrameGeneration enums (Game.SetUpscaleMode & co).
//   46: the file index (2026-09-25). nuke::FileIndex (API/Model/FileIndex.h): every host folder scanned once and
//       kept current by the OS (ReadDirectoryChangesW / inotify / FSEvents), immutable snapshots for the readers,
//       changes on the main thread + the "fs.changed" event. ResDB gained WatchContent / OnFileChanged /
//       HotReloadPath and its subscription state (appended): the boot scan reads the index, later edits reach
//       the DB live. The editor's per-frame disk walks (browser, settings, pickers) and the mtime polls
//       (hot reload, C#) read the index instead.
//   47: honest wide FOV (2026-09-25). Camera gained `panini` (cylindrical Panini projection strength, appended);
//       NukeCameraDesc gained `panini` (appended). The renderer over-scans the rectilinear pass and remaps the
//       final LDR image (panini.ps) before the HUD; Camera::ScreenRayDir follows the same mapping.
#define NUKE_ENGINE_ABI 47
extern "C" { NUKE_ABI_STAMP int nuke_engine_abi = NUKE_ENGINE_ABI; }

namespace nuke {

// When a plugin must be brought up: PHASE_BOOT during engine bootstrap (before the window/UI
// exist), PHASE_RUNTIME after the host is up (toggleable live).
enum PluginPhase { PHASE_BOOT = 0, PHASE_RUNTIME = 1 };

class BOOST_SYMBOL_EXPORT NUKEModule {
public:
	//Title of the plugin
	char title[256];
	
	//Description of the plugin
	char description[4096];
	
	//Author name
	char author[256];

	//Author or plugin site
	char site[1024];

	//Plugin version
	char version[30];

	//Path to the plugin, filled by runtime
	std::string modulePath;

	//Plugin DLL file name (e.g. "NukeScript.dll"), filled by runtime. Stable id for the per-project load list.
	std::string moduleFile;

	//Main process instance. Used by plugin from code.
	AppInstance* instance;

	//When Shutdown() called turn this to true, please. Otherwise plugin will work incorrectly.
	bool stopped;

	//True while the plugin is activated (OnLoad + Run done).
	bool loaded = false;

	//Synchronous activation hook, called by the loader BEFORE Run(). Register component types
	//here, NOT in a static initializer, so a disabled plugin leaves its types unregistered.
	virtual void OnLoad() {}

	//Function for run plugin. Runs on a BACKGROUND thread, not the game/UI thread: main-thread
	//state (PushWindow/PopWindow, the world, ResDB) must be deferred through Jobs::RunOnMain.
	virtual void Run(AppInstance* instance) = 0;

	//Returns true if mod has settings
	virtual bool HasSettings() = 0;

	//Opens menu if plugin has settings
	virtual void Settings() = 0;

	//Function that calls before plugin unloading. E.g. when app closes.
	virtual void Shutdown() = 0;

	// ---- Service metadata ----------------------------------------------------------------
	// ABI: new virtuals live at the END of the class so the vtable prefix stays stable.

	//Search/filter labels shown in the plugin window (e.g. {"lua", "scripting"}).
	std::vector<std::string> tags;

	//Engine service this plugin provides ("render"/"physics"/"audio"/"scripting"/...), or "" for
	//a utility plugin. At most ONE provider per exclusive service is active at a time.
	virtual const char* provides() { return ""; }

	//When the plugin must come up. PHASE_BOOT providers cannot be hot-swapped — switching
	//persists the choice and applies after restart.
	virtual int phase() { return PHASE_RUNTIME; }

	//For service providers: the interface instance to register under provides(). Registered by
	//the loader AFTER OnLoad() and revoked BEFORE Shutdown(). Utility plugins keep nullptr.
	virtual void* queryService() { return nullptr; }

	// Packaging hook: when the dependency walk reaches a file, every loaded module is asked.
	// Return TRUE if this module OWNS the file type (the file then ships), and append every
	// content it uses — content-relative paths or ResDB asset GUIDs — to `outUses` (each is
	// resolved and walked recursively). A file no engine loader and no loaded module claims
	// never ships. PURE: may be called from a worker thread, must not touch live module state.
	// ABI: appended at the END of the vtable.
	virtual bool cookContent(const char* contentRel, const char* bytes, uint64_t size,
	                         std::vector<std::string>& outUses) { return false; }

	// Whether this module's service is SHARED — several providers may be live at once (e.g.
	// scripting). Exclusive services keep false and the loader displaces the previous provider.
	// ABI: appended at the END of the vtable.
	virtual bool sharedService() { return false; }

	// Extra files to ship with a packaged game beyond this module's own DLL:
	//   * `pakFiles`  — project-relative paths forced into the game pak;
	//   * `distFiles` — (source -> dist-relative destination) copies into the dist tree. A
	//                   relative source resolves against the runtime dir being shipped, an
	//                   absolute one against itself; a directory source copies recursively.
	// ABI: appended at the END of the vtable.
	virtual void shipExtras(const char* projectDir,
	                        std::vector<std::string>& pakFiles,
	                        std::vector<std::pair<std::string, std::string>>& distFiles) {}

	// Editor-only companion module supplying asset editors/panels for a runtime module's file
	// types. The editor offers it by default, and never ships it with a game.
	// ABI level 2: callers must guard with ModuleAbi(m) >= 2.
	virtual bool editorTool() { return false; }

	// The RUNTIME module this one is a companion to, by file name ("NukeTilemap.dll"); "" = none.
	// A companion has nothing to edit while its runtime module is off, so the editor keeps it
	// off too. ABI level 3: callers MUST guard with ModuleAbi(m) >= 3 — a module built against
	// an older header has no such slot, and the call would land on whatever follows the vtable.
	virtual const char* companionOf() { return ""; }

	// Packaging hook: offer to REWRITE a shipping file (transcode/optimize). `srcPath` = the
	// on-disk source, `projectJson` = the project manifest (module settings live under its
	// "moduleSettings" object). Fill `out` with the replacement bytes — and `outRel` with the
	// new pak name when the format changes (e.g. beep.wav -> beep.ogg; "" = keep the name) —
	// and return true; false = ships unchanged. Worker thread, PURE like cookContent.
	// ABI level 5: callers MUST guard with ModuleAbi(m) >= 5.
	virtual bool cookTransform(const char* contentRel, const char* srcPath,
	                           const char* projectJson, std::string& outRel, std::string& out) { return false; }

	// Project-level settings this module contributes while it is in the project: data-only
	// descriptors the EDITOR renders in Project Settings; values persist under the .nuproj
	// "moduleSettings" object (modules read them back from the manifest, e.g. in cookTransform).
	// ABI level 5: callers MUST guard with ModuleAbi(m) >= 5.
	virtual void projectSettings(std::vector<NukeModuleSetting>& out) {}
};

}  // namespace nuke

#endif
