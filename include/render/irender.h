#ifndef IRENDER_H
#define IRENDER_H
#include <boost/function.hpp>
#include <cstdint>
#include <API/Model/Transform.h>
#include <API/Model/Mesh.h>
#include "UIDrawData.h"

namespace nuke {

class Material;   // forward (used by pointer below)
class Texture;

namespace b = boost;

// Backend-neutral light description for the world (PBR) pass. The engine gathers Light
// components each frame and pushes an array via iRender::setLights; the renderer packs
// them into its lighting constant buffer. POD, no engine/Diligent types across the seam.
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

// Backend-neutral environment/sky description. The engine fills it from the World's Environment
// component each frame; the renderer draws a procedural sky behind the scene + uses `ambient` for IBL.
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
    float whitePoint = 1.0f;                      // SDR tonemap white point: linear value mapped to pure white (Reinhard reaches 1.0)
};

// One post-process effect in a camera's chain: a custom fullscreen post shader pipeline + its packed
// parameter bytes (laid out by the shader's PostParams cbuffer, exactly like a material's MatCB). The
// engine resolves the shader GUID -> pipeline handle and packs the params; the renderer just binds + runs.
struct NukePostStage
{
    uint64_t     pipeline = 0;     // handle from iRender::createPostPipeline
    const float* params   = nullptr;
    int          paramFloats = 0;  // number of floats in `params` (PostParams cbuffer, /4 = float4 count)
};

// Backend-neutral camera description for one render pass. The renderer builds the
// view/projection matrices itself (from these POD fields) so no glm/Diligent math
// convention leaks across the seam.
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
    // Projection blend: 0 = perspective (use fov), 1 = orthographic (use orthoSize as the view
    // half-height in world units). Values in between blend the two projection matrices — the
    // engine animates it for a smooth perspective<->ortho transition.
    float    ortho     = 0.0f;
    float    orthoSize = 5.0f;
};

// Backend-neutral window description, filled by the app from its config and passed
// to iRender::init. The renderer translates these into its windowing backend (GLFW
// hints etc.) at creation time — so the game window can be borderless / sized /
// transparent without the app or config knowing about GLFW.
struct WindowDesc
{
    int         w = 1280, h = 720;
    const char* title = "NukeEngine";
    bool  decorated   = true;   // false => borderless
    bool  resizable   = true;
    bool  floating    = false;  // always-on-top
    bool  maximized   = false;
    // Window display mode (nuke::WindowMode as int; `fullscreen` is a legacy mirror of
    // mode != 0). 0 = windowed, 1 = borderless fullscreen (undecorated window covering the
    // monitor at the DESKTOP resolution, no mode switch — instant alt-tab), 2 = exclusive
    // fullscreen (the monitor switches to the window's resolution).
    int   mode        = 0;
    bool  fullscreen  = false;
    bool  transparent = false;  // per-pixel alpha to the desktop (DirectComposition swap chain)
    float opacity     = 1.0f;   // whole-window opacity 0..1 (cheap, always works — live-settable)
    int   backend     = 0;      // 0 = D3D11, 1 = D3D12 (chosen at launch; D3D12 enables ray tracing)
    bool  gpuValidation = false; // Debug: turn on the D3D12 validation layer + DRED (heavy; off by default)
};

class iRender
{
    friend class NukeOGL;
    friend class NukeBGFX;
private:
    static iRender* _instance;
public:
    // Service name this interface is registered under in the plugin service registry
    // (interface/Services.h): GetService<iRender>() resolves through it.
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
    // Draw one mesh with the current camera. Global position + rotation quaternion
    // (x,y,z,w) + scale passed as plain floats; the renderer builds the world matrix.
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
    // Backend-agnostic close callback. Plugins register here instead of casting
    // to a concrete renderer (e.g. NukeBGFX). Non-pure so existing backends that
    // don't implement it still compile; backends override as needed.
    virtual void setOnClose(bst::function<void()> cb) {}

    // --- Neutral UI seam ------------------------------------------------------
    // The renderer exposes a generic 2D draw capability so a UI module (ImGui or
    // anything else) can render through it without the renderer knowing the UI
    // library, and the UI without knowing the graphics API. Non-pure (no-op
    // defaults) so renderers that don't support UI still compile.
    //
    // Upload an RGBA8 texture (e.g. a font atlas); returns an opaque handle.
    virtual uint64_t createTexture2D(const void* rgbaPixels, int width, int height) { return 0; }
    // Release a texture previously returned by createTexture2D.
    virtual void destroyTexture2D(uint64_t handle) {}
    // Render backend-neutral 2D draw lists for this frame.
    virtual void renderDrawLists(const NukeUIDrawData& data) {}

    // --- World viewport / per-camera 3D seam ---------------------------------
    // Render targets are off-screen surfaces a camera can draw into; their color
    // texture can also be shown by the UI (ImGui::Image). Identified by a stable
    // id (so resize keeps the same id). 0 == the backbuffer.
    virtual uint64_t createRenderTarget(int w, int h) { return 0; }
    virtual void     resizeRenderTarget(uint64_t id, int w, int h) {}
    virtual uint64_t getRenderTargetTexture(uint64_t id) { return 0; } // current color SRV handle
    // One camera pass: bind its target, set viewport, clear, set view/projection.
    virtual void     beginCamera(const NukeCameraDesc& cam) {}
    virtual void     endCamera() {}
    // Read back the most recent camera pass's view & projection matrices (row-major,
    // exactly as the renderer used them) so the editor can overlay a gizmo that lines
    // up with the rendered image.
    virtual void     getViewProj(float* view16, float* proj16) {}

    virtual void keyboard(int key, int scancode, int action, int mods) = 0;
    virtual void mouseMove(double xpos, double ypos) = 0;
    virtual void mouseClick(int button, int action, int mods) = 0;
    virtual void setCursorMode(int mode) = 0;
    virtual void rawMouse(double xpos, double ypos) = 0;
    virtual void mouseEnterLeave(int entered) = 0;

    // NOTE: new virtuals go HERE, at the END of the interface — never mid-vtable. Inserting a
    // virtual earlier shifts every following slot, silently breaking any iRender consumer that
    // wasn't rebuilt (e.g. NukeImGui calling setOnGUI/renderDrawLists at the wrong offsets).
    //
    // Supply a named shader's source BEFORE init() (the renderer compiles it when building its
    // pipelines). The renderer does NO file IO — the engine loads shader files and pushes them
    // here. Built-in names the renderer needs: "world.vs","world.ps","ui.vs","ui.ps".
    virtual void setShaderSource(const char* name, const char* source) {}

    // Build a world-type pipeline from a shader asset's VS+PS source; returns an opaque handle
    // (0 on failure). Materials carry this handle (via their Shader) and renderObject selects
    // the pipeline by it; 0 / unknown falls back to the built-in default. Call after init().
    // `name` is the shader asset's guid/name: the renderer uses it to auto-build an RT closest-hit
    // for ray-traced reflections when a "<name>.surf.hlsl" surface file exists (no manual .rchit).
    virtual uint64_t createShaderPipeline(const char* name, const char* vs, const char* ps) { return 0; }

    // Draw a selection outline (editor highlight) around one mesh, using the same world transform as
    // renderObject. Call within the camera pass, after the objects. No-op if the renderer has no
    // outline pipeline. The renderer owns the look (inverted-hull silhouette).
    virtual void renderSelectionOutline(Mesh* mesh, const float pos[3], const float quat[4], const float scale[3]) {}

    // Set the OS window title (editor shows "NukeEngine Editor — <project> — <world>").
    virtual void setWindowTitle(const char* title) {}

    // Is the OS window currently focused/foreground? Editor uses focus-gain to re-check disk changes.
    virtual bool isWindowFocused() { return true; }

    // OS window maximized state (persisted in editor_state.json).
    virtual bool isWindowMaximized() { return false; }
    virtual void setWindowMaximized(bool maximized) {}

    // Pull-style input (so a runtime UI can read input without owning the push callbacks). Cursor in
    // window pixels; button 0/1/2 = left/right/middle.
    virtual void getCursorPos(double& x, double& y) { x = 0; y = 0; }
    virtual bool isMouseButtonDown(int button) { return false; }

    // Target the next renderDrawLists at a render target (id from createRenderTarget; 0 = backbuffer),
    // WITHOUT clearing it — so a runtime UI can composite into the camera/viewport RT. Pass 0 to reset.
    virtual void bindRenderTarget(uint64_t rtId) {}

    // Drop the cached GPU texture for this engine Texture (e.g. after a hot-reload) so it re-uploads.
    virtual void invalidateTexture(Texture* t) {}

    // Set the scene lights for the next camera pass(es). The engine gathers Light components and
    // pushes them here before drawing; the renderer packs them into its PBR lighting buffer. Passing
    // count 0 lets the renderer fall back to a default directional sun.
    virtual void setLights(const NukeLight* lights, int count) {}

    // --- Shadows (directional + spot 2D shadow maps; point cube maps are a later phase) --------
    // After setLights the renderer assigns a shadow-map slot to each shadow-casting dir/spot light and
    // returns the number of depth passes to run. The engine runs each pass (beginShadowPass(p) ->
    // renderShadowObject per caster -> endShadowPass) BEFORE the camera pass; the world pass samples them.
    virtual int  shadowPassCount() { return 0; }
    virtual void beginShadowPass(int pass) {}
    virtual void renderShadowObject(Mesh* mesh, const float pos[3], const float quat[4], const float scale[3], Material* mat) {}
    virtual void endShadowPass() {}

    // Set the world environment/sky for the next camera pass(es): the renderer draws a procedural sky
    // behind the scene and uses the ambient term. Engine pushes it from the Environment component.
    virtual void setSky(const NukeSky& sky) {}

    // Files dropped onto the OS window from the desktop/Explorer. The editor hooks this to import dropped
    // models/images into the current browser folder. Called once per dropped path (on the main thread).
    virtual void setOnFileDrop(bst::function<void(const char* path)> cb) {}

    // Hardware anti-aliasing sample count for the world passes (1 = off, else 2/4/8 snapped + clamped to
    // device support). Rebuilds the affected pipelines + targets; safe to call before init (applied at setup).
    virtual void setMSAA(int samples) {}
    virtual int  getMSAA() { return 1; }

    // HDR rendering: on = scene in float (RGBA16F) + tonemap in the post pass (real dynamic range, enables
    // bloom); off = LDR (RGBA8) + tonemap inline (cheaper). Rebuilds pipelines/targets; safe before init.
    virtual void setHDR(bool on) {}
    virtual bool getHDR() { return false; }

    // HDR DISPLAY output (distinct from setHDR's internal HDR rendering): create an HDR10 (RGB10A2 + PQ)
    // swap chain so the backbuffer drives a real HDR monitor. Must be called BEFORE init. The Player enables
    // it; the editor does not (its viewport is an SDR preview). No-op / SDR fallback if the display isn't HDR.
    virtual void setHDROutput(bool on) {}
    virtual bool getHDROutput() { return false; }   // true only if an HDR10 swap chain is actually active

    // HDR10 display mapping: diffuse-white luminance + highlight peak (nits). Used by the final PQ encode.
    virtual void setHDRNits(float paperWhite, float peak) {}

    // Global shadow settings (from the World's settings). resolution rebuilds the maps; distance = directional
    // ortho extent; depthBias/normalBias fight acne/peter-panning; softness scales the PCF kernel.
    virtual void setShadowSettings(int resolution, float distance, float depthBias, float normalBias, float softness) {}

    // Global RTX reflection quality (from Project Settings). The per-camera "rtreflect" post effect is the on/off
    // switch; these control how it traces. intensity = strength; maxDist = ray length; bounces = recursion depth;
    // roughCutoff = roughness past which reflections fade.
    virtual void setRTReflection(float intensity, float maxDist, int bounces, float roughCutoff) {}

    // Build a post-process effect pipeline from a fullscreen pixel shader (sampling `g_Source`, params in a
    // `PostParams` cbuffer). Returns an opaque handle (0 on failure). Call after init; one per post shader asset.
    virtual uint64_t createPostPipeline(const char* name, const char* ps) { return 0; }

    // Set the ordered post-process effect chain for the NEXT camera pass. The renderer ping-pongs the camera's
    // HDR result through each stage (fullscreen), then tonemaps/encodes. Empty = no effects (just tonemap).
    virtual void setPostChain(const NukePostStage* stages, int count) {}

    // --- Reflection probe (scene-captured cubemap) ----------------------------------------------------
    // Create a cube color target (HDR, mipped) for a probe; returns a stable id (0 on failure).
    virtual uint64_t createReflectionCube(int resolution) { return 0; }
    // Begin/end ONE cube-face capture pass: the renderer builds the face view/proj from pos + face 0..5, binds
    // that face, clears, draws the sky. The engine draws the scene between them via renderObject. The probe is
    // NOT sampled during capture (analytic IBL) so there's no feedback; endCubeFace on face 5 builds the mips.
    virtual void beginCubeFace(uint64_t cube, int face, const float pos[3], float nearZ, float farZ) {}
    virtual void endCubeFace(uint64_t cube, int face) {}
    // Bind a probe cubemap for the upcoming camera pass(es) (world shader samples it for reflections).
    // cube == 0 disables probe reflections (analytic-sky fallback).
    // boxHalf = parallax box half-extents (world units) centred on pos; {0,0,0} disables parallax correction.
    virtual void setReflectionProbe(uint64_t cube, const float pos[3], float intensity, float farZ, const float boxHalf[3]) {}

    // G-buffer prepass for screen-space reflections. A single-sample pass (run BEFORE beginCamera, same camera)
    // that captures normal + roughness + metalness + depth into the renderer's G-buffer; the built-in "ssr"
    // post effect samples it. Driven only when the camera's post chain contains an SSR effect.
    virtual void beginGBufferPass(const NukeCameraDesc& cam) {}
    virtual void renderGBufferObject(Mesh* mesh, Material* mat, const float pos[3], const float quat[4], const float scale[3],
                                     const float prevPos[3] = nullptr, const float prevQuat[4] = nullptr, const float prevScale[3] = nullptr) {}
    virtual void endGBufferPass() {}

    // Ray tracing (D3D12 + DXR-capable GPU). rtAvailable() gates all of it. Per frame, before the camera passes:
    // beginRTScene() -> addRTInstance() per mesh -> buildRTScene() builds the BLAS-per-mesh + a fresh TLAS the
    // world shader ray-queries (RT shadows, later RT reflections). No-op / false on D3D11 or unsupported GPUs.
    virtual bool rtAvailable() { return false; }
    virtual void beginRTScene() {}
    virtual void addRTInstance(Mesh* mesh, Material* mat, const float pos[3], const float quat[4], const float scale[3], bool inReflections = true) {}
    virtual void buildRTScene() {}

    // TAA: called per camera BEFORE beginGBufferPass/beginCamera. When enabled the renderer jitters the colour
    // projection sub-pixel each frame + accumulates via a per-camera history (needs the depth prepass too).
    virtual void setCameraTAA(bool enabled) {}

    // Ask the render loop to end after the current frame (the window closes, loop() returns,
    // the host shuts down). Game::Quit() drives this in the Player.
    virtual void requestClose() {}

    // Debug/gizmo line: world-space segment + RGBA color, accumulated for THIS frame
    // (thread-safe; the fixed thread may emit too), drawn depth-tested into every camera
    // pass and cleared at the next frame's start. The engine-side DebugDraw facade
    // decomposes wire shapes into these — backends only ever see lines.
    virtual void drawDebugLine(const float a[3], const float b[3], const float color[4]) {}

    // --- UI multi-viewport: detachable NATIVE OS windows -----------------------------
    // The UI module's platform backend (imgui_impl_glfw in NukeImGui) creates and manages
    // the secondary OS windows itself, sharing the process's ONE GLFW instance with the
    // renderer (both link glfw3.dll). The renderer's part is small: hand out the main
    // platform window, and own one swap chain per secondary window.
    //
    // Main platform window handle (GLFWwindow*). Null = the backend has no windowing
    // layer the UI can mount on; the UI then stays single-window.
    // (END of vtable — appended so existing slot indices don't shift.)
    virtual void* nativeWindow() { return nullptr; }
    // Render a UI draw list into a secondary OS window (nativeHandle = HWND on Windows,
    // from the platform backend). The renderer lazily creates a swap chain per window,
    // resizes it to w x h, draws, and presents (no vsync — the main window already syncs).
    virtual void uiViewportRender(void* nativeHandle, int w, int h, const NukeUIDrawData& data) {}
    // The secondary window is being destroyed — release its swap chain.
    virtual void uiViewportDestroy(void* nativeHandle) {}

    // --- frame statistics (editor status bar, roadmap 2.3) ---------------------------
    // Scene geometry submitted during the LAST completed frame: draw calls + triangles
    // (world/gbuffer/probe/shadow passes; UI and post-process excluded). Backends that
    // don't count report zeros.
    virtual void getFrameStats(int& drawCalls, int& triangles) { drawCalls = 0; triangles = 0; }

    // --- runtime-GUI input (roadmap 2.5) ----------------------------------------------
    // Polled by the runtime GUI backend (NukeGUI) once per frame. The queues DRAIN on
    // fetch and are size-capped inside the backend, so an idle consumer can't leak.
    // Key codes/actions/mods use GLFW numbering (the seam's de-facto codes — _UIkey).
    virtual int  fetchUIChars(unsigned int* out, int max) { return 0; }                  // typed codepoints
    virtual int  fetchUIKeys(int* keys, int* actions, int* mods, int max) { return 0; }  // key events
    virtual void getScrollDelta(double& x, double& y) { x = 0; y = 0; }                  // wheel since last call
    virtual const char* getClipboardText() { return ""; }        // valid until the next call
    virtual void setClipboardText(const char* text) {}

    // Drop every cached GPU resource built from this mesh (buffers, BLAS). MUST be called
    // before a Mesh object is deleted (asset removal, skinned-instance release) — a stale
    // cache entry keyed by a freed pointer serves WRONG buffers when the allocator reuses
    // the address (render safety).
    virtual void invalidateMesh(Mesh* m) {}

    // Runtime WINDOW control (Game.Set* window API — a game's video-settings menu). Applies
    // what the platform can change live: size, display mode (windowed / borderless-fullscreen
    // / exclusive-fullscreen), decoration (borderless), whole-window opacity. `transparent`
    // is a per-pixel creation-time property — it can't toggle live, so it takes effect on the
    // next launch (via the persisted config). No-op on backends without a window.
    virtual void applyWindow(const WindowDesc& desc) {}

    // Vertical sync for the MAIN window's Present: true = cap to the display refresh rate (no
    // tearing), false = present immediately (uncapped FPS — for benchmarking or low-latency).
    // Live-settable, honoured from config/main.json "vsync" and Game.SetVSync. Default on.
    // Secondary UI-viewport windows always present unsynced (the main window paces the frame).
    virtual void setVSync(bool on) {}
    virtual bool getVSync() { return true; }

    // ABI: new virtuals are appended at the END of the class, NEVER inserted mid-vtable —
    // plugins are separate DLLs built at different times, and an inserted slot shifts every
    // later one (an old NukeGUI.dll calling getScrollDelta through a shifted slot landed in
    // fetchUIKeys and corrupted its caller's stack). Keep it that way.
};

}  // namespace nuke

#endif // IRENDER_H
