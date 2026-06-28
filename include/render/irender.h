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
    bool  fullscreen  = false;
    bool  transparent = false;  // per-pixel alpha to desktop (needs renderer DComp support)
    float opacity     = 1.0f;   // whole-window opacity 0..1 (cheap, always works)
};

class iRender
{
    friend class NukeOGL;
    friend class NukeBGFX;
private:
    static iRender* _instance;
public:
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
    virtual uint64_t createShaderPipeline(const char* vs, const char* ps) { return 0; }

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
//    virtual ~iRender(){
//    }
};

}  // namespace nuke

#endif // IRENDER_H
