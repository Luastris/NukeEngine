#ifndef IRENDER_H
#define IRENDER_H
#include <boost/function.hpp>
#include <cstdint>
#include <API/Model/Transform.h>
#include <API/Model/Mesh.h>
#include "UIDrawData.h"

namespace nuke {

namespace b = boost;

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

    virtual void keyboard(int key, int scancode, int action, int mods) = 0;
    virtual void mouseMove(double xpos, double ypos) = 0;
    virtual void mouseClick(int button, int action, int mods) = 0;
    virtual void setCursorMode(int mode) = 0;
    virtual void rawMouse(double xpos, double ypos) = 0;
    virtual void mouseEnterLeave(int entered) = 0;
//    virtual ~iRender(){
//    }
};

}  // namespace nuke

#endif // IRENDER_H
