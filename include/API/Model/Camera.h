#pragma once
#ifndef NUKEE_CAMERA_H
#define NUKEE_CAMERA_H
#include "NukeAPI.h"
#include <string>
#include <boost/thread.hpp>
#include "render/irender.h"
#include <boost/bind.hpp>
#include "reflect/Reflect.h"

namespace nuke {

// Camera projection kind. Reflected enum (typed in C#/Lua; inspector combo via the enum= hint).
enum class Projection : int { Perspective = 0, Orthographic = 1 };
template<> struct NukeEnumInfo<Projection>
{
	static constexpr bool reflected = true;
	static const char* Name() { return "Projection"; }
	static void Register() { Reflect_RegisterEnum("Projection", { "Perspective", "Orthographic" }); }
};

class NUKEENGINE_API Camera : public Component
{
	NUKE_CLASS(Camera, Component, "Rendering")
private:
	boost::thread* renderThread;


public:
    [[nuke::prop(label="Invert Mouse", tip="Invert mouse Y in free-fly mode")]] bool invertMouse = false;
    iRender *renderer = nullptr;
	Texture renderTex;
	[[nuke::prop]] int r_width = 640;
	[[nuke::prop]] int r_height = 480;
    [[nuke::prop(label="FOV", tip="Vertical field of view, degrees (perspective projection)")]] float fov = 90;
    [[nuke::prop(label="Near", tip="Near clip plane distance — nothing closer is drawn")]] float _near = 0.3f;
    [[nuke::prop(label="Far", tip="Far clip plane distance — nothing farther is drawn")]] float _far = 10000;
    // Perspective uses fov, orthographic uses orthoSize (half-height in world units);
    // projTransition is the ease speed toward the target (0 = instant), blended in World::Render.
    [[nuke::prop(label="Projection", enum="Perspective,Orthographic", tip="Perspective uses FOV; Orthographic uses Ortho Size")]] Projection projection = Projection::Perspective;
    [[nuke::prop(label="Ortho Size", tip="Half-height of the orthographic view, world units")]] float orthoSize = 5.0f;
    [[nuke::prop(label="Proj Transition", tip="Ease speed of the perspective/orthographic switch; 0 = instant")]] float projTransition = 8.0f;
    float projBlend = 0.0f;        // runtime 0=perspective..1=orthographic (eased; not serialized)
    bool  projBlendInit = false;   // first frame snaps the blend to the target (no open-time animation)
    // Render-layer mask: bit i = render atoms with Atom::layer == i (nuke::Layers). -1 = everything.
    [[nuke::prop(label="Layer Mask", widget="layers", tip="Which render layers this camera draws (layers are named in Project Settings > Layers; atoms pick theirs via the Layer field)")]] int layerMask = -1;
    [[nuke::prop(label="Free Mode", tip="WASD + mouse free-fly control for this camera")]] bool freeMode = false;
    // The camera the GAME is viewed through: World::MainCamera() resolves to the one marked
    // Main, else the world's first camera.
    [[nuke::prop(label="Main Camera", tip="The game is viewed through this camera (PIE game view / player). With none marked Main, the world's first camera is used")]] bool mainCamera = false;
    // Set by the EDITOR on its own camera (not serialized): screen-space canvases render as
    // editable world-plane rectangles instead of being pinned to it.
    bool editorCamera = false;

    // Per-camera render contract (see World::Render).
    uint64_t renderTarget = 0;                       // iRender RT id; 0 = backbuffer
    [[nuke::prop(label="Depth", tip="Render order among cameras: lower depth draws first, higher draws on top (overlays)")]] int depth = 0;
    // Clear colour. Alpha < 1 only matters on a transparent window (Game.SetTransparent).
    [[nuke::prop(label="Background", tip="Clear colour where nothing is drawn. Alpha < 1 makes a transparent window see-through (Game.SetTransparent)")]] Color background = Color(0.20f, 0.30f, 0.45f, 1.0f);
    // If set, the camera renders into this RenderTexture asset (resolved into renderTarget).
    [[nuke::prop(asset="texture", label="Target Texture", tip="Render into this texture asset instead of the screen")]] std::string targetTexGuid;


	Camera();

	Camera(iRender* renderer);

	Camera(Atom* parent, iRender* renderer);

	Vector3 ScreenPosToWorldRay(
		int mouseX, int mouseY,             // Mouse position, in pixels, from bottom-left corner of the window
		int screenWidth, int screenHeight,  // Window size, in pixels
		glm::mat4 ViewMatrix,               // Camera position and orientation
		glm::mat4 ProjectionMatrix         // Camera parameters (ratio, field of view, near and far planes)
		, glm::vec3& out_origin
	);

	bool  RayOBBIntersection(
		glm::vec3 ray_origin,        // Ray origin, in world space
		glm::vec3 ray_direction,     // Ray direction (NOT target position!), in world space. Must be normalize()'d.
		glm::vec3 aabb_min,          // Minimum X,Y,Z coords of the mesh when not transformed at all.
		glm::vec3 aabb_max,          // Maximum X,Y,Z coords. Often aabb_min*-1 if your mesh is centered, but it's not always the case.
		glm::mat4 ModelMatrix,       // Transformation applied to the mesh (which will thus be also applied to its bounding box)
		float& intersection_distance // Output : distance between ray_origin and the intersection with the OBB
	);

	void ProcessKeyboard();


	// Reflected camera API. Setting the projection animates the switch (per projTransition).
	[[nuke::func]] void       SetProjection(Projection p);
	[[nuke::func]] Projection GetProjection();
	[[nuke::func]] void       SetOrthoSize(double size);
	[[nuke::func]] double     GetOrthoSize();
	// Render-layer mask (bitmask over nuke::Layers indices; compose with Layers.MaskOf("UI,FX")).
	[[nuke::func]] void       SetLayerMask(double mask);
	[[nuke::func]] double     GetLayerMask();

	// Screen -> world ray. px/py are GAME-SCREEN pixels (Screen.Width/Height space, top-left
	// origin — what Input.MouseX/Y return). Ortho-aware: follows the live projection blend.
	[[nuke::func]] Vector3 ScreenRayOrigin(double px, double py);
	[[nuke::func]] Vector3 ScreenRayDir(double px, double py);
	// The world point `depth` units along that ray.
	[[nuke::func]] Vector3 ScreenToWorldPoint(double px, double py, double depth);

	void Init(Atom* parent);
	void FixedUpdate();
	void Update();
	void Reset();
	void Pause();
	void Destroy();
};
}  // namespace nuke

#endif // !NUKEE_CAMERA_H
