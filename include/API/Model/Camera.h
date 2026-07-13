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

class NUKEENGINE_API Camera : public Component
{
	NUKE_CLASS(Camera, Component)
private:
	boost::thread* renderThread;


public:
    [[nuke::prop]] bool invertMouse = false;
    iRender *renderer = nullptr;
	Texture renderTex;
	[[nuke::prop]] int r_width = 640;
	[[nuke::prop]] int r_height = 480;
    [[nuke::prop]] float fov = 90;
    [[nuke::prop]] float _near = 0.3f;
    [[nuke::prop]] float _far = 10000;
	unsigned long int renderLayers;
    [[nuke::prop]] bool freeMode = false;

    // Per-camera render contract (see World::Render).
    uint64_t renderTarget = 0;                       // iRender RT id; 0 = backbuffer
    [[nuke::prop]] int depth = 0;                    // render order (lower drawn first)
    // Background: the colour the camera clears to where nothing is drawn. The ALPHA matters
    // for a transparent window (Game.SetTransparent) — set it below 1 for a see-through
    // background (0 = fully transparent -> desktop shows); ignored on an opaque window.
    [[nuke::prop(label="Background")]] Color background = Color(0.20f, 0.30f, 0.45f, 1.0f);
    // If set, the camera renders into this RenderTexture asset (World::Render resolves it -> renderTarget).
    [[nuke::prop(asset="texture", label="Target Texture")]] std::string targetTexGuid;


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


	void Init(Atom* parent);
	void FixedUpdate();
	void Update();
	void Reset();
	void Pause();
	void Destroy();
};
}  // namespace nuke

#endif // !NUKEE_CAMERA_H
