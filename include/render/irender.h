#ifndef IRENDER_H
#define IRENDER_H
#include <boost/function.hpp>
#include <cstdint>
#include <API/Model/Transform.h>
#include <API/Model/Mesh.h>
#include "UIDrawData.h"

namespace b = boost;

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
    virtual int render() = 0;
    virtual void renderObject(Mesh* mesh, Material* mat, Transform* transform) = 0;
    virtual int init(int w, int h) = 0;
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
    virtual void keyboard(int key, int scancode, int action, int mods) = 0;
    virtual void mouseMove(double xpos, double ypos) = 0;
    virtual void mouseClick(int button, int action, int mods) = 0;
    virtual void setCursorMode(int mode) = 0;
    virtual void rawMouse(double xpos, double ypos) = 0;
    virtual void mouseEnterLeave(int entered) = 0;
//    virtual ~iRender(){
//    }
};

#endif // IRENDER_H
