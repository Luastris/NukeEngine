#ifndef IRENDER_H
#define IRENDER_H
#include <boost/function.hpp>
#include <cstdint>
#include <vector>
#include <API/Model/Transform.h>
#include <API/Model/Mesh.h>
#include "UIDrawData.h"

namespace nuke {

class Material;
class Texture;

namespace b = boost;

// Backend-neutral POD light description for the world (PBR) pass; pushed via iRender::setLights.
struct NukeLight
{
    int   type = 0;                 // 0 = directional, 1 = point, 2 = spot
    float pos[3]   = {0, 0, 0};     // world position (point/spot)
    float dir[3]   = {0, -1, 0};    // world direction (directional/spot)
    float color[3] = {1, 1, 1};     // linear RGB
    float intensity = 1.0f;
    float range      = 10.0f;       // point/spot falloff distance
    float spotInner  = 0.9f;        // cos(inner cone half-angle)
    float spotOuter  = 0.8f;        // cos(outer cone half-angle)
    int   castShadows = 0;          // this light casts shadows (engine: Light::castShadows)
};

// Backend-neutral environment/sky description, filled from the World's Environment component.
struct NukeSky
{
    int   mode = 0;                 // 0 = none (clear color only), 1 = procedural gradient
    float top[3]     = {0.30f, 0.50f, 0.90f};
    float horizon[3] = {0.70f, 0.80f, 0.95f};
    float ground[3]  = {0.20f, 0.20f, 0.22f};
    float skyIntensity = 1.0f;
    float ambient[3]   = {0.50f, 0.55f, 0.60f};
    float ambientIntensity = 0.35f;
    float sunDir[3]   = {0.0f, -1.0f, 0.0f};   // direction the sun light travels (first directional light)
    float sunColor[3] = {1.0f, 1.0f, 1.0f};
    float sunIntensity = 0.0f;                 // 0 = no sun disk in the sky
    float stars = 0.0f;                        // star intensity (0 = none)
    Texture* starsTex = nullptr;               // optional equirect star panorama (else procedural)
    Texture* moonTex = nullptr;                // optional moon disk texture (null = no moon)
    float moonDir[3] = {0.0f, 1.0f, 0.0f};     // direction toward the moon
    float moonSize = 0.05f;                     // moon angular radius (radians)
    float moonAmount = 0.0f;                     // moon visibility (0 = hidden)
    float moonPhase = 0.5f;                      // 0/1 = new, 0.5 = full (procedural terminator)
    float exposure   = 1.0f;                      // SDR tonemap exposure multiplier
    float whitePoint = 1.0f;                      // SDR tonemap white point: linear value mapped to pure white
};

// One post-process effect in a camera's chain: a post shader pipeline + its packed
// parameter bytes (laid out by the shader's PostParams cbuffer, like a material's MatCB).
struct NukePostStage
{
    uint64_t     pipeline = 0;     // handle from iRender::createPostPipeline
    const float* params   = nullptr;
    int          paramFloats = 0;  // number of floats in `params` (PostParams cbuffer, /4 = float4 count)
};

// Backend-neutral camera description for one render pass; the renderer builds the
// view/projection matrices itself from these POD fields.
struct NukeCameraDesc
{
    uint64_t target = 0;                       // render-target id (0 = backbuffer)
    int      vpW = 0, vpH = 0;                  // viewport size in pixels
    float    clear[4] = {0.20f, 0.30f, 0.45f, 1.0f};
    float    camPos[3] = {0.0f, 0.0f, 0.0f};    // camera world position
    float    camFwd[3] = {0.0f, 0.0f, 1.0f};    // camera look direction
    float    camUp[3]  = {0.0f, 1.0f, 0.0f};    // camera up vector
    float    fov   = 1.0472f;                   // vertical FOV (radians, ~60deg)
    float    nearZ = 0.1f;
    float    farZ  = 1000.0f;
    float    ortho     = 0.0f;                  // 0 = perspective (fov), 1 = orthographic (orthoSize); blends in between
    float    orthoSize = 5.0f;                  // ortho view half-height in world units
    int      editorCamera = 0;                  // 1 = editor viewport camera, 0 = game/world camera (ABI: appended)
    uint64_t cameraId = 0;                      // stable id of the camera (its atom): keys the per-camera temporal state
                                                // (TAA / AO history, occlusion views) — two cameras on one target no longer
                                                // share one history. 0 = anonymous (falls back to the target). (abi 44, appended)
};

// Froxel volumetric lighting / fog (World::Settings): a global height-fog medium lit by the
// frame's lights and the GI probes, in a camera-aligned froxel grid. quality 0 = off.
struct NukeVolumetricsDesc
{
    int   quality = 0;               // 0 off, 1 Low, 2 Medium, 3 High (froxel size / slice count)
    float density = 0.0f;            // global fog extinction at the base height, 1/m (0 = none)
    float heightBase = 0.0f;         // world height of full density
    float heightFalloff = 0.1f;      // 1/m: density *= exp(-(y - base) * falloff) above the base
    float albedo[3] = {0.9f, 0.9f, 0.9f};
    float anisotropy = 0.3f;         // Henyey-Greenstein g: 0 isotropic, > 0 forward (glow around the sun)
    float maxDistance = 200.0f;      // far edge of the grid, world units
    float lightIntensity = 1.0f;     // scattering from the lights (god rays)
    float ambientIntensity = 1.0f;   // scattering from the probes / sky
    float shaftDensity = 0.003f;     // light scattering: lit-air scattering (same height profile) with NO
                                     // extinction and no ambient - the lights' rays in clear air, 1/m
    float sunShaftIntensity = 0.0f;  // screen-space crepuscular rays around the sun (0 = off); no grid needed
    float sunShaftLength = 0.6f;     // radial blur reach, fraction of the screen
};

// One local fog volume as the renderer sees it (World fills it from a FogVolume component):
// its medium is added to the froxel grid inside the shape.
struct NukeFogVolumeDesc
{
    float pos[3] = {0, 0, 0};
    int   shape = 0;                     // 0 box, 1 sphere, 2 ellipsoid
    float halfExt[3] = {5, 5, 5};        // local half extents / radii (world-scaled)
    float density = 0.1f;                // extinction added inside, 1/m
    float rot[4] = {0, 0, 0, 1};         // local -> world rotation quaternion (xyzw)
    float albedo[3] = {0.9f, 0.9f, 0.9f};
    float falloff = 0.5f;                // 0 hard edge .. 1 fades from the centre
    float emission[3] = {0, 0, 0};       // radiance per metre the medium emits
    float noise = 0.0f;                  // erosion amount 0..1
    float noiseScale = 4.0f;             // world units per noise feature
    float windAdvect = 1.0f;             // noise drift with the global wind (1 = wind speed)
    float shaftDensity = 0.0f;           // local light scattering: lit air inside the shape, no extinction, 1/m
    float heightFalloff = 0.0f;          // 1/m: the medium thins exponentially above the shape's bottom (0 = uniform); lets fields lift it
    int   fluidMode = 0;                 // 0 = grid (the density advected on the grid), 1 = clumps (parcels of medium carried by the flow)
    float clumpSize = 0.0f;              // clumps: a parcel's radius, m (0 = from the shape's size)
    // Fluid volume (abi 44, appended): the medium is a simulated 3D field inside the shape -
    // advected by the wind, pushed aside and swirled by the fog displacers, curling by itself,
    // refilling toward the shape at `fluidRefill` per second. `id` keys the renderer's sim state.
    unsigned long long id = 0;
    int   fluid = 0;
    int   fluidRes = 48;                 // cells along the longest axis
    float fluidTurbulence = 0.3f;        // curl-noise stirring, m/s
    float fluidRefill = 0.5f;            // 1/s toward the resting field
    float fluidDissipation = 0.1f;       // 1/s density loss
};

// A body moving through the fog: a solid the fluid volumes' medium parts around and follows
// (the World collects CharacterControllers and Rigidbodies). Force Fields act on the fluid
// through the bend volumes (setBendVolumes).
struct NukeFogDisplacerDesc
{
    float pos[3] = {0, 0, 0};
    float radius = 1.0f;
    float vel[3] = {0, 0, 0};            // world velocity, m/s
    float strength = 1.0f;
};

// The volumetric cloud layer (abi 44, appended): a shell around the planet above sea level (y 0),
// filled from the World's Environment component (Environment owns the whole sky).
struct NukeCloudsDesc
{
    int   enabled = 0;
    float coverage = 0.5f;            // 0..1, how much of the sky the weather covers
    float type = 0.5f;                // 0 = stratus (flat, low), 1 = cumulus (tall towers)
    float density = 1.0f;             // extinction scale
    float bottom = 1500.0f;           // layer bottom, m above sea level
    float thickness = 3500.0f;        // layer thickness, m
    float shapeScale = 8000.0f;       // base noise period, m
    float detailScale = 900.0f;       // erosion noise period, m
    float erosion = 0.35f;            // 0..1, how deep the detail eats the edges
    float weatherScale = 40000.0f;    // weather map period, m
    float windInfluence = 1.0f;       // global wind (setWind) times this drifts the clouds
    float driftSpeed = 5.0f;          // own drift, m/s
    float driftDirection = 0.0f;      // own drift heading, degrees (0 = +X, 90 = +Z)
    float sunIntensity = 1.0f;        // sun light scale
    float ambientIntensity = 1.0f;    // sky light scale
    float forwardScatter = 0.8f;      // HG forward lobe g
    float backScatter = -0.3f;        // HG back lobe g
    float multiScatter = 0.5f;        // multiple-scattering contribution
    float multiScatterFalloff = 0.5f; // per-octave attenuation
    float silverLining = 0.5f;        // forward-lobe boost at the edges toward the sun
    int   shadows = 1;                // the layer shadows the ground (cloud shadow map)
    float shadowStrength = 0.8f;
    float shadowArea = 4000.0f;       // shadow map square around the camera, m
    int   quality = 1;                // 0 = low (quarter res, 40 steps), 1 = medium (half, 64), 2 = high (half, 96)
    float maxDistance = 60000.0f;     // march range, m
};

// One dynamic-GI probe volume as the renderer sees it (World fills it from a GIVolume).
struct NukeGIVolumeDesc
{
    uint64_t id = 0;                 // stable identity (the atom id): probe history survives edits
    float    origin[3] = {0, 0, 0};  // probe (0,0,0) world position
    float    spacing[3] = {1, 1, 1}; // probe step per axis
    int      counts[3] = {2, 2, 2};  // probes per axis (>= 2)
    int      raysPerProbe = 128;
    float    hysteresis = 0.97f;
    float    normalBias = 0.25f, viewBias = 0.3f;   // probe-spacing units
    float    intensity = 1.0f;
    float    maxRayDistance = 100.0f;
    int      debugProbes = 0;        // draw the probes as small lit spheres in the camera pass
};

// Backend-neutral window description, filled by the app from its config and passed to
// iRender::init; the renderer translates it into its windowing backend at creation time.
struct WindowDesc
{
    int         w = 1280, h = 720;
    const char* title = "NukeEngine";
    bool  decorated   = true;   // false => borderless
    bool  resizable   = true;
    bool  floating    = false;  // always-on-top
    bool  maximized   = false;
    int   mode        = 0;      // nuke::WindowMode: 0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen
    bool  fullscreen  = false;  // legacy mirror of mode != 0
    bool  transparent = false;  // per-pixel alpha to the desktop (DirectComposition swap chain)
    float opacity     = 1.0f;   // whole-window opacity 0..1 (live-settable)
    int   backend     = 0;      // 0 = D3D11, 1 = D3D12 (D3D12 enables ray tracing)
    bool  gpuValidation = false; // D3D12 validation layer + DRED (heavy; off by default)
    bool  rayTracing  = true;   // false = force the raster path even on RT-capable GPUs
    // ABI: cross-DLL struct — new fields are APPENDED at the END, never inserted mid-struct.
    bool  clickThrough    = false;  // mouse input passes through to the windows beneath (live)
    bool  hideFromCapture = false;  // invisible to screen capture/recording; the user still sees it (live)
    int   textureStreamMB = 0;      // mip-streaming VRAM budget in MB (0 = off; live via setTextureStreaming)
};

// One record per instance in an instance buffer. Rows are HLSL-ready: row_i dot (localPos, 1)
// yields the world coordinate (columns of a row-vector world matrix, translation in row_i.w).
struct NukeInstanceData
{
    float row0[4];
    float row1[4];
    float row2[4];
    float color[4]  = { 1, 1, 1, 1 };   // multiplies the material base color
    float custom[4] = { 0, 0, 0, 0 };   // free per-instance data for shaders
};

class iRender
{
    friend class NukeOGL;
    friend class NukeBGFX;
private:
    static iRender* _instance;
public:
    // Name this interface is registered under in the plugin service registry (interface/Services.h).
    static constexpr const char* kServiceName = "render";

    static iRender* getSingleton(){
        return _instance;
    }
    Transform* transform = nullptr;
    int width = 0, height = 0;
    bool _crosshair = false;
    float fov = 90, Far = 1000, Near = 0.3f;
	bst::function<void()> _UIinit;
	bst::function<void(unsigned char c, int x, int y)> _UIkeyboard;
	bst::function<void(unsigned char c, int x, int y)> _UIkeyaboardUp;
	bst::function<void(int key, int x, int y)> _UIspecial;
	bst::function<void(int key, int x, int y)> _UIspecialUp;
	bst::function<void(int button, int state, int x, int y)> _UImouse;
	bst::function<void(int button, int dir, int x, int y)> _UImouseWheel;
	bst::function<void(int x, int y)> _UImove;
	bst::function<void(int x, int y)> _UIpmove;
	bst::function<void(int w, int h)> _UIreshape;
	bst::function<void(int key, int action, int mods)> _UIkey;   // raw key code/action/mods (GLFW numbering)
	bst::function<void(unsigned int codepoint)>        _UIchar;  // typed text character
    virtual int render() = 0;
    // Draw one mesh with the current camera. quat is (x,y,z,w); the renderer builds the world matrix.
    virtual void renderObject(Mesh* mesh, Material* mat,
                              const float pos[3], const float quat[4], const float scale[3]) {}
    virtual int init(const WindowDesc& desc) = 0;
    virtual void loop() = 0;
    virtual void deinit() = 0;
    virtual void update() = 0;
    virtual char* getEngine() = 0;
    virtual char* getVersion() = 0;
    virtual void setOnGUI(bst::function<void(void)> cb) = 0;
    virtual void setOnRender(bst::function<void(void)> cb) = 0;
    // Backend-agnostic close callback.
    virtual void setOnClose(bst::function<void()> cb) {}

    // --- Neutral UI seam ------------------------------------------------------
    // Upload an RGBA8 texture (e.g. a font atlas); returns an opaque handle.
    virtual uint64_t createTexture2D(const void* rgbaPixels, int width, int height) { return 0; }
    // Release a texture previously returned by createTexture2D.
    virtual void destroyTexture2D(uint64_t handle) {}
    // Render backend-neutral 2D draw lists for this frame.
    virtual void renderDrawLists(const NukeUIDrawData& data) {}

    // --- World viewport / per-camera 3D seam ---------------------------------
    // Off-screen surface a camera can draw into, keyed by a stable id (resize keeps it). 0 = backbuffer.
    virtual uint64_t createRenderTarget(int w, int h) { return 0; }
    virtual void     resizeRenderTarget(uint64_t id, int w, int h) {}
    virtual uint64_t getRenderTargetTexture(uint64_t id) { return 0; } // current color SRV handle
    // One camera pass: bind its target, set viewport, clear, set view/projection.
    virtual void     beginCamera(const NukeCameraDesc& cam) {}
    virtual void     endCamera() {}
    // Read back the last camera pass's view & projection matrices (row-major, as the renderer used them).
    virtual void     getViewProj(float* view16, float* proj16) {}

    virtual void keyboard(int key, int scancode, int action, int mods) = 0;
    virtual void mouseMove(double xpos, double ypos) = 0;
    virtual void mouseClick(int button, int action, int mods) = 0;
    // 0 Normal, 1 Hidden, 2 Locked (hidden + pinned to center, raw deltas flow), 3 Confined.
    // ABI: an original slot — keeps its mid-vtable position.
    virtual void setCursorMode(int mode) = 0;
    virtual void rawMouse(double xpos, double ypos) = 0;
    virtual void mouseEnterLeave(int entered) = 0;

    // ABI: new virtuals are appended HERE, at the END of the interface — never mid-vtable.

    // Supply a named shader's source BEFORE init(); the renderer does no file IO.
    // Built-in names it needs: "world.vs","world.ps","ui.vs","ui.ps".
    virtual void setShaderSource(const char* name, const char* source) {}

    // Build a world-type pipeline from a shader asset's VS+PS source; returns a handle (0 on failure).
    // `name` is the shader asset's guid — the renderer auto-builds an RT closest-hit from a
    // matching "<name>.surf.hlsl" surface file. Call after init().
    virtual uint64_t createShaderPipeline(const char* name, const char* vs, const char* ps) { return 0; }

    // Draw a selection outline around one mesh (same transform as renderObject), inside the camera pass.
    virtual void renderSelectionOutline(Mesh* mesh, const float pos[3], const float quat[4], const float scale[3]) {}

    // Outline over a WHOLE selection: begin the mask, add every mesh of the selected subtree,
    // then close it once — a composite model is one silhouette, not one outline per part.
    // Backends that don't implement these fall back to the single-mesh call above.
    virtual void selectionOutlineBegin() {}
    virtual void selectionOutlineAdd(Mesh* mesh, const float pos[3], const float quat[4], const float scale[3]) {}
    virtual void selectionOutlineEnd() {}

    // Set the OS window title.
    virtual void setWindowTitle(const char* title) {}

    // Is the OS window currently focused/foreground?
    virtual bool isWindowFocused() { return true; }

    // OS window maximized state.
    virtual bool isWindowMaximized() { return false; }
    virtual void setWindowMaximized(bool maximized) {}

    // Pull-style input. Cursor in window pixels; button 0/1/2 = left/right/middle.
    virtual void getCursorPos(double& x, double& y) { x = 0; y = 0; }
    virtual bool isMouseButtonDown(int button) { return false; }

    // Target the next renderDrawLists at a render target WITHOUT clearing it. 0 = backbuffer/reset.
    virtual void bindRenderTarget(uint64_t rtId) {}

    // Drop the cached GPU texture for this engine Texture so it re-uploads.
    virtual void invalidateTexture(Texture* t) {}

    // Set the scene lights for the next camera pass(es). count 0 = fall back to a default directional sun.
    virtual void setLights(const NukeLight* lights, int count) {}

    // --- Shadows (directional + spot 2D shadow maps) ------------------------------------------
    // Number of depth passes to run after setLights. Each pass (beginShadowPass(p) ->
    // renderShadowObject per caster -> endShadowPass) must run BEFORE the camera pass.
    virtual int  shadowPassCount() { return 0; }
    virtual void beginShadowPass(int pass) {}
    virtual void renderShadowObject(Mesh* mesh, const float pos[3], const float quat[4], const float scale[3], Material* mat) {}
    virtual void endShadowPass() {}

    // Set the world environment/sky for the next camera pass(es).
    virtual void setSky(const NukeSky& sky) {}

    // Files dropped onto the OS window. Called once per dropped path, on the main thread.
    virtual void setOnFileDrop(bst::function<void(const char* path)> cb) {}

    // Hardware AA sample count for the world passes (1 = off, else 2/4/8 clamped to device support).
    // Rebuilds the affected pipelines + targets; safe to call before init.
    virtual void setMSAA(int samples) {}
    virtual int  getMSAA() { return 1; }

    // HDR rendering: on = RGBA16F scene + tonemap in the post pass (enables bloom); off = LDR RGBA8
    // + inline tonemap. Rebuilds pipelines/targets; safe before init.
    virtual void setHDR(bool on) {}
    virtual bool getHDR() { return false; }

    // HDR DISPLAY output: an HDR10 (RGB10A2 + PQ) swap chain. Distinct from setHDR's internal HDR
    // rendering. Must be called BEFORE init. SDR fallback if the display isn't HDR.
    virtual void setHDROutput(bool on) {}
    virtual bool getHDROutput() { return false; }   // true only if an HDR10 swap chain is actually active

    // HDR10 display mapping: diffuse-white luminance + highlight peak, in nits.
    virtual void setHDRNits(float paperWhite, float peak) {}

    // Global shadow settings. resolution rebuilds the maps; distance = directional ortho extent;
    // depthBias/normalBias fight acne/peter-panning; softness scales the PCF kernel.
    virtual void setShadowSettings(int resolution, float distance, float depthBias, float normalBias, float softness) {}

    // Global RTX reflection quality: strength, ray length, recursion depth, roughness fade cutoff.
    virtual void setRTReflection(float intensity, float maxDist, int bounces, float roughCutoff) {}

    // Build a post-process pipeline from a fullscreen PS (samples `g_Source`, params in a `PostParams`
    // cbuffer); returns a handle (0 on failure). Call after init.
    virtual uint64_t createPostPipeline(const char* name, const char* ps) { return 0; }

    // Set the ordered post-process chain for the NEXT camera pass. Empty = no effects (just tonemap).
    virtual void setPostChain(const NukePostStage* stages, int count) {}

    // --- Reflection probe (scene-captured cubemap) ----------------------------------------------------
    // Create a cube color target (HDR, mipped) for a probe; returns a stable id (0 on failure).
    virtual uint64_t createReflectionCube(int resolution) { return 0; }
    // Begin/end ONE cube-face capture pass; the engine draws the scene between them via renderObject.
    // The probe is not sampled during capture; endCubeFace on face 5 builds the mips.
    virtual void beginCubeFace(uint64_t cube, int face, const float pos[3], float nearZ, float farZ) {}
    virtual void endCubeFace(uint64_t cube, int face) {}
    // Bind a probe cubemap for the upcoming camera pass(es). cube == 0 disables probe reflections.
    // boxHalf = parallax box half-extents centred on pos; {0,0,0} disables parallax correction.
    virtual void setReflectionProbe(uint64_t cube, const float pos[3], float intensity, float farZ, const float boxHalf[3]) {}

    // G-buffer prepass (normal/roughness/metalness/depth) for screen-space reflections.
    // Single-sample, run BEFORE beginCamera with the same camera.
    virtual void beginGBufferPass(const NukeCameraDesc& cam) {}
    virtual void renderGBufferObject(Mesh* mesh, Material* mat, const float pos[3], const float quat[4], const float scale[3],
                                     const float prevPos[3] = nullptr, const float prevQuat[4] = nullptr, const float prevScale[3] = nullptr) {}
    virtual void endGBufferPass() {}

    // Ray tracing (D3D12 + DXR-capable GPU); rtAvailable() gates all of it. Per frame, before the
    // camera passes: beginRTScene() -> addRTInstance() per mesh -> buildRTScene().
    virtual bool rtAvailable() { return false; }
    virtual void beginRTScene() {}
    // inReflections = visible to reflection rays (TLAS mask bit 0x01); castShadows = blocks shadow
    // rays (bit 0x02), so a caster can be shadow-only.
    virtual void addRTInstance(Mesh* mesh, Material* mat, const float pos[3], const float quat[4], const float scale[3], bool inReflections = true, bool castShadows = true) {}
    virtual void buildRTScene() {}

    // TAA: call per camera BEFORE beginGBufferPass/beginCamera. Needs the depth prepass too.
    virtual void setCameraTAA(bool enabled) {}

    // Ask the render loop to end after the current frame; loop() then returns.
    virtual void requestClose() {}

    // Debug/gizmo line: world-space segment + RGBA color, accumulated for THIS frame (thread-safe),
    // drawn as a depth-tested overlay in every camera pass and cleared at the next frame's start.
    virtual void drawDebugLine(const float a[3], const float b[3], const float color[4]) {}

    // --- UI multi-viewport: detachable NATIVE OS windows -----------------------------
    // Main platform window handle (GLFWwindow*). Null = no windowing layer; the UI stays single-window.
    virtual void* nativeWindow() { return nullptr; }
    // Render a UI draw list into a secondary OS window (nativeHandle = HWND on Windows). The renderer
    // lazily creates a swap chain per window, resizes it, draws and presents unsynced.
    virtual void uiViewportRender(void* nativeHandle, int w, int h, const NukeUIDrawData& data) {}
    // The secondary window is being destroyed — release its swap chain.
    virtual void uiViewportDestroy(void* nativeHandle) {}

    // Scene geometry submitted during the LAST completed frame (world/gbuffer/probe/shadow passes;
    // UI and post-process excluded). Backends that don't count report zeros.
    virtual void getFrameStats(int& drawCalls, int& triangles) { drawCalls = 0; triangles = 0; }

    // --- runtime-GUI input ------------------------------------------------------------
    // Polled once per frame; the queues DRAIN on fetch and are size-capped inside the backend.
    // Key codes/actions/mods use GLFW numbering.
    virtual int  fetchUIChars(unsigned int* out, int max) { return 0; }                  // typed codepoints
    virtual int  fetchUIKeys(int* keys, int* actions, int* mods, int max) { return 0; }  // key events
    virtual void getScrollDelta(double& x, double& y) { x = 0; y = 0; }                  // wheel since last call
    virtual const char* getClipboardText() { return ""; }        // valid until the next call
    virtual void setClipboardText(const char* text) {}

    // Drop every cached GPU resource built from this mesh (buffers, BLAS). MUST be called before a
    // Mesh is deleted: a cache entry keyed by a freed pointer serves wrong buffers once the address is reused.
    virtual void invalidateMesh(Mesh* m) {}

    // Runtime window control: applies what the platform can change live (size, display mode,
    // decoration, opacity). `transparent` is creation-time only and takes effect on the next launch.
    virtual void applyWindow(const WindowDesc& desc) {}

    // Vertical sync for the MAIN window's Present. Default on. Secondary UI-viewport windows
    // always present unsynced (the main window paces the frame).
    virtual void setVSync(bool on) {}
    virtual bool getVSync() { return true; }

    // Draw a textured, unlit, alpha-blended quad in the CURRENT camera pass, after opaque geometry.
    // Quad basis = `center` + half-extent vectors `right`/`up`; `uv` = {u0,v0,u1,v1}. Depth-tested,
    // no depth write. Call between beginCamera/endCamera.
    virtual void drawSprite(Texture* tex, const float center[3], const float right[3], const float up[3],
                            const float uv[4], const float tint[4]) {}

    // Draw a SCREEN-space sprite. `rect` = {centreX, centreY, width, height} in the canvas's reference
    // pixels (origin = screen centre, +y up); `refSize` = {refW, refH}, uniformly fitted to the target.
    // `afterPost` 0 = drawn with the scene before post, 1 = drawn on the final image.
    virtual void drawSpriteScreen(Texture* tex, const float rect[4], const float refSize[2],
                                  const float uv[4], const float tint[4], int afterPost) {}

    // Composite a screen-space decal box onto the scene colour after the opaque pass; `tex` is
    // projected along the box's local +Z. mode 0 = Albedo (blend), 1 = Light Projector (add).
    // appear = 0..1 envelope (spawn fade-in / pre-death fade-out); appearMode 0 = alpha ramp,
    // 1 = spread (reveal by the texture's density). Requires this camera's depth prepass.
    virtual void drawDecal(Texture* tex, const float pos[3], const float quat[4], const float scale[3],
                           const float tint[4], float intensity, float angleFade, int mode,
                           float appear = 1.0f, int appearMode = 0) {}

    // drawSpriteScreen with an explicit scale mode: 0 Fit (letterboxed), 1 Stretch, 2 Expand
    // (covers/crops), 3 FitWidth, 4 FitHeight. The old method behaves as mode 0.
    virtual void drawSpriteScreenEx(Texture* tex, const float rect[4], const float refSize[2],
                                    const float uv[4], const float tint[4], int afterPost, int scaleMode)
    { drawSpriteScreen(tex, rect, refSize, uv, tint, afterPost); }

    // Global scene fill mode: true = draw world geometry as wireframe. Affects mesh rendering only.
    virtual void setWireframe(bool on) {}
    virtual bool getWireframe() { return false; }

    // Debug line tested against the scene depth, so geometry occludes it (drawDebugLine is the
    // on-top overlay flavor). Call between beginCamera/endCamera.
    virtual void drawDebugLineDepth(const float a[3], const float b[3], const float color[4])
    { drawDebugLine(a, b, color); }

    // Bulk sprite append: verts = vertCount x 9 floats {x,y,z, u,v, r,g,b,a}, pre-baked quads
    // (two CCW triangles each). Consecutive same-texture runs collapse into one draw call.
    virtual void drawSpriteRun(Texture* tex, const float* verts, int vertCount) {}

    // Read a target's pixels back as tightly packed RGBA8, top-left origin. rtId 0 = the backbuffer
    // (last completed frame); else a createRenderTarget id, its LDR post output. SLOW (GPU flush +
    // staging copy + map) — never a per-frame path.
    virtual bool captureTarget(uint64_t rtId, int& w, int& h, std::vector<uint8_t>& rgba) { return false; }

    // drawSpriteRun plus a normal map: the run is Lambert-lit, tangent basis taken from the first
    // quad (a run shares one plane). `normalFlipY` true = OpenGL-authored (+Y up, flip green).
    virtual void drawSpriteRunLit(Texture* tex, Texture* normal, const float* verts, int vertCount,
                                  bool normalFlipY) { drawSpriteRun(tex, verts, vertCount); }

    // Current cursor mode. ABI: only this getter is an append; setCursorMode is an original slot.
    virtual int  getCursorMode() { return 0; }

    // --- GPU instancing: one mesh+material, N instances, ONE draw call. ---------------------
    // Instance buffers are persistent: create once, update on change, destroy when done. Draws
    // take a RANGE [first, first+count) so the engine can cull chunks and issue only visible ranges.
    virtual uint64_t createInstanceBuffer() { return 0; }
    virtual void     updateInstanceBuffer(uint64_t id, const NukeInstanceData* data, int count) {}
    virtual void     destroyInstanceBuffer(uint64_t id) {}
    virtual void renderObjectInstanced(Mesh* mesh, Material* mat, uint64_t instBuf, int first, int count) {}
    virtual void renderShadowInstanced(Mesh* mesh, uint64_t instBuf, int first, int count, Material* mat) {}
    virtual void renderGBufferInstanced(Mesh* mesh, Material* mat, uint64_t instBuf, int first, int count) {}

    // Current animated global wind, pushed once per frame. Lands in the world FrameCB as
    // g_Wind (dir.xyz, gusted strength) + g_Wind2 (turbAmount, 1/turbScale, time, gustFreq).
    virtual void setWind(const float dirStrength[4], const float params[4]) {}

    // Soft-particle fade distance for SUBSEQUENT sprite runs; needs this frame's depth prepass
    // (silently off otherwise). A change flushes the open batch. 0 = off.
    virtual void setSpriteSoftDepth(float dist) {}

    // World positions of things that part foliage blades this frame; float4 (x, y, z, radius)
    // each, up to 8. Bend is gated by instance custom.w. count 0 clears.
    virtual void setBendPushers(const float* xyzr, int count) {}

    // Analytic local volumes that bend foliage. 12 floats each: (pos.xyz, radius),
    // (dir.xyz, strength), (mode, falloff, seed, 0); mode 0=directional 1=radial 2=vortex
    // 3=turbulence. Up to 16; count 0 clears.
    virtual void setBendVolumes(const float* vols, int count) {}   // 20 floats a volume: (pos,r)(dir,strength)(mode,falloff,seed,pull)(inner,dentDepth,dentSharp,dentSize)(dentDensity,0,0,0)

    // --- Water ---------------------------------------------------------------------------
    // Global wave state, pushed once per frame. The renderer runs the Tessendorf FFT over it for
    // ocean surfaces and shares the same CPU-generated Gerstner wave set the engine samples for
    // buoyancy, so physics and pixels cannot drift apart.
    struct NukeWaterParams
    {
        float windDir[2]   = { 1, 0 };    // normalized XZ
        float windSpeed    = 6.0f;        // m/s (drives the spectrum energy)
        float choppiness   = 1.0f;        // horizontal displacement scale
        float patchSize    = 120.0f;      // FFT patch world size (m)
        float amplitude    = 1.0f;        // spectrum energy multiplier
        float foamJacobian = 0.85f;       // crest foam threshold (lower = more foam)
        float time         = 0.0f;        // seconds (unscaled game clock)
        int   seed         = 1337;        // spectrum seed (matches the CPU sampler)
        // Shared analytic wave set, 8 floats per wave: (dirX, dirZ, k, amplitude), (phase, speed, 0, 0).
        const float* waves = nullptr;
        int          waveCount = 0;
        // Scripted soliton crests, 8 floats each: (originX, originZ, dirX, dirZ, amplitude,
        // 1/halfWidth, horizontal push m/s, 0). The origin rides the crest, so consumers
        // evaluate amp * sech^2(dot(p - o, d) * invW).
        const float* solitons = nullptr;
        int          solitonCount = 0;
        float detail = 1.0f;    // short-wave energy on the swell: 1 = full tail, 0 = glassy (ABI: appended)
        int playing = 0;        // 1 while playing: the ripple window anchors to the GAME camera
        // Water-carving volumes, 3 float4 each (max 8): (center.xyz, mode 1 cut / 2 compartment),
        // (halfExtents.xyz, interior fill level Y), (quat xyzw). ABI: appended.
        const float* cutVolumes = nullptr;
        int cutCount = 0;
        // Active surf tubes (max 4), 12 floats each: (ox, oz, dx, dz), (H, L, Rworld, peelP),
        // (secLen, peelSign, collapse, restY). The shaders carve the barrel cavity out of the
        // underwater pass with these; the tube meshes arrive as type-5 surfaces. ABI: appended.
        const float* tubes = nullptr;
        int tubeCount = 0;
        // Scene-wide feature toggles, 1 = on. ABI: appended.
        int ripplesOn = 1;      // local ripple sim (rings/wakes/imprints/splashes)
        int wakeFoamOn = 1;     // wake-foam trail
        int tessOn = 1;         // tessellated near-camera grid
        int underwaterOn = 1;   // submerged-camera treatment (waterline split, fog, god rays, caustics)
        int wetnessOn    = 1;   // lens run-off film after surfacing; independent of the above
    };
    struct NukeWaterSurface
    {
        int    type = 0;                    // 0 ocean (FFT), 1 lake (Gerstner), 2 river, 3 waterfall sheet, 4 spread (SWE sheet), 5 surf tube (barrel)
        float  pos[3]  = { 0, 0, 0 };       // origin; water level = pos.y (sheet: top-center)
        float  quat[4] = { 0, 0, 0, 1 };    // orientation (sheets tilt; ocean/lake use yaw only)
        float  size[2] = { 100, 100 };      // XZ extent (sheet: width x drop height)
        float  absorb[3]  = { 0.45f, 0.09f, 0.06f };   // per-channel absorption (deep water color)
        float  scatter[3] = { 0.02f, 0.10f, 0.09f };   // in-scatter tint
        float  foamShoreDepth = 0.6f;       // shore foam within this depth difference (m)
        float  opacityDepth   = 4.0f;       // view depth where refraction fully absorbs (m)
        float  waveScale      = 1.0f;       // per-surface wave height multiplier
        // River: resampled spline (float4 per sample: x,y,z,halfWidth); flow runs along it.
        const float* spline = nullptr;
        int          splineCount = 0;
        float        flowSpeed = 1.5f;      // m/s along the spline / down the sheet
        // Bottom depth grid: bottomN x bottomN floats over the rect (row-major, -half..+half),
        // meters below the rest level. Drives shore shoaling + breakers.
        const float* bottomDepth = nullptr;
        int          bottomN = 0;
        int          bottomStamp = 0;       // bumps per refresh sweep (renderer re-upload key)
        float foamColor[3] = { 1, 1, 1 };   // crest/shore/swash/sheet foam tint
        int infinite = 0;                   // ocean only: surface follows the camera to the horizon (ABI: appended)
        // Shore tuning block, 12 x float4 for the shader's g_ShoreP — packed by WaterBody::OnRender
        // in a FIXED order and consumed verbatim by FillWaterCB. Zeroed amp (slot 0) = surf off.
        float shoreP[48] = {};
        // [0] caustic strength, [1] caustic sharpness, [2] god-ray strength, [3] droplet amount,
        // [4] droplet duration (s), rest spare. ABI: appended.
        float fx[8] = { 1.0f, 1.6f, 1.0f, 1.0f, 3.0f, 0, 0, 0 };
        int cutExempt = 0;                  // exempt from carving volumes (a compartment must not cut its own water)
    };
    virtual void setWaterParams(const NukeWaterParams& p) {}
    virtual void drawWaterSurface(const NukeWaterSurface& s) {}   // camera pass, after opaques
    // Local ripple impulse (steps, splashes, boats) into the GPU heightfield sim.
    virtual void addWaterRipple(const float pos[3], float radius, float strength) {}
    // Ortho top-down depth render over a body's rect, producing the depth-below-rest-level map
    // that shapes shore waves. Submit opaque meshes with renderShadowObject between begin/end;
    // fetchWaterBottom polls the async readback and fills out[n*n] (row-major, -half..+half,
    // meters below pos.y; misses = deep). One capture in flight; fetch returns true once per capture.
    // aboveY = how many meters ABOVE pos.y take part: 0 sees only the water column (nothing in
    // the air can pose as a shoal), positive keeps terrain that rises out of the water.
    virtual void beginWaterBottomPass(const float pos[3], const float quat[4], float sizeX, float sizeZ,
                                      float aboveY) {}
    virtual void endWaterBottomPass() {}
    virtual bool fetchWaterBottom(float* out, int n) { return false; }
    // Continuous contact-contour wave sources (the waterline cells of bodies crossing the surface),
    // 4 floats each: (x, z, radius m, signed strength = water column rate m/s). count 0 clears.
    // ABI: appended.
    virtual void setWaterImprints(const float* xzrs, int count) {}
    // Shallow-water spreading: every spread region runs its own sim, and a spill (one-shot volume
    // in m^3 over a radius) splats into whichever region contains it. fetchWaterSpread copies that
    // region's depth field downsampled into out[n*n] (row-major, meters of water) — an async
    // snapshot a few frames stale. `key` = the region's bottomDepth grid pointer. ABI: appended.
    virtual void addWaterSpill(const float pos[3], float radius, float volume) {}
    virtual bool fetchWaterSpread(const void* key, float* out, int n) { return false; }

    // Bounded FLIP volume: real 3D water in a box. The renderer owns the GPU sim (particles +
    // MAC grid) and the screen-space-fluid draw. Solids/emitters arrive in the volume's LOCAL frame.
    struct NukeWaterFlip
    {
        const void* key = nullptr;          // stable region identity (component address)
        float pos[3]  = { 0, 0, 0 };        // box center (world)
        float quat[4] = { 0, 0, 0, 1 };     // box orientation (tilt = slanted local gravity)
        float halfExtents[3] = { 2, 1, 2 };
        int   resolution = 48;              // grid cells along the LONGEST axis (16..96)
        int   budget = 65536;               // particle capacity (4096..262144)
        float fill = 0.0f;                  // seeded fill fraction of the box height
        int   reseedStamp = 0;              // bump -> re-seed the fill (Fill prop edited)
        float flipness = 0.95f;             // FLIP/PIC blend (1 = lively, 0 = syrup)
        float damping = 0.02f;              // extra velocity decay (1/s)
        float evaporation = 0.0f;           // particle kill rate (fraction/s)
        float particleR = 0.10f;            // render radius (m)
        float absorb[3]  = { 0.45f, 0.09f, 0.06f };
        float scatter[3] = { 0.04f, 0.10f, 0.09f };
        // Solid colliders overlapping the box, LOCAL frame, 16 floats each:
        // type (0 box / 1 sphere / 2 capsule), lpos.xyz, lquat.xyzw, size.xyz
        // (he / r,-,- / r,halfH,-), lvel.xyz, spare. Moving solids push water.
        const float* solids = nullptr;
        int solidCount = 0;
        // Emitters, LOCAL frame, 8 floats each: lpos.xyz, radius, lvel.xyz, rate (m^3/s).
        const float* emitters = nullptr;
        int emitterCount = 0;
    };
    virtual void drawWaterFlip(const NukeWaterFlip& v) {}
    // Async CPU mirror for physics/queries: out = 16x16x16 float4 (fluid fraction 0..1,
    // local velocity xyz) over the box, x-major rows, y slices outermost. n must be 16.
    virtual bool fetchWaterFlip(const void* key, float* out, int n) { return false; }

    // --- Mesh v2 (Anim/Mesh rework, stage 1): sectioned multi-material draws --------------
    // Draw a v4 mesh with per-SLOT materials: the renderer loops the active LOD's sections and
    // binds mats[section.slot] (out-of-range/null slots fall back to mats[0]). matCount >= 1.
    // blendPass filters sections by their material's blend mode so mixed meshes land in the
    // right pass: -1 = all, 0 = opaque sections only, 1 = transparent/additive sections only.
    // The single-material renderObject/renderShadowObject/renderGBufferObject stay valid for
    // sectioned meshes too — every section then draws with the one material.
    virtual void renderObjectMulti(Mesh* mesh, Material* const* mats, int matCount,
                                   const float pos[3], const float quat[4], const float scale[3],
                                   int blendPass = -1) {}
    virtual void renderShadowObjectMulti(Mesh* mesh, Material* const* mats, int matCount,
                                         const float pos[3], const float quat[4], const float scale[3]) {}
    virtual void renderGBufferObjectMulti(Mesh* mesh, Material* const* mats, int matCount,
                                          const float pos[3], const float quat[4], const float scale[3],
                                          const float prevPos[3], const float prevQuat[4], const float prevScale[3],
                                          int blendPass = 0) {}
    // RT: one TLAS entry PER SECTION of the active mesh (per-section BLAS), each with its
    // slot's material. Equivalent to addRTInstance for slot-less meshes.
    virtual void addRTInstanceMulti(Mesh* mesh, Material* const* mats, int matCount,
                                    const float pos[3], const float quat[4], const float scale[3],
                                    bool inReflections, bool castShadows) {}

    // --- GPU skinning (Anim/Mesh v2 stage 3) ----------------------------------------------
    // True when the renderer skins on the GPU (compute pre-pass): the engine then feeds
    // per-frame palettes instead of CPU-skinned vertices.
    virtual bool gpuSkin() { return false; }
    // Per-frame skin state for a skinned mesh INSTANCE: palette16 = boneCount column-major
    // 4x4 matrices (boneGlobal * invBind, model space). morphWeights follows source->morphs
    // order. The renderer dispatches the skin compute IMMEDIATELY (call before submitting
    // draws that consume `instance`), keeps the previous frame's positions for TAA motion
    // vectors, and refits the instance's BLAS over the animated positions.
    virtual void setSkinPalette(Mesh* instance, Mesh* source, const float* palette16, int boneCount,
                                const float* morphWeights = nullptr, int morphCount = 0) {}

    // Editor infinite ground grid: an analytic shader plane (AA lines, x10 adaptive LOD,
    // distance fade), drawn depth-tested under the gizmos. step <= 0 hides it; the editor
    // hands the snap step over once per frame.
    virtual void drawEditorGrid(float step) {}

    // LiveMaterial background refraction: called between the opaque and transparent
    // passes when a transparent material refracts — snapshot the scene for g_SceneRefr.
    virtual void beginTransparent() {}

    // RT reflections for a custom material shader: register its `Surface()` HLSL body (the
    // "<name>.surf.hlsl" contract) from CODE — modules embed shaders and have no file in the
    // shaders directory for the include machinery to find. Re-registering replaces; the RT
    // pipeline rebuilds with the shader's auto-generated closest-hit. ABI: appended.
    virtual void registerRTSurface(const char* shaderName, const char* surfHlsl) {}

    // Texture streaming: VRAM budget for the mip pool in BYTES (0 disables; live). Streamed
    // textures keep a low-mip tail resident and stream higher mips by camera distance.
    virtual void setTextureStreaming(long long budgetBytes) {}
    // Stats: bytes resident of streamed textures, bytes saved vs full residency, texture count.
    virtual void textureStreamInfo(long long& residentBytes, long long& savedBytes, int& streamedCount)
    { residentBytes = 0; savedBytes = 0; streamedCount = 0; }

    // Custom tessellation stages (abi 24): a world pipeline whose shader ships its OWN hull +
    // domain sources (terrain splat displacement — the shared world.hs/ds pair knows nothing
    // about vertex colors or per-layer heights). Falls back to the plain pipeline.
    virtual uint64_t createShaderPipelineTess(const char* name, const char* vs, const char* ps,
                                              const char* hs, const char* ds)
    { return createShaderPipeline(name, vs, ps); }

    // Terrain cluster culling (abi 23) — draw ONE index range of an indexed mesh with
    // the full material pipeline (same contract as renderObject otherwise).
    virtual void renderObjectRange(Mesh* mesh, Material* mat,
                                   const float pos[3], const float quat[4], const float scale[3],
                                   uint32_t firstIndex, uint32_t indexCount) {}
    // Current camera frustum: 6 world-space planes x (nx, ny, nz, d), normals pointing INTO
    // the volume (a point p is visible when dot(n, p) + d >= 0 for all six).
    virtual void getFrustum(float planes[24]) { for (int i = 0; i < 24; ++i) planes[i] = 0; }

    // Hi-Z occlusion culling (abi 25). The engine tags an opaque draw with a stable id and
    // its world AABB right before submitting it (one tag covers every section of that draw).
    // The renderer draws ids its visibility history calls visible, defers the rest, and at
    // endOpaque builds a depth pyramid from what was drawn, tests every tagged box on the GPU,
    // draws the deferred survivors (indirect, same frame) and feeds the verdicts back into the
    // history (readback, latency-hidden). Untagged draws are never culled.
    virtual void setOcclusionId(uint64_t id, const float mn[3], const float mx[3]) {}
    // End of the camera's opaque scope (before transparents): pyramid + test + deferred draws.
    virtual void endOpaque() {}
    // freeze = keep the last verdicts while the camera moves and draw the culled boxes (debug).
    virtual void setOcclusionCulling(bool enable, bool freeze) {}
    // Last completed camera: tagged draws, and how many the history held back.
    virtual void getOcclusionStats(int& tracked, int& culled) { tracked = 0; culled = 0; }

    // Custom cursor image (abi 29). mode: 0 = OS arrow, 1 = hardware cursor from the RGBA
    // pixels (`id` caches it — animation swaps are cheap), 2 = software (renderer draws the
    // image topmost at the cursor position). False = platform refused.
    virtual bool setCursorImage(uint64_t id, const unsigned char* rgba, int w, int h,
                                int hotX, int hotY, int mode) { (void)id; (void)rgba; (void)w; (void)h; (void)hotX; (void)hotY; (void)mode; return false; }

    // Fullscreen overlay (abi 30): `tex` letterboxed over the finished frame (under the
    // software cursor), sampled every frame; null clears. Lives in the renderer across world swaps.
    virtual void setScreenOverlay(Texture* tex) { (void)tex; }

    // Target-filtered decal (abi 29): re-render ONE mesh with the box projection so only that
    // surface catches it. pos/quat/scale = the decal box, tPos/tQuat/tScale = the mesh transform.
    virtual void drawDecalMesh(Texture* tex, const float pos[3], const float quat[4], const float scale[3],
                               const float tint[4], float intensity, float angleFade, int mode,
                               float appear, int appearMode, Mesh* target,
                               const float tPos[3], const float tQuat[4], const float tScale[3]) {}

    // Debug view over the camera passes (abi 31): 0 = off, 1 = mesh cost — every world mesh
    // draw renders flat-colored by its triangle load (tris x instances, log scale) with a dark
    // wireframe on top; materials are skipped.
    virtual void setDebugView(int mode) { (void)mode; }

    // Editor claim of the fullscreen-video overlay (abi 43): the EDITOR calls this every frame
    // to draw the overlay inside its game viewport itself. The first call permanently moves the
    // draw off the backbuffer for this renderer; the return is the overlay texture's UI handle
    // (a renderDrawLists texId; 0 = no video playing) with w/h receiving the video size for
    // letterboxing. Standalone players never call it, so the fullscreen draw stays theirs.
    virtual uint64_t claimScreenOverlay(int* w, int* h) { (void)w; (void)h; return 0; }

    // Screen-space ambient occlusion (abi 44): quality 0 = off, then the method tiers
    // 1 SSAO (hemisphere samples), 2 HBAO (horizons), 3 GTAO (analytic horizons), 4 VBAO
    // (visibility bitmask, thickness-aware), 5 RT-AO (DXR rays against the scene TLAS; runs
    // as GTAO without ray tracing). radius = world units occluders are searched
    // within, intensity = strength, power = contrast. Needs the G-buffer prepass; darkens
    // the ambient/IBL term only — direct light is untouched.
    virtual void setAmbientOcclusion(int quality, float radius, float intensity, float power) { (void)quality; (void)radius; (void)intensity; (void)power; }

    // --- Dynamic diffuse GI probe volumes (abi 44, appended) --------------------------------------
    // The World pushes every enabled GIVolume before the camera passes (count 0 = no dynamic GI).
    // Ray-tracing devices update the probes themselves in updateGIVolumes (probe rays against the
    // scene TLAS). Elsewhere the World captures probe cube faces through giCapture*, amortized:
    // giCaptureBudget probes per frame, six faces each, then giCaptureCommit folds them into the
    // atlases (the world shader marks back faces with alpha 0 during a capture, which classifies
    // probes sitting inside geometry). The world shader replaces the sky irradiance wherever a
    // volume covers the point.
    virtual void setGIVolumes(const NukeGIVolumeDesc* volumes, int count) { (void)volumes; (void)count; }
    virtual void updateGIVolumes() {}
    virtual int  giCaptureBudget() { return 0; }
    virtual bool giCaptureBegin(int slot, int face, float pos[3], float* nearZ, float* farZ) { (void)slot; (void)face; (void)pos; (void)nearZ; (void)farZ; return false; }
    virtual void giCaptureEnd(int slot, int face) { (void)slot; (void)face; }
    virtual void giCaptureCommit() {}
    // Screen-space GI (abi 44, appended): contact-scale diffuse bounce from last frame's lit
    // scene, layered on the probes. quality 0 = off, 1..3 = Low/Medium/High (rays x steps);
    // radius = world units the rays march; intensity = strength. Needs the G-buffer prepass.
    virtual void setScreenGI(int quality, float radius, float intensity) { (void)quality; (void)radius; (void)intensity; }
    // Froxel volumetrics (abi 44, appended): the medium in a per-camera froxel grid, in-scatter
    // from every FrameCB light (the sun through its shadow map, or inline shadow rays on
    // ray-tracing devices = god rays; point/spot with falloff, particle lights included) with
    // the Henyey-Greenstein phase and the DDGI/sky ambient; integrated front to back and
    // composited before the post chain (after the reflection effects). Needs the G-buffer prepass.
    virtual void setVolumetrics(const NukeVolumetricsDesc& desc) { (void)desc; }
    // Local fog volumes for this frame's froxel grid (abi 44, appended): the World pushes the
    // nearest 32 enabled FogVolume components each frame (count 0 = none).
    virtual void setFogVolumes(const NukeFogVolumeDesc* volumes, int count) { (void)volumes; (void)count; }
    // Froxel-grid lighting of the following sprite runs (abi 44, appended): 0 = off, 1 = the
    // grid's incident light (sun through its shadows, point/spot, probe ambient) multiplies the
    // sprite colour — smoke sits in the god rays. Sprite runs are also fogged by their own column
    // whenever the grid is active. Like setSpriteSoftDepth: set, draw, reset.
    virtual void setSpriteVolumeLight(float amount) { (void)amount; }
    // Six-way lit sprite run (abi 44, appended): two lightmaps hold the puff lit from six
    // directions in the billboard frame — A.rgb = from +right, -right, +up; B.rgb = from -up,
    // +front (toward the eye), -front; A.a = opacity. Lit by the frame's lights + ambient, then
    // treated like drawSpriteRun (soft depth, fog). Falls back to drawSpriteRun(lightA, ...).
    virtual void drawSpriteRunSixWay(Texture* lightA, Texture* lightB, const float* verts, int vertCount)
    { (void)lightB; drawSpriteRun(lightA, verts, vertCount); }
    // Short name of the active graphics backend for titles / overlays ("Vk", "Dx11", "Dx12"; "" = unknown). abi 44, appended.
    virtual const char* backendName() { return ""; }
    // This frame's fog displacers for the fluid volumes (abi 44, appended; count 0 = none).
    virtual void setFogDisplacers(const NukeFogDisplacerDesc* displacers, int count) { (void)displacers; (void)count; }
    // The volumetric cloud layer (abi 44, appended); enabled 0 = none.
    virtual void setClouds(const NukeCloudsDesc& clouds) { (void)clouds; }
    // 0 = no clouds, 1 = clouds requested but their pipeline / noise is still building, 2 = ready
    // (abi 44, appended). The World re-captures a static reflection probe when this changes.
    virtual int cloudsState() { return 0; }

    // ABI: new virtuals are appended at the END of the class, NEVER inserted mid-vtable —
    // plugins are separate DLLs built at different times, and an inserted slot shifts every later one.
};

}  // namespace nuke

#endif // IRENDER_H
