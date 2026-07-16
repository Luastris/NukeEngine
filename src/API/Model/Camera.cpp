#include "API/Model/Camera.h"
#include "API/Model/Layers.h"
#include "API/Model/Atom.h"
#include "API/Model/Transform.h"
#include "API/Model/Texture.h"
#include "API/Model/Screen.h"   // screen->world rays live in game-screen pixel space (6.7)
#include "interface/AppInstance.h"
#include <cmath>
#include <algorithm>

namespace nuke {

Camera::Camera() : Component("Camera") {}

Camera::Camera(iRender* renderer) : Component("Camera")
{
	this->renderer = renderer;
	this->renderer->transform = transform;
	renderer->width = this->r_width;
	renderer->height = this->r_height;
	renderer->fov = fov;
	renderer->Far = _far;
	renderer->Near = _near;
	// Secondary (render-target) cameras own their renderer and must init it.
	// The main renderer is the one held by AppInstance and is initialized by the bootstrap.
	if (renderer != AppInstance::GetSingleton()->render)
		{ WindowDesc _wd; _wd.w = r_width; _wd.h = r_height; renderer->init(_wd); }
	else
		cout << "[Camera]\t\t" << "[!] Camera of main renderer" << endl;
}

Camera::Camera(Atom* parent, iRender* renderer) : Component("Camera")
{
	this->renderer = renderer;
	Init(parent);
}

Vector3 Camera::ScreenPosToWorldRay(
	int mouseX, int mouseY,             // Mouse position, in pixels, from bottom-left corner of the window
	int screenWidth, int screenHeight,  // Window size, in pixels
	glm::mat4 ViewMatrix,               // Camera position and orientation
	glm::mat4 ProjectionMatrix         // Camera parameters (ratio, field of view, near and far planes)
	, glm::vec3& out_origin
) {

	// The ray Start and End positions, in Normalized Device Coordinates (Have you read Tutorial 4 ?)
	glm::vec4 RayStart_NDC(
		(2.0f * mouseX) / screenWidth - 1.0f, // [0,1024] -> [-1,1]
		1.0f - (2.0f * mouseY) / screenHeight, // [0, 768] -> [-1,1]
		-1.0, // The near plane maps to Z=-1 in Normalized Device Coordinates
		1.0f
	);
	glm::vec4 RayEnd_NDC(
		(2.0f * mouseX) / screenWidth - 1.0f,
		1.0f - (2.0f * mouseY) / screenHeight,
		0.0,
		1.0f
	);


	// The Projection matrix goes from Camera Space to NDC.
	// So inverse(ProjectionMatrix) goes from NDC to Camera Space.
	glm::mat4 InverseProjectionMatrix = glm::inverse(ProjectionMatrix);

	// The View Matrix goes from World Space to Camera Space.
	// So inverse(ViewMatrix) goes from Camera Space to World Space.
	glm::mat4 InverseViewMatrix = glm::inverse(ViewMatrix);

	glm::vec4 RayStart_camera = InverseProjectionMatrix * RayStart_NDC;
	RayStart_camera /= RayStart_camera.w;
	glm::vec4 RayStart_world = InverseViewMatrix * RayStart_camera;
	RayStart_world /= RayStart_world.w;
	glm::vec4 RayEnd_camera = InverseProjectionMatrix * RayEnd_NDC;
	RayEnd_camera /= RayEnd_camera.w;
	glm::vec4 RayEnd_world = InverseViewMatrix * RayEnd_camera;
	RayEnd_world /= RayEnd_world.w;


	// Faster way (just one inverse)
	//glm::mat4 M = glm::inverse(ProjectionMatrix * ViewMatrix);
	//glm::vec4 RayStart_world = M * RayStart_NDC; RayStart_world/=RayStart_world.w;
	//glm::vec4 RayEnd_world   = M * RayEnd_NDC  ; RayEnd_world  /=RayEnd_world.w;


	glm::vec3 RayDir_world(RayEnd_world - RayStart_world);
	RayDir_world = glm::normalize(RayDir_world);


	out_origin = glm::vec3(RayStart_world);
	auto out = glm::normalize(RayDir_world);
	return { out.x, out.y, out.z };
}

bool  Camera::RayOBBIntersection(
	glm::vec3 ray_origin,        // Ray origin, in world space
	glm::vec3 ray_direction,     // Ray direction (NOT target position!), in world space. Must be normalize()'d.
	glm::vec3 aabb_min,          // Minimum X,Y,Z coords of the mesh when not transformed at all.
	glm::vec3 aabb_max,          // Maximum X,Y,Z coords. Often aabb_min*-1 if your mesh is centered, but it's not always the case.
	glm::mat4 ModelMatrix,       // Transformation applied to the mesh (which will thus be also applied to its bounding box)
	float& intersection_distance // Output : distance between ray_origin and the intersection with the OBB
) {

	// Intersection method from Real-Time Rendering and Essential Mathematics for Games

	float tMin = 0.0f;
	float tMax = 100000.0f;

	glm::vec3 OBBposition_worldspace(ModelMatrix[3].x, ModelMatrix[3].y, ModelMatrix[3].z);

	glm::vec3 delta = OBBposition_worldspace - ray_origin;

	// Test intersection with the 2 planes perpendicular to the OBB's X axis
	{
		glm::vec3 xaxis(ModelMatrix[0].x, ModelMatrix[0].y, ModelMatrix[0].z);
		float e = glm::dot(xaxis, delta);
		float f = glm::dot(ray_direction, xaxis);

		if (fabs(f) > 0.001f) { // Standard case

			float t1 = (e + aabb_min.x) / f; // Intersection with the "left" plane
			float t2 = (e + aabb_max.x) / f; // Intersection with the "right" plane
			// t1 and t2 now contain distances betwen ray origin and ray-plane intersections

			// We want t1 to represent the nearest intersection,
			// so if it's not the case, invert t1 and t2
			if (t1 > t2) {
				float w = t1; t1 = t2; t2 = w; // swap t1 and t2
			}

			// tMax is the nearest "far" intersection (amongst the X,Y and Z planes pairs)
			if (t2 < tMax)
				tMax = t2;
			// tMin is the farthest "near" intersection (amongst the X,Y and Z planes pairs)
			if (t1 > tMin)
				tMin = t1;

			// And here's the trick :
			// If "far" is closer than "near", then there is NO intersection.
			// See the images in the tutorials for the visual explanation.
			if (tMax < tMin)
				return false;

		}
		else { // Rare case : the ray is almost parallel to the planes, so they don't have any "intersection"
			if (-e + aabb_min.x > 0.0f || -e + aabb_max.x < 0.0f)
				return false;
		}
	}


	// Test intersection with the 2 planes perpendicular to the OBB's Y axis
	// Exactly the same thing than above.
	{
		glm::vec3 yaxis(ModelMatrix[1].x, ModelMatrix[1].y, ModelMatrix[1].z);
		float e = glm::dot(yaxis, delta);
		float f = glm::dot(ray_direction, yaxis);

		if (fabs(f) > 0.001f) {

			float t1 = (e + aabb_min.y) / f;
			float t2 = (e + aabb_max.y) / f;

			if (t1 > t2) { float w = t1; t1 = t2; t2 = w; }

			if (t2 < tMax)
				tMax = t2;
			if (t1 > tMin)
				tMin = t1;
			if (tMin > tMax)
				return false;

		}
		else {
			if (-e + aabb_min.y > 0.0f || -e + aabb_max.y < 0.0f)
				return false;
		}
	}


	// Test intersection with the 2 planes perpendicular to the OBB's Z axis
	// Exactly the same thing than above.
	{
		glm::vec3 zaxis(ModelMatrix[2].x, ModelMatrix[2].y, ModelMatrix[2].z);
		float e = glm::dot(zaxis, delta);
		float f = glm::dot(ray_direction, zaxis);

		if (fabs(f) > 0.001f) {

			float t1 = (e + aabb_min.z) / f;
			float t2 = (e + aabb_max.z) / f;

			if (t1 > t2) { float w = t1; t1 = t2; t2 = w; }

			if (t2 < tMax)
				tMax = t2;
			if (t1 > tMin)
				tMin = t1;
			if (tMin > tMax)
				return false;

		}
		else {
			if (-e + aabb_min.z > 0.0f || -e + aabb_max.z < 0.0f)
				return false;
		}
	}

	intersection_distance = tMin;
	return true;

}

void Camera::ProcessKeyboard() {
	if (!freeMode)
		return;

	if (KeyBoard::getSingleton()->getKeyPressed('w'))
		transform->position += transform->direction() * 3;
	if (KeyBoard::getSingleton()->getKeyPressed('a'))
		transform->position += transform->right() * -3;
	if (KeyBoard::getSingleton()->getKeyPressed('s'))
		transform->position += transform->direction() * -3;
	if (KeyBoard::getSingleton()->getKeyPressed('d'))
		transform->position += transform->right() * 3;

	//cout << "CAM MOV [ " << transform->position.toStringA() << " ]" << endl;
}


void Camera::Init(Atom* parent)
{
	if (!renderer) renderer = AppInstance::GetSingleton()->render;   // e.g. cameras loaded from a scene
	transform = &parent->GetTransform();
	if (this->renderer)
		this->renderer->transform = transform;
	parent->components.push_back(this);
	if (renderer != AppInstance::GetSingleton()->render)
		{ WindowDesc _wd; _wd.w = r_width; _wd.h = r_height; renderer->init(_wd); }
	else
		cout << "[Camera]\t\t" << "[!] Camera of main renderer" << endl;
	//*KeyBoard::getSingleton() += bst::function<void(unsigned char, int, int)>(b::bind(&Camera::ProcessKeyboard, bst::ref(*this), _1, _2, _3));
//        *Mouse::getSingleton() += bst::function<void(int, int, int, int)>(b::bind(&Camera::ProcessMouse, bst::ref(*this), _1, _2, _3, _4));
//        *Mouse::getSingleton() &= bst::function<void(int, int)>(b::bind(&Camera::ProcessMouseMove, bst::ref(*this), _1, _2));
//        *Mouse::getSingleton() *= bst::function<void(int, int, int, int)>(b::bind(&Camera::mouseScroll, bst::ref(*this), _1, _2, _3, _4));
}
void Camera::FixedUpdate() {}
void Camera::Update() {
	//		renderer->width = this->r_width;
	//		renderer->height = this->r_height;
	renderer->fov = fov;
	renderer->Far = _far;
	renderer->Near = _near;
	//renderer->_crosshair = crosshair;


	// TODO: It crashes app on MacOS. Fix it.
#ifndef __APPLE__
	renderer->update();
#endif
	ProcessKeyboard();
}

void Camera::SetProjection(Projection p) { projection = p; }   // World::Render eases projBlend toward it
Projection Camera::GetProjection()       { return projection; }
void   Camera::SetOrthoSize(double size) { orthoSize = (float)size; }
double Camera::GetOrthoSize()            { return orthoSize; }
void   Camera::SetLayerMask(double mask) { layerMask = (int)(long long)mask; }
double Camera::GetLayerMask()            { return (double)(unsigned int)layerMask; }

// --- screen -> world (6.7) -----------------------------------------------------------------
// Mirrors the renderer's projection exactly (incl. the live persp<->ortho blend): NDC from
// game-screen pixels over Screen dims; perspective spreads DIRECTION from the camera origin,
// orthographic spreads ORIGIN across the view rectangle with a fixed direction.

Vector3 Camera::ScreenRayOrigin(double px, double py)
{
	if (!transform) return Vector3(0, 0, 0);
	const double w = std::max(1.0, Screen::Width()), h = std::max(1.0, Screen::Height());
	const double ndcx = px / w * 2.0 - 1.0;
	const double ndcy = 1.0 - py / h * 2.0;   // top-left pixel origin -> +y up NDC
	Vector3 p = transform->globalPosition();
	if (projBlend >= 0.5f)   // orthographic: parallel rays, origin slides over the view rect
	{
		const double oh = (orthoSize > 1e-4f) ? orthoSize : 1.0;
		const double ow = oh * (w / h);
		Vector3 r = transform->right(), u = transform->up();
		return Vector3(p.x + ndcx * ow * r.x + ndcy * oh * u.x,
		               p.y + ndcx * ow * r.y + ndcy * oh * u.y,
		               p.z + ndcx * ow * r.z + ndcy * oh * u.z);
	}
	return p;
}

Vector3 Camera::ScreenRayDir(double px, double py)
{
	if (!transform) return Vector3(0, 0, 1);
	Vector3 f = transform->direction();
	if (projBlend >= 0.5f) return f;   // orthographic: fixed direction
	const double w = std::max(1.0, Screen::Width()), h = std::max(1.0, Screen::Height());
	const double ndcx = px / w * 2.0 - 1.0;
	const double ndcy = 1.0 - py / h * 2.0;
	const double thf = std::tan((double)fov * 0.5 * 0.017453292519943295);
	Vector3 r = transform->right(), u = transform->up();
	Vector3 d(f.x + ndcx * thf * (w / h) * r.x + ndcy * thf * u.x,
	          f.y + ndcx * thf * (w / h) * r.y + ndcy * thf * u.y,
	          f.z + ndcx * thf * (w / h) * r.z + ndcy * thf * u.z);
	const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (len > 1e-12) { d.x /= len; d.y /= len; d.z /= len; }
	return d;
}

Vector3 Camera::ScreenToWorldPoint(double px, double py, double depth)
{
	Vector3 o = ScreenRayOrigin(px, py), d = ScreenRayDir(px, py);
	return Vector3(o.x + d.x * depth, o.y + d.y * depth, o.z + d.z * depth);
}

void Camera::Reset() {}
void Camera::Pause() {}

void Camera::Destroy()
{
	// NEVER deinit the MAIN renderer — this camera doesn't own it. Deleting a camera
	// atom used to tear the whole device down mid-session (render safety). Only a
	// camera-OWNED secondary renderer (init'ed in Init when renderer != main) is ours.
	if (renderer && renderer != AppInstance::GetSingleton()->render)
		renderer->deinit();
	renderer = nullptr;
}

}  // namespace nuke