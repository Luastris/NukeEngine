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

// Camera projection kind. A REFLECTED enum (typed in C#/Lua, like WindowMode): SetProjection/
// GetProjection take/return it; the inspector shows it as a combo via the prop's enum= hint.
enum class Projection : int { Perspective = 0, Orthographic = 1 };
template<> struct NukeEnumInfo<Projection>
{
	static constexpr bool reflected = true;
	static const char* Name() { return "Projection"; }
	static void Register() { Reflect_RegisterEnum("Projection", { "Perspective", "Orthographic" }); }
};

class NUKEENGINE_API Camera : public Component
{
	NUKE_CLASS(Camera, Component)
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
    // Projection: perspective (uses fov) or orthographic (uses orthoSize = half-height in world
    // units). projTransition is the ease speed toward the target when it changes (0 = instant);
    // the actual blend is animated per-frame in World::Render so switching is smooth.
    [[nuke::prop(label="Projection", enum="Perspective,Orthographic", tip="Perspective uses FOV; Orthographic uses Ortho Size")]] Projection projection = Projection::Perspective;
    [[nuke::prop(label="Ortho Size", tip="Half-height of the orthographic view, world units")]] float orthoSize = 5.0f;
    [[nuke::prop(label="Proj Transition", tip="Ease speed of the perspective/orthographic switch; 0 = instant")]] float projTransition = 8.0f;
    float projBlend = 0.0f;        // runtime 0=perspective..1=orthographic (eased; not serialized)
    bool  projBlendInit = false;   // first frame snaps the blend to the target (no open-time animation)
    // Render-layer MASK: bit i = this camera renders atoms with Atom::layer == i (see nuke::Layers).
    // -1 = everything. The inspector draws it as a named multi-select (CamComponent override).
    [[nuke::prop(label="Layer Mask", widget="layers", tip="Which render layers this camera draws (layers are named in Project Settings > Layers; atoms pick theirs via the Layer field)")]] int layerMask = -1;
    [[nuke::prop(label="Free Mode", tip="WASD + mouse free-fly control for this camera")]] bool freeMode = false;
    // The camera the GAME is viewed through (UE 'possess'): PIE's "game camera" view and any
    // World::MainCamera() lookup resolve to the camera marked Main, else the world's first camera.
    [[nuke::prop(label="Main Camera", tip="The game is viewed through this camera (PIE game view / player). With none marked Main, the world's first camera is used")]] bool mainCamera = false;
    // Set by the EDITOR on its own camera (not serialized): screen-space canvases are NOT pinned to
    // this camera — they render as editable world-plane rectangles instead (see World::DrawSprites).
    bool editorCamera = false;

    // Per-camera render contract (see World::Render).
    uint64_t renderTarget = 0;                       // iRender RT id; 0 = backbuffer
    [[nuke::prop(label="Depth", tip="Render order among cameras: lower depth draws first, higher draws on top (overlays)")]] int depth = 0;
    // Background: the colour the camera clears to where nothing is drawn. The ALPHA matters
    // for a transparent window (Game.SetTransparent) — set it below 1 for a see-through
    // background (0 = fully transparent -> desktop shows); ignored on an opaque window.
    [[nuke::prop(label="Background", tip="Clear colour where nothing is drawn. Alpha < 1 makes a transparent window see-through (Game.SetTransparent)")]] Color background = Color(0.20f, 0.30f, 0.45f, 1.0f);
    // If set, the camera renders into this RenderTexture asset (World::Render resolves it -> renderTarget).
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


	// Reflected camera API (C#/Lua). Setting the projection animates the switch (per projTransition).
	[[nuke::func]] void       SetProjection(Projection p);
	[[nuke::func]] Projection GetProjection();
	[[nuke::func]] void       SetOrthoSize(double size);
	[[nuke::func]] double     GetOrthoSize();
	// Render-layer mask (bitmask over nuke::Layers indices; compose with Layers.MaskOf("UI,FX")).
	[[nuke::func]] void       SetLayerMask(double mask);
	[[nuke::func]] double     GetLayerMask();

	void Init(Atom* parent);
	void FixedUpdate();
	void Update();
	void Reset();
	void Pause();
	void Destroy();
};
}  // namespace nuke

#endif // !NUKEE_CAMERA_H
